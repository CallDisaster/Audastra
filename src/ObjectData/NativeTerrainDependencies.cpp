#include "War3MapCore/ObjectData/NativeTerrainDependencies.h"

#include "NativeTerrainObjectInputs.h"
#include "War3MapCore/Hash.h"

#include <limits>

namespace War3Map::ObjectData {
namespace {
using namespace NativeTerrainObjectDetail;

struct DependencyInputs {
    std::array<const ArchiveExactMember*, 6> sources{};
    std::size_t work = 0;
    std::size_t maximum = 0;

    void Reserve(std::size_t bytes) {
        if (work > maximum || bytes > maximum - work)
            Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "通行依赖描述超过累计剩余预算。");
        work += bytes;
    }
};

bool RelevantKey(std::string_view key, ObjectType type) {
    return key == "pathtex" || key == "fixedrot"
        || (type == ObjectType::Destructable ? key == "walkable" : key == "category");
}

void UniqueRawRole(const Text::SlkCells& cells, const ObjectFieldSchema& schema,
    const ObjectFieldDefinition& field, ObjectType type, std::string_view key) {
    const auto& header = cells.at(1);
    const auto column = std::find_if(header.begin(), header.end(), [](const auto& cell) {
        return Text::LowerAscii(cell.second) == "field";
    });
    const auto dataColumn = std::find_if(header.begin(), header.end(), [](const auto& cell) {
        return Text::LowerAscii(cell.second) == "data";
    });
    if (column == header.end()) Fail("TERRAIN_NATIVE_DEPENDENCY_ADDRESS", "实际metadata缺少field列。");
    std::size_t candidates = 0;
    for (const auto& [number, row] : cells) {
        if (number == 1) continue;
        const auto cell = row.find(column->first);
        if (cell == row.end()) continue;
        const auto sourceKey = Text::LowerAscii(cell->second);
        auto addressed = sourceKey;
        if (dataColumn != header.end()) {
            const auto dataCell = row.find(dataColumn->first);
            if (dataCell != row.end() && !Text::TrimCWhitespace(dataCell->second).empty()) {
                const auto text = Text::TrimCWhitespace(dataCell->second);
                std::int32_t data = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), data);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
                    if (key.starts_with(sourceKey))
                        Fail("TERRAIN_NATIVE_DEPENDENCY_ADDRESS", "实际通行metadata存在无法闭合的data拼接地址。");
                } else if (data > 0 && data <= 26) addressed += static_cast<char>('a' + data - 1);
            }
        }
        if (sourceKey != key && addressed != key) continue;
        if (++candidates != 1 || sourceKey != key || schema.FindNative(type, row.at(1)) != &field)
            Fail("TERRAIN_NATIVE_DEPENDENCY_ADDRESS", "实际metadata通行字段重复、存在拼接别名或没有绑定。");
    }
    if (candidates != 1) Fail("TERRAIN_NATIVE_DEPENDENCY_ADDRESS", "实际metadata缺少唯一通行字段。");
}

// 只对实际通行地址使用严格地址验证；模型/颜色/名称等覆盖不取得保存资格，也不成为新门。
const ObjectFieldDefinition& DependencyField(const Text::SlkCells& cells, const ObjectFieldSchema& schema,
    ObjectType type, const ObjectFieldContext& context, std::string_view key, bool custom) {
    if (type == ObjectType::Doodad) {
        const auto& field = UniqueDoodadField(schema, context, key);
        UniqueRawRole(cells, schema, field, type, key);
        if (custom) ValidateCustomBridgeSelector(field);
        return field;
    }
    // probe读取与完整桥资格分开：动态对象的真实walkable scalar也可能使用index=-1。
    const auto match = schema.MatchSourceField(type, key, context);
    if (!match.definition || !match.safeToEdit || match.sourceKey != key || match.sourceIndex)
        Fail("TERRAIN_NATIVE_DEPENDENCY_ADDRESS", "通行依赖字段没有唯一实际地址：" + std::string(key));
    const auto& field = *match.definition;
    ValidateMetadataNumbers(field);
    if (Text::LowerAscii(field.sourceTable) != "destructabledata" || field.repeat || field.data
        || !field.nativeValueType || field.appendIndex || field.effectIndex || !field.effectType.empty())
        Fail("TERRAIN_NATIVE_DEPENDENCY_ADDRESS", "通行依赖需要明确的DestructableData标量地址。");
    UniqueRawRole(cells, schema, field, type, key);
    for (const auto& candidate : schema.Definitions()) {
        if (candidate.objectType != type || &candidate == &field) continue;
        auto addressed = Text::LowerAscii(candidate.sourceField);
        if (candidate.data > 0 && candidate.data <= 26) addressed += static_cast<char>('a' + candidate.data - 1);
        if (Text::LowerAscii(candidate.sourceField) == key || addressed == key)
            Fail("TERRAIN_NATIVE_DEPENDENCY_ADDRESS", "通行字段存在metadata别名。");
    }
    if (custom) ValidateCustomBridgeSelector(field);
    return field;
}

void UnknownWire(const ObjectRecord& owner, const ObjectModification& modification, const ArchiveExactMember& source,
    std::vector<std::byte>& wire, DependencyInputs& inputs) {
    if (!modification.origin || modification.origin->offset > source.bytes.size()
        || modification.origin->length > source.bytes.size() - modification.origin->offset)
        Fail("TERRAIN_NATIVE_DEPENDENCY_UNKNOWN", "未知字段缺少可逐字保留的实际wire范围。");
    const auto offset = static_cast<std::size_t>(modification.origin->offset);
    const auto length = static_cast<std::size_t>(modification.origin->length);
    constexpr auto headerBytes = std::size_t{12};
    if (length > std::numeric_limits<std::size_t>::max() - headerBytes
        || length + headerBytes > (inputs.maximum - inputs.work) / 2)
        Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "未知字段wire及散列工作超过剩余预算。");
    inputs.Reserve((length + headerBytes) * 2);
    for (const unsigned char character : owner.key.rawcode) wire.push_back(std::byte{character});
    for (unsigned shift = 0; shift < 64; shift += 8)
        wire.push_back(std::byte((static_cast<std::uint64_t>(length) >> shift) & 255));
    wire.insert(wire.end(), source.bytes.begin() + offset, source.bytes.begin() + offset + length);
}

const ObjectModification* Override(const ObjectRecord* native, const NativeObjectDocument* document,
    const ObjectFieldSchema* schema, const Text::SlkCells* metadata,
    const ObjectFieldContext& context, ObjectType type, std::string_view key) {
    if (!native) return nullptr;
    if (!schema || !metadata || !document)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "通行依赖覆盖缺少同次实际metadata或完整对象表。");
    const ObjectModification* result = nullptr;
    for (const auto& modification : native->modifications) {
        const auto* definition = schema->FindNative(type, modification.key.fieldId);
        if (!definition) continue;
        const auto sourceKey = Text::LowerAscii(definition->sourceField);
        auto addressed = sourceKey;
        if (definition->data > 0 && definition->data <= 26)
            addressed += static_cast<char>('a' + definition->data - 1);
        if (sourceKey != key && addressed != key) continue;
        const auto& field = DependencyField(*metadata, *schema, type, context, key, native->parentRawcode.has_value());
        const auto match = schema->MatchNativeField(type, modification.key, context);
        if (result || !match.safeToEdit || match.definition != &field || match.sourceKey != key || match.sourceIndex
            || modification.key.level.value_or(0) || modification.key.variation.value_or(0)
            || modification.key.dataIndex.value_or(0) || modification.valueWasOpaque
            || document->ModificationValueType(native->key, modification.key) != field.nativeValueType)
            Fail("TERRAIN_NATIVE_DEPENDENCY_ADDRESS", "通行依赖覆盖没有唯一的零索引实际wire地址：" + modification.key.fieldId);
        result = &modification;
    }
    return result;
}

std::string_view StringValue(const ObjectModification* overridden, std::string_view fallback) {
    if (!overridden) return fallback;
    const auto* value = std::get_if<std::string>(&overridden->value);
    if (!value) Fail("TERRAIN_NATIVE_DEPENDENCY_TYPE", "通行字符串覆盖不是实际String值。");
    return *value;
}

std::optional<float> RotationValue(const ObjectModification* overridden, std::string_view fallback) {
    if (!overridden) {
        const auto value = FixedRotation(fallback);
        if (!fallback.empty() && !value)
            Fail("TERRAIN_NATIVE_DEPENDENCY_TYPE", "标准fixedRot不是完整的有限F32，不能判定依赖未变。");
        return value;
    }
    const auto* value = std::get_if<float>(&overridden->value);
    if (!value || !std::isfinite(*value))
        Fail("TERRAIN_NATIVE_DEPENDENCY_TYPE", "通行fixedRot覆盖不是实际有限F32值。");
    return *value;
}
} // namespace

Result<NativeTerrainDependencyDescription> InspectNativeTerrainDependencies(
    std::span<const std::string_view> placedRawcodes, std::span<const std::string_view> specialRawcodes,
    std::span<const ArchiveExactMember> sources, std::size_t maximumWorkingBytes) {
    using Output = Result<NativeTerrainDependencyDescription>;
    using namespace NativeTerrainObjectDetail;
    try {
        constexpr std::array<std::string_view, 6> paths{"war3map.w3b", "war3map.w3d", "units/destructabledata.slk",
            "doodads/doodads.slk", "units/destructablemetadata.slk", "doodads/doodadmetadata.slk"};
        if (sources.size() != paths.size() || !maximumWorkingBytes || maximumWorkingBytes > 64u * 1024u * 1024u)
            Fail("TERRAIN_NATIVE_DEPENDENCY_SOURCE", "通行依赖需要明确六项来源和不超过64MiB的剩余预算。");
        DependencyInputs inputs; inputs.maximum = maximumWorkingBytes;
        for (const auto& source : sources) {
            const auto name = Text::LowerAscii(source.name);
            const auto found = std::find(paths.begin(), paths.end(), name);
            if (found == paths.end() || inputs.sources[found - paths.begin()] || (!source.present && !source.bytes.empty()))
                Fail("TERRAIN_NATIVE_DEPENDENCY_SOURCE", "通行依赖来源重复、越界或缺失状态不一致。");
            inputs.sources[found - paths.begin()] = &source;
            if (source.bytes.size() > (maximumWorkingBytes - inputs.work) / kWorkPerInputByte)
                Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "通行依赖实际SLK/codec工作超过剩余预算。");
            inputs.Reserve(source.bytes.size() * kWorkPerInputByte);
        }
        if (placedRawcodes.size() > kRows || specialRawcodes.size() > kRows - placedRawcodes.size())
            Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "通行依赖摆放类型数量超过4096。");
        inputs.Reserve((placedRawcodes.size() + specialRawcodes.size())
            * (sizeof(NativeTerrainDependencyType) + 8 * sizeof(void*)));
        const auto reader = [&](const std::string& path) -> Result<std::vector<std::byte>> {
            const auto key = Text::LowerAscii(path);
            for (const auto* source : inputs.sources) if (Text::LowerAscii(source->name) == key && source->present)
                return Result<std::vector<std::byte>>::Success(source->bytes);
            return Result<std::vector<std::byte>>::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_SOURCE", "依赖解析请求了未捕获来源。");
        };
        UniqueTable(*inputs.sources[2], "destructableid"); UniqueTable(*inputs.sources[3], "doodid");
        CatalogLoadOptions catalogOptions;
        catalogOptions.sources = {{ObjectType::Destructable, {"Units/DestructableData.slk"}, {}},
            {ObjectType::Doodad, {"Doodads/Doodads.slk"}, {}}};
        catalogOptions.stringFiles.clear(); catalogOptions.maximumFileBytes = kTableBytes;
        catalogOptions.rejectInvalidPresentSources = true;
        const auto catalog = Take(StandardObjectCatalog::Load(reader, catalogOptions));
        std::array<std::optional<ObjectFieldSchema>, 2> schemas;
        std::array<std::optional<Text::SlkCells>, 2> metadata;
        const auto loadSchema = [&](ObjectType type) {
            const auto index = type == ObjectType::Destructable ? 0u : 1u;
            if (schemas[index]) return;
            metadata[index] = UniqueTable(*inputs.sources[4 + index], "id");
            ObjectFieldSchemaOptions options;
            options.sources = {{type, std::string(paths[4 + index])}};
            options.stringFiles.clear(); options.worldEditDataFile.clear(); options.unitEditorDataFile.clear();
            options.maximumFileBytes = kTableBytes;
            schemas[index] = Take(ObjectFieldSchema::Load(reader, options));
            ValidateSchemaSource(*metadata[index], *schemas[index], type);
        };
        auto destructableOwner = Native(*inputs.sources[0], ObjectType::Destructable);
        auto doodadOwner = Native(*inputs.sources[1], ObjectType::Doodad);
        const auto* destructables = dynamic_cast<const NativeObjectDocument*>(destructableOwner.get());
        const auto* doodads = dynamic_cast<const NativeObjectDocument*>(doodadOwner.get());
        const auto destructableIndex = Index(destructables), doodadIndex = Index(doodads);
        NativeTerrainDependencyDescription result;
        result.objects.reserve(placedRawcodes.size() + specialRawcodes.size());
        std::set<std::pair<bool, std::string_view>> seen;
        for (std::size_t row = 0; row < placedRawcodes.size() + specialRawcodes.size(); ++row) {
            const bool special = row >= placedRawcodes.size();
            const auto rawcode = special ? specialRawcodes[row - placedRawcodes.size()] : placedRawcodes[row];
            if (!Rawcode(rawcode)) Fail("TERRAIN_NATIVE_OBJECT_ID", "通行依赖摆放rawcode无效。");
            if (!seen.emplace(special, rawcode).second) continue;
            const auto* standardDoodad = catalog.Find(ObjectType::Doodad, rawcode);
            const auto* standardDestructable = catalog.Find(ObjectType::Destructable, rawcode);
            const auto d = doodadIndex.find(rawcode), b = destructableIndex.find(rawcode);
            const bool isDoodad = special || standardDoodad || d != doodadIndex.end();
            if (isDoodad && (standardDestructable || b != destructableIndex.end()))
                Fail("TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "通行依赖对象同时命中两类数据表。");
            const auto type = isDoodad ? ObjectType::Doodad : ObjectType::Destructable;
            const auto index = isDoodad ? 1u : 0u;
            const auto& nativeIndex = isDoodad ? doodadIndex : destructableIndex;
            const auto at = nativeIndex.find(rawcode);
            const auto* native = at == nativeIndex.end() ? nullptr : at->second;
            const auto* document = isDoodad ? doodads : destructables;
            auto* standard = isDoodad ? standardDoodad : standardDestructable;
            const ObjectRecord* parentRecord = nullptr;
            NativeTerrainDependencyType value;
            value.rawcode = rawcode; value.special = special;
            if (native && native->parentRawcode) {
                value.parentRawcode = *native->parentRawcode;
                const auto& otherIndex = isDoodad ? destructableIndex : doodadIndex;
                const auto otherType = isDoodad ? ObjectType::Destructable : ObjectType::Doodad;
                const auto parent = nativeIndex.find(value.parentRawcode);
                parentRecord = parent == nativeIndex.end() ? nullptr : parent->second;
                if (standard || (parentRecord && parentRecord->parentRawcode)
                    || otherIndex.contains(value.parentRawcode) || catalog.Find(otherType, value.parentRawcode))
                    Fail("TERRAIN_NATIVE_DEPENDENCY_INHERITANCE", "通行依赖无法唯一解释实际父覆盖或多层继承。");
                standard = catalog.Find(type, value.parentRawcode);
            }
            if (!standard) Fail("TERRAIN_NATIVE_DEPENDENCY_INHERITANCE", "通行依赖对象没有唯一标准类型或一层标准父。");
            if (native) loadSchema(type);
            ObjectFieldContext context;
            context.rawcode = standard->key.rawcode; context.baseCode = standard->Field("code");
            std::vector<std::byte> unknown;
            if (parentRecord) for (const auto& modification : parentRecord->modifications) {
                const auto* definition = schemas[index]->FindNative(type, modification.key.fieldId);
                if (!definition) UnknownWire(*parentRecord, modification, *inputs.sources[index], unknown, inputs);
                else {
                    auto key = Text::LowerAscii(definition->sourceField);
                    if (RelevantKey(key, type))
                        Fail("TERRAIN_NATIVE_DEPENDENCY_INHERITANCE", "标准父包含尚未证明继承优先级的通行字段覆盖。");
                    if (definition->data > 0 && definition->data <= 26) key += static_cast<char>('a' + definition->data - 1);
                    if (RelevantKey(key, type))
                        Fail("TERRAIN_NATIVE_DEPENDENCY_INHERITANCE", "标准父包含通行字段拼接覆盖。");
                }
            }
            if (native) for (const auto& modification : native->modifications)
                if (!schemas[index]->FindNative(type, modification.key.fieldId))
                    UnknownWire(*native, modification, *inputs.sources[index], unknown, inputs);
            const auto overrideFor = [&](std::string_view key) {
                return Override(native, document, schemas[index] ? &*schemas[index] : nullptr,
                    metadata[index] ? &*metadata[index] : nullptr, context, type, key);
            };
            bool walkable = false;
            if (!isDoodad) {
                const auto* walkableOverride = overrideFor("walkable");
                if (walkableOverride) {
                    const auto* actual = std::get_if<std::int32_t>(&walkableOverride->value);
                    if (!actual || (*actual != 0 && *actual != 1))
                        Fail("TERRAIN_NATIVE_DEPENDENCY_TYPE", "walkable必须是实际Int32的0或1。");
                    walkable = *actual != 0;
                } else {
                    const auto actual = standard->Field("walkable");
                    if (actual != "0" && actual != "1")
                        Fail("TERRAIN_NATIVE_DEPENDENCY_TYPE", "标准walkable缺少明确0或1。");
                    walkable = actual == "1";
                }
            }
            value.contribution = special ? NativeTerrainDependencyContribution::Special : isDoodad
                ? NativeTerrainDependencyContribution::StaticRgb : walkable
                    ? NativeTerrainDependencyContribution::WalkableBridge : NativeTerrainDependencyContribution::Dynamic;
            if (isDoodad || walkable) {
                const auto* pathOverride = overrideFor("pathtex");
                const auto path = standard->fields.find("pathtex");
                if (!pathOverride && path == standard->fields.end())
                    Fail("TERRAIN_NATIVE_DEPENDENCY_TYPE", "静态对象缺少明确pathTex单元格。");
                const auto actual = StringValue(pathOverride, path == standard->fields.end() ? std::string_view{} : path->second);
                if (!actual.empty() && actual != "_" && actual != "none") value.pathTexture = actual;
                if (!special && !value.pathTexture.empty()) {
                    value.fixedRotationDegrees = RotationValue(overrideFor("fixedrot"), standard->Field("fixedrot"));
                    if (isDoodad) value.category = StringValue(overrideFor("category"), standard->Field("category"));
                    else value.category = std::string(1, rawcode[1]);
                }
            }
            if (!unknown.empty()) value.unknownFieldsSha256 = Take(Sha256(unknown));
            inputs.Reserve(value.rawcode.capacity() + value.parentRawcode.capacity() + value.pathTexture.capacity()
                + value.category.capacity() + value.unknownFieldsSha256.capacity() + 5);
            result.objects.push_back(std::move(value));
        }
        std::sort(result.objects.begin(), result.objects.end(), [](const auto& a, const auto& b) {
            return std::tie(a.special, a.rawcode) < std::tie(b.special, b.rawcode);
        });
        result.workBytes = inputs.work;
        return Output::Success(std::move(result));
    } catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Output::Failure("Terrain.NativeObjects", "TERRAIN_NATIVE_OBJECT_BUDGET", "通行依赖分配失败。"); }
    catch (const std::length_error&) { return Output::Failure("Terrain.NativeObjects", "TERRAIN_NATIVE_OBJECT_BUDGET", "通行依赖容量超限。"); }
}
} // namespace War3Map::ObjectData
