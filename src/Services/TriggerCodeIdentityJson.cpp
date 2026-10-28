#include "War3MapCore/Services/TriggerCodeIdentityJson.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
using Trigger::TextEncoding;
namespace {
using Json = nlohmann::json;
[[noreturn]] void Invalid(std::string message) { throw std::runtime_error(std::move(message)); }
void Shape(const Json& object, std::initializer_list<std::string_view> required, std::initializer_list<std::string_view> optional = {}) {
    if (!object.is_object()) Invalid("需要JSON对象。");
    for (auto key : required) if (!object.contains(key)) Invalid("缺少字段：" + std::string(key));
    for (auto it = object.begin(); it != object.end(); ++it) if (std::find(required.begin(), required.end(), it.key()) == required.end()
        && std::find(optional.begin(), optional.end(), it.key()) == optional.end()) Invalid("未知字段：" + it.key());
}
std::string String(const Json& value) { if (!value.is_string()) Invalid("需要UTF-8字符串。"); return value.get<std::string>(); }
std::string Sha(const Json& value) {
    auto text = String(value);
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) Invalid("SHA须为64位小写十六进制。");
    return text;
}
StableId Id(const Json& value, bool zero = false) {
    const auto text = String(value); StableId id = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text[0] == '0') || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || (!zero && !id)) Invalid("稳定ID须为规范十进制字符串。");
    return id;
}
Trigger::RawText Raw(const Json& value) {
    Shape(value, {"text", "encoding", "raw_bytes_hex"}); Trigger::RawText raw; raw.text = String(value.at("text"));
    const auto encoding = String(value.at("encoding"));
    if (encoding == "utf8") raw.encoding = TextEncoding::Utf8;
    else if (encoding == "utf8_bom") raw.encoding = TextEncoding::Utf8Bom;
    else if (encoding == "gb18030") raw.encoding = TextEncoding::Gb18030;
    else if (encoding == "latin1") raw.encoding = TextEncoding::Latin1;
    else Invalid("未知原名编码。");
    const auto hex = String(value.at("raw_bytes_hex")); if (hex.size() > 8192 || hex.size() % 2 || raw.text.size() > 4096) Invalid("原名超过预算或原字节不完整。");
    const auto digit = [](char c) -> unsigned { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; Invalid("原字节须为小写十六进制。"); };
    for (std::size_t i = 0; i < hex.size(); i += 2) raw.originalBytes.push_back(static_cast<std::byte>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    return raw;
}
}
Result<ParsedTriggerCodeIdentityEdit> ParseTriggerCodeIdentityEdit(std::string_view text) {
    using R = Result<ParsedTriggerCodeIdentityEdit>;
    try {
        if (text.size() > 65536) Invalid("代码身份请求超过64KiB。");
        std::vector<std::set<std::string>> keys;
        const auto root = Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 16) Invalid("JSON嵌套超过16层。");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            else if (event == Json::parse_event_t::object_end) keys.pop_back();
            else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) Invalid("JSON含重复字段。");
            return true;
        });
        Shape(root, {"expected", "operations", "dry_run"}, {"schema_version"});
        if (root.contains("schema_version") && (!root.at("schema_version").is_number_integer() || root.at("schema_version") != 1)) Invalid("未知schema_version。");
        if (!root.at("dry_run").is_boolean()) Invalid("dry_run必须显式为布尔值。");
        const auto& operations = root.at("operations"); if (!operations.is_array() || operations.size() != 1) Invalid("每个代码身份请求须恰好一个操作。");
        const auto& value = operations[0];
        if (!value.is_object() || !value.contains("op")) Invalid("缺少代码操作op。");
        const auto op = String(value.at("op"));
        if (op != "rename_custom_trigger" && op != "copy_custom_trigger") Invalid("未知代码身份操作。");
        if (op == "copy_custom_trigger") Shape(value, {"op", "trigger_id", "expected_name", "expected_code_sha256", "name", "category", "after_trigger_id", "expected_category_order"});
        else Shape(value, {"op", "trigger_id", "expected_name", "expected_code_sha256", "name"});
        ParsedTriggerCodeIdentityEdit parsed; auto& request = parsed.request;
        parsed.dryRun = root.at("dry_run").get<bool>(); request.action = op == "copy_custom_trigger" ? TriggerCodeIdentityAction::Copy : TriggerCodeIdentityAction::Rename;
        const auto& binding = root.at("expected");
        Shape(binding, {"source_wtg_sha256", "source_wct_sha256", "ui_catalog_sha256", "current_wtg_sha256", "current_wct_sha256"}, {"map_object_references_sha256"});
        request.expected = {Sha(binding.at("source_wtg_sha256")), Sha(binding.at("source_wct_sha256")), Sha(binding.at("ui_catalog_sha256")), Sha(binding.at("current_wtg_sha256")), Sha(binding.at("current_wct_sha256"))};
        if (binding.contains("map_object_references_sha256")) request.expected.mapObjectReferencesSha256 = Sha(binding.at("map_object_references_sha256"));
        request.triggerId = Id(value.at("trigger_id")); request.expectedName = Raw(value.at("expected_name"));
        request.expectedCodeSha256 = Sha(value.at("expected_code_sha256")); request.name = String(value.at("name"));
        if (request.name.size() > 4096 || request.name.find('\0') != request.name.npos) Invalid("新名称超过预算或含NUL。");
        if (request.action == TriggerCodeIdentityAction::Copy) {
            const auto& category = value.at("category"); Shape(category, {"kind", "id"});
            if (category.at("kind") != "category") Invalid("复制目标须为分类ref。");
            request.categoryRecordId = Id(category.at("id")); request.afterTriggerId = Id(value.at("after_trigger_id"), true);
            const auto& order = value.at("expected_category_order"); if (!order.is_array() || order.size() > 4096) Invalid("类别顺序须为有界ID数组。");
            for (const auto& id : order) request.expectedCategoryOrder.push_back(Id(id));
        }
        return R::Success(std::move(parsed));
    } catch (const std::exception& error) { return R::Failure("Trigger.CodeIdentity", "TRIGGER_CODE_REQUEST_INVALID", std::string("代码身份请求无效：") + error.what()); }
}
std::string TriggerCodeIdentitySummaryJson(const TriggerCodeIdentitySummary& summary,
    const Trigger::TriggerGraphEditReceipt* graph) {
    Json result{{"trigger_id", std::to_string(summary.triggerId)}, {"result_id", std::to_string(summary.resultId)},
        {"copied", summary.copied}, {"has_changes", summary.changed}, {"old_name", summary.oldName}, {"new_name", summary.newName},
        {"old_symbol", summary.oldSymbol}, {"new_symbol", summary.newSymbol}, {"changed_blocks", summary.changedBlocks},
        {"changed_references", summary.changedReferences}, {"locations", summary.locations}};
    if (graph) {
        const auto binding = [](const Trigger::TriggerGraphBinding& value) {
            Json encoded{{"source_wtg_sha256", value.sourceWtgSha256}, {"source_wct_sha256", value.sourceWctSha256},
                {"ui_catalog_sha256", value.uiCatalogSha256}, {"current_wtg_sha256", value.currentWtgSha256},
                {"current_wct_sha256", value.currentWctSha256}};
            if (!value.mapObjectReferencesSha256.empty()) encoded["map_object_references_sha256"] = value.mapObjectReferencesSha256;
            return encoded;
        };
        result["before"] = binding(graph->before);
        result["after"] = binding(graph->after);
    }
    return result.dump();
}
}
