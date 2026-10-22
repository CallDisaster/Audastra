#include "War3MapCore/ObjectData/NativeDestructablePathing.h"

#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::ObjectData {
namespace {
constexpr const char* kSubsystem = "Terrain.NativeObjects";
constexpr std::size_t kTableBytes = 2u * 1024u * 1024u;
constexpr std::size_t kRows = 4096, kCells = 262144, kCellBytes = 4096;
// 原字节之外，为现有codec的wire/public双存储、三个表loader及字符串/节点
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
std::string_view TextBytes(std::span<const std::byte> bytes) {
    if (bytes.empty()) return {};
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    return text;
}
void UniqueTable(const ArchiveExactMember& input, std::string_view idHeader) {
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
void ValidateAddress(const ObjectFieldDefinition& field) {
    for (const auto* name : {"repeat", "data", "index", "appendindex", "effectindex"}) {
        const auto found = field.metadata.find(name);
        if (found == field.metadata.end()) continue;
        const auto text = Text::TrimCWhitespace(found->second);
        std::int32_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            Fail("TERRAIN_NATIVE_OBJECT_METADATA", "walkable元数据索引不是完整整数：" + std::string(name));
    }
    if (Text::LowerAscii(field.sourceTable) != "destructabledata" || field.repeat != 0 || field.data != 0
        || (Text::LowerAscii(field.valueType) != "bool" && Text::LowerAscii(field.valueType) != "int")
        || field.nativeValueType != NativeValueType::Int32)
        Fail("TERRAIN_NATIVE_OBJECT_METADATA", "walkable必须是DestructableData中的唯一非重复Int32字段。");
}
} // namespace

Result<std::size_t> ValidateNativeDestructablePathing(std::span<const std::string_view> placedRawcodes,
    std::span<const ArchiveExactMember> sources, std::size_t maximumWorkingBytes) {
    try {
        constexpr std::array<std::string_view, 5> paths{"war3map.w3b", "war3map.w3d", "units/destructabledata.slk",
            "doodads/doodads.slk", "units/destructablemetadata.slk"};
        if (sources.size() != paths.size()) Fail("TERRAIN_NATIVE_OBJECT_SOURCE", "严格对象捕获必须包含完整五项来源。");
        std::array<const ArchiveExactMember*, paths.size()> inputs{};
        std::size_t work = 0;
        for (const auto& source : sources) {
            const auto name = Text::LowerAscii(source.name);
            const auto found = std::find(paths.begin(), paths.end(), name);
            if (found == paths.end() || inputs[found - paths.begin()]) Fail("TERRAIN_NATIVE_OBJECT_SOURCE", "严格对象来源重复或越界。");
            inputs[found - paths.begin()] = &source;
            if (source.bytes.size() > (maximumWorkingBytes - work) / kWorkPerInputByte)
                Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "原生对象与实际表的解析工作超过累计剩余预算。");
            work += source.bytes.size() * kWorkPerInputByte;
        }
        if (placedRawcodes.size() > (maximumWorkingBytes - work) / (sizeof(std::string_view) * 4))
            Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "摆放分类工作超过累计剩余预算。");
        work += placedRawcodes.size() * sizeof(std::string_view) * 4;
        if (!inputs[0]->present) Fail("TERRAIN_NATIVE_OBJECT_SOURCE", "严格W3B解析入口缺少实际W3B。");
        UniqueTable(*inputs[2], "destructableid"); UniqueTable(*inputs[3], "doodid"); UniqueTable(*inputs[4], "id");
        const auto reader = [&](const std::string& path) -> Result<std::vector<std::byte>> {
            const auto name = Text::LowerAscii(path);
            for (const auto* input : inputs) if (Text::LowerAscii(input->name) == name && input->present)
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
        const auto destructableOwner = Native(*inputs[0], ObjectType::Destructable), doodadOwner = Native(*inputs[1], ObjectType::Doodad);
        const auto* destructables = dynamic_cast<const NativeObjectDocument*>(destructableOwner.get());
        const auto* doodads = dynamic_cast<const NativeObjectDocument*>(doodadOwner.get());
        const auto destructableIndex = Index(destructables), doodadIndex = Index(doodads);
        std::set<std::string_view> inspected;
        for (const auto rawcode : placedRawcodes) {
            if (!Rawcode(rawcode)) Fail("TERRAIN_NATIVE_OBJECT_ID", "摆放记录rawcode无效。");
            if (!inspected.insert(rawcode).second) continue;
            const auto* standard = catalog.Find(ObjectType::Destructable, rawcode);
            const auto record = destructableIndex.find(rawcode);
            const auto* native = record == destructableIndex.end() ? nullptr : record->second;
            const bool doodad = catalog.Find(ObjectType::Doodad, rawcode) || doodadIndex.contains(rawcode);
            if (doodad || (!standard && !native))
                Fail(doodad && (standard || native) ? "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS" : "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED",
                    "摆放对象未唯一分类为可破坏物：" + std::string(rawcode));
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
            if (overridden) {
                const auto* value = std::get_if<std::int32_t>(&overridden->value);
                if (!value || (*value != 0 && *value != 1)) Fail("TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "walkable覆盖必须是Int32的0或1。");
                if (*value != 0) Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "walkable可破坏物尚未接入静态通行：" + std::string(rawcode));
            } else {
                const auto value = standard->Field("walkable");
                if (value != "0" && value != "1") Fail("TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "标准walkable缺失或不是显式0/1：" + std::string(rawcode));
                if (value != "0") Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "walkable可破坏物尚未接入静态通行：" + std::string(rawcode));
            }
        }
        return Result<std::size_t>::Success(work);
    } catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "严格对象解析内存分配失败。"); }
    catch (const std::length_error&) { return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "严格对象解析容器容量不足。"); }
}
} // namespace War3Map::ObjectData
