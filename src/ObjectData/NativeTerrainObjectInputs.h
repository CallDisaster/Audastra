#pragma once

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

// 严格资格检查与只读依赖描述共用同一组实际SLK/codec/元数据帮助。
// 窄描述读取不调用全字段保存资格检查。
namespace War3Map::ObjectData::NativeTerrainObjectDetail {
inline constexpr const char* kSubsystem = "Terrain.NativeObjects";
inline constexpr std::size_t kTableBytes = 2u * 1024u * 1024u;
inline constexpr std::size_t kRows = 4096, kCells = 262144, kCellBytes = 4096;
// 原字节之外，为现有codec的wire/public双存储、三至四个表loader及字符串/节点
// 同时存活预留保守额度；不是提升parser限额，也不是进程峰值内存测量。
inline constexpr std::size_t kWorkPerInputByte = 64;
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] inline void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
template<class T> inline T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
inline bool Rawcode(std::string_view value) {
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}
inline std::optional<float> FixedRotation(std::string_view value) {
    if (value.empty()) return std::nullopt;
    float result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result))
        return std::nullopt;
    return result;
}
inline std::string_view TextBytes(std::span<const std::byte> bytes) {
    if (bytes.empty()) return {};
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    return text;
}
inline Text::SlkCells UniqueTable(const ArchiveExactMember& input, std::string_view idHeader) {
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
inline constexpr std::array<std::string_view, 3> kDoodadColorFields{"vertr", "vertg", "vertb"};
struct SchemaSourceEvidence {
    std::size_t doodadCategoryFields = 0;
    std::array<std::size_t, 3> doodadColorFields{};
    std::array<bool, 3> doodadColorAliases{};
};
inline bool DoodadColorVariantAlias(std::string_view source, std::string_view color) {
    if (!source.starts_with(color) || source.size() == color.size()) return false;
    const auto suffix = source.substr(color.size());
    std::uint32_t variation = 0;
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), variation);
    return parsed.ec == std::errc{} && parsed.ptr == suffix.data() + suffix.size() && variation >= 1 && variation <= 10;
}
inline SchemaSourceEvidence ValidateSchemaSource(const Text::SlkCells& cells, const ObjectFieldSchema& schema, ObjectType type) {
    std::map<std::string, int, std::less<>> columns;
    for (const auto& [column, name] : cells.at(1)) columns.emplace(Text::LowerAscii(name), column);
    std::size_t doodadPathFields = 0, doodadRotationFields = 0;
    SchemaSourceEvidence evidence;
    for (const auto& [rowNumber, row] : cells) {
        if (rowNumber == 1) continue;
        const auto logical = [&](std::string_view name) -> std::string_view {
            const auto column = columns.find(name);
            if (column == columns.end()) return {};
            const auto value = row.find(column->second);
            return value == row.end() ? std::string_view{} : value->second;
        };
        const auto* field = schema.FindNative(type, row.at(1));
        if (type == ObjectType::Doodad) {
            const auto source = Text::LowerAscii(logical("field"));
            auto dataSource = source;
            const auto dataText = Text::TrimCWhitespace(logical("data"));
            std::int32_t data = 0;
            bool dataKnown = dataText.empty();
            if (!dataText.empty()) {
                const auto parsed = std::from_chars(dataText.data(), dataText.data() + dataText.size(), data);
                dataKnown = parsed.ec == std::errc{} && parsed.ptr == dataText.data() + dataText.size();
                if (dataKnown && data > 0 && data <= 26) dataSource += static_cast<char>('a' + data - 1);
            }
            for (std::size_t i = 0; i < kDoodadColorFields.size(); ++i) {
                const auto key = kDoodadColorFields[i];
                if (source == key) ++evidence.doodadColorFields[i];
                if (source != key && dataSource == key) evidence.doodadColorAliases[i] = true;
                // 扫描原始行而非仅schema保留下来的行；slk为空、不可编辑的行也不能
                // 与vertR01..10等实际列地址形成别名后被忽略。
                if (DoodadColorVariantAlias(source, key) || DoodadColorVariantAlias(dataSource, key)
                    || (!dataKnown && key.starts_with(source))) evidence.doodadColorAliases[i] = true;
            }
        }
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
        // category只在实际覆盖参与普通占地时要求唯一；仍统计原始候选，防止loader
        // 丢掉slk为空的行后，剩下的绑定被误判成唯一地址。
        if (type == ObjectType::Doodad && Text::LowerAscii(logical("field")) == "category")
            ++evidence.doodadCategoryFields;
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
    return evidence;
}
inline std::unique_ptr<CodecDocument> Native(const ArchiveExactMember& source, ObjectType type) {
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
inline ObjectIndex Index(const NativeObjectDocument* document) {
    ObjectIndex result;
    if (!document) return result;
    if (document->Objects().size() > kRows) Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "原生对象数量超过严格读取预算。");
    for (const auto& object : document->Objects()) {
        if (!Rawcode(object.key.rawcode) || !result.emplace(object.key.rawcode, &object).second)
            Fail("TERRAIN_NATIVE_OBJECT_ID", "原生对象表没有唯一rawcode。");
    }
    return result;
}
inline bool HasUnsupportedParentRecord(const ObjectIndex& index, std::string_view rawcode) {
    const auto parent = index.find(rawcode);
    // Native()已要求完整、无歧义的经典v2；codec只为Custom设置parentRawcode。
    // 空Original没有modifier，不能改变标准父的字段；任一实际字段仍未解释。
    return parent != index.end()
        && (parent->second->parentRawcode.has_value() || !parent->second->modifications.empty());
}
inline void ValidateMetadataNumbers(const ObjectFieldDefinition& field) {
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
inline void ValidateAddress(const ObjectFieldDefinition& field) {
    ValidateMetadataNumbers(field);
    if (Text::LowerAscii(field.sourceTable) != "destructabledata" || field.repeat != 0 || field.data != 0
        || (Text::LowerAscii(field.valueType) != "bool" && Text::LowerAscii(field.valueType) != "int")
        || field.nativeValueType != NativeValueType::Int32)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "walkable必须是DestructableData中的唯一非重复Int32字段。");
}
inline void ValidateDoodadAddress(const ObjectFieldDefinition& field) {
    ValidateMetadataNumbers(field);
    // metadata使用逻辑表名DoodadData，原版将其绑定到Doodads/Doodads.slk；文件干名Doodads不是别名。
    // 原版Doodad metadata的第二索引getter恒0；repeat=0才查询variation=0。
    // 不以通用schema的默认值吞掉坏文本，也不扩张到其支持的非零data/变体域。
    if (!field.metadata.contains("repeat") || field.repeat != 0 || field.data != 0
        || Text::LowerAscii(field.sourceTable) != "doodaddata" || field.sourceField.empty()
        || !field.nativeValueType)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物字段需要DoodadData中的明确非重复零索引地址：" + field.fieldId);
}
inline void ValidateDoodadColorAddress(const ObjectFieldDefinition& field, const SchemaSourceEvidence& evidence) {
    ValidateMetadataNumbers(field);
    const auto name = Text::LowerAscii(field.sourceField);
    const auto color = std::find(kDoodadColorFields.begin(), kDoodadColorFields.end(), name);
    // 这里只解释原有v2记录，不开放schema的变体编辑门。原版510A00将repeat=10
    // 的字段展开为1..10，DoodadMetaData虚槽+34恒0；5B5050按ID与两索引精确查找。
    if (color == kDoodadColorFields.end() || !field.metadata.contains("repeat") || field.repeat != 10
        || !field.metadata.contains("index") || field.index != 0 || field.data != 0
        || field.appendIndex != 0 || field.effectIndex != 0 || !field.effectType.empty()
        || Text::LowerAscii(field.sourceTable) != "doodaddata" || Text::LowerAscii(field.valueType) != "int"
        || field.nativeValueType != NativeValueType::Int32 || !field.useSpecific.empty() || !field.notSpecific.empty())
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "仅支持实际DoodadData中明确零索引的十变体Int32顶点颜色字段：" + field.fieldId);
    for (const auto* key : {"usespecific", "notspecific"}) {
        const auto selector = field.metadata.find(key);
        if (selector != field.metadata.end() && !selector->second.empty())
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物顶点颜色含未支持的对象选择器：" + field.fieldId);
    }
    const auto channel = static_cast<std::size_t>(color - kDoodadColorFields.begin());
    if (evidence.doodadColorFields[channel] != 1 || evidence.doodadColorAliases[channel])
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物顶点颜色没有唯一原始元数据地址或存在变体列别名：" + field.fieldId);
}
inline const ObjectFieldDefinition& UniqueDoodadField(const ObjectFieldSchema& schema,
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
inline std::string_view EffectiveDoodadPath(const StandardObjectEntry& standard, const ObjectRecord& original,
    const NativeObjectDocument& document, const ObjectFieldSchema& schema, std::string_view fallback,
    const SchemaSourceEvidence& evidence,
    const ObjectModification*& categoryOverride, const ObjectModification*& rotationOverride) {
    ObjectFieldContext context;
    context.rawcode = standard.key.rawcode; context.baseCode = standard.Field("code");
    const auto& field = UniqueDoodadField(schema, context, "pathtex");
    if (field.nativeValueType != NativeValueType::String)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "pathTex必须是实际DoodadData中的String字段。");
    const ObjectModification* overridden = nullptr;
    std::set<std::tuple<std::string_view, std::uint32_t, std::uint32_t>> addresses;
    for (const auto& modification : original.modifications) {
        const auto match = schema.MatchNativeField(ObjectType::Doodad, modification.key, context);
        if (!match.definition || match.sourceIndex)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物修改字段没有可靠的唯一地址：" + modification.key.fieldId);
        const auto variation = modification.key.variation.value_or(0);
        const bool color = match.definition->repeat != 0;
        if (color) ValidateDoodadColorAddress(*match.definition, evidence);
        else {
            if (!match.safeToEdit)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物修改字段没有可靠的唯一地址：" + modification.key.fieldId);
            ValidateDoodadAddress(*match.definition);
        }
        if (modification.key.level.value_or(0) != 0 || (color ? variation < 1 || variation > 10 : variation != 0)
            || modification.key.dataIndex.value_or(0) != 0 || modification.valueWasOpaque
            || document.ModificationValueType(original.key, modification.key) != match.definition->nativeValueType
            || !addresses.emplace(modification.key.fieldId, modification.key.variation.value_or(0),
                modification.key.dataIndex.value_or(0)).second)
            Fail("TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "装饰物覆盖必须是唯一、类型明确且属于已证索引域的字段：" + standard.key.rawcode);
        if (color && !std::holds_alternative<std::int32_t>(modification.value))
            Fail("TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "装饰物顶点颜色覆盖必须保持实际Int32值。");
        if (match.sourceKey == "category") {
            if (categoryOverride)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "装饰物category覆盖存在重复或别名。");
            categoryOverride = &modification;
        }
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
inline unsigned char EffectiveDoodadCategory(const StandardObjectEntry& standard, std::string_view placedRawcode,
    const ObjectModification* overridden, const ObjectFieldSchema* schema, std::size_t rawCategoryFields) {
    auto category = standard.Field("category");
    // 标准String getter还会经4EECC0解析WESTRING_；该来源不在本次类别owner内。
    // native覆盖命中则直接复制，不能给它套用标准文本的本地化分支。
    if (!overridden && Text::LowerAscii(category).starts_with("westring_"))
        Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "标准装饰物类别需要未捕获的本地化来源：" + std::string(placedRawcode));
    if (overridden) {
        if (!schema || rawCategoryFields != 1)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "category覆盖需要同次实际元数据中的唯一原始字段候选。");
        ObjectFieldContext context;
        context.rawcode = standard.key.rawcode; context.baseCode = standard.Field("code");
        const auto& field = UniqueDoodadField(*schema, context, "category");
        if (field.nativeValueType != NativeValueType::String || !field.metadata.contains("index")
            || field.index != 0 || field.appendIndex != 0 || field.effectIndex != 0)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "category必须是实际DoodadData中明确零索引的String标量字段。");
        // 已证实际dcat没有对象选择器；不把通用schema的适用判断当成原版选择器证据。
        for (const auto* key : {"usespecific", "notspecific"}) {
            const auto selector = field.metadata.find(key);
            if (selector != field.metadata.end() && !selector->second.empty())
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "category覆盖含未支持的对象选择器：" + field.fieldId);
        }
        if (schema->MatchNativeField(ObjectType::Doodad, overridden->key, context).definition != &field)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "category覆盖与实际唯一元数据地址不一致。");
        const auto* value = std::get_if<std::string>(&overridden->value);
        if (!value) Fail("TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "category覆盖不是实际String值。");
        // 516FE0→5B8A00→5B5CA0命中后直接复制record+0x10，显式空串也不借标准值。
        category = *value;
    }
    // 502580只读取原始首字节；缺格/空串或任何前导'_'均回退wire DWORD>>8。
    // Custom未覆盖时先沿唯一标准父取值，但此处回退始终使用DOO新ID的第二字节。
    const auto first = static_cast<unsigned char>(category.empty() || category.front() == '_'
        ? placedRawcode[1] : category.front());
    if (first < 33 || first > 126)
        Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "有效装饰物类别首字节超出已支持的可打印ASCII域：" + std::string(placedRawcode));
    return first;
}
inline std::optional<float> EffectiveDoodadRotation(const StandardObjectEntry& standard,
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
inline const ObjectFieldDefinition& UniqueBridgeField(const Text::SlkCells& cells, const ObjectFieldSchema& schema,
    const ObjectFieldContext& context, std::string_view key) {
    const auto match = schema.MatchSourceField(ObjectType::Destructable, key, context);
    if (!match.definition || !match.safeToEdit || match.sourceKey != key || match.sourceIndex)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", std::string(key) + "没有适用于标准桥的唯一元数据地址：" + context.rawcode);
    const auto& field = *match.definition;
    ValidateMetadataNumbers(field);
    // 原版DestructableMetaData是平面表，实际三字段index均为0，没有Doodad的repeat列。
    // 显式非零索引、重复或拼接元数据不属于本单元；缺失index也不能猜成0。
    if (Text::LowerAscii(field.sourceTable) != "destructabledata" || !field.metadata.contains("index")
        || field.index != 0 || field.repeat != 0 || field.data != 0 || field.appendIndex != 0 || field.effectIndex != 0)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥覆盖需要DestructableData中的明确零索引标量字段：" + field.fieldId);
    if (key == "walkable") ValidateAddress(field);
    else if (key == "pathtex" && field.nativeValueType != NativeValueType::String)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥pathTex必须是实际DestructableData中的String字段。");
    else if (key == "fixedrot" && field.nativeValueType != NativeValueType::Real && field.nativeValueType != NativeValueType::Unreal)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥fixedRot必须是实际DestructableData中的Real或Unreal字段。");
    else if (key == "name" && (field.nativeValueType != NativeValueType::String
        || Text::LowerAscii(field.valueType) != "string"))
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "桥名称必须是实际DestructableData中的String字段。");
    std::size_t candidates = 0;
    const auto& header = cells.at(1);
    const auto column = std::find_if(header.begin(), header.end(), [](const auto& cell) {
        return Text::LowerAscii(cell.second) == "field";
    });
    if (column == header.end()) Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥元数据缺少field列。");
    const auto dataColumn = std::find_if(header.begin(), header.end(), [](const auto& cell) {
        return Text::LowerAscii(cell.second) == "data";
    });
    for (const auto& [number, row] : cells) {
        if (number == 1) continue;
        const auto actual = row.find(column->first);
        if (actual == row.end()) continue;
        const auto sourceKey = Text::LowerAscii(actual->second);
        // Name只在实际使用时扩大范围；检查原始行的data拼接别名，不能让缺slk等被
        // schema丢弃的nam+E行消失后，将剩下的Name误判为唯一地址。
        if (key == "name" && sourceKey.size() + 1 == key.size() && key.starts_with(sourceKey)
            && dataColumn != header.end()) {
            const auto dataCell = row.find(dataColumn->first);
            if (dataCell != row.end()) {
                const auto text = Text::TrimCWhitespace(dataCell->second);
                if (!text.empty()) {
                    std::int32_t data = 0;
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), data);
                    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                        || (data > 0 && data <= 26 && static_cast<char>('a' + data - 1) == key.back()))
                        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "实际桥名称元数据存在未闭合或重复的data拼接候选。");
                }
            }
        }
        if (sourceKey != key) continue;
        if (++candidates != 1 || schema.FindNative(ObjectType::Destructable, row.at(1)) != &field)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥元数据含未绑定或重复的" + std::string(key) + "候选。");
    }
    if (candidates != 1) Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥元数据缺少显式" + std::string(key) + "候选。");
    for (const auto& candidate : schema.Definitions()) {
        if (candidate.objectType != ObjectType::Destructable || &candidate == &field) continue;
        auto candidateKey = Text::LowerAscii(candidate.sourceField);
        const bool sameField = candidateKey == key;
        if (candidate.data > 0 && candidate.data <= 26) candidateKey += static_cast<char>('a' + candidate.data - 1);
        if (sameField || candidateKey == key)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "多个可破坏物字段可能映射标准桥的" + std::string(key) + "：" + candidate.fieldId);
    }
    return field;
}
inline void ValidateCustomBridgeSelector(const ObjectFieldDefinition& field) {
    // 已证Custom三getter分别从新ID取覆盖、从标准父取缺省；实际三字段没有对象选择器。
    // 未证useSpecific/notSpecific如何参与Custom身份解析，不能沿用父context猜测其适用性。
    for (const auto* name : {"usespecific", "notspecific"}) {
        const auto selector = field.metadata.find(name);
        if (selector != field.metadata.end() && !selector->second.empty())
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "桥实际使用的字段含未支持的对象选择器：" + field.fieldId);
    }
}
inline void ApplyBridgeOverrides(const ObjectRecord& original, const NativeObjectDocument& document,
    const StandardObjectEntry& standard, const ObjectFieldSchema& schema, const Text::SlkCells& cells,
    std::optional<std::string_view>& path, std::optional<float>& rotation) {
    ObjectFieldContext context;
    context.rawcode = standard.key.rawcode; context.baseCode = standard.Field("code");
    std::set<std::string_view> addresses;
    for (const auto& modification : original.modifications) {
        const auto match = schema.MatchNativeField(ObjectType::Destructable, modification.key, context);
        if (!match.definition)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥覆盖含未知元数据字段：" + modification.key.fieldId);
        // 先识别元数据声明的逻辑字段，再检查标量地址；data拼接后的sourceKey不能把坏地址伪装成其它字段。
        const auto sourceKey = Text::LowerAscii(match.definition->sourceField);
        if (sourceKey != "walkable" && sourceKey != "pathtex" && sourceKey != "fixedrot" && sourceKey != "name")
            Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "可行走桥仅支持walkable、pathTex、fixedRot和Name覆盖：" + modification.key.fieldId);
        const auto& field = UniqueBridgeField(cells, schema, context, sourceKey);
        // Name是新保留域，两类对象均限已证的无selector元数据；不收紧原三字段Original域。
        if (original.parentRawcode || sourceKey == "name") ValidateCustomBridgeSelector(field);
        if (!match.safeToEdit || match.definition != &field || match.sourceKey != sourceKey || match.sourceIndex)
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥覆盖没有可靠的唯一标量地址：" + modification.key.fieldId);
        if (modification.key.level || modification.key.variation || modification.key.dataIndex || modification.valueWasOpaque
            || document.ModificationValueType(original.key, modification.key) != field.nativeValueType
            || !addresses.emplace(field.fieldId).second)
            Fail("TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE", "标准桥覆盖必须是唯一、wire类型与元数据一致的无索引值：" + modification.key.fieldId);
        if (sourceKey == "walkable") {
            ValidateAddress(field);
            const auto* value = std::get_if<std::int32_t>(&modification.value);
            if (!value || *value != 1)
                Fail("TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE", "存活桥的walkable覆盖必须是Int32的1。");
        } else if (sourceKey == "pathtex") {
            if (field.nativeValueType != NativeValueType::String)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥pathTex必须是实际DestructableData中的String字段。");
            const auto* value = std::get_if<std::string>(&modification.value);
            if (!value) Fail("TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE", "标准桥pathTex覆盖不是实际String值。");
            // 516FE0命中Original后直接走5B8A00；明确空串也不能回退标准占地。
            path = *value;
        } else if (sourceKey == "name") {
            // 5038C0/503340/503440按实际字段地址只取pathTex/fixedRot/walkable；
            // 516FE0等三getter不会把Name当作这三项缺省或覆盖。保留原String，
            // 不参与占地描述，也不修改原生对象字节；空名称同样不改变分类。
            if (!std::holds_alternative<std::string>(modification.value))
                Fail("TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE", "桥Name覆盖不是实际String值。");
        } else {
            if (field.nativeValueType != NativeValueType::Real && field.nativeValueType != NativeValueType::Unreal)
                Fail("TERRAIN_NATIVE_OBJECT_METADATA", "标准桥fixedRot必须是实际DestructableData中的Real或Unreal字段。");
            const auto* value = std::get_if<float>(&modification.value);
            if (!value) Fail("TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE", "标准桥fixedRot覆盖不是实际F32值。");
            if (!std::isfinite(*value) || *value < -360 || *value > 360)
                Fail("TERRAIN_DOODAD_FIXED_ROTATION", "标准桥fixedRot原生覆盖须有限且处于[-360,360]。");
            // 503340经767D24+1C进入517180，wire1/2按实际metadata读取F32；0/负值均保留。
            rotation = *value;
        }
    }
}
struct ResolvedDoodadFootprint {
    std::string_view path;
    MapData::TerrainDoodadGridPolicy gridPolicy;
    std::optional<float> rotation;
    MapData::TerrainNativeFootprintContribution contribution = MapData::TerrainNativeFootprintContribution::StaticRgbAttributes;
};
} // namespace War3Map::ObjectData::NativeTerrainObjectDetail
