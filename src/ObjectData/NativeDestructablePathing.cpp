#include "War3MapCore/ObjectData/NativeDestructablePathing.h"

#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

namespace War3Map::ObjectData {
namespace {
constexpr const char* kSubsystem = "Terrain.NativeObjects";
constexpr std::size_t kTableBytes = 2u * 1024u * 1024u;
constexpr std::size_t kRows = 4096, kCells = 262144, kCellBytes = 4096;
// 原字节之外，为现有codec的wire/public双存储、三至四个表loader及字符串/节点
// 同时存活预留保守额度；不是提升parser限额，也不是进程峰值内存测量。
constexpr std::size_t kWorkPerInputByte = 64;
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
bool Rawcode(std::string_view value) {
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}
std::optional<float> FixedRotation(std::string_view value) {
    if (value.empty()) return std::nullopt;
    float result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result))
        return std::nullopt;
    return result;
}
std::string_view TextBytes(std::span<const std::byte> bytes) {
    if (bytes.empty()) return {};
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    return text;
}
Text::SlkCells UniqueTable(const ArchiveExactMember& input, std::string_view idHeader) {
    if (!input.present || input.bytes.empty()) Fail("TERRAIN_NATIVE_OBJECT_TABLE_MISSING", "缺少实际对象表：" + input.name);
    if (input.bytes.size() > kTableBytes) Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "对象表超过单表预算：" + input.name);
    auto parsed = Text::ParseSlkCells(TextBytes(input.bytes), Text::SlkCellMode::CatalogUnique);
    if (!parsed.Succeeded() || !parsed.value) {
        for (auto& diagnostic : parsed.diagnostics) diagnostic.message = input.name + "：" + diagnostic.message;
        throw Failure{std::move(parsed.diagnostics)};
    }
    const auto& cells = *parsed.value;
    if (cells.size() - 1 > kRows) Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "对象表行数超限：" + input.name);
    const auto& header = cells.at(1);
    if (Text::LowerAscii(header.at(1)) != idHeader) Fail("TERRAIN_NATIVE_OBJECT_HEADER", "对象表首列无效：" + input.name);
    std::set<std::string> columns, ids;
    for (const auto& [column, value] : header) {
        if (value.empty() || !columns.insert(Text::LowerAscii(value)).second)
            Fail("TERRAIN_NATIVE_OBJECT_COLUMN", input.name + "列" + std::to_string(column) + "为空或重复。");
    }
    std::size_t count = 0;
    for (const auto& [row, values] : cells) {
        if (values.size() > kCells - count) Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "对象表单元格超限：" + input.name);
        count += values.size();
        for (const auto& [column, value] : values) {
            if (!header.contains(column)) Fail("TERRAIN_NATIVE_OBJECT_COLUMN", input.name + "行" + std::to_string(row) + "含未声明列。");
            if (value.size() > kCellBytes) Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "对象表单元格文本超限：" + input.name);
        }
        if (row == 1) continue;
        const auto id = values.find(1);
        if (id == values.end() || !Rawcode(id->second) || !ids.insert(id->second).second)
            Fail("TERRAIN_NATIVE_OBJECT_ID", input.name + "行" + std::to_string(row) + "缺少唯一四字节ID。");
    }
    return std::move(*parsed.value);
}
void ValidateSchemaSource(const Text::SlkCells& cells, const ObjectFieldSchema& schema, ObjectType type) {
    std::map<std::string, int, std::less<>> columns;
    for (const auto& [column, name] : cells.at(1)) columns.emplace(Text::LowerAscii(name), column);
    std::size_t doodadPathFields = 0, doodadRotationFields = 0;
    for (const auto& [rowNumber, row] : cells) {
        if (rowNumber == 1) continue;
        const auto logical = [&](std::string_view name) -> std::string_view {
            const auto column = columns.find(name);
            if (column == columns.end()) return {};
            const auto value = row.find(column->second);
            return value == row.end() ? std::string_view{} : value->second;
        };
        const auto* field = schema.FindNative(type, row.at(1));
        // schema会丢弃slk为空的行，适用过滤也未经此处原版getter证明；
        // pathTex的原始候选不能因此消失后形成假的“唯一字段”。
        if (type == ObjectType::Doodad && Text::LowerAscii(logical("field")) == "pathtex") {
            if (++doodadPathFields != 1 || !field)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "实际DoodadMetaData的pathTex候选缺失绑定或不唯一。");
        }
        if (type == ObjectType::Doodad && Text::LowerAscii(logical("field")) == "fixedrot") {
            if (++doodadRotationFields != 1 || !field)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "实际DoodadMetaData的fixedRot候选缺失绑定或不唯一。");
        }
        if (!field) continue;
        // 普通schema的历史Token处理不能作为保存资格的再次解码。
        // 绑定键必须与严格SLK读取的一次解码逻辑值相同，不能剥掉字面引号或空白后获得地址。
        for (const auto& [name, column] : columns) {
            const auto actual = row.find(column);
            if (actual == row.end()) continue;
            const auto interpreted = field->metadata.find(name);
            if (interpreted == field->metadata.end() || interpreted->second != actual->second)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "元数据解析与实际SLK逻辑值不一致：" + field->fieldId + "." + name);
        }
        if (logical("field") != field->sourceField || logical("slk") != field->sourceTable
            || logical("type") != field->valueType || logical("effecttype") != field->effectType)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "元数据绑定键经过再次解码，不能证明原生字段地址：" + field->fieldId);
        // 适用对象列表仍由schema匹配；本入口不解释含字面引号的rawcode列表。
        for (const auto* name : {"usespecific", "notspecific"})
            if (logical(name).find('"') != std::string_view::npos)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "元数据适用对象列表含未支持的字面引号：" + field->fieldId);
    }
    if (type == ObjectType::Doodad && doodadPathFields != 1)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "实际DoodadMetaData没有唯一的显式pathTex字段。");
}
std::unique_ptr<CodecDocument> Native(const ArchiveExactMember& source, ObjectType type) {
    if (!source.present) {
        if (!source.bytes.empty()) Fail("TERRAIN_NATIVE_OBJECT_SOURCE", "缺失对象表却携带字节。");
        return {};
    }
    auto parsed = NativeObjectCodec(type).Parse(source.bytes);
    if (!parsed.Succeeded() || !parsed.document) throw Failure{std::move(parsed.diagnostics)};
    const auto* document = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
    if (!document || !document->IsStructurallySafe() || !document->Supports(NativeObjectOperation::EditExistingValue)
        || document->Version() != 2)
        Fail("TERRAIN_NATIVE_OBJECT_DOCUMENT", source.name + "不属于可唯一读取wire类型的完整经典v2对象表。");
    return std::move(parsed.document);
}
using ObjectIndex = std::map<std::string_view, const ObjectRecord*, std::less<>>;
ObjectIndex Index(const NativeObjectDocument* document) {
    ObjectIndex result;
    if (!document) return result;
    if (document->Objects().size() > kRows) Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "原生对象数量超过严格读取预算。");
    for (const auto& object : document->Objects()) {
        if (!Rawcode(object.key.rawcode) || !result.emplace(object.key.rawcode, &object).second)
            Fail("TERRAIN_NATIVE_OBJECT_ID", "原生对象表没有唯一rawcode。");
    }
    return result;
}
void ValidateMetadataNumbers(const ObjectFieldDefinition& field) {
    for (const auto* name : {"repeat", "data", "index", "appendindex", "effectindex"}) {
        const auto found = field.metadata.find(name);
        if (found == field.metadata.end()) continue;
        const auto text = Text::TrimCWhitespace(found->second);
        std::int32_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "元数据索引不是完整整数：" + field.fieldId + "." + name);
    }
}
void ValidateAddress(const ObjectFieldDefinition& field) {
    ValidateMetadataNumbers(field);
    if (Text::LowerAscii(field.sourceTable) != "destructabledata" || field.repeat != 0 || field.data != 0
        || (Text::LowerAscii(field.valueType) != "bool" && Text::LowerAscii(field.valueType) != "int")
        || field.nativeValueType != NativeValueType::Int32)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "walkable必须是DestructableData中的唯一非重复Int32字段。");
}
void ValidateDoodadAddress(const ObjectFieldDefinition& field) {
    ValidateMetadataNumbers(field);
    // metadata使用逻辑表名DoodadData，原版将其绑定到Doodads/Doodads.slk；文件干名Doodads不是别名。
    // 原版Doodad metadata的第二索引getter恒0；repeat=0才查询variation=0。
    // 不以通用schema的默认值吞掉坏文本，也不扩张到其支持的非零data/变体域。
    if (!field.metadata.contains("repeat") || field.repeat != 0 || field.data != 0
        || Text::LowerAscii(field.sourceTable) != "doodaddata" || field.sourceField.empty()
        || !field.nativeValueType)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物字段需要DoodadData中的明确非重复零索引地址：" + field.fieldId);
}
const ObjectFieldDefinition& UniqueDoodadField(const ObjectFieldSchema& schema,
    const ObjectFieldContext& context, std::string_view key) {
    const auto field = schema.MatchSourceField(ObjectType::Doodad, key, context);
    if (!field.definition || !field.safeToEdit || field.sourceKey != key || field.sourceIndex)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", std::string(key) + "没有适用于该装饰物的唯一元数据地址：" + context.rawcode);
    ValidateDoodadAddress(*field.definition);
    // MatchSourceField不返回无变体后缀的repeat字段，故另外排除同名及data拼接别名。
    for (const auto& candidate : schema.Definitions()) {
        if (candidate.objectType != ObjectType::Doodad || &candidate == field.definition) continue;
        auto candidateKey = Text::LowerAscii(candidate.sourceField);
        const bool sameField = candidateKey == key;
        if (candidate.data > 0 && candidate.data <= 26) candidateKey += static_cast<char>('a' + candidate.data - 1);
        if (sameField || candidateKey == key)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "多个装饰物元数据字段可能映射" + std::string(key) + "：" + candidate.fieldId);
    }
    return *field.definition;
}
std::string_view EffectiveDoodadPath(const StandardObjectEntry& standard, const ObjectRecord& original,
    const NativeObjectDocument& document, const ObjectFieldSchema& schema, std::string_view fallback,
    bool& categoryModified, const ObjectModification*& rotationOverride) {
    ObjectFieldContext context;
    context.rawcode = standard.key.rawcode; context.baseCode = standard.Field("code");
    const auto& field = UniqueDoodadField(schema, context, "pathtex");
    if (field.nativeValueType != NativeValueType::String)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "pathTex必须是实际DoodadData中的String字段。");
    const ObjectModification* overridden = nullptr;
    std::set<std::tuple<std::string_view, std::uint32_t, std::uint32_t>> addresses;
    for (const auto& modification : original.modifications) {
        const auto match = schema.MatchNativeField(ObjectType::Doodad, modification.key, context);
        if (!match.definition || !match.safeToEdit || match.sourceIndex)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物修改字段没有可靠的唯一地址：" + modification.key.fieldId);
        ValidateDoodadAddress(*match.definition);
        if (modification.key.level.value_or(0) != 0 || modification.key.variation.value_or(0) != 0
            || modification.key.dataIndex.value_or(0) != 0 || modification.valueWasOpaque
            || document.ModificationValueType(original.key, modification.key) != match.definition->nativeValueType
            || !addresses.emplace(modification.key.fieldId, modification.key.variation.value_or(0),
                modification.key.dataIndex.value_or(0)).second)
            Fail("TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "装饰物覆盖必须是唯一、类型明确且两索引为零的字段：" + standard.key.rawcode);
        if (match.sourceKey == "category") categoryModified = true;
        if (match.sourceKey == "fixedrot") {
            if (rotationOverride)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物fixedRot覆盖存在重复或别名。");
            rotationOverride = &modification;
        }
        if (match.sourceKey != "pathtex") continue;
        if (overridden || match.definition != &field)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物pathTex覆盖存在重复或别名。");
        overridden = &modification;
    }
    if (!overridden) return fallback;
    const auto* value = std::get_if<std::string>(&overridden->value);
    if (!value) Fail("TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "pathTex覆盖不是实际String值。");
    // 显式空串在原版getter命中后即返回，不能因empty而回退标准非空占地。
    return *value;
}
std::optional<float> EffectiveDoodadRotation(const StandardObjectEntry& standard,
    const ObjectModification* overridden, const ObjectFieldSchema* schema) {
    if (!overridden) return FixedRotation(standard.Field("fixedrot"));
    if (!schema) Fail("TERRAIN_NATIVE_OBJECT_METADATA", "fixedRot原生覆盖缺少同次捕获的实际元数据。");
    ObjectFieldContext context;
    context.rawcode = standard.key.rawcode; context.baseCode = standard.Field("code");
    const auto& field = UniqueDoodadField(*schema, context, "fixedrot");
    if ((field.nativeValueType != NativeValueType::Real && field.nativeValueType != NativeValueType::Unreal)
        || !field.metadata.contains("index") || field.index != 0)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "fixedRot必须是实际DoodadData中明确零索引的Real或Unreal字段。");
    const auto match = schema->MatchNativeField(ObjectType::Doodad, overridden->key, context);
    if (match.definition != &field)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "fixedRot覆盖与实际唯一元数据地址不一致。");
    // 517180→5B8950→5B5C00读取实际wire的F32；显式0/负值均不回退标准值。
    const auto* value = std::get_if<float>(&overridden->value);
    if (!value)
        Fail("TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "fixedRot覆盖不是实际F32值。");
    if (!std::isfinite(*value) || *value < -360 || *value > 360)
        Fail("TERRAIN_DOODAD_FIXED_ROTATION", "非空占地的fixedRot原生覆盖须有限且处于[-360,360]。");
    return *value;
}
struct ResolvedDoodadFootprint {
    std::string_view path;
    MapData::TerrainDoodadGridPolicy gridPolicy;
    std::optional<float> rotation;
    MapData::TerrainNativeFootprintContribution contribution = MapData::TerrainNativeFootprintContribution::StaticRgbAttributes;
};
} // namespace

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
        const auto destructableOwner = Native(*inputs[0], ObjectType::Destructable), doodadOwner = Native(*inputs[1], ObjectType::Doodad);
        const auto* destructables = dynamic_cast<const NativeObjectDocument*>(destructableOwner.get());
        const auto* doodads = dynamic_cast<const NativeObjectDocument*>(doodadOwner.get());
        const auto destructableIndex = Index(destructables), doodadIndex = Index(doodads);
        std::set<std::pair<bool, std::string_view>> inspected;
        // path借用同次解析的owner，实际有效F32/网格策略随类型存储；最终结果另拥有字符串。
        std::map<std::string_view, ResolvedDoodadFootprint, std::less<>> footprintPaths;
        std::map<std::string_view, std::string_view, std::less<>> specialPaths;
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
                if (nativeDoodad && nativeDoodad->parentRawcode) {
                    const auto& parent = *nativeDoodad->parentRawcode;
                    if (standardDoodad || doodadIndex.contains(parent) || destructableIndex.contains(parent)
                        || catalog.Find(ObjectType::Destructable, parent))
                        Fail("TERRAIN_NATIVE_OBJECT_INHERITANCE", "普通装饰物仅支持无原生父覆盖的一层标准继承：" + std::string(rawcode));
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
                bool categoryModified = false;
                const ObjectModification* rotationOverride = nullptr;
                if (nativeDoodad) {
                    if (!inputs[5])
                        Fail("TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED", "装饰物原生覆盖需要同一owner补捕获实际DoodadMetaData后完整重验。");
                    if (!doodadSchema) {
                        const auto cells = UniqueTable(*inputs[5], "id");
                        auto options = schemaOptions;
                        options.sources = {{ObjectType::Doodad, "Doodads/DoodadMetaData.slk"}};
                        doodadSchema = Take(ObjectFieldSchema::Load(reader, options));
                        ValidateSchemaSource(cells, *doodadSchema, ObjectType::Doodad);
                    }
                    value = EffectiveDoodadPath(*standardDoodad, *nativeDoodad, *doodads, *doodadSchema,
                        value, categoryModified, rotationOverride);
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
                    if (categoryModified)
                        Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "非空占地的类别原生覆盖尚未接入，不能沿用标准类别：" + std::string(rawcode));
                    const auto category = standardDoodad->fields.find("category");
                    if (category == standardDoodad->fields.end() || category->second.size() != 1
                        || static_cast<unsigned char>(category->second.front()) < 33
                        || static_cast<unsigned char>(category->second.front()) > 126 || category->second == "_")
                        Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "非空占地需要标准装饰物的明确单可打印ASCII类别，不能推测缺失类别：" + std::string(rawcode));
                    // 临时索引节点、最终vector元素及两个拥有内容的字符串均先预留；
                    // 完整分类后一次性分配最终vector，避免逐项增长遗漏额外容量。
                    const auto outputWork = sizeof(std::string_view) * 3 + sizeof(ResolvedDoodadFootprint)
                        + sizeof(MapData::TerrainNativeDoodadPathingType)
                        + rawcode.size() + value.size() + 2;
                    if (outputWork > maximumWorkingBytes - work)
                        Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "装饰物占地结果超过累计剩余预算。");
                    work += outputWork;
                    const auto policy = category->second == "C"
                        ? MapData::TerrainDoodadGridPolicy::RequireTwoAndFourEquivalent : MapData::TerrainDoodadGridPolicy::TwoCells;
                    footprintPaths.emplace(rawcode, ResolvedDoodadFootprint{value, policy,
                        EffectiveDoodadRotation(*standardDoodad, rotationOverride, doodadSchema ? &*doodadSchema : nullptr)});
                }
                continue;
            }
            if (!standard && !native)
                Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "摆放对象没有已知的唯一标准类别：" + std::string(rawcode));
            if (native && native->parentRawcode) {
                const auto& parent = *native->parentRawcode;
                if (standard || destructableIndex.contains(parent) || doodadIndex.contains(parent)
                    || catalog.Find(ObjectType::Doodad, parent))
                    Fail("TERRAIN_NATIVE_OBJECT_INHERITANCE", "仅支持无原生父覆盖的一层标准继承：" + std::string(rawcode));
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
                // 生存桥首域不解释W3B覆盖或自定义继承；同rawcode的Doodad/W3D已在上方排除。
                // 其它对象的W3B仍按各自来源解析，不把整份W3B是否存在当作桥资格。
                if (native || standard->key.rawcode != rawcode)
                    Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "可行走对象仅支持没有相关W3B记录的唯一标准类型：" + std::string(rawcode));
                const auto path = standard->fields.find("pathtex");
                const auto rotation = FixedRotation(standard->Field("fixedrot"));
                if (path == standard->fields.end() || path->second.empty() || path->second == "_" || path->second == "none"
                    || !rotation || *rotation < -360 || *rotation > 360)
                    Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "可行走标准对象需要非空pathTex和明确有限的[-360,360] fixedRot：" + std::string(rawcode));
                // 5496D0向Doodads目录查询category；这里已证明无同名Doodad行/原生覆盖。
                // 其缺行getter先清空字符串，因此502580回退到wire DWORD>>8的低字节。
                // 426890按小端原样读DWORD，该字节就是文件rawcode的第二个字符。
                // 非C无论类别目录是否包含该字符都不能命中502600，故确定采用2格。
                if (rawcode[1] == 'C')
                    Fail("TERRAIN_WALKABLE_DESTRUCTABLE_GRID", "可行走对象的C类别回退尚未绑定最终目录，不能确定吸附网格：" + std::string(rawcode));
                const auto outputWork = sizeof(std::string_view) * 3 + sizeof(ResolvedDoodadFootprint)
                    + sizeof(MapData::TerrainNativeDoodadPathingType) + rawcode.size() + path->second.size() + 2;
                if (outputWork > maximumWorkingBytes - work)
                    Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "可行走对象占地结果超过累计剩余预算。");
                work += outputWork;
                footprintPaths.emplace(rawcode, ResolvedDoodadFootprint{path->second,
                    MapData::TerrainDoodadGridPolicy::TwoCells, rotation,
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
