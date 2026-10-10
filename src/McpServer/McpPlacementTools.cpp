#include "McpPlacementTools.h"
#include "McpMapDataJson.h"
#include "McpToolInternal.h"

#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace War3Map::Mcp {
namespace {
using namespace MapData;
using namespace Services;
using namespace MapDataJson;
constexpr std::size_t kBodyBytes = 252U * 1024U;
constexpr std::size_t kIdentityBytes = 4096;
class PlacementJsonError final : public std::runtime_error {
public:
    PlacementJsonError(const char* code, std::string message) : std::runtime_error(std::move(message)), code(code) {}
    const char* code;
};
void Keys(const QJsonObject& object, const QJsonObject& properties, const QJsonArray& required) {
    for (const auto& key : required) if (!object.contains(key.toString()))
        throw PlacementJsonError("ENTITY_ARGUMENT_REQUIRED", "缺少摆放参数：" + key.toString().toStdString());
    for (auto it = object.begin(); it != object.end(); ++it) if (!properties.contains(it.key()))
        throw PlacementJsonError("ENTITY_ARGUMENT_UNKNOWN", "未知摆放参数：" + it.key().toStdString());
}
std::string Text(const QJsonObject& object, const char* key, std::size_t maximum) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || value.toString().contains(QChar::Null))
        throw PlacementJsonError("ENTITY_ARGUMENT_TYPE", std::string(key) + " 须为无 NUL 字符串。");
    const auto bytes = value.toString().toUtf8();
    if (static_cast<std::size_t>(bytes.size()) > maximum)
        throw PlacementJsonError("ENTITY_ARGUMENT_BUDGET", std::string(key) + " 超出 UTF-8 字节预算。");
    return bytes.toStdString();
}
float Number(const QJsonObject& object, const char* key, double minimum, double maximum,
    bool exclusiveMaximum, const char* rangeCode) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isDouble() || !std::isfinite(value.toDouble())
        || std::abs(value.toDouble()) > std::numeric_limits<float>::max())
        throw PlacementJsonError("ENTITY_ARGUMENT_TYPE", std::string(key) + " 须为有限 float 数值。");
    // 在窄化前检查协议区间，避免负极小数和略超上限的 double 被 float 舍入掩盖。
    if (value.toDouble() < minimum || (exclusiveMaximum ? value.toDouble() >= maximum : value.toDouble() > maximum))
        throw PlacementJsonError(rangeCode, std::string(key) + " 超出公开数值区间。");
    return static_cast<float>(value.toDouble());
}
QJsonObject Identity(const PlacementToolHost& host) {
    const auto raw = host.identity(); QJsonObject result;
    for (const auto* key : {"project_identity", "revision", "transaction_id", "transaction_revision", "transaction_active",
        "document_generation", "scope", "content_scope"}) if (raw.contains(key)) result.insert(key, raw.value(key));
    if (!result.value("project_identity").isString() || !result.value("revision").isString()
        || result.value("project_identity").toString().isEmpty() || result.value("revision").toString().isEmpty()
        || static_cast<std::size_t>(QJsonDocument(result).toJson(QJsonDocument::Compact).size()) > kIdentityBytes)
        throw PlacementJsonError("ENTITY_HOST_IDENTITY", "摆放宿主须提供真实有界身份与修订。");
    return result;
}
bool SameIdentity(const QJsonObject& first, const QJsonObject& second) {
    return first.value("project_identity") == second.value("project_identity") && first.value("revision") == second.value("revision");
}
bool SameDocument(const MapDocument& first, const MapDocument& second) {
    for (std::size_t i = 0; i < first.Members().size(); ++i) {
        const auto& a = first.Members()[i]; const auto& b = second.Members()[i];
        if (a.state != b.state || a.memberName != b.memberName || a.sources.size() != b.sources.size()) return false;
        for (std::size_t j = 0; j < a.sources.size(); ++j)
            if (a.sources[j].name != b.sources[j].name || a.sources[j].data != b.sources[j].data) return false;
    }
    return true;
}
Status BodyBudget(const QJsonObject& body) {
    return static_cast<std::size_t>(QJsonDocument(body).toJson(QJsonDocument::Compact).size()) <= kBodyBytes
        ? Status::Ok() : Status::Failure("MCP.Placements", "ENTITY_RESPONSE_BUDGET", "完整摆放回执超过 256KiB，候选未发布。");
}
MapDataPlacementSource Source(const QJsonObject& object, const MapDocument& document, MapDataKind kind, const std::string& sha) {
    Keys(object, {{"source_index", QJsonObject{}}, {"row_key", QJsonObject{}}}, {"source_index", "row_key"});
    const auto raw = Text(object, "source_index", 20);
    auto parsed = ParseUnsignedId(object, "source_index", true);
    if (!parsed.Succeeded() || !parsed.value || std::to_string(*parsed.value) != raw
        || *parsed.value > std::numeric_limits<std::uint32_t>::max())
        throw PlacementJsonError("ENTITY_INDEX_INVALID", "source_index 须为规范 uint32 十进制字符串。");
    const auto row = static_cast<std::uint32_t>(*parsed.value);
    const auto file = kind == MapDataKind::Units ? PlacementFile::Units : PlacementFile::Doodads;
    if (kind == MapDataKind::Doodads && document.Doodads() && row >= document.Doodads()->doodads.size()
        && static_cast<std::size_t>(row) - document.Doodads()->doodads.size() < document.Doodads()->specialDoodads.size())
        throw PlacementJsonError("PLACEMENT_SPECIAL_SOURCE_UNSUPPORTED", "特殊地物不能旋转或复制。");
    if (kind == MapDataKind::Units ? (!document.Units() || row >= document.Units()->units.size())
        : (!document.Doodads() || row >= document.Doodads()->doodads.size()))
        throw PlacementJsonError("ENTITY_SOURCE_INDEX_INVALID", "来源行不在当前普通摆放表内。");
    const auto id = kind == MapDataKind::Units ? MakePlacementId(file, row, document.Units()->units[row])
        : MakePlacementId(file, row, document.Doodads()->doodads[row]);
    return {id, Text(object, "row_key", 16384), sha};
}
Result<QJsonObject> Rows(const std::shared_ptr<const MapDocument>& document, MapDataKind kind,
    const MapDataToolContext& context, std::span<const std::uint32_t> indices) {
    const auto snapshot = CaptureMapDataToolSnapshot(context, document);
    if (!snapshot.Succeeded()) return {{}, snapshot.diagnostics};
    const auto described = DescribeMapData(*snapshot.value, context.binding);
    if (!described.Succeeded()) return {{}, described.diagnostics};
    QJsonArray rows;
    for (const auto index : indices) {
        const auto page = QueryMapDataPage(*snapshot.value, context.binding, kind,
            MapDataQueryCursor{context.binding, kind, index}, 1, kBodyBytes);
        if (!page.Succeeded()) return {{}, page.diagnostics};
        if (page.value->rows.size() != 1)
            return Result<QJsonObject>::Failure("MCP.Placements", "ENTITY_SOURCE_INDEX_INVALID", "候选摆放行缺失。");
        rows.append(RowJson(page.value->rows.front()));
    }
    return Result<QJsonObject>::Success({{"member", MemberJson(described.value->members[static_cast<std::size_t>(kind)])}, {"rows", rows}});
}
QJsonObject TextSchema(int maximum) { return {{"type", "string"}, {"maxLength", maximum}}; }
} // namespace

std::vector<ToolDefinition> PlacementToolDefinitions() {
    const auto source = BasicSchema({{"source_index", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}},
        {"row_key", TextSchema(16384)}}, {"source_index", "row_key"});
    QJsonObject common{{"project_identity", TextSchema(4096)}, {"expected_revision", TextSchema(4096)},
        {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"units", "doodads"}}}},
        {"expected_member_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}, {"maxLength", 64}}}};
    std::vector<ToolDefinition> definitions;
    const auto add = [&](const char* name, const char* description, QJsonObject properties, QJsonArray required) {
        ToolDefinition tool; tool.name = QString::fromUtf8(name); tool.description = QString::fromUtf8(description);
        tool.permission = ToolPermission::Edit; tool.inputSchema = BasicSchema(std::move(properties), std::move(required));
        tool.maxRequestIdBytes = 4096; tool.maxResponseBytes = 1024U * 1024U; definitions.push_back(std::move(tool));
    };
    auto rotate = common; rotate.insert("source", source);
    rotate.insert("facing_radians", QJsonObject{{"type", "number"}, {"minimum", 0}, {"exclusiveMaximum", 6.283185307179586}});
    add("entities_rotate", "设置一个当前单位或普通地物的绝对朝向，facing_radians为[0,2π)弧度（与C1 facing度输入不同）；XYZ不变，sloc只改朝向不改W3I。原会话Undo/独立事务，不保存。",
        rotate, {"project_identity", "expected_revision", "kind", "expected_member_sha256", "source", "facing_radians"});
    auto copy = common; copy.insert("sources", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", source}});
    const QJsonObject number{{"type", "number"}, {"minimum", -1000000}, {"maximum", 1000000}};
    copy.insert("offset", BasicSchema({{"x", number}, {"y", number}, {"z", number}}, {"x", "y", "z"}));
    add("entities_copy", "按共同游戏世界XYZ偏移复制1..32个同类单位或普通地物，完整原生记录保留；sloc与特殊地物拒绝。GUI用当前已有行与原Undo；独立事务只接受一批原始来源，不混字段/旋转/第二复制。64MiB wire预检，不保存。",
        copy, {"project_identity", "expected_revision", "kind", "expected_member_sha256", "sources", "offset"});
    return definitions;
}

QJsonObject InvokePlacementTool(const QString& name, const QJsonObject& arguments, const PlacementToolHost& host) {
    try {
        if (!host.identity || !host.context || !host.apply) return Failure("ENTITY_HOST_UNAVAILABLE", "摆放宿主未接通。");
        const auto definitions = PlacementToolDefinitions();
        const auto definition = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) { return value.name == name; });
        if (definition == definitions.end()) return Failure("ENTITY_TOOL_UNKNOWN", "未知摆放工具。");
        if (static_cast<std::size_t>(QJsonDocument(arguments).toJson(QJsonDocument::Compact).size()) > 1024U * 1024U)
            return Failure("ENTITY_ARGUMENT_BUDGET", "摆放请求超过 1MiB。");
        Keys(arguments, definition->inputSchema.value("properties").toObject(), definition->inputSchema.value("required").toArray());
        const auto identity = Identity(host);
        if (QString::fromStdString(Text(arguments, "project_identity", 4096)) != identity.value("project_identity")
            || QString::fromStdString(Text(arguments, "expected_revision", 4096)) != identity.value("revision"))
            return Failure("ENTITY_REVISION_CONFLICT", "工程或修订已变化，请重新查询当前实体。");
        auto captured = host.context();
        if (!captured.Succeeded() || !captured.value) return Failure(captured);
        const auto& context = *captured.value;
        if (!context.original || !context.document) return Failure("ENTITY_HOST_UNAVAILABLE", "宿主缺少原来源和当前类型化文档。");
        if (!SameIdentity(identity, Identity(host))) return Failure("ENTITY_REVISION_CONFLICT", "查询上下文时修订已变化。");
        const auto kindName = Text(arguments, "kind", 16);
        if (kindName != "units" && kindName != "doodads") return Failure("ENTITY_KIND_INVALID", "C2 只接受 units/doodads。");
        const auto kind = kindName == "units" ? MapDataKind::Units : MapDataKind::Doodads;
        const auto sha = Text(arguments, "expected_member_sha256", 64);
        if (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            return Failure("ENTITY_MEMBER_STALE", "expected_member_sha256 必须为当前成员的64位小写SHA。");
        MapDataPlacementIntent intent = MapDataPlacementRotation{};
        std::vector<std::uint32_t> oldIndices;
        const bool copy = name == QStringLiteral("entities_copy");
        if (copy) {
            if (!arguments.value("sources").isArray() || arguments.value("sources").toArray().isEmpty() || arguments.value("sources").toArray().size() > 32)
                return Failure("MAP_PLACEMENT_LIMIT", "复制来源须为1..32项。");
            MapDataPlacementCopy command;
            for (const auto& value : arguments.value("sources").toArray()) {
                if (!value.isObject()) return Failure("ENTITY_ARGUMENT_TYPE", "每个来源须为定位对象。");
                command.sources.push_back(Source(value.toObject(), *context.document, kind, sha));
                oldIndices.push_back(command.sources.back().id.index);
            }
            if (!arguments.value("offset").isObject()) return Failure("ENTITY_ARGUMENT_TYPE", "offset须为world XYZ对象。");
            const auto offset = arguments.value("offset").toObject();
            Keys(offset, {{"x", QJsonObject{}}, {"y", QJsonObject{}}, {"z", QJsonObject{}}}, {"x", "y", "z"});
            command.offset = {Number(offset, "x", -1000000, 1000000, false, "PLACEMENT_COORDINATE_INVALID"),
                Number(offset, "y", -1000000, 1000000, false, "PLACEMENT_COORDINATE_INVALID"),
                Number(offset, "z", -1000000, 1000000, false, "PLACEMENT_COORDINATE_INVALID")}; intent = std::move(command);
        } else {
            if (!arguments.value("source").isObject()) return Failure("ENTITY_ARGUMENT_TYPE", "source须为定位对象。");
            auto source = Source(arguments.value("source").toObject(), *context.document, kind, sha); oldIndices.push_back(source.id.index);
            intent = MapDataPlacementRotation{std::move(source),
                Number(arguments, "facing_radians", 0, 6.283185307179586, true, "MAP_PLACEMENT_FACING_INVALID")};
        }
        auto plan = PrepareMapDataPlacement(*context.original, *context.document, intent);
        if (!plan.Succeeded()) return Failure(plan);
        std::vector<std::uint32_t> newIndices;
        if (copy) for (const auto& id : plan.value->NewIds()) newIndices.push_back(id.index);
        else newIndices = oldIndices;
        QJsonObject prepared; bool previewed = false;
        const PlacementPrepublishValidator validate = [&](const MapDocument& before, const MapDocument& after,
            const MapDataEditSession::EditReceipt& receipt, const QJsonObject& hostChanges) -> Status {
            if (previewed) return Status::Failure("MCP.Placements", "ENTITY_HOST_PROTOCOL", "发布门禁不得重复执行。");
            previewed = true;
            if (!SameIdentity(identity, Identity(host))) return Status::Failure("MCP.Placements", "ENTITY_REVISION_CONFLICT", "发布前修订已变化。");
            if (!SameDocument(before, plan.value->Before()) || !SameDocument(after, plan.value->After()) || !receipt.SessionIdentity()
                || !receipt.CommandIdentity() || receipt.AfterRevision() != receipt.BeforeRevision() + 1)
                return Status::Failure("MCP.Placements", "ENTITY_HOST_PROTOCOL", "宿主候选或原会话回执与密封计划不同。");
            auto oldRows = Rows(plan.value->OwnedBefore(), kind, context, oldIndices), newRows = Rows(plan.value->OwnedAfter(), kind, context, newIndices);
            if (!oldRows.Succeeded()) return {oldRows.diagnostics};
            if (!newRows.Succeeded()) return {newRows.diagnostics};
            const auto oldArray = oldRows.value->value("rows").toArray(), newArray = newRows.value->value("rows").toArray();
            auto panel = hostChanges;
            if (panel.isEmpty()) {
                QJsonArray items;
                for (qsizetype i = 0; i < newArray.size(); ++i) items.append(QJsonObject{{"kind", MapKindName(kind)}, {"object_id", ""},
                    {"source_index", QString::number(newIndices[static_cast<std::size_t>(i)])},
                    {"row_key", newArray[i].toObject().value("row_key")}, {"field", copy ? "record" : "facing"},
                    {"title", copy ? "复制摆放" : "绝对朝向（弧度）"}, {"before", oldArray[i]}, {"after", newArray[i]},
                    {"target", QString::fromStdString(after.Member(kind).memberName) + QStringLiteral(" [%1]").arg(newIndices[static_cast<std::size_t>(i)])}});
                panel = {{"title", copy ? "复制预放实例" : "设置预放朝向"}, {"items", items}};
            }
            prepared = {{"ok", true}, {"kind", MapKindName(kind)}, {"operation", copy ? "copy" : "rotate"}, {"changed", true},
                {"before_binding", BindingJson(context.binding)}, {"before", *oldRows.value}, {"after", *newRows.value},
                {"applied_placements", QString::number(oldIndices.size())}, {"new_ids", QJsonArray{}},
                {"session_identity", QString::number(receipt.SessionIdentity())}, {"command_identity", QString::number(receipt.CommandIdentity())},
                {"session_revision_before", QString::number(receipt.BeforeRevision())}, {"session_revision_after", QString::number(receipt.AfterRevision())},
                {"includes_active_transaction_overlay", context.includesActiveTransactionOverlay}, {"includes_unsaved_ui_revision", context.includesUnsavedUiRevision},
                {"locator_scope", "snapshot-revision"}, {"position_unit", "game-world"}, {"rotation_unit", "radians"},
                {"field_display_angle_unit", "degrees"},
                {"saved", false}, {"runtime_verified", false}, {"changes", panel}};
            QJsonArray ids;
            for (qsizetype i = 0; i < newArray.size() && copy; ++i) ids.append(QJsonObject{{"kind", MapKindName(kind)},
                {"source_index", newArray[i].toObject().value("source_index")}, {"row_key", newArray[i].toObject().value("row_key")},
                {"member_sha256", QString::fromStdString(plan.value->AfterMemberSha256())}});
            prepared["new_ids"] = ids;
            if (!copy) prepared["requested_facing_radians"] = std::get<MapDataPlacementRotation>(intent).facingRadians;
            return BodyBudget(prepared);
        };
        const auto applied = host.apply(*plan.value, validate);
        if (!applied.Succeeded()) return Failure(applied);
        if (!previewed || !applied.value || prepared.isEmpty()
            || applied.value->SessionIdentity() != prepared.value("session_identity").toString().toULongLong()
            || applied.value->CommandIdentity() != prepared.value("command_identity").toString().toULongLong())
            return Failure("ENTITY_HOST_PROTOCOL", "宿主未发布同一原会话预检命令。");
        const auto afterIdentity = Identity(host);
        for (auto it = afterIdentity.begin(); it != afterIdentity.end(); ++it) prepared.insert(it.key(), it.value());
        return prepared;
    } catch (const PlacementJsonError& error) { return Failure(error.code, error.what()); }
    catch (const std::bad_alloc&) { return Failure("ENTITY_ALLOCATION_FAILED", "摆放参数或回执内存不足，未完成发布。"); }
    catch (const std::length_error&) { return Failure("ENTITY_ALLOCATION_FAILED", "摆放参数或回执超过容器容量。"); }
}

} // namespace War3Map::Mcp
