#include "War3MapCore/ObjectData/NativeDestructablePathing.h"
#include "NativeTerrainObjectInputs.h"

namespace War3Map::ObjectData {
using namespace NativeTerrainObjectDetail;

Result<MapData::TerrainNativeObjectPathing> InspectNativeTerrainObjects(std::span<const std::string_view> placedRawcodes,
    std::span<const std::string_view> specialRawcodes, std::span<const ArchiveExactMember> sources, std::size_t maximumWorkingBytes) {
    try {
        constexpr std::array<std::string_view, 6> paths{"war3map.w3b", "war3map.w3d", "units/destructabledata.slk",
            "doodads/doodads.slk", "units/destructablemetadata.slk", "doodads/doodadmetadata.slk"};
        if (sources.size() != 5 && sources.size() != paths.size())
            Fail("TERRAIN_NATIVE_OBJECT_SOURCE", "严格对象捕获需要完整五项基础来源和至多一项DoodadMetaData。");
        std::array<const ArchiveExactMember*, paths.size()> inputs{};
        std::size_t work = 0;
        for (const auto& source : sources) {
            const auto name = Text::LowerAscii(source.name);
            const auto found = std::find(paths.begin(), paths.end(), name);
            if (found == paths.end() || inputs[found - paths.begin()]) Fail("TERRAIN_NATIVE_OBJECT_SOURCE", "严格对象来源重复或越界。");
            if (!source.present && !source.bytes.empty()) Fail("TERRAIN_NATIVE_OBJECT_SOURCE", "缺失对象来源却携带字节。");
            inputs[found - paths.begin()] = &source;
            if (source.bytes.size() > (maximumWorkingBytes - work) / kWorkPerInputByte)
                Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "原生对象与实际表的解析工作超过累计剩余预算。");
            work += source.bytes.size() * kWorkPerInputByte;
        }
        if (std::any_of(inputs.begin(), inputs.begin() + 5, [](const auto* input) { return !input; }))
            Fail("TERRAIN_NATIVE_OBJECT_SOURCE", "严格对象捕获缺少基础来源。");
        const auto availableRecords = (maximumWorkingBytes - work) / (sizeof(std::string_view) * 4);
        if (placedRawcodes.size() > availableRecords || specialRawcodes.size() > availableRecords - placedRawcodes.size())
            Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "摆放分类工作超过累计剩余预算。");
        work += (placedRawcodes.size() + specialRawcodes.size()) * sizeof(std::string_view) * 4;
        UniqueTable(*inputs[2], "destructableid"); UniqueTable(*inputs[3], "doodid");
        const auto metadataCells = UniqueTable(*inputs[4], "id");
        const auto reader = [&](const std::string& path) -> Result<std::vector<std::byte>> {
            const auto name = Text::LowerAscii(path);
            for (const auto* input : inputs) if (input && Text::LowerAscii(input->name) == name && input->present)
                return Result<std::vector<std::byte>>::Success(input->bytes);
            return Result<std::vector<std::byte>>::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_SOURCE", "解析器请求了未捕获的对象来源。");
        };
        CatalogLoadOptions catalogOptions;
        catalogOptions.sources = {{ObjectType::Destructable, {"Units/DestructableData.slk"}, {}},
            {ObjectType::Doodad, {"Doodads/Doodads.slk"}, {}}};
        catalogOptions.stringFiles.clear(); catalogOptions.maximumFileBytes = kTableBytes;
        catalogOptions.rejectInvalidPresentSources = true;
        const auto catalog = Take(StandardObjectCatalog::Load(reader, catalogOptions));
        ObjectFieldSchemaOptions schemaOptions;
        schemaOptions.sources = {{ObjectType::Destructable, "Units/DestructableMetaData.slk"}};
        schemaOptions.stringFiles.clear(); schemaOptions.worldEditDataFile.clear(); schemaOptions.unitEditorDataFile.clear();
        schemaOptions.maximumFileBytes = kTableBytes;
        const auto schema = Take(ObjectFieldSchema::Load(reader, schemaOptions));
        ValidateSchemaSource(metadataCells, schema, ObjectType::Destructable);
        std::optional<ObjectFieldSchema> doodadSchema;
        SchemaSourceEvidence doodadEvidence;
        const auto destructableOwner = Native(*inputs[0], ObjectType::Destructable), doodadOwner = Native(*inputs[1], ObjectType::Doodad);
        const auto* destructables = dynamic_cast<const NativeObjectDocument*>(destructableOwner.get());
        const auto* doodads = dynamic_cast<const NativeObjectDocument*>(doodadOwner.get());
        const auto destructableIndex = Index(destructables), doodadIndex = Index(doodads);
        std::set<std::pair<bool, std::string_view>> inspected;
        // path借用同次解析的owner，实际有效F32/网格策略随类型存储；最终结果另拥有字符串。
        std::map<std::string_view, ResolvedDoodadFootprint, std::less<>> footprintPaths;
        std::map<std::string_view, std::string_view, std::less<>> specialPaths;
        std::set<std::string_view, std::less<>> validatedDoodadParents;
        for (std::size_t row = 0; row < placedRawcodes.size() + specialRawcodes.size(); ++row) {
            const bool special = row >= placedRawcodes.size();
            const auto rawcode = special ? specialRawcodes[row - placedRawcodes.size()] : placedRawcodes[row];
            if (!Rawcode(rawcode)) Fail("TERRAIN_NATIVE_OBJECT_ID", "摆放记录rawcode无效。");
            if (!inspected.emplace(special, rawcode).second) continue;
            const auto* standard = catalog.Find(ObjectType::Destructable, rawcode);
            const auto record = destructableIndex.find(rawcode);
            const auto* native = record == destructableIndex.end() ? nullptr : record->second;
            const auto* standardDoodad = catalog.Find(ObjectType::Doodad, rawcode);
            const auto doodadRecord = doodadIndex.find(rawcode);
            const auto* nativeDoodad = doodadRecord == doodadIndex.end() ? nullptr : doodadRecord->second;
            if ((standardDoodad || nativeDoodad) && (standard || native))
                Fail("TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "摆放对象同时命中装饰物与可破坏物：" + std::string(rawcode));
            if (special || standardDoodad || nativeDoodad) {
                if (special && (standard || native))
                    Fail("TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "特殊地形记录不能按可破坏物解释：" + std::string(rawcode));
                const ObjectRecord* nativeDoodadParent = nullptr;
                if (nativeDoodad && nativeDoodad->parentRawcode) {
                    const auto& parent = *nativeDoodad->parentRawcode;
                    const auto parentRecord = doodadIndex.find(parent);
                    nativeDoodadParent = parentRecord == doodadIndex.end() ? nullptr : parentRecord->second;
                    if (standardDoodad || (nativeDoodadParent && nativeDoodadParent->parentRawcode) || destructableIndex.contains(parent)
                        || catalog.Find(ObjectType::Destructable, parent))
                        Fail("TERRAIN_NATIVE_OBJECT_INHERITANCE", "普通装饰物需要同类别的唯一一层标准父：" + std::string(rawcode));
                    standardDoodad = catalog.Find(ObjectType::Doodad, parent);
                }
                if (!standardDoodad)
                    Fail("TERRAIN_NATIVE_OBJECT_INHERITANCE", "普通装饰物没有唯一标准父对象：" + std::string(rawcode));
                // 原版明确禁用pathTex时，在加载贴图前清空占地宽高，注册器不写静态层。
                // Field()会合并缺失与空值，故直接检查单元格是否存在；不再次解码或归一化。
                const auto path = standardDoodad->fields.find("pathtex");
                if (path == standardDoodad->fields.end())
                    Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "标准装饰物缺少明确pathTex单元格：" + std::string(rawcode));
                std::string_view value = path->second;
                const ObjectModification* categoryOverride = nullptr;
                const ObjectModification* rotationOverride = nullptr;
                if (nativeDoodad) {
                    if (!inputs[5])
                        Fail("TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED", "装饰物原生覆盖需要同一owner补捕获实际DoodadMetaData后完整重验。");
                    if (!doodadSchema) {
                        const auto cells = UniqueTable(*inputs[5], "id");
                        auto options = schemaOptions;
                        options.sources = {{ObjectType::Doodad, "Doodads/DoodadMetaData.slk"}};
                        doodadSchema = Take(ObjectFieldSchema::Load(reader, options));
                        doodadEvidence = ValidateSchemaSource(cells, *doodadSchema, ObjectType::Doodad);
                    }
                    if (nativeDoodadParent && !nativeDoodadParent->modifications.empty()
                        && !validatedDoodadParents.contains(standardDoodad->key.rawcode)) {
                        // 只接受可完整解释的展示字段；不将父Original的覆盖合并到Custom。
                        // 父含通行字段时即使值相同也保留旧拒绝，不能用等值猜继承。
                        for (const auto& modification : nativeDoodadParent->modifications) {
                            const auto* definition = doodadSchema->FindNative(ObjectType::Doodad, modification.key.fieldId);
                            if (definition) {
                                const auto field = Text::LowerAscii(definition->sourceField);
                                if (field == "pathtex" || field == "category" || field == "fixedrot")
                                    Fail("TERRAIN_NATIVE_OBJECT_INHERITANCE", "装饰物标准父含通行字段覆盖，尚不能解释其继承：" + std::string(rawcode));
                            }
                        }
                        const ObjectModification* parentCategory = nullptr;
                        const ObjectModification* parentRotation = nullptr;
                        EffectiveDoodadPath(*standardDoodad, *nativeDoodadParent, *doodads, *doodadSchema,
                            value, doodadEvidence, parentCategory, parentRotation);
                        constexpr auto parentWork = sizeof(std::string_view) * 4;
                        if (parentWork > maximumWorkingBytes - work)
                            Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "装饰物父字段验证缓存超过累计剩余预算。");
                        work += parentWork;
                        validatedDoodadParents.emplace(standardDoodad->key.rawcode);
                    }
                    value = EffectiveDoodadPath(*standardDoodad, *nativeDoodad, *doodads, *doodadSchema,
                        value, doodadEvidence, categoryOverride, rotationOverride);
                }
                const bool nonempty = !value.empty() && value != "_" && value != "none";
                if (special) {
                    const auto specialPath = nonempty ? value : std::string_view{};
                    const auto outputWork = sizeof(std::string_view) * 5 + sizeof(MapData::TerrainNativeDoodadPathingType)
                        + rawcode.size() + specialPath.size() + 2;
                    if (outputWork > maximumWorkingBytes - work)
                        Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "特殊地形占地结果超过累计剩余预算。");
                    work += outputWork;
                    specialPaths.emplace(rawcode, specialPath);
                } else if (nonempty) {
                    const auto category = EffectiveDoodadCategory(*standardDoodad, rawcode, categoryOverride,
                        doodadSchema ? &*doodadSchema : nullptr, doodadEvidence.doodadCategoryFields);
                    // 临时索引节点、最终vector元素及两个拥有内容的字符串均先预留；
                    // 完整分类后一次性分配最终vector，避免逐项增长遗漏额外容量。
                    const auto outputWork = sizeof(std::string_view) * 3 + sizeof(ResolvedDoodadFootprint)
                        + sizeof(MapData::TerrainNativeDoodadPathingType)
                        + rawcode.size() + value.size() + 2;
                    if (outputWork > maximumWorkingBytes - work)
                        Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "装饰物占地结果超过累计剩余预算。");
                    work += outputWork;
                    const auto policy = category == 'C'
                        ? MapData::TerrainDoodadGridPolicy::StartupCategoryC : MapData::TerrainDoodadGridPolicy::TwoCells;
                    footprintPaths.emplace(rawcode, ResolvedDoodadFootprint{value, policy,
                        EffectiveDoodadRotation(*standardDoodad, rotationOverride, doodadSchema ? &*doodadSchema : nullptr)});
                }
                continue;
            }
            if (!standard && !native)
                Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "摆放对象没有已知的唯一标准类别：" + std::string(rawcode));
            if (native && native->parentRawcode) {
                const auto& parent = *native->parentRawcode;
                if (standard || HasUnsupportedParentRecord(destructableIndex, parent) || doodadIndex.contains(parent)
                    || catalog.Find(ObjectType::Doodad, parent))
                    Fail("TERRAIN_NATIVE_OBJECT_INHERITANCE", "仅支持父无实际字段覆盖的一层标准继承：" + std::string(rawcode));
                standard = catalog.Find(ObjectType::Destructable, parent);
            }
            if (!standard) Fail("TERRAIN_NATIVE_OBJECT_INHERITANCE", "原生对象没有唯一标准父对象：" + std::string(rawcode));
            ObjectFieldContext context;
            context.rawcode = standard->key.rawcode; context.baseCode = standard->Field("code");
            const auto field = schema.MatchSourceField(ObjectType::Destructable, "walkable", context);
            if (!field.definition || !field.safeToEdit || field.sourceKey != "walkable" || field.sourceIndex)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "walkable没有适用于该对象的唯一元数据地址：" + std::string(rawcode));
            ValidateAddress(*field.definition);
            const ObjectModification* overridden = nullptr;
            if (native) for (const auto& modification : native->modifications) {
                const auto* definition = schema.FindNative(ObjectType::Destructable, modification.key.fieldId);
                if (!definition)
                    Fail("TERRAIN_NATIVE_OBJECT_METADATA", "摆放对象含未知元数据字段，不能证明其不影响walkable：" + modification.key.fieldId);
                if (modification.key.fieldId != field.definition->fieldId) continue;
                const auto match = schema.MatchNativeField(ObjectType::Destructable, modification.key, context);
                if (overridden || !match.definition || !match.safeToEdit || match.sourceKey != "walkable"
                    || modification.key.level || modification.key.variation || modification.key.dataIndex
                    || modification.valueWasOpaque
                    || destructables->ModificationValueType(native->key, modification.key) != NativeValueType::Int32)
                    Fail("TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "walkable原生覆盖不是唯一无索引Int32：" + std::string(rawcode));
                overridden = &modification;
            }
            bool walkable = false;
            if (overridden) {
                const auto* value = std::get_if<std::int32_t>(&overridden->value);
                if (!value || (*value != 0 && *value != 1)) Fail("TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "walkable覆盖必须是Int32的0或1。");
                walkable = *value != 0;
            } else {
                const auto value = standard->Field("walkable");
                if (value != "0" && value != "1") Fail("TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "标准walkable缺失或不是显式0/1：" + std::string(rawcode));
                walkable = value != "0";
            }
            if (walkable) {
                // Custom的唯一标准父、父无实际字段覆盖和跨类别冲突已由上方逐项核实。
                // 5B6B40/5B6BB0/5B6CA0只在字段缺失时将查询ID替换为5B6740返回的标准父。
                const bool custom = native && native->parentRawcode;
                if (standard->key.rawcode != rawcode && !custom)
                    Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "可行走对象没有唯一标准类型或一层标准父：" + std::string(rawcode));
                if (custom) ValidateCustomBridgeSelector(*field.definition);
                const auto standardPath = standard->fields.find("pathtex");
                std::optional<std::string_view> path;
                if (standardPath != standard->fields.end()) path = standardPath->second;
                auto rotation = FixedRotation(standard->Field("fixedrot"));
                if (native) ApplyBridgeOverrides(*native, *destructables, *standard, schema, metadataCells, path, rotation);
                if (!path || path->empty() || *path == "_" || *path == "none"
                    || !rotation || *rotation < -360 || *rotation > 360)
                    Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "可行走标准对象需要有效的非空pathTex和明确有限的[-360,360] fixedRot：" + std::string(rawcode));
                // 5496D0向Doodads目录查询category；这里已证明无同名Doodad行/原生覆盖。
                // 其缺行getter先清空字符串，因此502580回退到wire DWORD>>8的低字节。
                // 426890按小端原样读DWORD，该字节就是文件rawcode的第二个字符。
                // 62B420→62BA10→564900→563EF0保留DOO新ID于record+4，549680原样传入；Custom不能改用父ID。
                // 非C确定2格；C与普通占地共用可信startup owner，不能仅凭字面C断言4格。
                const auto policy = rawcode[1] == 'C' ? MapData::TerrainDoodadGridPolicy::StartupCategoryC
                    : MapData::TerrainDoodadGridPolicy::TwoCells;
                const auto outputWork = sizeof(std::string_view) * 3 + sizeof(ResolvedDoodadFootprint)
                    + sizeof(MapData::TerrainNativeDoodadPathingType) + rawcode.size() + path->size() + 2;
                if (outputWork > maximumWorkingBytes - work)
                    Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "可行走对象占地结果超过累计剩余预算。");
                work += outputWork;
                footprintPaths.emplace(rawcode, ResolvedDoodadFootprint{*path, policy, rotation,
                    MapData::TerrainNativeFootprintContribution::WalkableTerrainSuppression});
            }
        }
        MapData::TerrainNativeObjectPathing result;
        result.workBytes = work;
        result.footprints.reserve(footprintPaths.size());
        for (const auto& [rawcode, resolved] : footprintPaths) {
            result.footprints.push_back({std::string(rawcode), std::string(resolved.path), resolved.gridPolicy, resolved.rotation,
                resolved.contribution});
        }
        result.specialFootprints.reserve(specialPaths.size());
        for (const auto& [rawcode, path] : specialPaths)
            result.specialFootprints.push_back({std::string(rawcode), std::string(path), MapData::TerrainDoodadGridPolicy::SpecialTerrain, std::nullopt});
        return Result<MapData::TerrainNativeObjectPathing>::Success(std::move(result));
    } catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Result<MapData::TerrainNativeObjectPathing>::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "严格对象解析内存分配失败。"); }
    catch (const std::length_error&) { return Result<MapData::TerrainNativeObjectPathing>::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "严格对象解析容器容量不足。"); }
}

Result<MapData::TerrainNativeObjectPathing> InspectNativeDestructablePathing(std::span<const std::string_view> placedRawcodes,
    std::span<const ArchiveExactMember> sources, std::size_t maximumWorkingBytes) {
    return InspectNativeTerrainObjects(placedRawcodes, {}, sources, maximumWorkingBytes);
}

Result<std::size_t> ValidateNativeDestructablePathing(std::span<const std::string_view> placedRawcodes,
    std::span<const ArchiveExactMember> sources, std::size_t maximumWorkingBytes) {
    try {
        auto inspected = InspectNativeDestructablePathing(placedRawcodes, sources, maximumWorkingBytes);
        if (!inspected.Succeeded() || !inspected.value) return {{}, std::move(inspected.diagnostics)};
        if (!inspected.value->footprints.empty())
            return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED",
                "对象的pathTex未明确禁用占地，不能由零贡献兼容接口授予保存资格：" + inspected.value->footprints.front().rawcode);
        return Result<std::size_t>::Success(inspected.value->workBytes);
    } catch (const std::bad_alloc&) { return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "严格对象解析内存分配失败。"); }
    catch (const std::length_error&) { return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "严格对象解析容器容量不足。"); }
}
} // namespace War3Map::ObjectData
