#include "War3MapCore/Trigger/WtgSource.h"
#include "War3MapCore/Hash.h"
#include "WtgDecoder.h"
#include "TriggerBinaryRead.h"

#include <algorithm>
#include <new>

namespace War3Map::Trigger {
namespace {
bool ValidLimits(const WtgSourceLimits& limits) {
    const WtgSourceLimits hard;
    return limits.maxBytes <= hard.maxBytes && limits.maxDecodedBytes <= hard.maxDecodedBytes
        && limits.maxItems <= hard.maxItems && limits.maxNodes <= WtgSourceLimits::hardMaxNodes
        && limits.maxStringBytes <= hard.maxStringBytes && limits.maxDepth <= hard.maxDepth;
}
bool HashIdentity(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
using PageResult = Result<WtgSourcePage>;
PageResult Failure(const char* code, const char* message) {
    return PageResult::Failure("Trigger.WtgSource", code, message);
}

class RowWriter {
public:
    explicit RowWriter(std::size_t& remaining) : remaining_(remaining) {}
    WtgSourceRow row;
    void Integer(std::string name, std::int32_t value) { Add(std::move(name), "i32", std::to_string(value)); }
    void Id(std::string name, std::uint64_t value) { Add(std::move(name), "u64", std::to_string(value)); }
    void Optional(std::string name, std::optional<std::int32_t> value) { if (value) Integer(std::move(name), *value); }
    void Text(std::string name, const RawText& text) {
        const auto& bytes = text.originalBytes;
        if (bytes.size() > remaining_ / 2) throw Detail::FormatError("WTG_SOURCE_QUERY_BUDGET");
        std::string hex; hex.reserve(bytes.size() * 2);
        constexpr char digits[] = "0123456789abcdef";
        for (const auto byte : bytes) {
            const auto value = std::to_integer<unsigned char>(byte);
            hex.push_back(digits[value >> 4]); hex.push_back(digits[value & 15]);
        }
        Add(std::move(name), "bytes-hex", std::move(hex));
    }
    void Indices(std::string name, const std::vector<std::uint32_t>& values) {
        std::string encoded;
        for (const auto value : values) {
            const auto number = std::to_string(value);
            if (encoded.size() + number.size() + 1 > remaining_) throw Detail::FormatError("WTG_SOURCE_QUERY_BUDGET");
            if (!encoded.empty()) encoded += ',';
            encoded += number;
        }
        Add(std::move(name), "u32-list", std::move(encoded));
    }
private:
    void Add(std::string name, std::string encoding, std::string value) {
        const auto bytes = name.size() + encoding.size() + value.size();
        if (bytes > remaining_) throw Detail::FormatError("WTG_SOURCE_QUERY_BUDGET");
        remaining_ -= bytes;
        row.fields.push_back({std::move(name), std::move(encoding), std::move(value)});
    }
    std::size_t& remaining_;
};

std::optional<std::size_t> Total(const WtgSourceSnapshot& data, WtgSourceTable table) {
    switch (table) {
    case WtgSourceTable::Items: return data.items.size();
    case WtgSourceTable::Variables: return data.document.variables.size();
    case WtgSourceTable::Ecas: return data.document.ecaNodes.size();
    case WtgSourceTable::Arguments: return data.document.argumentNodes.size();
    case WtgSourceTable::DeletedIds: {
        std::size_t count = 0;
        for (const auto& group : data.counts) count += group.deletedIds.size();
        return count;
    }
    default: return std::nullopt;
    }
}

void FillRow(RowWriter& writer, const WtgSourceSnapshot& data, WtgSourceTable table, std::size_t index) {
    const auto& document = data.document;
    auto& row = writer.row;
    row.index = index;
    switch (table) {
    case WtgSourceTable::Items: {
        const auto& item = data.items[index];
        row.sourceId = item.sourceId; row.origin = item.origin;
        writer.Integer("item_type", static_cast<std::int32_t>(item.type));
        writer.Optional("stored_id", item.storedId); writer.Optional("parent_id", item.parentId);
        writer.Optional("expanded_raw", item.expandedRaw);
        if (item.type == 1 || item.type == 4) {
            const auto& record = document.categories[item.recordIndex];
            writer.Text("name", record.name);
            if (data.baseVersion >= 7) writer.Integer("comment_raw", record.comment);
        } else if (item.type == 64) writer.Text("name", data.variableItems[item.recordIndex]);
        else {
            const auto& record = document.triggers[item.recordIndex];
            writer.Text("name", record.name); writer.Text("description", record.description);
            if (data.baseVersion >= 7) writer.Integer("comment_raw", record.type);
            writer.Integer("enabled_raw", record.enabled); writer.Integer("custom_text_raw", record.customTextFlag);
            writer.Integer("initially_off_raw", record.initiallyClosed); writer.Integer("run_on_init_raw", record.runOnInit);
            writer.Indices("root_eca_indices", record.ecas);
        }
        break;
    }
    case WtgSourceTable::Variables: {
        const auto& record = document.variables[index];
        const auto& identity = data.variableIdentities[index];
        row.sourceId = record.id; row.origin = record.origin;
        writer.Optional("stored_id", identity.storedId); writer.Optional("parent_id", identity.parentId);
        writer.Text("name", record.name); writer.Text("type", record.typeName);
        writer.Integer("unknown_raw", record.unknown); writer.Integer("array_raw", record.isArray);
        if (data.baseVersion >= 7) writer.Integer("array_size_raw", record.size);
        writer.Integer("initialized_raw", record.hasDefault); writer.Text("initial_value", record.value);
        break;
    }
    case WtgSourceTable::Ecas: {
        const auto& record = document.ecaNodes[index];
        row.sourceId = record.id; row.origin = record.origin;
        writer.Integer("eca_type", record.type); writer.Optional("child_branch", record.childId);
        writer.Text("name", record.name); writer.Integer("enabled_raw", record.enabled);
        writer.Id("source_trigger_id", record.sourceTriggerId);
        writer.Indices("argument_indices", record.arguments); writer.Indices("child_indices", record.children);
        break;
    }
    case WtgSourceTable::Arguments: {
        const auto& record = document.argumentNodes[index];
        row.sourceId = record.id; row.origin = record.origin;
        writer.Integer("parameter_type", record.type); writer.Text("value", record.value);
        writer.Integer("have_function_raw", record.insertCall); writer.Integer("have_index_raw", record.insertIndex);
        if (record.callEca) writer.Id("function_eca_index", *record.callEca);
        if (record.indexArgument) writer.Id("index_argument_index", *record.indexArgument);
        break;
    }
    case WtgSourceTable::DeletedIds:
        for (const auto& group : data.counts) {
            if (index >= group.deletedIds.size()) { index -= group.deletedIds.size(); continue; }
            const auto offset = group.origin.offset + 8 + index * 4;
            row.sourceId = Detail::MakeSourceId(0x17, offset);
            row.origin = Detail::MakeOrigin("war3map.wtg", offset, offset + 4);
            writer.Integer("item_type", static_cast<std::int32_t>(group.type));
            writer.Integer("stored_id", group.deletedIds[index]);
            return;
        }
        break;
    }
}
} // namespace

Result<WtgSource> ParseWtgSource(std::span<const std::byte> bytes, const UiCatalog& catalog, const WtgSourceLimits& limits) {
    if (!ValidLimits(limits)) return Result<WtgSource>::Failure("Trigger.WtgSource", "WTG_SOURCE_LIMITS",
        "ECA/参数节点预算不能超过1000000；来源字节、解码载荷、项目、字符串和深度预算只能收紧");
    if (bytes.size() > limits.maxBytes) return Result<WtgSource>::Failure("Trigger.WtgSource", "WTG_SOURCE_BYTE_LIMIT", "WTG 超过来源字节预算");
    if (!HashIdentity(catalog.Sha256())) return Result<WtgSource>::Failure("Trigger.WtgSource", "WTG_SOURCE_CATALOG_IDENTITY", "缺少完整的 Catalog SHA 身份");
    try {
        auto snapshot = std::make_shared<WtgSourceSnapshot>(Detail::DecodeWtgSource(bytes, catalog, limits));
        const auto hash = Sha256(bytes);
        if (!hash.Succeeded()) { Result<WtgSource> failure; failure.diagnostics = hash.diagnostics; return failure; }
        snapshot->sourceSha256 = *hash.value;
        snapshot->catalogSha256 = catalog.Sha256();
        WtgSource source;
        source.bytes_ = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
        source.snapshot_ = std::move(snapshot);
        return Result<WtgSource>::Success(std::move(source));
    } catch (const Detail::SourceReadError& error) {
        Result<WtgSource> failure;
        failure.diagnostics.push_back({DiagnosticSeverity::Error, "Trigger.WtgSource", error.code, error.what(),
            Detail::MakeOrigin("war3map.wtg", error.offset, error.offset + std::min<std::size_t>(4, bytes.size() - error.offset))});
        return failure;
    } catch (const Detail::MissingUiDefinitionError& error) {
        Result<WtgSource> failure;
        failure.diagnostics.push_back({DiagnosticSeverity::Error, "Trigger.WtgSource", "WTG_SOURCE_SIGNATURE_MISSING",
            error.what(), Detail::MakeOrigin("war3map.wtg", error.Offset(), error.EndOffset())});
        return failure;
    } catch (const std::bad_alloc&) {
        return Result<WtgSource>::Failure("Trigger.WtgSource", "WTG_SOURCE_ALLOCATION", "无法分配只读触发器来源");
    } catch (const std::exception& error) {
        return Result<WtgSource>::Failure("Trigger.WtgSource", "WTG_SOURCE_PARSE", error.what());
    }
}

Result<WtgSourcePage> QueryWtgSource(const WtgSource& source, std::string_view expectedSourceSha256,
    std::string_view expectedCatalogSha256, WtgSourceTable table, std::size_t start, std::size_t limit) {
    const auto data = source.Snapshot();
    if (!data) return Failure("WTG_SOURCE_UNAVAILABLE", "尚无完整的 WTG 来源");
    if (data->sourceSha256 != expectedSourceSha256 || data->catalogSha256 != expectedCatalogSha256)
        return Failure("WTG_SOURCE_STALE", "WTG 或 Catalog 来源身份已改变");
    const auto total = Total(*data, table);
    if (!total) return Failure("WTG_SOURCE_TABLE", "未知的只读触发器表");
    if (limit == 0 || limit > 128 || start > *total) return Failure("WTG_SOURCE_QUERY_BOUNDS", "页范围无效，单页必须为 1..128 行");
    try {
        WtgSourcePage page{data->sourceSha256, data->catalogSha256, *total, {}};
        const auto count = std::min(limit, *total - start);
        std::size_t remaining = 1024 * 1024;
        page.rows.reserve(count);
        for (std::size_t n = 0; n < count; ++n) {
            RowWriter writer(remaining);
            FillRow(writer, *data, table, start + n);
            page.rows.push_back(std::move(writer.row));
        }
        return PageResult::Success(std::move(page));
    } catch (const std::bad_alloc&) {
        return Failure("WTG_SOURCE_QUERY_ALLOCATION", "无法分配查询结果");
    } catch (const Detail::FormatError&) {
        return Failure("WTG_SOURCE_QUERY_BUDGET", "查询结果超过 1 MiB 字符载荷预算，请缩小范围");
    }
}
} // namespace War3Map::Trigger
