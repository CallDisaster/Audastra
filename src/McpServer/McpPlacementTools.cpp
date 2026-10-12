#include "McpPlacementTools.h"
#include "McpMapDataJson.h"
#include "McpToolInternal.h"

#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
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
double BoundedNumber(const QJsonObject& object, const char* key, double minimum, double maximum,
    bool exclusiveMaximum, const char* rangeCode) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isDouble() || !std::isfinite(value.toDouble())
        || std::abs(value.toDouble()) > std::numeric_limits<float>::max())
        throw PlacementJsonError("ENTITY_ARGUMENT_TYPE", std::string(key) + " 须为有限 float 数值。");
    // 在窄化前检查协议区间，避免负极小数和略超上限的 double 被 float 舍入掩盖。
    if (value.toDouble() < minimum || (exclusiveMaximum ? value.toDouble() >= maximum : value.toDouble() > maximum))
        throw PlacementJsonError(rangeCode, std::string(key) + " 超出公开数值区间。");
    return value.toDouble();
}
float Number(const QJsonObject& object, const char* key, double minimum, double maximum,
    bool exclusiveMaximum, const char* rangeCode) {
    return static_cast<float>(BoundedNumber(object, key, minimum, maximum, exclusiveMaximum, rangeCode));
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
        throw PlacementJsonError("PLACEMENT_SPECIAL_SOURCE_UNSUPPORTED", "特殊地物尚不支持此编辑命令。");
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
QJsonObject IntegerSchema(int minimum, int maximum) { return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}}; }
int Integer(const QJsonObject& object, const char* key, int minimum, int maximum) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::floor(value.toDouble()) != value.toDouble()
        || value.toDouble() < minimum || value.toDouble() > maximum)
        throw PlacementJsonError("ENTITY_ARGUMENT_TYPE", std::string(key) + " 须为指定范围内的整数。");
    return static_cast<int>(value.toDouble());
}
QJsonObject CreationSchema() {
    const QJsonObject number{{"type", "number"}, {"minimum", -1000000}, {"maximum", 1000000}};
    const QJsonObject scale{{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1000}};
    const auto item = BasicSchema({{"item_id", TextSchema(4)}, {"chance_percent", IntegerSchema(0, 100)}}, {"item_id", "chance_percent"});
    const auto set = BasicSchema({{"items", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", item}}}}, {"items"});
    return BasicSchema({{"object_kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"unit", "item", "doodad", "destructable"}}}},
        {"object_id", TextSchema(4)}, {"x", number}, {"y", number}, {"height_offset", number},
        {"facing_radians", QJsonObject{{"type", "number"}, {"minimum", 0}, {"exclusiveMaximum", 6.283185307179586}}},
        {"scale", BasicSchema({{"x", scale}, {"y", scale}, {"z", scale}}, {"x", "y", "z"})},
        {"variation", IntegerSchema(0, std::numeric_limits<int>::max())}, {"owner", IntegerSchema(0, 15)},
        {"hero_level", IntegerSchema(1, std::numeric_limits<int>::max())}, {"life_percent", IntegerSchema(-1, 100)},
        {"mana", IntegerSchema(-1, std::numeric_limits<int>::max())},
        {"item_sets", QJsonObject{{"type", "array"}, {"maxItems", 32}, {"items", set}}}},
        {"object_kind", "object_id", "x", "y"});
}
PlacementCreateRequest Creation(const QJsonObject& object) {
    const auto schema = CreationSchema(); Keys(object, schema.value("properties").toObject(), schema.value("required").toArray());
    PlacementCreateRequest request;
    const auto kind = Text(object, "object_kind", 16);
    if (kind == "unit") request.type = ObjectData::ObjectType::Unit;
    else if (kind == "item") request.type = ObjectData::ObjectType::Item;
    else if (kind == "doodad") request.type = ObjectData::ObjectType::Doodad;
    else if (kind == "destructable") request.type = ObjectData::ObjectType::Destructable;
    else throw PlacementJsonError("PLACEMENT_OBJECT_KIND", "object_kind 须为 unit/item/doodad/destructable。");
    request.rawcode = Text(object, "object_id", 4);
    // 地图边界由地形宿主检查原 double，不能先舍入为 float 后把略越界请求放回边界内。
    request.point = {BoundedNumber(object, "x", -1000000, 1000000, false, "PLACEMENT_COORDINATE_INVALID"),
        BoundedNumber(object, "y", -1000000, 1000000, false, "PLACEMENT_COORDINATE_INVALID")};
    if (object.contains("height_offset")) request.heightOffset = Number(object, "height_offset", -1000000, 1000000, false, "PLACEMENT_COORDINATE_INVALID");
    if (object.contains("facing_radians")) request.facingRadians = Number(object, "facing_radians", 0, 6.283185307179586, true, "MAP_PLACEMENT_FACING_INVALID");
    if (object.contains("scale")) {
        if (!object.value("scale").isObject()) throw PlacementJsonError("ENTITY_ARGUMENT_TYPE", "scale 须为 XYZ 对象。");
        const auto scale = object.value("scale").toObject();
        Keys(scale, {{"x", QJsonObject{}}, {"y", QJsonObject{}}, {"z", QJsonObject{}}}, {"x", "y", "z"});
        request.scale = {Number(scale, "x", 0, 1000, false, "PLACEMENT_TRANSFORM_INVALID"),
            Number(scale, "y", 0, 1000, false, "PLACEMENT_TRANSFORM_INVALID"), Number(scale, "z", 0, 1000, false, "PLACEMENT_TRANSFORM_INVALID")};
    }
    if (object.contains("variation")) request.variation = Integer(object, "variation", 0, std::numeric_limits<int>::max());
    if (object.contains("owner")) request.owner = Integer(object, "owner", 0, 15);
    if (object.contains("hero_level")) request.heroLevel = Integer(object, "hero_level", 1, std::numeric_limits<int>::max());
    if (object.contains("life_percent")) request.hitPointsPercent = Integer(object, "life_percent", -1, 100);
    if (object.contains("mana")) request.mana = Integer(object, "mana", -1, std::numeric_limits<int>::max());
    if (object.contains("item_sets")) {
        if (!object.value("item_sets").isArray() || object.value("item_sets").toArray().size() > 32)
            throw PlacementJsonError("PLACEMENT_DROPS_INVALID", "item_sets 须为最多 32 组的数组。");
        for (const auto& value : object.value("item_sets").toArray()) {
            if (!value.isObject()) throw PlacementJsonError("ENTITY_ARGUMENT_TYPE", "掉落组须为 items 对象。");
            const auto group = value.toObject(); Keys(group, {{"items", QJsonObject{}}}, {"items"});
            if (!group.value("items").isArray() || group.value("items").toArray().isEmpty() || group.value("items").toArray().size() > 32)
                throw PlacementJsonError("PLACEMENT_DROPS_INVALID", "每组 items 须为 1..32 项数组。");
            PlacementItemSet set;
            for (const auto& choice : group.value("items").toArray()) {
                if (!choice.isObject()) throw PlacementJsonError("ENTITY_ARGUMENT_TYPE", "掉落物品须为 item_id/chance_percent 对象。");
                const auto item = choice.toObject(); Keys(item, {{"item_id", QJsonObject{}}, {"chance_percent", QJsonObject{}}}, {"item_id", "chance_percent"});
                set.items.push_back({Text(item, "item_id", 4), Integer(item, "chance_percent", 0, 100)});
            }
            request.itemSets.push_back(std::move(set));
        }
    }
    return request;
}
} // namespace

std::vector<ToolDefinition> PlacementToolDefinitions() {
    const auto source = BasicSchema({{"source_index", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}},
        {"row_key", TextSchema(16384)}}, {"source_index", "row_key"});
    QJsonObject common{{"project_identity", TextSchema(4096)}, {"expected_revision", TextSchema(4096)},
        {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"units", "doodads"}}}},
        {"expected_member_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}, {"maxLength", 64}}},
        {"dry_run", QJsonObject{{"type", "boolean"}}}};
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
    auto create = common; create.insert("placements", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", CreationSchema()}});
    add("entities_create", "按实际物编ID新建1..32个实例；units容纳unit/item，doodads容纳doodad/destructable。同界面校验类型、父链和地图边界；height_offset相对当前地面，mana沿现有绝对值协议。内联掉落需真实物品ID。dry_run仅预览，实际编辑返回新行ID；独立事务只接受一批同类操作，不保存。",
        create, {"project_identity", "expected_revision", "kind", "expected_member_sha256", "placements"});
    auto remove = common; remove.insert("sources", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", source}});
    add("entities_delete", "按当前成员SHA和完整行身份删除1..32个实例，单次事务保留原Undo；拒绝sloc和特殊地物。dry_run仅预览，不消耗命令身份，不保存。独立事务只接受一批原始来源操作。",
        remove, {"project_identity", "expected_revision", "kind", "expected_member_sha256", "sources"});
    return definitions;
}

QJsonObject InvokePlacementTool(const QString& name, const QJsonObject& arguments, const PlacementToolHost& host) {
    try {
        if (!host.identity || !host.context) return Failure("ENTITY_HOST_UNAVAILABLE", "摆放宿主未接通。");
        const auto definitions = PlacementToolDefinitions();
        const auto definition = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) { return value.name == name; });
        if (definition == definitions.end()) return Failure("ENTITY_TOOL_UNKNOWN", "未知摆放工具。");
        if (static_cast<std::size_t>(QJsonDocument(arguments).toJson(QJsonDocument::Compact).size()) > 1024U * 1024U)
            return Failure("ENTITY_ARGUMENT_BUDGET", "摆放请求超过 1MiB。");
        Keys(arguments, definition->inputSchema.value("properties").toObject(), definition->inputSchema.value("required").toArray());
        if (arguments.contains("dry_run") && !arguments.value("dry_run").isBool())
            return Failure("ENTITY_ARGUMENT_TYPE", "dry_run 须为布尔值。");
        const bool dryRun = arguments.value("dry_run").toBool(false);
        if (!dryRun && !host.apply) return Failure("ENTITY_HOST_UNAVAILABLE", "摆放编辑宿主未接通。");
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
        if (kindName != "units" && kindName != "doodads") return Failure("ENTITY_KIND_INVALID", "摆放命令只接受 units/doodads。");
        const auto kind = kindName == "units" ? MapDataKind::Units : MapDataKind::Doodads;
        const auto sha = Text(arguments, "expected_member_sha256", 64);
        if (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            return Failure("ENTITY_MEMBER_STALE", "expected_member_sha256 必须为当前成员的64位小写SHA。");
        MapDataPlacementIntent intent = MapDataPlacementRotation{};
        std::vector<std::uint32_t> oldIndices;
        const bool copy = name == QStringLiteral("entities_copy");
        const bool create = name == QStringLiteral("entities_create"), remove = name == QStringLiteral("entities_delete");
        if (create) {
            if (!host.objectContext || !host.terrainPoint)
                return Failure("ENTITY_CREATE_HOST_UNAVAILABLE", "新建需要实际物编目录与当前地形采样，不能猜测来源。");
            if (!arguments.value("placements").isArray() || arguments.value("placements").toArray().isEmpty()
                || arguments.value("placements").toArray().size() > 32)
                return Failure("MAP_PLACEMENT_LIMIT", "新建须为1..32项。");
            MapDataPlacementCreate command; command.expectedMemberSha256 = sha;
            if (kind == MapDataKind::Units) command.records = std::vector<UnitPlacement>{};
            else command.records = std::vector<DoodadPlacement>{};
            std::array<std::optional<ObjectCommandContext>, 7> sources;
            const auto objectContext = [&](ObjectData::ObjectType type) -> Result<ObjectCommandContext> {
                auto& cached = sources[static_cast<std::size_t>(type)];
                if (!cached) {
                    const auto source = host.objectContext(type);
                    if (!source.Succeeded()) return source;
                    cached = *source.value;
                }
                return Result<ObjectCommandContext>::Success(*cached);
            };
            for (const auto& value : arguments.value("placements").toArray()) {
                if (!value.isObject()) return Failure("ENTITY_ARGUMENT_TYPE", "每个新建项须为对象。");
                const auto request = Creation(value.toObject());
                const bool unitFile = request.type == ObjectData::ObjectType::Unit || request.type == ObjectData::ObjectType::Item;
                if (unitFile != (kind == MapDataKind::Units)) return Failure("ENTITY_KIND_INVALID", "一批新建只能写同一个原生成员。");
                const auto source = objectContext(request.type);
                if (!source.Succeeded()) return Failure(source);
                const auto terrain = host.terrainPoint(request.point);
                if (!terrain.Succeeded()) return Failure(terrain);
                std::optional<ObjectCommandContext> drops;
                if (!request.itemSets.empty()) {
                    const auto items = objectContext(ObjectData::ObjectType::Item);
                    if (!items.Succeeded()) return Failure(items);
                    drops = *items.value;
                }
                auto record = PreparePlacementCreation(request, *source.value, *terrain.value, drops ? &*drops : nullptr);
                if (!record.Succeeded()) return Failure(record);
                if (unitFile) std::get<std::vector<UnitPlacement>>(command.records).push_back(std::get<UnitPlacement>(std::move(*record.value)));
                else std::get<std::vector<DoodadPlacement>>(command.records).push_back(std::get<DoodadPlacement>(std::move(*record.value)));
            }
            intent = std::move(command);
        } else if (copy || remove) {
            if (!arguments.value("sources").isArray() || arguments.value("sources").toArray().isEmpty() || arguments.value("sources").toArray().size() > 32)
                return Failure("MAP_PLACEMENT_LIMIT", "编辑来源须为1..32项。");
            MapDataPlacementCopy command;
            for (const auto& value : arguments.value("sources").toArray()) {
                if (!value.isObject()) return Failure("ENTITY_ARGUMENT_TYPE", "每个来源须为定位对象。");
                command.sources.push_back(Source(value.toObject(), *context.document, kind, sha));
                oldIndices.push_back(command.sources.back().id.index);
            }
            if (remove) intent = MapDataPlacementDelete{std::move(command.sources)};
            else {
                if (!arguments.value("offset").isObject()) return Failure("ENTITY_ARGUMENT_TYPE", "offset须为world XYZ对象。");
                const auto offset = arguments.value("offset").toObject();
                Keys(offset, {{"x", QJsonObject{}}, {"y", QJsonObject{}}, {"z", QJsonObject{}}}, {"x", "y", "z"});
                command.offset = {Number(offset, "x", -1000000, 1000000, false, "PLACEMENT_COORDINATE_INVALID"),
                    Number(offset, "y", -1000000, 1000000, false, "PLACEMENT_COORDINATE_INVALID"),
                    Number(offset, "z", -1000000, 1000000, false, "PLACEMENT_COORDINATE_INVALID")}; intent = std::move(command);
            }
        } else {
            if (!arguments.value("source").isObject()) return Failure("ENTITY_ARGUMENT_TYPE", "source须为定位对象。");
            auto source = Source(arguments.value("source").toObject(), *context.document, kind, sha); oldIndices.push_back(source.id.index);
            intent = MapDataPlacementRotation{std::move(source),
                Number(arguments, "facing_radians", 0, 6.283185307179586, true, "MAP_PLACEMENT_FACING_INVALID")};
        }
        if (!SameIdentity(identity, Identity(host))) return Failure("ENTITY_REVISION_CONFLICT", "类型或地形校验期间修订已变化。");
        auto plan = PrepareMapDataPlacement(*context.original, *context.document, intent);
        if (!plan.Succeeded()) return Failure(plan);
        std::vector<std::uint32_t> newIndices;
        if (copy || create) for (const auto& id : plan.value->NewIds()) newIndices.push_back(id.index);
        else if (!remove) newIndices = oldIndices;
        QJsonObject prepared; bool previewed = false;
        const char* operation = create ? "create" : remove ? "delete" : copy ? "copy" : "rotate";
        const char* title = create ? "新建摆放" : remove ? "删除摆放" : copy ? "复制摆放" : "绝对朝向（弧度）";
        const auto response = [&](const MapDataEditSession::EditReceipt* receipt, const QJsonObject& hostChanges) -> Status {
            auto oldRows = Rows(plan.value->OwnedBefore(), kind, context, oldIndices), newRows = Rows(plan.value->OwnedAfter(), kind, context, newIndices);
            if (!oldRows.Succeeded()) return {oldRows.diagnostics};
            if (!newRows.Succeeded()) return {newRows.diagnostics};
            const auto oldArray = oldRows.value->value("rows").toArray(), newArray = newRows.value->value("rows").toArray();
            auto panel = hostChanges;
            const auto count = remove ? oldIndices.size() : newIndices.size();
            if (panel.isEmpty()) {
                QJsonArray items;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto oldRow = create ? QJsonValue(QJsonValue::Null) : QJsonValue(oldArray[static_cast<qsizetype>(i)]);
                    const auto newRow = remove ? QJsonValue(QJsonValue::Null) : QJsonValue(newArray[static_cast<qsizetype>(i)]);
                    const auto index = remove ? oldIndices[i] : newIndices[i];
                    items.append(QJsonObject{{"kind", MapKindName(kind)}, {"object_id", ""}, {"source_index", QString::number(index)},
                        {"row_key", (remove ? oldRow : newRow).toObject().value("row_key")}, {"field", copy || create || remove ? "record" : "facing"},
                        {"title", QString::fromUtf8(title)}, {"before", oldRow}, {"after", newRow},
                        {"target", QString::fromStdString(plan.value->After().Member(kind).memberName) + QStringLiteral(" [%1]").arg(index)}});
                }
                panel = {{"title", QString::fromUtf8(title)}, {"items", items}};
            }
            prepared = {{"ok", true}, {"kind", MapKindName(kind)}, {"operation", operation}, {"changed", !dryRun},
                {"dry_run", dryRun}, {"applied", !dryRun}, {"planned_placements", QString::number(count)},
                {"before_binding", BindingJson(context.binding)}, {"before", *oldRows.value}, {"after", *newRows.value},
                {"applied_placements", QString::number(dryRun ? 0 : count)}, {"new_ids", QJsonArray{}},
                {"includes_active_transaction_overlay", context.includesActiveTransactionOverlay}, {"includes_unsaved_ui_revision", context.includesUnsavedUiRevision},
                {"locator_scope", "snapshot-revision"}, {"position_unit", "game-world"}, {"rotation_unit", "radians"},
                {"field_display_angle_unit", "degrees"},
                {"saved", false}, {"runtime_verified", false}, {"changes", panel}};
            QJsonArray ids;
            for (qsizetype i = 0; i < newArray.size() && (copy || create); ++i) ids.append(QJsonObject{{"kind", MapKindName(kind)},
                {"source_index", newArray[i].toObject().value("source_index")}, {"row_key", newArray[i].toObject().value("row_key")},
                {"creation_id", QString::number(plan.value->NewIds()[static_cast<std::size_t>(i)].creationId)},
                {"member_sha256", QString::fromStdString(plan.value->AfterMemberSha256())}});
            prepared[dryRun ? "candidate_ids" : "new_ids"] = ids;
            if (!copy && !create && !remove) prepared["requested_facing_radians"] = std::get<MapDataPlacementRotation>(intent).facingRadians;
            if (receipt) {
                prepared["session_identity"] = QString::number(receipt->SessionIdentity());
                prepared["command_identity"] = QString::number(receipt->CommandIdentity());
                prepared["session_revision_before"] = QString::number(receipt->BeforeRevision());
                prepared["session_revision_after"] = QString::number(receipt->AfterRevision());
            }
            return BodyBudget(prepared);
        };
        if (dryRun) {
            const auto preview = response(nullptr, {});
            if (!preview.Succeeded()) return Failure(preview);
            const auto currentIdentity = Identity(host);
            if (!SameIdentity(identity, currentIdentity)) return Failure("ENTITY_REVISION_CONFLICT", "预览期间修订已变化。");
            for (auto it = currentIdentity.begin(); it != currentIdentity.end(); ++it) prepared.insert(it.key(), it.value());
            return prepared;
        }
        const PlacementPrepublishValidator validate = [&](const MapDocument& before, const MapDocument& after,
            const MapDataEditSession::EditReceipt& receipt, const QJsonObject& hostChanges) -> Status {
            if (previewed) return Status::Failure("MCP.Placements", "ENTITY_HOST_PROTOCOL", "发布门禁不得重复执行。");
            previewed = true;
            if (!SameIdentity(identity, Identity(host))) return Status::Failure("MCP.Placements", "ENTITY_REVISION_CONFLICT", "发布前修订已变化。");
            if (!SameDocument(before, plan.value->Before()) || !SameDocument(after, plan.value->After()) || !receipt.SessionIdentity()
                || !receipt.CommandIdentity() || receipt.AfterRevision() != receipt.BeforeRevision() + 1)
                return Status::Failure("MCP.Placements", "ENTITY_HOST_PROTOCOL", "宿主候选或原会话回执与密封计划不同。");
            return response(&receipt, hostChanges);
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
