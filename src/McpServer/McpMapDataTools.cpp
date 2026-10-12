#include "McpMapDataTools.h"
#include "McpMapDataJson.h"
#include "McpToolInternal.h"
#include "War3MapCore/Services/NameResolution.h"

#include <QJsonDocument>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace War3Map::Mcp {

Result<Services::MapDataQuerySnapshot> CaptureMapDataToolSnapshot(const MapDataToolContext& context,
    const std::shared_ptr<const MapDocument>& document) {
    const auto sameOwner = [](const auto& left, const auto& right) {
        return left && right && left.get() == right.get() && !left.owner_before(right) && !right.owner_before(left);
    };
    const bool current = sameOwner(document, context.document);
    if (current && context.preparedSnapshot && context.preparedSnapshot->Binding() == context.binding
        && sameOwner(context.preparedSnapshot->OwnedDocument(), document))
        return Result<Services::MapDataQuerySnapshot>::Success(*context.preparedSnapshot);
    auto captured = Services::CaptureMapDataSnapshot(document, context.binding, context.hasOpaqueArchiveMembers);
    if (captured.Succeeded() && current && context.acceptSnapshot) {
        // 只复制七份摘要元数据；同一个 shared MapDocument 正文由 Capture 和缓存共同拥有。
        const auto accepted = context.acceptSnapshot(std::make_shared<const Services::MapDataQuerySnapshot>(*captured.value));
        if (!accepted.Succeeded()) return {{}, accepted.diagnostics};
    }
    return captured;
}

namespace {
using namespace MapData;
using namespace Services;
using namespace MapDataJson;
constexpr std::size_t kResponseBytes = 256U * 1024U;
constexpr std::size_t kIdentityBytes = 4096;
constexpr std::size_t kRequestBytes = 1024U * 1024U;

class EntityJsonError final : public std::runtime_error {
public:
    EntityJsonError(const char* code, std::string message) : std::runtime_error(std::move(message)), code(code) {}
    const char* code;
};
QJsonObject TextSchema(int maximum = 65536) { return {{"type", "string"}, {"maxLength", maximum}}; }
QJsonObject IndexSchema() { return {{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}; }
QJsonObject ShaSchema(bool missing = false) {
    return {{"type", "string"}, {"pattern", missing ? "^([0-9a-f]{64}|missing)$" : "^[0-9a-f]{64}$"}, {"maxLength", 64}};
}
void Keys(const QJsonObject& object, const QJsonObject& properties, const QJsonArray& required = {}) {
    for (const auto& key : required) if (!object.contains(key.toString()))
        throw EntityJsonError("ENTITY_ARGUMENT_REQUIRED", "缺少实体参数：" + key.toString().toStdString());
    for (auto it = object.begin(); it != object.end(); ++it) if (!properties.contains(it.key()))
        throw EntityJsonError("ENTITY_ARGUMENT_UNKNOWN", "未知实体参数：" + it.key().toStdString());
}
std::string Text(const QJsonObject& object, const char* key, std::size_t maximum = 65536) {
    const auto raw = object.value(QLatin1String(key));
    if (!raw.isString() || raw.toString().contains(QChar::Null))
        throw EntityJsonError("ENTITY_ARGUMENT_TYPE", std::string(key) + " 必须为无 NUL 字符串。");
    const auto bytes = raw.toString().toUtf8();
    if (QString::fromUtf8(bytes) != raw.toString())
        throw EntityJsonError("ENTITY_ARGUMENT_ENCODING", std::string(key) + " 含有无法无损编码为UTF-8的文字。");
    if (static_cast<std::size_t>(bytes.size()) > maximum)
        throw EntityJsonError("ENTITY_ARGUMENT_BUDGET", std::string(key) + " 超出 UTF-8 字节预算。");
    return bytes.toStdString();
}
std::uint64_t Index(const QJsonObject& object, const char* key) {
    if (!object.value(QLatin1String(key)).isString())
        throw EntityJsonError("ENTITY_INDEX_INVALID", "行索引必须为规范 uint64 十进制字符串。");
    const auto parsed = ParseUnsignedId(object, key, true);
    if (!parsed.Succeeded() || !parsed.value || std::to_string(*parsed.value) != Text(object, key, 20))
        throw EntityJsonError("ENTITY_INDEX_INVALID", "行索引无效、溢出或不是规范十进制字符串。");
    return *parsed.value;
}
bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
MapDataKind Kind(const QJsonObject& arguments) {
    const auto kind = ParseMapDataKindId(Text(arguments, "kind", 16));
    if (!kind || (*kind != MapDataKind::Units && *kind != MapDataKind::Doodads
        && *kind != MapDataKind::Regions && *kind != MapDataKind::Cameras))
        throw EntityJsonError("ENTITY_KIND_INVALID", "C1 查询仅接受 units/doodads/regions/cameras。");
    return *kind;
}
QJsonObject Identity(const MapDataToolHost& host) {
    const auto raw = host.identity();
    QJsonObject result;
    for (const auto* key : {"project_identity", "revision", "transaction_id", "transaction_revision", "transaction_active",
        "document_generation", "scope", "content_scope"}) if (raw.contains(key)) result.insert(key, raw.value(key));
    if (!result.value("project_identity").isString() || !result.value("revision").isString()
        || result.value("project_identity").toString().isEmpty() || result.value("revision").toString().isEmpty()
        || static_cast<std::size_t>(QJsonDocument(result).toJson(QJsonDocument::Compact).size()) > kIdentityBytes)
        throw EntityJsonError("ENTITY_HOST_IDENTITY", "实体宿主必须提供有界的真实工程身份和修订。");
    return result;
}
bool SameIdentity(const QJsonObject& first, const QJsonObject& second) {
    return first.value("project_identity") == second.value("project_identity") && first.value("revision") == second.value("revision");
}
QJsonObject Stamp(QJsonObject body, const QJsonObject& identity) {
    for (auto it = identity.begin(); it != identity.end(); ++it) body.insert(it.key(), it.value());
    return body;
}
Status BodyBudget(const QJsonObject& body) {
    if (static_cast<std::size_t>(QJsonDocument(body).toJson(QJsonDocument::Compact).size()) > kResponseBytes - kIdentityBytes)
        return Status::Failure("MCP.Entities", "ENTITY_RESPONSE_BUDGET", "完整实体回执超过 256 KiB，候选未发布；请缩小批次或页长。");
    return Status::Ok();
}
QJsonObject Scope(const MapDataToolContext& context) {
    return {{"includes_active_transaction_overlay", context.includesActiveTransactionOverlay},
        {"includes_unsaved_ui_revision", context.includesUnsavedUiRevision}, {"locator_scope", "snapshot-revision"}};
}
bool SameDocumentBytes(const MapDocument& first, const MapDocument& second) {
    for (std::size_t i = 0; i < first.Members().size(); ++i) {
        const auto& a = first.Members()[i]; const auto& b = second.Members()[i];
        if (a.state != b.state || a.memberName != b.memberName || a.sources.size() != b.sources.size()) return false;
        for (std::size_t j = 0; j < a.sources.size(); ++j)
            if (a.sources[j].name != b.sources[j].name || a.sources[j].data != b.sources[j].data) return false;
    }
    return true;
}
QJsonObject SettingsText(const MapDocument& document, std::string_view wire);
QJsonObject EntityRowJson(MapDataQueryRow row, const MapDocument& document, MapDataKind kind, bool soundWritable = false) {
    // 共用只读目录较宽；C1 的实际 native 布局没有写入证明时不能显示为可写。
    const bool writable = kind == MapDataKind::Units ? (document.Units() && document.Units()->version == 8
        && document.Units()->subversion == 11 && document.Units()->recordLayout == PlacementRecordLayout::Classic)
        : kind == MapDataKind::Settings ? (document.Member(kind).state == MapDataState::Ready
            && document.Member(kind).sources.size() == 1 && document.Settings()
            && document.Settings()->HasClassicLayout() && document.Settings()->FullyParsed())
        : kind == MapDataKind::Strings ? (document.Member(kind).state == MapDataState::Ready
            && document.Member(kind).sources.size() == 1 && document.Strings())
        : kind == MapDataKind::Regions ? (document.Regions() && document.Regions()->version == 5)
        : kind == MapDataKind::Cameras ? (document.Cameras() && document.Cameras()->version == 0
            && document.Cameras()->layout == CameraLayout::Classic) : kind == MapDataKind::Sounds ? soundWritable : false;
    if (!writable) for (auto& field : row.fields) field.writable = false;
    auto result = RowJson(row);
    if (kind == MapDataKind::Sounds) result.insert("sound_id", QString::fromStdString(row.rowKey));
    if (kind == MapDataKind::Settings) {
        QJsonArray fields;
        for (const auto& field : row.fields) {
            auto json = FieldJson(field);
            if (field.value.type == MapFieldType::Bytes) {
                const auto text = SettingsText(document, field.value.bytes);
                for (auto it = text.begin(); it != text.end(); ++it) json.insert(it.key(), it.value());
                json.insert("resolution_diagnostics", text.value("diagnostics"));
            }
            fields.append(json);
        }
        result.insert("fields", fields); result.insert("setting_id", "w3i");
    }
    return result;
}

QJsonObject SettingsText(const MapDocument& document, std::string_view wire) {
    NameResolutionContext context;
    context.mapDocument = &document; context.preserveStringFormatting = true;
    const auto name = ResolveName(context, wire, "string");
    const bool available = name.state == NameResolutionState::Resolved || name.state == NameResolutionState::Literal;
    const auto id = StringReferenceId(wire);
    // 原正文十六进制仍绑定实际来源字节，显示用 UTF-8 单独返回。
    const auto resolved = document.ResolveString(id ? "TRIGSTR_" + std::to_string(*id) : std::string(wire));
    return {{"wire_hex", Hex(wire)}, {"wire_value", Utf8OrNull(wire)},
        {"raw_value", Utf8OrNull(wire)}, {"display_name", QString::fromStdString(name.displayName)},
        {"name_state", QString::fromStdString(std::string(NameResolutionStateName(name.state)))}, {"name_reason", QString::fromStdString(name.reason)},
        {"value", available ? QJsonValue(QString::fromStdString(name.displayName)) : QJsonValue(QJsonValue::Null)},
        {"resolved_hex", resolved.Succeeded() && resolved.value ? QJsonValue(Hex(*resolved.value)) : QJsonValue(QJsonValue::Null)},
        {"resolution", available ? (name.state == NameResolutionState::Resolved ? "resolved" : "literal") : "unresolved"},
        {"diagnostics", DiagnosticsJson(resolved.diagnostics).value("diagnostics")}};
}

QJsonObject ReadonlySettings(const MapDocument& document) {
    if (!document.Settings()) return {};
    const auto& settings = *document.Settings();
    const auto screen = [&](const MapScreenSettings& value) {
        return QJsonObject{{"path", SettingsText(document, value.path)}, {"text", SettingsText(document, value.text)},
            {"title", SettingsText(document, value.title)}, {"subtitle", SettingsText(document, value.subtitle)}};
    };
    QJsonArray players, forces, upgrades, tech;
    constexpr std::size_t limit = 128;
    for (std::size_t i = 0; i < std::min(limit, settings.players.size()); ++i) {
        const auto& player = settings.players[i];
        players.append(QJsonObject{{"player_id", QString::number(player.number)}, {"controller", player.controller},
            {"race", player.race}, {"flags", QString::number(player.flags)}, {"name", SettingsText(document, player.name)},
            {"start_x_raw", QString::fromStdString(FormatMapFieldRaw({MapFieldType::Float32, 0, std::bit_cast<std::uint32_t>(player.startX), {}}))},
            {"start_y_raw", QString::fromStdString(FormatMapFieldRaw({MapFieldType::Float32, 0, std::bit_cast<std::uint32_t>(player.startY), {}}))},
            {"ally_low_priority_mask", QString::number(player.allyLowPriorityMask)}, {"ally_high_priority_mask", QString::number(player.allyHighPriorityMask)}});
    }
    for (std::size_t i = 0; i < std::min(limit, settings.forces.size()); ++i) {
        const auto& force = settings.forces[i];
        forces.append(QJsonObject{{"source_index", QString::number(i)}, {"flags", QString::number(force.flags)},
            {"player_mask", QString::number(force.playerMask)}, {"name", SettingsText(document, force.name)}});
    }
    if (settings.custom) {
        for (std::size_t i = 0; i < std::min(limit, settings.custom->upgrades.size()); ++i) {
            const auto& entry = settings.custom->upgrades[i];
            upgrades.append(QJsonObject{{"player_mask", QString::number(entry.playerMask)},
                {"id_hex", Hex({reinterpret_cast<const char*>(entry.id.data()), entry.id.size()})},
                {"level", entry.level}, {"availability", entry.availability}});
        }
        for (std::size_t i = 0; i < std::min(limit, settings.custom->techRestrictions.size()); ++i) {
            const auto& entry = settings.custom->techRestrictions[i];
            tech.append(QJsonObject{{"player_mask", QString::number(entry.playerMask)},
                {"id_hex", Hex({reinterpret_cast<const char*>(entry.id.data()), entry.id.size()})}});
        }
    }
    const auto page = [](const QJsonArray& rows, std::size_t total) {
        return QJsonObject{{"rows", rows}, {"total", QString::number(total)}, {"truncated", rows.size() < static_cast<qsizetype>(total)}};
    };
    const auto& weather = settings.environment.weather;
    return {{"writable", false}, {"loading_background", settings.loadingBackground}, {"loading_screen", screen(settings.loadingScreen)},
        {"prologue_screen", screen(settings.prologueScreen)},
        {"weather_id_hex", Hex({reinterpret_cast<const char*>(weather.data()), weather.size()})},
        {"sound_environment", SettingsText(document, settings.environment.soundEnvironment)},
        {"players", page(players, settings.players.size())}, {"forces", page(forces, settings.forces.size())},
        {"custom_known", settings.custom.has_value()},
        {"upgrades", settings.custom ? QJsonValue(page(upgrades, settings.custom->upgrades.size())) : QJsonValue(QJsonValue::Null)},
        {"tech_restrictions", settings.custom ? QJsonValue(page(tech, settings.custom->techRestrictions.size())) : QJsonValue(QJsonValue::Null)},
        {"random_unit_tables", settings.custom ? QJsonValue(QString::number(settings.custom->randomUnits.size())) : QJsonValue(QJsonValue::Null)},
        {"random_item_tables", settings.custom ? QJsonValue(QString::number(settings.custom->randomItems.size())) : QJsonValue(QJsonValue::Null)}};
}
QJsonObject EntityFilterSchema() {
    const auto number = QJsonObject{{"type", "number"}};
    return BasicSchema({{"type_id", TextSchema(12)},
        {"player", QJsonObject{{"type", "integer"}, {"minimum", std::numeric_limits<std::int32_t>::min()},
            {"maximum", std::numeric_limits<std::int32_t>::max()}}},
        {"world_rect", BasicSchema({{"left", number}, {"bottom", number}, {"right", number}, {"top", number}},
            {"left", "bottom", "right", "top"})}});
}
MapDataEntityFilter EntityFilter(const QJsonObject& object) {
    Keys(object, EntityFilterSchema().value("properties").toObject());
    MapDataEntityFilter filter;
    if (object.contains("type_id")) {
        auto bytes = Text(object, "type_id", 12);
        if (bytes.size() == 12 && bytes.starts_with("hex:")) {
            const auto hex = std::string_view(bytes).substr(4);
            if (hex.size() != 8 || !std::all_of(hex.begin(), hex.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            })) throw EntityJsonError("ENTITY_FILTER_TYPE_ID", "type_id 须为精确四字节UTF-8文本或hex:xxxxxxxx。");
            bytes = QByteArray::fromHex(QByteArray(hex.data(), static_cast<qsizetype>(hex.size()))).toStdString();
        }
        if (bytes.size() != 4) throw EntityJsonError("ENTITY_FILTER_TYPE_ID", "type_id 须为精确四字节UTF-8文本或hex:xxxxxxxx。");
        filter.typeIdBytes = std::move(bytes);
    }
    if (object.contains("player")) {
        const auto player = ParseInt32(object, "player");
        if (!player.Succeeded() || !player.value) throw EntityJsonError("ENTITY_FILTER_PLAYER", "player 须为精确int32整数，不限制到写入用的0..15。");
        filter.player = *player.value;
    }
    if (object.contains("world_rect")) {
        if (!object.value("world_rect").isObject())
            throw EntityJsonError("ENTITY_FILTER_RECT", "world_rect 须为有限世界坐标矩形对象。");
        const auto raw = object.value("world_rect").toObject();
        const auto schema = EntityFilterSchema().value("properties").toObject().value("world_rect").toObject();
        Keys(raw, schema.value("properties").toObject(), schema.value("required").toArray());
        const auto number = [&](const char* key) {
            const auto value = raw.value(QLatin1String(key));
            if (!value.isDouble() || !std::isfinite(value.toDouble()))
                throw EntityJsonError("ENTITY_FILTER_RECT", "world_rect 的四边必须为有限数值。");
            return value.toDouble();
        };
        MapDataWorldRect rectangle{number("left"), number("bottom"), number("right"), number("top")};
        if (rectangle.left > rectangle.right || rectangle.bottom > rectangle.top)
            throw EntityJsonError("ENTITY_FILTER_RECT", "world_rect 须满足 left≤right、bottom≤top；四边均包含。");
        filter.worldRect = rectangle;
    }
    return filter;
}
QJsonObject EntityFilterJson(const MapDataEntityFilter& filter) {
    QJsonObject result;
    if (filter.typeIdBytes) result.insert("type_id", "hex:" + Hex(*filter.typeIdBytes));
    if (filter.player) result.insert("player", *filter.player);
    if (filter.worldRect) {
        const auto& rect = *filter.worldRect;
        result.insert("world_rect", QJsonObject{{"left", rect.left}, {"bottom", rect.bottom},
            {"right", rect.right}, {"top", rect.top}});
    }
    return result;
}
QJsonObject EntityCursorSchema() {
    const auto binding = BasicSchema({{"document_id", TextSchema(4096)}, {"document_generation", IndexSchema()},
        {"revision", IndexSchema()}, {"source_sha256", ShaSchema()}},
        {"document_id", "document_generation", "revision", "source_sha256"});
    return BasicSchema({{"binding", binding}, {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"units", "doodads", "regions", "cameras"}}}},
        {"member_sha256", ShaSchema(true)}, {"filter", EntityFilterSchema()}, {"next_index", IndexSchema()}},
        {"binding", "kind", "member_sha256", "filter", "next_index"});
}
QJsonObject EntityCursorJson(const MapDataEntityQueryCursor& cursor) {
    return {{"binding", BindingJson(cursor.source.binding)}, {"kind", QString::fromStdString(std::string(MapDataKindId(cursor.source.kind)))},
        {"member_sha256", cursor.memberSha256 ? QString::fromStdString(*cursor.memberSha256) : QStringLiteral("missing")},
        {"filter", EntityFilterJson(cursor.filter)}, {"next_index", QString::number(cursor.source.nextIndex)}};
}
Result<QJsonObject> SelectedRows(const MapDataQuerySnapshot& snapshot, MapDataKind kind, const std::set<std::uint64_t>& selected) {
    auto described = DescribeMapData(snapshot, snapshot.Binding());
    if (!described.Succeeded() || !described.value) return {{}, described.diagnostics};
    QJsonArray rows;
    std::size_t logicalBytes = 0;
    const bool soundWritable = kind == MapDataKind::Sounds && ValidateSoundFieldSource(*snapshot.OwnedDocument()).Succeeded();
    for (const auto row : selected) {
        auto page = QueryMapDataPage(snapshot, snapshot.Binding(), kind,
            MapDataQueryCursor{snapshot.Binding(), kind, row}, 1, kResponseBytes - kIdentityBytes);
        if (!page.Succeeded() || !page.value) return {{}, page.diagnostics};
        if (page.value->rows.size() != 1)
            return Result<QJsonObject>::Failure("MCP.Entities", "ENTITY_ROW_MISSING", "修改回执中的实际行缺失。");
        if (page.value->logicalBytes > kResponseBytes - kIdentityBytes - logicalBytes)
            return Result<QJsonObject>::Failure("MCP.Entities", "ENTITY_RESPONSE_BUDGET", "完整实体回执逻辑字节超额，候选未发布。");
        logicalBytes += page.value->logicalBytes;
        rows.append(EntityRowJson(page.value->rows.front(), *snapshot.OwnedDocument(), kind, soundWritable));
    }
    return Result<QJsonObject>::Success({{"member", MemberJson(described.value->members[static_cast<std::size_t>(kind)])}, {"rows", rows}});
}
} // namespace

namespace {
QJsonObject SoundCursorSchema() {
    return BasicSchema({{"binding", EntityCursorSchema().value("properties").toObject().value("binding").toObject()},
        {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"sounds"}}}}, {"member_sha256", ShaSchema(true)},
        {"query", TextSchema(4096)}, {"next_index", IndexSchema()}}, {"binding", "kind", "member_sha256", "query", "next_index"});
}
std::size_t SoundRow(const MapDocument& document, std::string_view id) {
    if (!id.starts_with("sound:") || id.size() > 16384)
        throw EntityJsonError("MAP_SOUND_ID_INVALID", "sound_id须为查询返回的原名字字节hex稳定标识。");
    const auto raw = std::string("hex:") + std::string(id.substr(6));
    const auto parsed = ParseMapFieldRaw(MapFieldType::Bytes, raw);
    if (!parsed.Succeeded() || !parsed.value || FormatMapFieldRaw(*parsed.value) != raw)
        throw EntityJsonError("MAP_SOUND_ID_INVALID", "sound_id只接受规范小写十六进制原名字，不能使用界面行号或显示文字。");
    if (!document.Sounds()) throw EntityJsonError("MAP_SOUND_TABLE_UNAVAILABLE", "当前声音成员未解析，不能选择定义。");
    std::size_t row = 0, matches = 0;
    for (std::size_t index = 0; index < document.Sounds()->entries.size(); ++index)
        if (document.Sounds()->entries[index].name == parsed.value->bytes) { row = index; ++matches; }
    if (matches != 1) throw EntityJsonError(matches ? "MAP_FIELD_SOUND_NAME_AMBIGUOUS" : "MAP_SOUND_ID_MISSING",
        "sound_id必须对应当前来源中的一个唯一已有声音原名字。");
    return row;
}
QJsonObject SoundRead(const QString& name, const QJsonObject& arguments, const QJsonObject& toolSchema,
    const MapDataToolContext& context, const MapDataQuerySnapshot& snapshot,
    const MapDataQueryMember& member, const QJsonObject& identity) {
    const auto& document = *context.document;
    const auto qualification = ValidateSoundFieldSource(document);
    const bool writable = qualification.Succeeded();
    const auto sha = member.memberSha256 ? *member.memberSha256 : std::string("missing");
    auto body = Scope(context); body.insert("ok", true); body.insert("schema_version", 1);
    body.insert("binding", BindingJson(context.binding)); body.insert("member", MemberJson(member));
    body.insert("write_diagnostics", DiagnosticsJson(qualification.diagnostics).value("diagnostics"));
    if (name == "sounds_get") {
        const auto row = SoundRow(document, Text(arguments, "sound_id", 16384));
        std::set<std::string> requested;
        if (arguments.contains("fields")) {
            if (!arguments.value("fields").isArray()) throw EntityJsonError("ENTITY_ARGUMENT_TYPE", "fields须为字段键数组。");
            const auto fields = arguments.value("fields").toArray();
            if (fields.isEmpty() || fields.size() > 32) throw EntityJsonError("ENTITY_ARGUMENT_BUDGET", "fields须包含1..32个唯一字段。");
            for (const auto& value : fields) {
                const auto field = Text(QJsonObject{{"field", value}}, "field", 64);
                if (!FindMapField(MapDataKind::Sounds, field)) throw EntityJsonError("MAP_FIELD_UNKNOWN", "声音目录没有请求的字段。");
                if (!requested.insert(field).second) throw EntityJsonError("MAP_FIELD_DUPLICATE", "fields不能重复。");
            }
        }
        const auto page = QueryMapDataPage(snapshot, context.binding, MapDataKind::Sounds,
            MapDataQueryCursor{context.binding, MapDataKind::Sounds, row}, 1, kResponseBytes - kIdentityBytes);
        if (!page.Succeeded() || !page.value) return Stamp(Failure(page), identity);
        if (page.value->rows.size() != 1) return Failure("ENTITY_ROW_MISSING", "声音查询行缺失。");
        auto result = EntityRowJson(page.value->rows.front(), document, MapDataKind::Sounds, writable);
        if (!requested.empty()) {
            QJsonArray fields;
            for (const auto& field : result.value("fields").toArray())
                if (requested.contains(field.toObject().value("id").toString().toStdString())) fields.append(field);
            if (static_cast<std::size_t>(fields.size()) != requested.size())
                return Stamp(Failure("MAP_FIELD_NOT_APPLICABLE", "请求的声音字段不在当前记录布局中。"), identity);
            result.insert("fields", fields);
        }
        body.insert("sound_id", result.value("sound_id")); body.insert("rows", QJsonArray{result});
        body.insert("total", "1"); body.insert("truncated", false);
    } else {
        const auto query = arguments.contains("query") ? Text(arguments, "query", 4096) : std::string{};
        std::uint64_t start = 0; std::size_t limit = 64; QJsonObject page;
        if (arguments.contains("page")) {
            if (!arguments.value("page").isObject()) throw EntityJsonError("ENTITY_ARGUMENT_TYPE", "page须为对象。");
            page = arguments.value("page").toObject();
            Keys(page, toolSchema.value("properties").toObject().value("page").toObject().value("properties").toObject());
            if (page.contains("limit")) {
                const auto parsed = ParseInt32(page, "limit");
                if (!parsed.Succeeded() || !parsed.value || *parsed.value < 1 || *parsed.value > 128)
                    throw EntityJsonError("ENTITY_QUERY_LIMIT", "页长须为1..128整数。");
                limit = static_cast<std::size_t>(*parsed.value);
            }
            if (page.contains("start_index")) start = Index(page, "start_index");
            if (page.contains("cursor")) {
                if (page.contains("start_index") || !page.value("cursor").isObject())
                    throw EntityJsonError("ENTITY_QUERY_CURSOR_INVALID", "cursor须为对象，且不能同时传start_index。");
                const auto cursor = page.value("cursor").toObject(); const auto schema = SoundCursorSchema();
                Keys(cursor, schema.value("properties").toObject(), schema.value("required").toArray());
                if (!cursor.value("binding").isObject() || cursor.value("binding").toObject() != BindingJson(context.binding)
                    || cursor.value("kind") != "sounds" || cursor.value("member_sha256") != QString::fromStdString(sha)
                    || Text(cursor, "query", 4096) != query)
                    throw EntityJsonError("ENTITY_QUERY_CURSOR_STALE", "声音续页的修订、成员SHA、query或类别已变化。");
                start = Index(cursor, "next_index");
            }
        }
        if (!document.Sounds() && member.state != MapDataQueryState::Missing) {
            auto refused = Failure("MAP_SOUND_TABLE_UNAVAILABLE", "声音来源损坏、歧义或不可读；未伪造空表。");
            refused.insert("member", MemberJson(member)); return Stamp(std::move(refused), identity);
        }
        const auto count = document.Sounds() ? document.Sounds()->entries.size() : 0;
        if (start > count) throw EntityJsonError("ENTITY_QUERY_CURSOR_INVALID", "声音起点超过当前定义数。");
        const bool totalKnown = count <= 4096;
        std::size_t examined = 0, matched = 0, nextIndex = static_cast<std::size_t>(start), logical = 0;
        bool more = false; QJsonArray rows;
        for (std::size_t index = totalKnown ? 0 : static_cast<std::size_t>(start); index < count && examined < 4096; ++index) {
            ++examined;
            const auto& sound = document.Sounds()->entries[index];
            if (!totalKnown) nextIndex = index + 1;
            if (sound.name.find(query) == std::string::npos && sound.filePath.find(query) == std::string::npos) continue;
            ++matched;
            if (index < start) continue;
            if (static_cast<std::size_t>(rows.size()) == limit) { more = true; continue; }
            const auto row = QueryMapDataPage(snapshot, context.binding, MapDataKind::Sounds,
                MapDataQueryCursor{context.binding, MapDataKind::Sounds, index}, 1, kResponseBytes - kIdentityBytes);
            if (!row.Succeeded() || !row.value) return Stamp(Failure(row), identity);
            if (row.value->rows.size() != 1) return Stamp(Failure("ENTITY_ROW_MISSING", "声音查询行缺失。"), identity);
            if (row.value->logicalBytes > kResponseBytes - kIdentityBytes - logical)
                return Stamp(Failure("ENTITY_RESPONSE_BUDGET", "声音页超过256KiB，请缩小页长。"), identity);
            logical += row.value->logicalBytes;
            const auto& native = row.value->rows.front();
            if (native.rowKey.size() > 16384) throw EntityJsonError("ENTITY_RESPONSE_BUDGET", "声音原名字超出稳定ID预算，未截断身份。");
            rows.append(QJsonObject{{"sound_id", QString::fromStdString(native.rowKey)}, {"source_index", QString::number(index)},
                {"name", Utf8OrNull(sound.name)}, {"name_hex", Hex(sound.name)}, {"file_path", Utf8OrNull(sound.filePath)},
                {"file_path_hex", Hex(sound.filePath)}, {"writable", writable}});
            if (totalKnown) nextIndex = index + 1;
            else if (static_cast<std::size_t>(rows.size()) == limit) break;
        }
        if (!totalKnown) more = nextIndex < count;
        QJsonValue cursor(QJsonValue::Null);
        if (more) cursor = QJsonObject{{"binding", BindingJson(context.binding)}, {"kind", "sounds"},
            {"member_sha256", QString::fromStdString(sha)}, {"query", QString::fromStdString(query)}, {"next_index", QString::number(nextIndex)}};
        body.insert("rows", rows); body.insert("query", QString::fromStdString(query)); body.insert("next_cursor", cursor);
        body.insert("total", QString::number(count)); body.insert("matched_total", totalKnown ? QJsonValue(QString::number(matched)) : QJsonValue(QJsonValue::Null));
        body.insert("matched_total_complete", totalKnown); body.insert("examined", QString::number(examined));
        body.insert("scan_budget_exhausted", !totalKnown && examined == 4096 && more); body.insert("truncated", more);
    }
    const auto bounded = BodyBudget(body); if (!bounded.Succeeded()) return Failure(bounded);
    return Stamp(std::move(body), identity);
}
} // namespace

std::vector<ToolDefinition> MapDataToolDefinitions() {
    QJsonObject identity{{"project_identity", TextSchema(4096)}, {"expected_revision", TextSchema(4096)}};
    QJsonObject kind{{"type", "string"}, {"enum", QJsonArray{"units", "doodads", "regions", "cameras"}}};
    const auto change = BasicSchema({{"source_index", IndexSchema()}, {"row_key", TextSchema(16384)},
        {"field", TextSchema(64)}, {"expected_raw", TextSchema()}, {"value", TextSchema()}},
        {"source_index", "row_key", "field", "expected_raw", "value"});
    std::vector<ToolDefinition> result;
    const auto add = [&](const char* name, const char* description, QJsonObject properties, QJsonArray required, ToolPermission permission) {
        ToolDefinition definition; definition.name = QString::fromUtf8(name); definition.description = QString::fromUtf8(description);
        definition.inputSchema = BasicSchema(std::move(properties), std::move(required)); definition.permission = permission;
        definition.maxResponseBytes = 1024U * 1024U; definition.maxRequestIdBytes = 4096; result.push_back(std::move(definition));
    };
    auto query = identity; query.insert("kind", kind); query.insert("expected_member_sha256", ShaSchema(true));
    query.insert("filter", EntityFilterSchema());
    query.insert("page", BasicSchema({{"start_index", IndexSchema()}, {"cursor", EntityCursorSchema()},
        {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}}}));
    add("entities_query", "查询当前原会话/活动事务的单位、地物、区域或镜头；默认64、最多128行，完整JSON最多256KiB。filter条件取交集：type_id精确四字节且区分大小写（可用hex:xxxxxxxx），仅units/doodads可用；player精确int32，仅units可用。world_rect={left,bottom,right,top}为有限世界坐标闭矩形，匹配摆放XY、镜头targetXY或相交区域；匹配到只有网格坐标的特殊地物或非有限位置时明确拒绝，不猜坐标。先筛选再分页，每次最多扫描4096条，未知总数为null。续页回传原cursor和同filter，不同时传start_index。source_index和row_key仅在返回修订内有效；查询不编辑或保存。", query, {"kind"}, ToolPermission::Query);
    auto write = identity; write.insert("kind", kind); write.insert("expected_member_sha256", ShaSchema());
    write.insert("changes", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", change}});
    add("entities_set_fields", "原子修改已有单位owner/facing/hit_points/mana、普通classic单位绝对native世界x/y/z、区域四边或经典镜头target_x/target_y/z_offset/distance。单位XYZ有限±1000000，不采样地形或自动改其它轴，sloc/SkinId/不完整来源拒绝XYZ。1..32项同类字段须绑定真实身份、成员SHA、行身份和精确前值；沿原Undo/活动事务，不保存。尚不支持create/delete/rename，地物字段只读。", write,
        {"project_identity", "expected_revision", "kind", "expected_member_sha256", "changes"}, ToolPermission::Edit);
    auto settingsRead = identity; settingsRead.insert("expected_member_sha256", ShaSchema(true));
    add("map_settings_get", "读取当前原会话/活动事务的真实W3I。文字value为TRIGSTR解析结果，wire_value为原wire文字，raw为写入预条件；解析不明或非UTF-8时value为null并返回原hex/诊断。返回稳定setting_id=w3i、成员SHA、修订、总数及截断；玩家/队伍/科技/天气/加载画面仅只读，单节最多128项、完整JSON最多256KiB。", settingsRead, {}, ToolPermission::Query);
    const auto settingChange = BasicSchema({{"field", TextSchema(64)}, {"expected_raw", TextSchema()}, {"value", TextSchema(4096)}},
        {"field", "expected_raw", "value"});
    auto settingsWrite = identity; settingsWrite.insert("expected_member_sha256", ShaSchema());
    settingsWrite.insert("setting_id", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"w3i"}}});
    settingsWrite.insert("write_mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"literal", "shared_string", "mixed"}}});
    settingsWrite.insert("changes", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", settingChange}});
    add("map_settings_set", "原子修改经典完整v25 W3I既有地图名/作者/描述/建议玩家/game_data_set，1..32项。文字value是UTF-8字面量，最多4096字节、不接受TRIGSTR_开头。作者/描述/建议玩家原值是TRIGSTR时保留原引用并改写对应唯一WTS正文，所有同编号引用一并受影响；须显式write_mode=shared_string。同时改W3I literal/数值时用mixed；只改W3I用literal。地图名沿用非空literal写入。expected_raw绑定原wire字节而非解析文字。setting_id=w3i、expected_revision和原W3I SHA必填；原WTS来源另行绑定，缺失/重复/漂移拒绝，沿原Undo/密封事务，不保存。", settingsWrite,
        {"project_identity", "expected_revision", "expected_member_sha256", "setting_id", "write_mode", "changes"}, ToolPermission::Edit);
    auto stringsRead = identity; stringsRead.insert("expected_member_sha256", ShaSchema(true));
    const auto stringCursor = BasicSchema({{"binding", EntityCursorSchema().value("properties").toObject().value("binding").toObject()},
        {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"strings"}}}}, {"member_sha256", ShaSchema(true)}, {"next_index", IndexSchema()}},
        {"binding", "kind", "member_sha256", "next_index"});
    stringsRead.insert("page", BasicSchema({{"start_index", IndexSchema()}, {"cursor", stringCursor},
        {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}}}));
    add("strings_query", "读取当前WTS原字符串表，value为正文文字，不递归解析正文中的TRIGSTR。默认64、最多128行/256KiB，返回稳定string_id、精确raw、成员SHA、修订、总数、截断和续页cursor；cursor绑定原修订/成员SHA，不能同时传start_index。缺失/歧义/重复ID保留状态，不补空表。只读，不保存。", stringsRead, {}, ToolPermission::Query);
    const auto stringChange = BasicSchema({{"string_id", IndexSchema()}, {"expected_raw", TextSchema()}, {"value", TextSchema(4096)}},
        {"string_id", "expected_raw", "value"});
    auto stringsWrite = identity; stringsWrite.insert("expected_member_sha256", ShaSchema());
    stringsWrite.insert("write_mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"shared_string"}}});
    stringsWrite.insert("changes", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", stringChange}});
    add("strings_set", "按稳定string_id原子修改1..32项已存在的唯一WTS正文，value为UTF-8字面量、每项最多4096字节。必须显式write_mode=shared_string：会影响所有引用该ID的字段；不改W3I wire引用，不创建/删除/重编号。expected_raw绑定原正文、expected_revision和成员SHA必填；重复ID/缺失/歧义/非法编码拒绝，沿原Undo/密封事务，不保存。", stringsWrite,
        {"project_identity", "expected_revision", "expected_member_sha256", "write_mode", "changes"}, ToolPermission::Edit);
    auto soundsRead = identity; soundsRead.insert("expected_member_sha256", ShaSchema(true));
    soundsRead.insert("query", TextSchema(4096));
    soundsRead.insert("page", BasicSchema({{"start_index", IndexSchema()}, {"cursor", SoundCursorSchema()},
        {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}}}));
    add("sounds_list", "只读当前声音定义清单，不是声音文件。query按原名字/路径UTF-8字节区分大小写包含匹配；默认64、最多128行/256KiB，每次最多检查4096定义。返回sound:原名字hex稳定ID、来源SHA/修订、完整定义总数、未知匹配总数null与续页cursor。缺失明确返回missing，歧义/损坏不伪造空表；不编辑或保存。", soundsRead, {}, ToolPermission::Query);
    auto soundGet = identity; soundGet.insert("expected_member_sha256", ShaSchema(true)); soundGet.insert("sound_id", TextSchema(16384));
    soundGet.insert("fields", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"uniqueItems", true}, {"items", TextSchema(64)}});
    add("sounds_get", "按查询返回的原名字hex sound_id读取唯一声音定义，fields可选目录字段键；保留精确raw、默认哨兵、成员SHA和修订。非UTF-8名字保留hex，不用界面行号；重复原名字拒绝选择，生成脚本符号冲突及非classic v1仅只读。完整JSON最多256KiB，不编辑或保存。", soundGet, {"sound_id"}, ToolPermission::Query);
    const auto soundChange = BasicSchema({{"field", TextSchema(64)}, {"expected_raw", TextSchema()},
        {"value", QJsonObject{{"type", "integer"}, {"minimum", -1}, {"maximum", std::numeric_limits<std::int32_t>::max()}}}},
        {"field", "expected_raw", "value"});
    auto soundsWrite = identity; soundsWrite.insert("expected_member_sha256", ShaSchema()); soundsWrite.insert("sound_id", TextSchema(16384));
    soundsWrite.insert("changes", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", soundChange}});
    add("sounds_update", "原子修改唯一已有classic W3S v1声音volume/fade_in/fade_out。value必须是真整数：volume=-1默认或0..127；淡入/淡出为原速率，-1默认或非负int32，不是毫秒。须原sound_id、expected_revision、成员SHA与expected_raw；重复原名字/gg_snd符号、未知布局、晚失败全部拒绝。1..32项走原Undo/密封事务及32项累计预算，返回前后值和新修订，不保存。create/delete、其它属性及导入写不支持。", soundsWrite,
        {"project_identity", "expected_revision", "expected_member_sha256", "sound_id", "changes"}, ToolPermission::Edit);
    return result;
}

QJsonObject InvokeMapDataTool(const QString& name, const QJsonObject& arguments, const MapDataToolHost& host) {
    try {
        if (!host.identity || !host.context) return Failure("ENTITY_HOST_UNAVAILABLE", "实体宿主上下文未接通。");
        const auto definitions = MapDataToolDefinitions();
        const auto definition = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) { return value.name == name; });
        if (definition == definitions.end()) return Failure("ENTITY_OPERATION_UNSUPPORTED", "没有此实体工具。");
        if (static_cast<std::size_t>(QJsonDocument(arguments).toJson(QJsonDocument::Compact).size()) > kRequestBytes)
            return Failure("ENTITY_REQUEST_BUDGET", "实体请求超过 1 MiB 字节预算。");
        Keys(arguments, definition->inputSchema.value("properties").toObject(), definition->inputSchema.value("required").toArray());
        const bool write = definition->permission == ToolPermission::Edit;
        const auto identity = Identity(host);
        if (write || arguments.contains("project_identity") || arguments.contains("expected_revision")) {
            if (!arguments.value("project_identity").isString() || !arguments.value("expected_revision").isString()
                || arguments.value("project_identity") != identity.value("project_identity")
                || arguments.value("expected_revision") != identity.value("revision"))
                return Failure("ENTITY_REVISION_CONFLICT", "工程或实体修订已变化，请重新查询。");
        }
        const bool settingsTool = name == "map_settings_get" || name == "map_settings_set";
        const bool stringsTool = name == "strings_query" || name == "strings_set";
        const bool soundTool = name == "sounds_list" || name == "sounds_get" || name == "sounds_update";
        const auto kind = settingsTool ? MapDataKind::Settings : stringsTool ? MapDataKind::Strings : soundTool ? MapDataKind::Sounds : Kind(arguments);
        if (write && (settingsTool || stringsTool)) {
            const auto mode = Text(arguments, "write_mode", 32);
            if (settingsTool ? mode != "literal" && mode != "shared_string" && mode != "mixed" : mode != "shared_string")
                return Failure("MAP_TEXT_WRITE_MODE", "须显式确认literal、shared_string或mixed写入语义。");
            if (settingsTool && Text(arguments, "setting_id", 16) != "w3i")
                return Failure("MAP_SETTING_ID_INVALID", "地图设置的稳定目标必须为w3i。");
        }
        const auto contextResult = host.context();
        if (!contextResult.Succeeded() || !contextResult.value) return Failure(contextResult);
        const auto& context = *contextResult.value;
        if (!context.document || (write && !context.original)) return Failure("ENTITY_HOST_UNAVAILABLE", "当前 typed 文档或原来源不可用。");
        auto snapshot = CaptureMapDataToolSnapshot(context, context.document);
        if (!snapshot.Succeeded() || !snapshot.value) return Failure(snapshot);
        const auto described = DescribeMapData(*snapshot.value, context.binding);
        if (!described.Succeeded() || !described.value) return Failure(described);
        const auto& member = described.value->members[static_cast<std::size_t>(kind)];
        const auto memberSha = member.memberSha256 ? *member.memberSha256 : std::string("missing");
        if (arguments.contains("expected_member_sha256")) {
            const auto expected = Text(arguments, "expected_member_sha256", 64);
            if (!(Sha(expected) || (!write && expected == "missing"))) return Failure("ENTITY_MEMBER_SHA_INVALID", "成员身份须为小写SHA或查询用missing。");
            if (expected != memberSha) return Failure("ENTITY_MEMBER_STALE", "成员 SHA 或 presence 已变化，请重新查询。");
        }
        if (!write && soundTool) {
            auto result = SoundRead(name, arguments, definition->inputSchema, context, *snapshot.value, member, identity);
            if (!SameIdentity(identity, Identity(host))) return Failure("ENTITY_REVISION_CONFLICT", "声音查询期间来源修订变化。");
            return result;
        }
        if (!write) {
            std::uint64_t start = 0; std::size_t limit = 64;
            QJsonObject pageInput;
            if (arguments.contains("page")) {
                if (!arguments.value("page").isObject()) throw EntityJsonError("ENTITY_ARGUMENT_TYPE", "page 必须为对象。");
                const auto page = arguments.value("page").toObject();
                pageInput = page;
                Keys(page, definition->inputSchema.value("properties").toObject().value("page").toObject().value("properties").toObject());
                if (page.contains("start_index")) start = Index(page, "start_index");
                if (page.contains("limit")) {
                    const auto parsed = ParseInt32(page, "limit");
                    if (!parsed.Succeeded() || !parsed.value || *parsed.value < 1 || *parsed.value > 128)
                        return Failure("ENTITY_PAGE_LIMIT", "页长须为1..128整数。");
                    limit = static_cast<std::size_t>(*parsed.value);
                }
            }
            if (stringsTool && pageInput.contains("cursor")) {
                if (pageInput.contains("start_index") || !pageInput.value("cursor").isObject())
                    return Failure("ENTITY_QUERY_CURSOR_INVALID", "cursor须为对象，且不能同时提供start_index。");
                const auto cursor = pageInput.value("cursor").toObject();
                const auto schema = definition->inputSchema.value("properties").toObject().value("page").toObject()
                    .value("properties").toObject().value("cursor").toObject();
                Keys(cursor, schema.value("properties").toObject(), schema.value("required").toArray());
                if (!cursor.value("binding").isObject() || cursor.value("binding").toObject() != BindingJson(context.binding)
                    || cursor.value("kind") != "strings" || cursor.value("member_sha256") != QString::fromStdString(memberSha))
                    return Failure("ENTITY_QUERY_CURSOR_STALE", "WTS续页的来源修订、成员SHA或类别已变化。");
                start = Index(cursor, "next_index");
            }
            if (arguments.contains("filter") || (pageInput.contains("cursor") && !stringsTool)) {
                if (arguments.contains("filter") && !arguments.value("filter").isObject())
                    return Failure("ENTITY_FILTER_INVALID", "filter须为对象。");
                const auto filter = EntityFilter(arguments.value("filter").toObject());
                if ((filter.player && kind != MapDataKind::Units)
                    || (filter.typeIdBytes && kind != MapDataKind::Units && kind != MapDataKind::Doodads))
                    return Failure("ENTITY_FILTER_KIND", "player仅适用于units，type_id仅适用于units/doodads。");
                std::optional<MapDataEntityQueryCursor> cursor;
                if (pageInput.contains("cursor")) {
                    if (pageInput.contains("start_index") || !pageInput.value("cursor").isObject())
                        return Failure("ENTITY_QUERY_CURSOR_INVALID", "cursor须为对象，且不能同时提供start_index。");
                    const auto raw = pageInput.value("cursor").toObject();
                    const auto schema = EntityCursorSchema();
                    Keys(raw, schema.value("properties").toObject(), schema.value("required").toArray());
                    if (!raw.value("binding").isObject() || !raw.value("filter").isObject()
                        || !raw.value("kind").isString() || !raw.value("member_sha256").isString())
                        return Failure("ENTITY_QUERY_CURSOR_INVALID", "cursor身份和filter类型无效。");
                    const auto cursorFilter = EntityFilter(raw.value("filter").toObject());
                    if (raw.value("binding").toObject() != BindingJson(context.binding)
                        || raw.value("kind") != QString::fromStdString(std::string(MapDataKindId(kind)))
                        || raw.value("member_sha256") != QString::fromStdString(memberSha) || cursorFilter != filter)
                        return Failure("ENTITY_QUERY_CURSOR_STALE", "cursor的文档修订、成员SHA/presence或筛选条件已变化。");
                    cursor = MapDataEntityQueryCursor{{context.binding, kind, Index(raw, "next_index")}, filter, member.memberSha256};
                } else cursor = MapDataEntityQueryCursor{{context.binding, kind, start}, filter, member.memberSha256};
                auto page = QueryMapDataEntitiesPage(*snapshot.value, context.binding, kind, filter, cursor, limit, 4096,
                    kResponseBytes - kIdentityBytes);
                if (!page.Succeeded() || !page.value) {
                    auto failure = Failure(page); failure.insert("member", MemberJson(member)); return Stamp(std::move(failure), identity);
                }
                QJsonArray rows;
                for (const auto& row : page.value->rows) rows.append(EntityRowJson(row, *context.document, kind));
                auto body = Scope(context); body.insert("ok", true); body.insert("schema_version", 1);
                body.insert("binding", BindingJson(context.binding)); body.insert("member", MemberJson(page.value->member));
                body.insert("rows", rows); body.insert("filter", EntityFilterJson(filter));
                body.insert("next_cursor", page.value->next ? QJsonValue(EntityCursorJson(*page.value->next)) : QJsonValue(QJsonValue::Null));
                body.insert("matched_total", page.value->matchedTotal ? QJsonValue(QString::number(*page.value->matchedTotal)) : QJsonValue(QJsonValue::Null));
                if (!arguments.contains("filter"))
                    body.insert("total", page.value->member.recordCount ? QJsonValue(QString::number(*page.value->member.recordCount)) : QJsonValue(QJsonValue::Null));
                body.insert("matched_total_complete", page.value->matchedTotal.has_value());
                body.insert("examined", QString::number(page.value->examined));
                body.insert("scan_budget_exhausted", page.value->scanBudgetExhausted);
                body.insert("truncated", page.value->next.has_value());
                body.insert("logical_bytes", QString::number(page.value->logicalBytes));
                const auto bounded = BodyBudget(body); if (!bounded.Succeeded()) return Failure(bounded);
                if (!SameIdentity(identity, Identity(host))) return Failure("ENTITY_REVISION_CONFLICT", "查询期间实体来源修订变化。");
                return Stamp(std::move(body), identity);
            }
            auto page = QueryMapDataPage(*snapshot.value, context.binding, kind,
                MapDataQueryCursor{context.binding, kind, start}, limit, kResponseBytes - kIdentityBytes);
            if (!page.Succeeded() || !page.value) {
                auto failure = Failure(page); failure.insert("member", MemberJson(member)); return Stamp(std::move(failure), identity);
            }
            QJsonArray rows;
            for (const auto& row : page.value->rows) rows.append(EntityRowJson(row, *context.document, kind));
            QJsonValue next(QJsonValue::Null);
            if (page.value->next) next = QJsonObject{{"binding", BindingJson(page.value->next->binding)},
                {"kind", MapKindName(kind)}, {"next_index", QString::number(page.value->next->nextIndex)}};
            if (name == "entities_query" && page.value->next)
                next = EntityCursorJson({*page.value->next, {}, member.memberSha256});
            if (stringsTool && next.isObject()) {
                auto cursor = next.toObject(); cursor.insert("member_sha256", QString::fromStdString(memberSha)); next = cursor;
            }
            auto body = Scope(context); body.insert("ok", true); body.insert("schema_version", 1);
            body.insert("binding", BindingJson(context.binding)); body.insert("member", MemberJson(page.value->member));
            body.insert("rows", rows); body.insert("next_cursor", next); body.insert("logical_bytes", QString::number(page.value->logicalBytes));
            body.insert("total", page.value->member.recordCount ? QJsonValue(QString::number(*page.value->member.recordCount)) : QJsonValue(QJsonValue::Null));
            body.insert("truncated", page.value->next.has_value());
            if (settingsTool) { body.insert("setting_id", "w3i"); body.insert("read_only_sections", ReadonlySettings(*context.document)); }
            const auto bounded = BodyBudget(body); if (!bounded.Succeeded()) return Failure(bounded);
            if (!SameIdentity(identity, Identity(host))) return Failure("ENTITY_REVISION_CONFLICT", "查询期间实体来源修订变化。");
            return Stamp(std::move(body), identity);
        }
        if (!host.apply) return Failure("ENTITY_HOST_READ_ONLY", "该宿主没有开放字段写入。");
        if (!arguments.value("changes").isArray()) throw EntityJsonError("ENTITY_ARGUMENT_TYPE", "changes 必须为数组。");
        const auto changes = arguments.value("changes").toArray();
        if (changes.isEmpty() || changes.size() > static_cast<qsizetype>(kMaxMapFieldEdits))
            return Failure("MAP_FIELD_EDIT_COUNT", "同一请求须包含1..32项字段修改。");
        const auto itemSchema = definition->inputSchema.value("properties").toObject().value("changes").toObject().value("items").toObject();
        std::vector<MapFieldEdit> edits; std::set<std::uint64_t> selected;
        const auto soundRow = soundTool ? SoundRow(*context.document, Text(arguments, "sound_id", 16384)) : 0;
        for (const auto& value : changes) {
            if (!value.isObject()) throw EntityJsonError("ENTITY_ARGUMENT_TYPE", "每项字段修改必须为对象。");
            const auto change = value.toObject(); Keys(change, itemSchema.value("properties").toObject(), itemSchema.value("required").toArray());
            std::uint64_t row = 0;
            std::string rowKey;
            if (soundTool) { row = soundRow; rowKey = Text(arguments, "sound_id", 16384); }
            else if (settingsTool) rowKey = "w3i";
            else if (stringsTool) {
                const auto id = Index(change, "string_id");
                if (id > std::numeric_limits<std::uint32_t>::max()) return Failure("MAP_STRING_ID_INVALID", "string_id须为uint32十进制字符串。");
                if (!context.document->Strings()) return Failure("MAP_STRING_TABLE_UNAVAILABLE", "WTS来源未完整解析，不能写入。");
                std::size_t matches = 0;
                const auto& entries = context.document->Strings()->entries;
                for (std::size_t index = 0; index < entries.size(); ++index) if (entries[index].id == id) { row = index; ++matches; }
                if (matches != 1) return Failure(matches ? "WTS_REFERENCE_AMBIGUOUS" : "WTS_REFERENCE_MISSING",
                    "string_id必须匹配当前来源中的一个唯一既有字符串。");
                rowKey = "string:" + std::to_string(id);
            } else { row = Index(change, "source_index"); rowKey = Text(change, "row_key", 16384); }
            if (row > std::numeric_limits<std::size_t>::max()) return Failure("ENTITY_INDEX_INVALID", "行索引超过本机范围。");
            const auto fieldId = stringsTool ? std::string("value") : Text(change, "field", 64); const auto* field = FindMapField(kind, fieldId);
            if (!field) return Failure("MAP_FIELD_UNKNOWN", "字段目录中没有此字段。");
            if (!field->writable) return Failure("MAP_FIELD_READ_ONLY", "该字段尚未开放编辑，只能读取。");
            const auto raw = Text(change, "expected_raw");
            auto expected = ParseMapFieldRaw(field->type, raw);
            const auto input = soundTool ? std::string{} : Text(change, "value", settingsTool || stringsTool ? 4096 : 65536);
            // D文字值是明确的UTF-8字面量；hex:前缀不能被自然输入解析器当成原始字节。
            auto next = (settingsTool || stringsTool) && field->type == MapFieldType::Bytes
                ? Result<MapFieldValue>::Success({MapFieldType::Bytes, 0, 0, input}) : ParseMapFieldInput(*field, input);
            if (soundTool) {
                const auto parsed = ParseInt32(change, "value");
                if (!parsed.Succeeded() || !parsed.value) return Failure(parsed);
                next = Result<MapFieldValue>::Success({MapFieldType::Int32, *parsed.value, 0, {}});
            }
            if (!expected.Succeeded() || !expected.value) return Failure(expected);
            if (FormatMapFieldRaw(*expected.value) != raw) return Failure("ENTITY_RAW_INVALID", "精确前值须使用查询返回的规范raw文本。");
            if (!next.Succeeded() || !next.value) return Failure(next);
            edits.push_back({kind, static_cast<std::size_t>(row), std::move(rowKey), fieldId, *expected.value, *next.value});
            selected.insert(row);
        }
        auto plan = PrepareMapDataFields(*context.original, *context.document, Text(arguments, "expected_member_sha256", 64), edits);
        if (!plan.Succeeded() || !plan.value) return Failure(plan);
        const auto& oldStrings = plan.value->Before().Member(MapDataKind::Strings);
        const auto& newStrings = plan.value->After().Member(MapDataKind::Strings);
        const bool stringsChanged = oldStrings.sources.size() == 1 && newStrings.sources.size() == 1
            && oldStrings.sources.front().data != newStrings.sources.front().data;
        const bool settingsChanged = settingsTool && plan.value->BeforeMemberSha256() != plan.value->AfterMemberSha256();
        const QString writeMode = stringsTool ? QStringLiteral("shared_string") : stringsChanged
            ? (settingsChanged ? QStringLiteral("mixed") : QStringLiteral("shared_string")) : QStringLiteral("literal");
        if (settingsTool && QString::fromStdString(Text(arguments, "write_mode", 32)) != writeMode)
            return Failure("MAP_TEXT_WRITE_MODE", "write_mode与当前字段的实际写入目标不符；TRIGSTR正文修改影响所有同编号引用，请使用shared_string，混合W3I修改请使用mixed。");
        QJsonObject prepared; bool previewed = false;
        const MapDataPrepublishValidator validate = [&](const MapDocument& before, const MapDocument& after,
            const MapDataEditSession::EditReceipt& receipt, const QJsonObject& hostChanges) -> Status {
            if (previewed) return Status::Failure("MCP.Entities", "ENTITY_HOST_PROTOCOL", "宿主不能重复执行同一次发布门禁。");
            previewed = true;
            if (!SameIdentity(identity, Identity(host))) return Status::Failure("MCP.Entities", "ENTITY_REVISION_CONFLICT", "发布前实体修订变化。");
            if (!SameDocumentBytes(before, plan.value->Before()) || !SameDocumentBytes(after, plan.value->After())
                || !receipt.SessionIdentity() || !receipt.CommandIdentity() || receipt.AfterRevision() != receipt.BeforeRevision() + 1)
                return Status::Failure("MCP.Entities", "ENTITY_HOST_PROTOCOL", "宿主候选或原会话回执与密封字段计划不同。");
            auto beforeSnapshot = CaptureMapDataSnapshot(plan.value->OwnedBefore(), context.binding, context.hasOpaqueArchiveMembers);
            auto afterSnapshot = CaptureMapDataSnapshot(plan.value->OwnedAfter(), context.binding, context.hasOpaqueArchiveMembers);
            if (!beforeSnapshot.Succeeded() || !beforeSnapshot.value) return {beforeSnapshot.diagnostics};
            if (!afterSnapshot.Succeeded() || !afterSnapshot.value) return {afterSnapshot.diagnostics};
            auto oldRows = SelectedRows(*beforeSnapshot.value, kind, selected), newRows = SelectedRows(*afterSnapshot.value, kind, selected);
            if (!oldRows.Succeeded() || !oldRows.value) return {oldRows.diagnostics};
            if (!newRows.Succeeded() || !newRows.value) return {newRows.diagnostics};
            auto panel = hostChanges;
            if (panel.isEmpty()) {
                QJsonArray items;
                for (const auto& edit : edits) {
                    const auto* field = FindMapField(kind, edit.field);
                    const auto objectId = settingsTool ? QStringLiteral("w3i") : stringsTool
                        ? QString::number(after.Strings()->entries[edit.row].id) : soundTool ? QString::fromStdString(edit.expectedRowKey) : QString{};
                    items.append(QJsonObject{{"kind", MapKindName(kind)}, {"object_id", objectId}, {"source_index", QString::number(edit.row)},
                        {"row_key", QString::fromStdString(edit.expectedRowKey)}, {"field_id", QString::fromStdString(edit.field)},
                        {"title", QString::fromUtf8(field->label.data(), static_cast<qsizetype>(field->label.size())) + QStringLiteral(" #%1").arg(edit.row)},
                        {"target", QString::fromStdString(after.Member(kind).memberName) + QStringLiteral(" [%1].%2").arg(edit.row).arg(QString::fromStdString(edit.field))},
                        {"before", QString::fromStdString(FormatMapFieldRaw(edit.expected))}, {"after", QString::fromStdString(FormatMapFieldRaw(edit.value))}});
                }
                panel = {{"items", items}, {"changed_records", QString::number(selected.size())}};
            }
            prepared = Scope(context); prepared.insert("ok", true); prepared.insert("kind", MapKindName(kind)); prepared.insert("changed", true);
            prepared.insert("before_binding", BindingJson(context.binding)); prepared.insert("before", *oldRows.value); prepared.insert("after", *newRows.value);
            prepared.insert("applied_fields", QString::number(edits.size())); prepared.insert("changed_rows", QString::number(selected.size()));
            prepared.insert("session_identity", QString::number(receipt.SessionIdentity())); prepared.insert("command_identity", QString::number(receipt.CommandIdentity()));
            prepared.insert("session_revision_before", QString::number(receipt.BeforeRevision())); prepared.insert("session_revision_after", QString::number(receipt.AfterRevision()));
            prepared.insert("saved", false); prepared.insert("runtime_verified", false); prepared.insert("changes", panel);
            if (settingsTool || stringsTool) {
                prepared.insert("write_mode", writeMode);
                prepared.insert("changes_shared_references", stringsChanged);
                if (stringsChanged) {
                    QJsonArray ids;
                    const auto& oldEntries = before.Strings()->entries;
                    const auto& newEntries = after.Strings()->entries;
                    for (std::size_t row = 0; row < oldEntries.size(); ++row)
                        if (oldEntries[row].value != newEntries[row].value) ids.append(QString::number(oldEntries[row].id));
                    prepared.insert("shared_string_ids", ids);
                    prepared.insert("shared_reference_notice", "保留原 W3I 引用；同一 WTS 编号的其它地图字段也会读取新文字。");
                }
            }
            return BodyBudget(prepared);
        };
        const auto applied = host.apply(*plan.value, validate);
        if (!applied.Succeeded() || !applied.value) return Failure(applied);
        if (!previewed || prepared.isEmpty()) return Failure("ENTITY_HOST_PROTOCOL", "宿主未执行完整发布前回执预算门禁。");
        if (applied.value->SessionIdentity() != prepared.value("session_identity").toString().toULongLong()
            || applied.value->CommandIdentity() != prepared.value("command_identity").toString().toULongLong())
            return Failure("ENTITY_HOST_PROTOCOL", "宿主发布回执不是预检的原会话命令。");
        return Stamp(std::move(prepared), Identity(host));
    } catch (const EntityJsonError& error) {
        return Failure(error.code, error.what());
    }
}

} // namespace War3Map::Mcp
