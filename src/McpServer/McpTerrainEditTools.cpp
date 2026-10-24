#include "McpTerrainEditTools.h"
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
constexpr std::size_t kRequestBytes = 1024 * 1024, kBodyBytes = 256 * 1024;

class ArgumentError final : public std::runtime_error {
public:
    ArgumentError(const char* code, const char* message) : std::runtime_error(message), code(code) {}
    const char* code;
};

QJsonObject TextSchema(int maximum) { return {{"type", "string"}, {"maxLength", maximum}}; }
QJsonObject IntegerSchema(double minimum, double maximum) {
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}
QJsonObject DecimalIdSchema() {
    return {{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}};
}
void Keys(const QJsonObject& object, const QJsonObject& properties, const QJsonArray& required = {}) {
    for (const auto& key : required) if (!object.contains(key.toString()))
        throw ArgumentError("TERRAIN_ARGUMENT_REQUIRED", "地形请求缺少必需参数。");
    for (auto it = object.begin(); it != object.end(); ++it) if (!properties.contains(it.key()))
        throw ArgumentError("TERRAIN_ARGUMENT_UNKNOWN", "地形请求含有未知参数或当前笔刷不使用的参数。");
}
double Number(const QJsonObject& object, const char* key, double minimum, double maximum, bool integer = false) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < minimum || value.toDouble() > maximum
        || (integer && std::trunc(value.toDouble()) != value.toDouble()))
        throw ArgumentError("TERRAIN_ARGUMENT_NUMBER", "地形数值不是允许范围内的有限数或精确整数。");
    return value.toDouble();
}
bool Boolean(const QJsonObject& object, const char* key, bool fallback) {
    if (!object.contains(QLatin1String(key))) return fallback;
    if (!object.value(QLatin1String(key)).isBool())
        throw ArgumentError("TERRAIN_ARGUMENT_TYPE", "地形开关必须为布尔值。");
    return object.value(QLatin1String(key)).toBool();
}
QString Text(const QJsonObject& object, const char* key, qsizetype maximum) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || value.toString().contains(QChar::Null) || value.toString().toUtf8().size() > maximum
        || QString::fromUtf8(value.toString().toUtf8()) != value.toString())
        throw ArgumentError("TERRAIN_ARGUMENT_TEXT", "地形文字必须为预算内的有效 UTF-8 字符串。");
    return value.toString();
}
TerrainEditBinding Binding(const TerrainEditToolContext& context) {
    return {context.document, context.session->SessionIdentity(), context.session->Revision(), context.session->TerrainSourceRevision()};
}
QJsonObject Identity(const TerrainEditToolHost& host) {
    if (!host.identity) throw ArgumentError("TERRAIN_HOST_IDENTITY", "地形宿主未提供工程身份。");
    const auto source = host.identity(); QJsonObject identity;
    for (const auto* key : {"project_identity", "revision", "transaction_id", "transaction_revision", "transaction_active"})
        if (source.contains(key)) identity.insert(key, source.value(key));
    if (!identity.value("project_identity").isString() || identity.value("project_identity").toString().isEmpty()
        || !identity.value("revision").isString() || identity.value("revision").toString().isEmpty()
        || QJsonDocument(identity).toJson(QJsonDocument::Compact).size() > 4096)
        throw ArgumentError("TERRAIN_HOST_IDENTITY", "地形宿主工程身份或修订无效、超预算。");
    return identity;
}
QJsonObject Stamp(QJsonObject body, const QJsonObject& identity) {
    for (auto it = identity.begin(); it != identity.end(); ++it) body.insert(it.key(), it.value());
    return body;
}
Status BodyBudget(const QJsonObject& body) {
    return static_cast<std::size_t>(QJsonDocument(body).toJson(QJsonDocument::Compact).size()) <= kBodyBytes - 4096
        ? Status::Ok() : Status::Failure("MCP.Terrain", "TERRAIN_RESPONSE_BUDGET", "地形回执超过 256 KiB，未发布编辑。");
}
QJsonObject GateJson(const Status& gate) {
    return {{"passed", gate.Succeeded()}, {"scope", "terrain-pathing-only"}, {"map_save_completed", false},
        {"diagnostics", DiagnosticsJson(gate.diagnostics).value("diagnostics")}};
}
QJsonObject Scope(const TerrainEditToolContext& context) {
    return {{"includes_active_transaction_overlay", context.includesActiveTransactionOverlay},
        {"includes_unsaved_ui_revision", context.includesUnsavedUiRevision}, {"pathing_source", "current-static-wpm"},
        {"game_dynamic_pathing_verified", false}};
}
std::vector<TerrainStrokePoint> Points(const QJsonValue& value, std::size_t maximum) {
    if (!value.isArray() || value.toArray().isEmpty() || static_cast<std::size_t>(value.toArray().size()) > maximum)
        throw ArgumentError("TERRAIN_POINT_BUDGET", "坐标数量为空或超过本工具预算。");
    std::vector<TerrainStrokePoint> points; points.reserve(value.toArray().size());
    for (const auto& item : value.toArray()) {
        if (!item.isObject()) throw ArgumentError("TERRAIN_ARGUMENT_TYPE", "每个坐标须为 x/y 对象。");
        const auto object = item.toObject(); Keys(object, {{"x", {}}, {"y", {}}}, {"x", "y"});
        constexpr double maximumCoordinate = 16777216;
        points.push_back({Number(object, "x", -maximumCoordinate, maximumCoordinate),
            Number(object, "y", -maximumCoordinate, maximumCoordinate)});
    }
    return points;
}
TerrainBrush Brush(const QJsonValue& value) {
    if (!value.isObject()) throw ArgumentError("TERRAIN_ARGUMENT_TYPE", "笔刷须为对象。");
    const auto object = value.toObject(); const auto kind = Text(object, "kind", 24);
    static const std::pair<const char*, TerrainBrushKind> kinds[]{
        {"texture", TerrainBrushKind::Texture}, {"blight", TerrainBrushKind::Blight}, {"raise", TerrainBrushKind::Raise},
        {"lower", TerrainBrushKind::Lower}, {"plateau", TerrainBrushKind::Plateau}, {"smooth", TerrainBrushKind::Smooth},
        {"cliff_level", TerrainBrushKind::CliffLevel}, {"noise", TerrainBrushKind::Noise},
        {"cliff_texture", TerrainBrushKind::CliffTexture}, {"water_height", TerrainBrushKind::WaterHeight},
        {"remove_water", TerrainBrushKind::RemoveWater},
        {"ramp", TerrainBrushKind::Ramp}, {"corner_ramp", TerrainBrushKind::CornerRamp}, {"boundary", TerrainBrushKind::Boundary},
        {"pathing_restriction", TerrainBrushKind::PathingRestriction}};
    const auto matched = std::find_if(std::begin(kinds), std::end(kinds), [&](const auto& entry) { return kind == entry.first; });
    if (matched == std::end(kinds)) throw ArgumentError("TERRAIN_BRUSH_KIND", "未知地形笔刷。");
    TerrainBrush brush; brush.kind = matched->second;
    if (brush.kind != TerrainBrushKind::CornerRamp) brush.size = 1;
    QJsonObject allowed{{"kind", {}}};
    if (brush.kind != TerrainBrushKind::Ramp && brush.kind != TerrainBrushKind::CornerRamp) {
        allowed.insert("size", {}); allowed.insert("shape", {});
        if (object.contains("size")) brush.size = static_cast<std::uint32_t>(Number(object, "size", 1, 8, true));
        if (object.contains("shape")) {
            const auto shape = Text(object, "shape", 8);
            if (shape != "circle" && shape != "square") throw ArgumentError("TERRAIN_BRUSH_SHAPE", "笔刷形状只接受 circle/square。");
            brush.shape = shape == "square" ? TerrainBrushShape::Square : TerrainBrushShape::Circle;
        }
    }
    if (brush.kind == TerrainBrushKind::Blight || brush.kind == TerrainBrushKind::Ramp || brush.kind == TerrainBrushKind::CornerRamp
        || brush.kind == TerrainBrushKind::Boundary || brush.kind == TerrainBrushKind::PathingRestriction) {
        allowed.insert("value", {}); brush.value = Boolean(object, "value", true);
    }
    if (brush.kind == TerrainBrushKind::PathingRestriction) {
        allowed.insert("blocked_type", {});
        const auto type = Text(object, "blocked_type", 8);
        if (type == "walk") brush.pathingRestrictions = 0x02;
        else if (type == "fly") brush.pathingRestrictions = 0x04;
        else if (type == "build") brush.pathingRestrictions = 0x08;
        else throw ArgumentError("PATHING_RESTRICTION_FLAGS", "blocked_type 只接受 walk、fly 或 build。");
    }
    if (brush.kind == TerrainBrushKind::Raise || brush.kind == TerrainBrushKind::Lower
        || brush.kind == TerrainBrushKind::Noise || brush.kind == TerrainBrushKind::Smooth) {
        allowed.insert("strength", {});
        if (object.contains("strength")) brush.strength = static_cast<float>(Number(object, "strength", 0, 1024));
        else if (brush.kind == TerrainBrushKind::Smooth) brush.strength = 0.5f;
    }
    if (brush.kind == TerrainBrushKind::Noise || brush.kind == TerrainBrushKind::Texture) {
        allowed.insert("seed", {});
        if (object.contains("seed")) brush.seed = static_cast<std::uint32_t>(Number(object, "seed", 0, 4294967295.0, true));
    }
    if (brush.kind == TerrainBrushKind::Texture) {
        allowed.insert("ground_texture", {}); allowed.insert("variation", {}); allowed.insert("randomize_variation", {});
        brush.groundTexture = Text(object, "ground_texture", 4).toStdString();
        if (brush.groundTexture.size() != 4) throw ArgumentError("TERRAIN_TEXTURE_ID", "地面贴图类型必须为四字节 rawcode。");
        if (object.contains("variation")) brush.variation = static_cast<std::uint8_t>(Number(object, "variation", 0, 17, true));
        brush.randomizeVariation = Boolean(object, "randomize_variation", false);
    }
    if (brush.kind == TerrainBrushKind::CliffTexture || brush.kind == TerrainBrushKind::CliffLevel) {
        allowed.insert("cliff_texture", {});
        brush.cliffTexture = static_cast<std::uint8_t>(Number(object, "cliff_texture", 0, 14, true));
    }
    if (brush.kind == TerrainBrushKind::CliffLevel) {
        allowed.insert("level_delta", {}); brush.levelDelta = static_cast<int>(Number(object, "level_delta", -1, 1, true));
    }
    if (brush.kind == TerrainBrushKind::WaterHeight) {
        allowed.insert("water_height", {}); brush.waterHeight = static_cast<float>(Number(object, "water_height", -2048, 2047.75));
    }
    Keys(object, allowed, {"kind"});
    return brush;
}
std::optional<TerrainWaterStrokeOptions> WaterOptions(const QJsonValue& value) {
    if (!value.isObject()) throw ArgumentError("TERRAIN_ARGUMENT_TYPE", "笔刷须为对象。");
    const auto object = value.toObject(); const auto kind = Text(object, "kind", 24);
    if (kind != "shallow_water" && kind != "deep_water") return {};
    Keys(object, {{"kind", {}}, {"shape", {}}, {"size", {}}, {"cliff_texture", {}}, {"seed", {}}, {"enforce_water_limits", {}}},
        {"kind", "shape", "size", "cliff_texture", "seed", "enforce_water_limits"});
    TerrainWaterStrokeOptions options;
    options.kind = kind == "deep_water" ? TerrainWaterBrushKind::Deep : TerrainWaterBrushKind::Shallow;
    const auto shape = Text(object, "shape", 8);
    if (shape != "circle" && shape != "square") throw ArgumentError("TERRAIN_BRUSH_SHAPE", "水刷形状只接受circle/square。");
    options.shape = shape == "square" ? TerrainBrushShape::Square : TerrainBrushShape::Circle;
    options.size = static_cast<std::uint32_t>(Number(object, "size", 1, 8, true));
    options.cliffTexture = static_cast<std::uint8_t>(Number(object, "cliff_texture", 0, 14, true));
    options.randomSeed = static_cast<std::uint32_t>(Number(object, "seed", 0, 4294967295.0, true));
    options.enforceWaterLimits = Boolean(object, "enforce_water_limits", true);
    return options;
}
QJsonValue Rawcode(const std::optional<std::string>& rawcode) {
    if (!rawcode) return QJsonValue(QJsonValue::Null);
    return QJsonObject{{"raw_hex", MapDataJson::Hex(*rawcode)}, {"raw_utf8", MapDataJson::Utf8OrNull(*rawcode)}};
}
QJsonObject SampleJson(const TerrainPointSample& point) {
    const auto& vertex = point.vertex;
    QJsonObject pathing{{"state", point.pathingState == TerrainPathingState::Available ? "available"
        : point.pathingState == TerrainPathingState::Outside ? "outside" : "unavailable"},
        {"raw_flags", point.pathingFlags ? QJsonValue(*point.pathingFlags) : QJsonValue(QJsonValue::Null)},
        {"source_flags", point.originalPathingFlags ? QJsonValue(*point.originalPathingFlags) : QJsonValue(QJsonValue::Null)},
        {"added_restrictions", point.addedPathingRestrictions}};
    if (point.pathingFlags) { pathing.insert("x", static_cast<int>(point.pathingX)); pathing.insert("y", static_cast<int>(point.pathingY)); }
    return {{"x", point.point.x}, {"y", point.point.y}, {"ground_height", point.groundHeight},
        {"ground_height_approximate", point.groundHeightApproximate},
        {"nearest_vertex", QJsonObject{{"x", static_cast<int>(point.vertexX)}, {"y", static_cast<int>(point.vertexY)},
            {"ground_height_raw", vertex.groundHeightRaw}, {"water_height_flags_raw", vertex.waterHeightAndFlagsRaw},
            {"texture_flags_raw", vertex.textureAndFlagsRaw}, {"variation_raw", vertex.variationRaw},
            {"cliff_layer_raw", vertex.cliffAndLayerRaw}, {"cliff_level", vertex.CliffLevel()},
            {"ramp", vertex.IsRamp()}, {"water", vertex.HasWater()}, {"blight", vertex.IsBlight()},
            {"boundary", vertex.IsBoundary()}, {"map_edge", vertex.IsMapEdge()},
            {"ground_texture", Rawcode(point.groundTexture)}, {"cliff_texture", Rawcode(point.cliffTexture)}}},
        {"pathing", pathing}};
}
QJsonObject ChangedJson(const MapDataEditSession::PreparedTerrainEdit& plan) {
    const auto& changed = plan.Changed();
    return {{"has_changes", plan.HasChanges()}, {"header_changed", changed.headerChanged},
        {"vertex_bounds", QJsonObject{{"x0", static_cast<int>(changed.x0)}, {"y0", static_cast<int>(changed.y0)},
            {"x1", static_cast<int>(changed.x1)}, {"y1", static_cast<int>(changed.y1)}, {"half_open", true}}}};
}

} // namespace

QJsonObject TerrainEditBindingJson(const Services::TerrainEditBinding& binding) {
    auto output = MapDataJson::BindingJson(binding.document);
    output.insert("session_identity", QString::number(binding.sessionIdentity));
    output.insert("terrain_revision", QString::number(binding.terrainRevision));
    output.insert("terrain_source_revision", QString::number(binding.terrainSourceRevision));
    return output;
}

std::vector<ToolDefinition> TerrainEditToolDefinitions() {
    const QJsonObject identity{{"project_identity", TextSchema(4096)}, {"expected_revision", TextSchema(4096)}};
    const auto binding = BasicSchema({{"document_id", TextSchema(128)}, {"document_generation", DecimalIdSchema()},
        {"revision", DecimalIdSchema()}, {"source_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
        {"session_identity", DecimalIdSchema()}, {"terrain_revision", DecimalIdSchema()}, {"terrain_source_revision", DecimalIdSchema()}},
        {"document_id", "document_generation", "revision", "source_sha256", "session_identity", "terrain_revision", "terrain_source_revision"});
    const auto points = [](int maximum) {
        return QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", maximum},
            {"items", BasicSchema({{"x", QJsonObject{{"type", "number"}}}, {"y", QJsonObject{{"type", "number"}}}}, {"x", "y"})}};
    };
    const auto brush = BasicSchema({{"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{
        "texture", "blight", "raise", "lower", "plateau", "smooth", "cliff_level", "noise", "cliff_texture", "water_height", "remove_water", "shallow_water", "deep_water", "ramp", "corner_ramp", "boundary", "pathing_restriction"}}}},
        {"blocked_type", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"walk", "fly", "build"}}}},
        {"size", IntegerSchema(1, 8)}, {"shape", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"circle", "square"}}}},
        {"ground_texture", TextSchema(4)}, {"cliff_texture", IntegerSchema(0, 14)}, {"level_delta", IntegerSchema(-1, 1)},
        {"strength", QJsonObject{{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1024}}},
        {"seed", IntegerSchema(0, 4294967295.0)}, {"variation", IntegerSchema(0, 17)},
        {"enforce_water_limits", QJsonObject{{"type", "boolean"}}},
        {"value", QJsonObject{{"type", "boolean"}}}, {"randomize_variation", QJsonObject{{"type", "boolean"}}},
        {"water_height", QJsonObject{{"type", "number"}, {"minimum", -2048}, {"maximum", 2047.75}}}}, {"kind"});
    std::vector<ToolDefinition> definitions;
    const auto add = [&](const char* name, const char* description, QJsonObject properties, QJsonArray required, ToolPermission permission) {
        ToolDefinition definition;
        definition.name = name; definition.description = QString::fromUtf8(description);
        definition.inputSchema = BasicSchema(std::move(properties), std::move(required));
        definition.permission = permission; definition.maxResponseBytes = 1024 * 1024; definition.maxRequestIdBytes = 4096;
        definitions.push_back(std::move(definition));
    };
    add("get_terrain_edit_state", "读取当前地形会话、尺寸、贴图表与地形/WPM保存资格。返回明确来源和编辑修订，不能将这道保存门当作整图保存或游戏验证。", identity, {}, ToolPermission::Query);
    auto query = identity; query.insert("terrain_binding", binding); query.insert("points", points(256));
    add("query_terrain_points", "按原terrain_binding查询1..256个世界坐标的地高、最近原顶点、贴图、悬崖和静态WPM原始通行位。缺WPM、超出WPM半开边界分别报告；不等同游戏动态寻路。", query, {"terrain_binding", "points"}, ToolPermission::Query);
    auto bounds = identity; bounds.insert("terrain_binding", binding);
    for (const char* edge : {"west", "south", "east", "north"}) bounds.insert(edge, IntegerSchema(0, 2048));
    bounds.insert("dry_run", QJsonObject{{"type", "boolean"}});
    add("edit_terrain_bounds", "预览或提交同尺寸可玩区域与游戏镜头边界。west/south/east/north为整数地格半开矩形；不缩放地图、不搬对象，不接受客户端CameraMargins或规则。宿主从实际来源捕获原版规则并复核；提交沿共用Undo，不自行保存。独立目录宿主只预览；实际地图写出须用save_map_edit_candidate的playable_bounds命令，连同最终W3I重建镜头初始化脚本。",
        bounds, {"project_identity", "expected_revision", "terrain_binding", "west", "south", "east", "north", "dry_run"}, ToolPermission::Edit);
    auto edit = identity; edit.insert("terrain_binding", binding); edit.insert("points", points(4096)); edit.insert("brush", brush);
    edit.insert("dry_run", QJsonObject{{"type", "boolean"}}); edit.insert("follow_doodads", QJsonObject{{"type", "boolean"}});
    add("edit_terrain_stroke", "使用界面同一Core命令预览或提交一笔1..4096点地形。必须显式dry_run，提交沿原Undo/活动事务，不自行保存；默认尺寸1、圆形、装饰物不跟随。ramp仅一级干地直坡；corner_ramp仅一级九点外角，可含水，每笔恰好一个世界点，两轴各为offset+128*n+32或offset+128*n+96，不自动吸附，brush只接受kind/value，固定检查水位限制，由实际资源Service核对完整修改块和分类邻域。water_height只调已有水面；remove_water只清水面标志，保留地高、原始水高和全部摆放，接受shape/size，不需要水刷规则，正式保存仍须完整重建通行性。shallow_water/deep_water须显式给出kind/shape/size/cliff_texture/seed/enforce_water_limits六项，由宿主捕获实际资源；boundary只改边界标志。pathing_restriction按32单位WPM格工作，必须指定blocked_type=walk/fly/build；value=false只撤回本工程新增限制，源限制全部保留。未知派生规则保持原生保存门关闭，独立宿主不可提交尚未通过保存门的笔划；越界和旧修订整笔拒绝。", edit,
        {"project_identity", "expected_revision", "terrain_binding", "points", "brush", "dry_run"}, ToolPermission::Edit);
    return definitions;
}

QJsonObject InvokeTerrainEditTool(const QString& name, const QJsonObject& arguments, const TerrainEditToolHost& host) {
    try {
        const auto definitions = TerrainEditToolDefinitions();
        const auto definition = std::find_if(definitions.begin(), definitions.end(), [&](const auto& item) { return item.name == name; });
        if (definition == definitions.end()) return Failure("TERRAIN_TOOL_UNKNOWN", "未知地形工具。");
        if (static_cast<std::size_t>(QJsonDocument(arguments).toJson(QJsonDocument::Compact).size()) > kRequestBytes)
            return Failure("TERRAIN_REQUEST_BUDGET", "地形请求超过 1 MiB。");
        Keys(arguments, definition->inputSchema.value("properties").toObject(), definition->inputSchema.value("required").toArray());
        const auto identity = Identity(host); const bool write = definition->permission == ToolPermission::Edit;
        if ((write || arguments.contains("project_identity") || arguments.contains("expected_revision"))
            && (arguments.value("project_identity") != identity.value("project_identity")
                || arguments.value("expected_revision") != identity.value("revision")))
            return Failure("TERRAIN_EDIT_STALE", "工程或编辑修订已变化，请重新查询。");
        if (!host.context) return Failure("TERRAIN_HOST_UNAVAILABLE", "地形宿主未接入当前会话。");
        const auto captured = host.context();
        if (!captured.Succeeded() || !captured.value) return Failure(captured);
        const auto& context = *captured.value;
        if (!context.session) return Failure("TERRAIN_HOST_UNAVAILABLE", "当前会话没有地形。");
        const auto binding = Binding(context); const auto bindingJson = TerrainEditBindingJson(binding);
        if (name == "get_terrain_edit_state") {
            const auto state = CaptureTerrainEditState(*context.session, context.document);
            if (!state.Succeeded() || !state.value) return Failure(state);
            QJsonArray ground, cliff;
            for (const auto& rawcode : state.value->header.groundTilesets) ground.append(Rawcode(rawcode));
            for (const auto& rawcode : state.value->header.cliffTilesets) cliff.append(Rawcode(rawcode));
            return Stamp({{"ok", true}, {"terrain_binding", bindingJson}, {"scope", Scope(context)},
                {"width", static_cast<int>(state.value->header.width)}, {"height", static_cast<int>(state.value->header.height)},
                {"offset", QJsonArray{state.value->header.offset[0], state.value->header.offset[1]}},
                {"ground_tilesets", ground}, {"cliff_tilesets", cliff}, {"pathing_available", state.value->pathingAvailable},
                {"terrain_pathing_gate", GateJson(state.value->terrainPathingGate)}}, identity);
        }
        if (!arguments.value("terrain_binding").isObject() || arguments.value("terrain_binding").toObject() != bindingJson)
            return Failure("TERRAIN_EDIT_STALE", "地形来源或修订已变化；请精确回传最新 terrain_binding。");
        if (name == "edit_terrain_bounds") {
            TerrainBoundsEditIntent intent{{static_cast<std::uint32_t>(Number(arguments, "west", 0, 2048, true)),
                static_cast<std::uint32_t>(Number(arguments, "south", 0, 2048, true)),
                static_cast<std::uint32_t>(Number(arguments, "east", 0, 2048, true)),
                static_cast<std::uint32_t>(Number(arguments, "north", 0, 2048, true))}};
            const bool dryRun = Boolean(arguments, "dry_run", true);
            if (intent.playable.west >= intent.playable.east || intent.playable.south >= intent.playable.north)
                return Failure("TERRAIN_BOUNDS_RECTANGLE", "可玩区需要非空半开地格矩形。");
            if (!host.prepareBounds || !host.verifyBounds)
                return Failure("TERRAIN_BOUNDS_HOST_UNAVAILABLE", "宿主未接入实际来源范围规则捕获与提交前复核。");
            const auto prepared = host.prepareBounds(context, binding, intent);
            if (!prepared.Succeeded() || !prepared.value) return Failure(prepared);
            const auto verified = host.verifyBounds(); if (!verified.Succeeded()) return Failure(verified);
            QJsonArray camera; for (const auto value : prepared.value->Plan().CameraBounds()) camera.append(value);
            QJsonObject body{{"ok", true}, {"dry_run", dryRun}, {"applied", false}, {"binding_before", bindingJson},
                {"scope", Scope(context)}, {"map_saved", false}, {"native_save_required", true},
                {"changes", QJsonObject{{"has_changes", prepared.value->Plan().HasChanges()},
                    {"map_edge_vertices", static_cast<double>(prepared.value->Plan().ChangedMapEdges())}, {"camera_bounds", camera}}}};
            const auto responseBudget = BodyBudget(body); if (!responseBudget.Succeeded()) return Failure(responseBudget);
            if (dryRun || !prepared.value->Plan().HasChanges()) return Stamp(std::move(body), identity);
            if (!host.applyBounds) return Failure("TERRAIN_BOUNDS_EDIT_UNSUPPORTED", "宿主尚未接通密封范围命令的共用撤销提交。");
            const auto validate = [&](const MapDataEditSession::EditReceipt& receipt) -> Status {
                const auto resources = host.verifyBounds(); if (!resources.Succeeded()) return resources;
                if (Identity(host) != identity || receipt.SessionIdentity() != context.session->SessionIdentity()
                    || receipt.BeforeRevision() != binding.terrainRevision)
                    return Status::Failure("MCP.Terrain", "TERRAIN_EDIT_STALE", "提交前工程、范围来源或原修订已变化。");
                body["edit_receipt"] = QJsonObject{{"command_identity", QString::number(receipt.CommandIdentity())},
                    {"before_revision", QString::number(receipt.BeforeRevision())}, {"after_revision", QString::number(receipt.AfterRevision())}};
                return BodyBudget(body);
            };
            const auto applied = host.applyBounds(*prepared.value, validate);
            if (!applied.Succeeded()) return Failure(applied);
            body["applied"] = true; body["query_state_again"] = true;
            return Stamp(std::move(body), Identity(host));
        }
        const auto points = Points(arguments.value("points"), write ? TerrainEditMaximumPoints : TerrainQueryMaximumPoints);
        if (!write) {
            const auto queried = QueryTerrainPoints(*context.session, context.document, binding, points);
            if (!queried.Succeeded() || !queried.value) return Failure(queried);
            QJsonArray samples; for (const auto& sample : *queried.value) samples.append(SampleJson(sample));
            QJsonObject body{{"ok", true}, {"terrain_binding", bindingJson}, {"scope", Scope(context)}, {"points", samples}};
            const auto budget = BodyBudget(body); if (!budget.Succeeded()) return Failure(budget);
            return Stamp(std::move(body), identity);
        }
        const bool dryRun = Boolean(arguments, "dry_run", true);
        const auto water = WaterOptions(arguments.value("brush"));
        const auto brush = water ? MapData::TerrainBrush{} : Brush(arguments.value("brush"));
        const bool cliff = !water && brush.kind == MapData::TerrainBrushKind::CliffLevel;
        const bool corner = !water && brush.kind == MapData::TerrainBrushKind::CornerRamp;
        if (water && (!host.prepareWater || !host.verifyWater))
            return Failure("TERRAIN_WATER_HOST_UNAVAILABLE", "宿主未接入实际来源水刷捕获与提交前复核。");
        if (cliff && (!host.prepareCliff || !host.verifyWater))
            return Failure("TERRAIN_CLIFF_HOST_UNAVAILABLE", "宿主未接入普通悬崖的实际规则捕获与提交前复核。");
        if (corner && points.size() != 1)
            return Failure("TERRAIN_CORNER_RAMP_POINT_COUNT", "角坡每笔必须恰好包含一个规范世界坐标点。");
        if (corner && (!host.prepareCornerRamp || !host.verifyWater))
            return Failure("TERRAIN_CORNER_RAMP_HOST_UNAVAILABLE", "宿主未接入角坡的实际规则捕获与提交前复核。");
        const auto prepared = water
            ? host.prepareWater(context, binding, *water, points, Boolean(arguments, "follow_doodads", false))
            : cliff ? host.prepareCliff(context, binding, brush, points, Boolean(arguments, "follow_doodads", false))
            : corner ? host.prepareCornerRamp(context, binding, brush, points, Boolean(arguments, "follow_doodads", false))
            : PrepareTerrainEdit(*context.session, context.document, binding, brush, points, Boolean(arguments, "follow_doodads", false));
        if (!prepared.Succeeded() || !prepared.value) return Failure(prepared);
        if (water || cliff || corner) { const auto verified = host.verifyWater(); if (!verified.Succeeded()) return Failure(verified); }
        QJsonObject body{{"ok", true}, {"dry_run", dryRun}, {"applied", false}, {"binding_before", bindingJson},
            {"scope", Scope(context)}, {"changes", ChangedJson(prepared.value->Plan())},
            {"terrain_pathing_gate", GateJson(prepared.value->Plan().TerrainPathingGate())}, {"map_saved", false}};
        if (dryRun || !prepared.value->Plan().HasChanges()) return Stamp(std::move(body), identity);
        if (!host.apply) return Failure("TERRAIN_EDIT_UNSUPPORTED", "宿主尚未接通同一事务的地形提交。");
        const auto validate = [&](const MapDataEditSession::EditReceipt& receipt) -> Status {
            if (water || cliff || corner) { const auto verified = host.verifyWater(); if (!verified.Succeeded()) return verified; }
            if (Identity(host) != identity || receipt.SessionIdentity() != context.session->SessionIdentity()
                || receipt.BeforeRevision() != binding.terrainRevision)
                return Status::Failure("MCP.Terrain", "TERRAIN_EDIT_STALE", "提交前工程、来源或原地形修订变化。");
            body["edit_receipt"] = QJsonObject{{"command_identity", QString::number(receipt.CommandIdentity())},
                {"before_revision", QString::number(receipt.BeforeRevision())}, {"after_revision", QString::number(receipt.AfterRevision())}};
            return BodyBudget(body);
        };
        const auto applied = host.apply(*prepared.value, validate);
        if (!applied.Succeeded() || !applied.value) return Failure(applied);
        body["applied"] = true; body["query_state_again"] = true;
        return Stamp(std::move(body), Identity(host));
    } catch (const ArgumentError& error) {
        return Failure(error.code, error.what());
    } catch (const std::exception&) {
        return Failure("TERRAIN_TOOL_FAILED", "地形工具未能完成请求；请重新查询当前会话。");
    }
}

} // namespace War3Map::Mcp
