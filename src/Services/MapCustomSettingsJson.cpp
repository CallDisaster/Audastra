#include "War3MapCore/Services/MapCustomSettingsEditing.h"
#include "War3MapCore/Services/NameResolution.h"
#include "War3MapCore/Text/Utf8Scalar.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {
using Json = nlohmann::json;
using Section = MapCustomSettingsSection;
using Action = MapCustomSettingsAction;
using namespace MapData;
[[noreturn]] void Invalid(std::string message) { throw std::runtime_error(std::move(message)); }
Json Parse(std::string_view text) {
    if (text.size() > 1024 * 1024) Invalid("JSON 超过 1 MiB。");
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 32) Invalid("JSON 嵌套超过 32 层。");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) Invalid("JSON 含重复字段。");
        return true;
    });
}
void Shape(const Json& value, std::initializer_list<std::string_view> required, std::initializer_list<std::string_view> optional = {}) {
    if (!value.is_object()) Invalid("需要 JSON 对象。");
    for (auto key : required) if (!value.contains(key)) Invalid("缺少字段：" + std::string(key));
    for (auto it = value.begin(); it != value.end(); ++it)
        if (std::find(required.begin(), required.end(), it.key()) == required.end()
            && std::find(optional.begin(), optional.end(), it.key()) == optional.end()) Invalid("未知字段：" + it.key());
}
std::string String(const Json& v) { if (!v.is_string()) Invalid("需要字符串。"); return v.get<std::string>(); }
std::int64_t Number(const Json& v, std::int64_t min, std::int64_t max) {
    if (!v.is_number_integer() || (v.is_number_unsigned() && v.get<std::uint64_t>() > static_cast<std::uint64_t>(max))) Invalid("需要范围内的整数。");
    const auto n = v.get<std::int64_t>(); if (n < min || n > max) Invalid("整数超出范围。"); return n;
}
std::int32_t I32(const Json& v) { return static_cast<std::int32_t>(Number(v, INT32_MIN, INT32_MAX)); }
std::uint32_t U32(const Json& v) { return static_cast<std::uint32_t>(Number(v, 0, UINT32_MAX)); }
bool Sha(std::string_view s) { return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'); }); }
std::string Hex(std::string_view s) {
    constexpr char digits[] = "0123456789abcdef"; std::string result; result.reserve(s.size() * 2);
    for (unsigned char c : s) { result += digits[c >> 4]; result += digits[c & 15]; } return result;
}
std::string Hex(const SettingsRawcode& id) { return Hex({reinterpret_cast<const char*>(id.data()), 4}); }
SettingsRawcode Raw(const Json& value) {
    auto text = String(value); if (text.size() != 8) Invalid("id_hex 须为 8 位小写十六进制。");
    SettingsRawcode id{};
    const auto digit = [](char c) -> unsigned { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; Invalid("id_hex 须为小写十六进制。"); };
    for (std::size_t i = 0; i < 4; ++i) id[i] = static_cast<std::byte>(digit(text[i * 2]) * 16 + digit(text[i * 2 + 1])); return id;
}
SettingsRawcode Choice(const Json& value) {
    if (!value.is_object() || !value.contains("kind")) Invalid("随机 ID 需要 typed kind。");
    auto kind = String(value.at("kind"));
    if (kind == "none") { Shape(value, {"kind"}); return {}; }
    if (kind != "object") Invalid("类别/等级伪 ID 不开放新写。");
    Shape(value, {"kind", "id_hex"}); auto id = Raw(value.at("id_hex"));
    if (id == SettingsRawcode{}) Invalid("空 ID 须用 kind=none 表示。"); return id;
}
void Name(const Json& value, std::string& name, bool& keep) {
    if (value.is_object() && value.contains("keep")) { Shape(value, {"keep"}); if (value.at("keep") != true) Invalid("keep 须为 true。"); keep = true; return; }
    Shape(value, {"utf8"}); name = String(value.at("utf8"));
    if (name.size() > 4096 || name.find('\0') != name.npos || name.starts_with("TRIGSTR_")) Invalid("名称须为至多 4096 字节的 UTF-8 字面量，无 NUL 或隐式 TRIGSTR 引用。");
}
Section SectionOf(const std::string& name) {
    if (name == "upgrades") return Section::Upgrades;
    if (name == "tech_restrictions") return Section::TechRestrictions;
    if (name == "random_units") return Section::RandomUnits;
    if (name == "random_items") return Section::RandomItems;
    Invalid("未知 section。");
}
Json Binding(const MapDataQueryBinding& b) { return {{"document_id", b.documentId}, {"document_generation", std::to_string(b.generation)}, {"revision", std::to_string(b.revision)}, {"source_sha256", b.sourceSha256}}; }
Json Diagnostics(const std::vector<Diagnostic>& ds) { Json result = Json::array(); for (const auto& d : ds) result.push_back({{"code", d.code}, {"message", d.message}, {"severity", d.severity == DiagnosticSeverity::Error ? "error" : d.severity == DiagnosticSeverity::Warning ? "warning" : "info"}}); return result; }
Json Text(const MapDocument& document, const std::string& raw) {
    NameResolutionContext context; context.mapDocument = &document; context.preserveStringFormatting = true;
    const auto resolved = ResolveName(context, raw, "string");
    bool utf8 = true;
    for (std::size_t at = 0; at < resolved.displayName.size();) {
        const auto scalar = War3Map::Text::Detail::ReadUtf8Scalar(resolved.displayName, at);
        if (!scalar.Valid()) { utf8 = false; break; } at += scalar.bytes;
    }
    return {{"raw_hex", Hex(raw)}, {"display", utf8 ? Json(resolved.displayName) : Json(nullptr)}, {"display_utf8_valid", utf8},
        {"resolution", NameResolutionStateName(resolved.state)}, {"reason", utf8 ? resolved.reason : "显示文字未能证明为 UTF-8，原字节见 raw_hex。"}};
}
Json Key(Section section, const MapCustomSettingsValue& record, int version) {
    auto key = MapCustomSettingsEditing::RecordKey(section, record, version);
    return key.Succeeded() ? Json(*key.value) : Json(nullptr);
}
} // namespace

Result<MapCustomSettingsEditRequest> ParseMapCustomSettingsEdit(std::string_view text) {
    using R = Result<MapCustomSettingsEditRequest>;
    try {
        const auto root = Parse(text); Shape(root, {"expected_member_sha256", "operations"});
        MapCustomSettingsEditRequest result; result.expectedMemberSha256 = String(root.at("expected_member_sha256"));
        if (!Sha(result.expectedMemberSha256)) Invalid("expected_member_sha256 须为小写 SHA-256。");
        const auto& operations = root.at("operations");
        if (!operations.is_array() || operations.empty() || operations.size() > 128) Invalid("operations 须为 1 到 128 项。");
        for (const auto& operation : operations) {
            Shape(operation, {"section", "op"}, {"expected_count", "index", "number", "expected_row_key", "expected_table_key", "value"});
            MapCustomSettingsCommand command; command.section = SectionOf(String(operation.at("section")));
            const bool table = command.section == Section::RandomUnits || command.section == Section::RandomItems;
            const auto action = String(operation.at("op"));
            if (action == "append") command.action = Action::Append;
            else if (action == "delete") command.action = Action::Delete;
            else if (action == (table ? "replace" : "update")) command.action = Action::Update;
            else Invalid("section 的 op 不受支持。");
            if (operation.contains("expected_count")) command.expectedCount = U32(operation.at("expected_count"));
            if (operation.contains("index")) command.index = U32(operation.at("index"));
            if (operation.contains("number")) command.number = I32(operation.at("number"));
            const auto key = table ? "expected_table_key" : "expected_row_key";
            if (operation.contains(table ? "expected_row_key" : "expected_table_key")) Invalid("记录令牌字段与 section 不符。");
            if (operation.contains(key)) { command.expectedKey = String(operation.at(key)); if (!Sha(command.expectedKey)) Invalid("记录令牌须为小写 SHA-256。"); }
            if (command.action == Action::Delete) { if (operation.contains("value")) Invalid("delete 不接受 value。"); }
            else {
                if (!operation.contains("value")) Invalid("非删除操作需要 value。"); const auto& value = operation.at("value");
                if (command.section == Section::Upgrades) {
                    Shape(value, {"player_mask", "id_hex", "level", "availability"});
                    command.value = MapUpgradeAvailability{U32(value.at("player_mask")), Raw(value.at("id_hex")), I32(value.at("level")), I32(value.at("availability"))};
                } else if (command.section == Section::TechRestrictions) {
                    Shape(value, {"player_mask", "id_hex"}); command.value = MapTechRestriction{U32(value.at("player_mask")), Raw(value.at("id_hex"))};
                } else if (command.section == Section::RandomUnits) {
                    Shape(value, {"name", "column_types", "rows"}); MapRandomUnitTable record; record.number = command.number.value_or(-1);
                    Name(value.at("name"), record.name, command.keepName);
                    const auto& columns = value.at("column_types"); const auto& rows = value.at("rows");
                    if (!columns.is_array() || columns.size() > 64 || !rows.is_array() || rows.size() > 4096 || (!columns.empty() && rows.size() > 16384 / columns.size())) Invalid("随机单位表超出行列预算。");
                    for (const auto& c : columns) record.columnTypes.push_back(I32(c));
                    for (const auto& r : rows) { Shape(r, {"chance", "ids"}); MapRandomUnitRow row; row.chance = I32(r.at("chance"));
                        if (!r.at("ids").is_array() || r.at("ids").size() != columns.size()) Invalid("每行 ID 数须等于列数。");
                        for (const auto& id : r.at("ids")) row.ids.push_back(Choice(id)); record.rows.push_back(std::move(row)); }
                    command.value = std::move(record);
                } else {
                    Shape(value, {"name", "sets"}); MapRandomItemTable record; record.number = command.number.value_or(-1); Name(value.at("name"), record.name, command.keepName);
                    const auto& sets = value.at("sets"); if (!sets.is_array() || sets.size() > 4096) Invalid("随机物品集合数超出预算。"); std::size_t total = 0;
                    for (const auto& set : sets) { if (!set.is_array() || set.size() > 16384 - total) Invalid("随机物品总数超过 16384。"); total += set.size();
                        std::vector<MapRandomItem> items; for (const auto& item : set) { Shape(item, {"chance", "id"}); items.push_back({I32(item.at("chance")), Choice(item.at("id"))}); } record.sets.push_back(std::move(items)); }
                    command.value = std::move(record);
                }
            }
            result.operations.push_back(std::move(command));
        }
        return R::Success(std::move(result));
    } catch (const std::exception& e) { return R::Failure("Map.CustomSettings", "SETTINGS_JSON_INVALID", e.what()); }
}

Result<std::string> QueryMapCustomSettings(const MapDocument& document, const MapDataQueryBinding& binding, std::string_view text) {
    using R = Result<std::string>;
    try {
        auto query = Parse(text); Shape(query, {"section"}, {"settings_binding", "part", "number", "set_index", "offset", "limit"});
        if (binding.documentId.empty() || !binding.generation || !Sha(binding.sourceSha256)) Invalid("宿主文档 binding 不完整。");
        const auto section = String(query.at("section")); const bool summary = section == "summary";
        const auto kind = summary ? Section::Upgrades : SectionOf(section);
        if (summary) for (auto it = query.begin(); it != query.end(); ++it)
            if (it.key() != "section" && it.key() != "settings_binding") Invalid("summary 不接受分页或表选择参数。");
        auto sha = MapCustomSettingsEditing::MemberIdentity(document);
        const Json settingsBinding{{"document", Binding(binding)}, {"member_sha256", sha.Succeeded() ? Json(*sha.value) : Json(nullptr)}};
        if (!summary && (!query.contains("settings_binding") || query.at("settings_binding") != settingsBinding))
            return R::Failure("Map.CustomSettings", "SETTINGS_QUERY_STALE", "分页须精确回传 summary 的 settings_binding。");
        if (summary && query.contains("settings_binding") && query.at("settings_binding") != settingsBinding)
            return R::Failure("Map.CustomSettings", "SETTINGS_QUERY_STALE", "查询 binding 已变化。");
        const auto allowed = MapCustomSettingsEditing::WriteQualification(document);
        const auto& member = document.Member(MapDataKind::Settings);
        constexpr std::string_view states[]{"missing", "ready", "partial", "invalid", "ambiguous", "unreadable"};
        Json result{{"settings_binding", settingsBinding}, {"member_state", states[static_cast<unsigned>(member.state)]}, {"diagnostics", Diagnostics(member.diagnostics)},
            {"native_v25_writable", allowed.Succeeded()}, {"write_blockers", Diagnostics(allowed.diagnostics)}, {"random_commit_supported", false}};
        if (!document.Settings()) { result["custom_present"] = nullptr; return R::Success(result.dump()); }
        const auto& settings = *document.Settings(); result["version"] = settings.formatVersion; result["custom_present"] = settings.custom.has_value();
        result["unknown_tail_bytes"] = settings.UnknownTail().size();
        if (!settings.custom) return R::Success(result.dump());
        const auto& tables = *settings.custom;
        result["counts"] = {{"upgrades", tables.upgrades.size()}, {"tech_restrictions", tables.techRestrictions.size()}, {"random_units", tables.randomUnits.size()}, {"random_items", tables.randomItems.size()}};
        result["random_initialization_blocked"] = !tables.randomUnits.empty() || !tables.randomItems.empty();
        if (summary) return R::Success(result.dump());
        const auto offset = query.contains("offset") ? Number(query.at("offset"), 0, INT32_MAX) : 0;
        const auto limit = query.contains("limit") ? Number(query.at("limit"), 1, 256) : 64;
        const auto part = query.contains("part") ? String(query.at("part")) : "table_headers";
        Json rows = Json::array(); std::size_t total = 0;
        const auto page = [&](const auto& source, const auto& render) { total = source.size(); if (static_cast<std::uint64_t>(offset) > total) Invalid("offset 超出当前集合。");
            for (std::size_t i = static_cast<std::size_t>(offset); i < source.size() && rows.size() < static_cast<std::size_t>(limit); ++i) rows.push_back(render(source[i], i)); };
        if (kind == Section::Upgrades || kind == Section::TechRestrictions) {
            if (query.contains("part") || query.contains("number") || query.contains("set_index")) Invalid("科技行不接受随机子表选择。");
            if (kind == Section::Upgrades) page(tables.upgrades, [&](const auto& row, auto i) { return Json{{"index", i}, {"row_key", Key(kind, row, settings.formatVersion)}, {"player_mask", row.playerMask}, {"id_hex", Hex(row.id)}, {"level", row.level}, {"availability", row.availability}}; });
            else page(tables.techRestrictions, [&](const auto& row, auto i) { return Json{{"index", i}, {"row_key", Key(kind, row, settings.formatVersion)}, {"player_mask", row.playerMask}, {"id_hex", Hex(row.id)}}; });
        } else {
            const auto tablePage = [&](const auto& source) {
                using Table = typename std::decay_t<decltype(source)>::value_type;
                if (part == "table_headers") {
                    if (query.contains("number") || query.contains("set_index")) Invalid("表头页不接受子表选择。");
                    page(source, [&](const auto& table, auto i) { Json row{{"index", i}, {"number", table.number}, {"table_key", Key(kind, table, settings.formatVersion)}, {"name", Text(document, table.name)}};
                        if constexpr (std::is_same_v<Table, MapRandomUnitTable>) { row["column_count"] = table.columnTypes.size(); row["row_count"] = table.rows.size(); }
                        else row["set_count"] = table.sets.size(); return row; }); return;
                }
                if (!query.contains("number")) Invalid("子表查询须提供 number。");
                auto number = I32(query.at("number")); const Table* table = nullptr;
                for (const auto& candidate : source) if (candidate.number == number) { if (table) Invalid("源表 number 重复，无法唯一定位。"); table = &candidate; }
                if (!table) Invalid("表编号不存在。");
                if constexpr (std::is_same_v<Table, MapRandomUnitTable>) {
                    if (query.contains("set_index")) Invalid("单位表不接受 set_index。");
                    if (part == "columns") page(table->columnTypes, [](const auto& column, auto i) { return Json{{"index", i}, {"type", column}}; });
                    else if (part == "rows") page(table->rows, [](const auto& row, auto i) { Json ids = Json::array(); for (const auto& id : row.ids) ids.push_back({{"id_hex", Hex(id)}}); return Json{{"index", i}, {"chance", row.chance}, {"ids", ids}}; });
                    else Invalid("单位表 part 须为 table_headers/columns/rows。");
                } else {
                    if (part == "sets") { if (query.contains("set_index")) Invalid("sets 页不接受 set_index。"); page(table->sets, [](const auto& set, auto i) { return Json{{"index", i}, {"item_count", set.size()}}; }); }
                    else if (part == "items") { if (!query.contains("set_index")) Invalid("items 页需要 set_index。"); const auto s = U32(query.at("set_index")); if (s >= table->sets.size()) Invalid("set_index 超出范围。");
                        page(table->sets[s], [](const auto& item, auto i) { return Json{{"index", i}, {"chance", item.chance}, {"id_hex", Hex(item.id)}}; }); }
                    else Invalid("物品表 part 须为 table_headers/sets/items。");
                }
            };
            if (kind == Section::RandomUnits) tablePage(tables.randomUnits); else tablePage(tables.randomItems);
        }
        result["rows"] = std::move(rows); result["total"] = total;
        const auto next = static_cast<std::size_t>(offset) + result["rows"].size(); result["next_offset"] = next < total ? Json(next) : Json(nullptr);
        auto encoded = result.dump(-1, ' ', false, Json::error_handler_t::replace); if (encoded.size() > 1024 * 1024) return R::Failure("Map.CustomSettings", "SETTINGS_QUERY_BYTE_BUDGET", "查询页超过 1 MiB，请减小 limit；源数据未被截断。");
        return R::Success(std::move(encoded));
    } catch (const std::exception& e) { return R::Failure("Map.CustomSettings", "SETTINGS_QUERY_INVALID", e.what()); }
}
std::string MapCustomSettingsEditReceipt(const PreparedMapCustomSettingsEdit& plan) {
    auto before = MapCustomSettingsEditing::MemberIdentity(plan.Before()); auto after = MapCustomSettingsEditing::MemberIdentity(plan.After());
    return Json{{"has_changes", plan.HasChanges()}, {"can_commit", plan.CanCommit()}, {"map_saved", false}, {"before_member_sha256", before.value.value_or("")},
        {"after_member_sha256", after.value.value_or("")}, {"step_keys", plan.StepKeys()}, {"diagnostics", Diagnostics(plan.Diagnostics())},
        {"save_blockers", plan.CanCommit() ? Json::array() : Json::array({plan.InitializationBlocked() ? "MAP_INIT_RANDOM_TABLE_UNSUPPORTED" : "SETTINGS_RANDOM_COMMIT_UNSUPPORTED"})}}.dump();
}
} // namespace War3Map::Services
