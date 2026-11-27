#include "McpWorldViewportTools.h"
#include "McpToolInternal.h"
#include <algorithm>
#include <cmath>

namespace War3Map::Mcp {
namespace {
QJsonObject IdSchema() { return {{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}; }
QJsonObject CameraSchema() {
    QJsonObject fields;
    QJsonArray required;
    for (const auto* key : {"target_x", "target_y", "target_z", "distance", "yaw_radians", "pitch_radians"}) {
        fields.insert(key, QJsonObject{{"type", "number"}}); required.append(key);
    }
    return BasicSchema(fields, required);
}
} // namespace
std::vector<ToolDefinition> WorldViewportToolDefinitions() {
    std::vector<ToolDefinition> tools;
    for (const auto* name : {"world_viewport_camera_read", "world_viewport_camera_set",
        "world_viewport_capture", "world_viewport_capture_result", "world_scatter_stroke"}) {
        ToolDefinition tool; tool.name = name;
        tool.permission = tool.name == "world_viewport_camera_set" || tool.name == "world_scatter_stroke"
            ? ToolPermission::Edit : ToolPermission::Query;
        QJsonObject properties{{"project_identity", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
            {"expected_revision", IdSchema()}};
        QJsonArray required{"project_identity", "expected_revision"};
        if (tool.name == "world_viewport_camera_read")
            tool.description = "读取活动世界视口的请求相机与视口修订；完成帧相机以截图回执为准";
        else if (tool.name == "world_viewport_camera_set") {
            tool.description = "预览或设置活动世界视口相机；不修改地图镜头对象，参数与界面共用校验";
            properties.insert("expected_viewport_revision", IdSchema()); required.append("expected_viewport_revision");
            properties.insert("camera", CameraSchema()); required.append("camera");
            properties.insert("dry_run", QJsonObject{{"type", "boolean"}}); required.append("dry_run");
        } else if (tool.name == "world_scatter_stroke") {
            tool.description = "用世界页散布笔刷沿路径画一笔（与界面同一规则与撤销：一笔一步）。表面为地形和/或符合过滤的装饰物模型；"
                "模型表面在后台读取，未就绪时返回 WORLD_SCATTER_SURFACE_BUILDING，稍后重试。dry_run 只返回落点不改地图";
            const QJsonObject number{{"type", "number"}};
            const QJsonObject rawcode{{"type", "string"}, {"pattern", "^[ -~]{4}$"}};
            properties.insert("mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"paint", "erase"}}});
            properties.insert("palette", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                {"items", QJsonObject{{"type", "object"}, {"additionalProperties", false},
                    {"properties", QJsonObject{{"rawcode", rawcode}, {"weight", number},
                        {"type", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"doodad", "destructable"}}}}}},
                    {"required", QJsonArray{"rawcode"}}}}});
            QJsonObject settings;
            for (const auto* key : {"radius", "density_per_100m2", "units_per_metre", "yaw_min", "yaw_max", "scale_min", "scale_max",
                    "min_spacing", "slope_min", "slope_max"}) settings.insert(key, number);
            settings.insert("count_per_dab", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}});
            settings.insert("random_variation", QJsonObject{{"type", "boolean"}});
            properties.insert("settings", QJsonObject{{"type", "object"}, {"additionalProperties", false}, {"properties", settings},
                {"description", "未给出的项沿用界面当前值；给 count_per_dab 时按每笔个数，否则按每 100 平方米密度"}});
            properties.insert("surface", QJsonObject{{"type", "object"}, {"additionalProperties", false},
                {"properties", QJsonObject{{"terrain", QJsonObject{{"type", "boolean"}}},
                    {"filter", QJsonObject{{"type", "string"}, {"maxLength", 512}}}}}});
            properties.insert("path", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 256},
                {"items", QJsonObject{{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", number}}}});
            properties.insert("seed", QJsonObject{{"type", "string"}, {"pattern", "^[0-9]{1,20}$"}});
            properties.insert("dry_run", QJsonObject{{"type", "boolean"}});
            for (const auto* key : {"mode", "palette", "path", "dry_run"}) required.append(key);
        } else if (tool.name == "world_viewport_capture") {
            tool.description = "读取实际完成帧并后台编码PNG，返回有界请求ID；不截取桌面或其它窗口";
            properties.insert("expected_viewport_revision", IdSchema()); required.append("expected_viewport_revision");
            properties.insert("max_rgba_bytes", QJsonObject{{"type", "integer"}, {"minimum", 4}, {"maximum", 64 * 1024 * 1024}});
        } else {
            tool.description = "查询完成帧PNG及帧、相机、场景和来源身份；include_png默认关闭，PNG最多4MiB";
            properties.insert("capture_id", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 64}});
            required.append("capture_id");
            properties.insert("include_png", QJsonObject{{"type", "boolean"}});
        }
        tool.inputSchema = BasicSchema(properties, required);
        tool.maxRequestIdBytes = 128; tool.maxResponseBytes = 6 * 1024 * 1024;
        tool.pngImageResult = tool.name == "world_viewport_capture_result";
        tools.push_back(std::move(tool));
    }
    return tools;
}
QJsonObject InvokeWorldViewportTool(const QString& name, const QJsonObject& args, const WorldViewportToolHost& host) {
    const auto definitions = WorldViewportToolDefinitions();
    const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) { return value.name == name; });
    if (found == definitions.end()) return Failure("WORLD_VIEWPORT_TOOL_UNKNOWN", "未知世界视口工具。");
    const auto properties = found->inputSchema.value("properties").toObject();
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!properties.contains(it.key())) return Failure("WORLD_VIEWPORT_ARGUMENT", "视口工具不接受路径或未知参数。");
    for (const auto& key : found->inputSchema.value("required").toArray())
        if (!args.contains(key.toString())) return Failure("WORLD_VIEWPORT_ARGUMENT", "视口工具缺少必需参数。");
    const auto identity = args.value("project_identity");
    if (!identity.isString() || identity.toString().isEmpty() || identity.toString().size() > 128 || identity.toString().contains(QChar::Null)
        || !ParseUnsignedId(args, "expected_revision", true).Succeeded())
        return Failure("WORLD_VIEWPORT_ARGUMENT", "工程身份或修订无效。");
    if (args.contains("expected_viewport_revision") && !ParseUnsignedId(args, "expected_viewport_revision", true).Succeeded())
        return Failure("WORLD_VIEWPORT_ARGUMENT", "视口修订必须是十进制整数串。");
    if (name == "world_viewport_camera_set") {
        const auto camera = args.value("camera");
        if (!camera.isObject() || camera.toObject().size() != 6 || !args.value("dry_run").isBool())
            return Failure("WORLD_VIEWPORT_ARGUMENT", "相机需要六个数值字段和明确的dry_run。");
        for (const auto& key : CameraSchema().value("required").toArray()) {
            const auto value = camera.toObject().value(key.toString());
            if (!value.isDouble() || !std::isfinite(value.toDouble()))
                return Failure("WORLD_VIEWPORT_ARGUMENT", "相机字段必须是有限数。");
        }
    }
    if (args.contains("max_rgba_bytes")) {
        const auto value = args.value("max_rgba_bytes"); const auto bytes = value.toDouble();
        if (!value.isDouble() || !std::isfinite(bytes) || std::floor(bytes) != bytes || bytes < 4 || bytes > 64 * 1024 * 1024)
            return Failure("WORLD_VIEWPORT_ARGUMENT", "RGBA预算必须在4字节至64MiB之间。");
    }
    if (name == "world_viewport_capture_result") {
        const auto id = args.value("capture_id");
        if (!id.isString() || id.toString().isEmpty() || id.toString().size() > 64 || id.toString().contains(QChar::Null)
            || (args.contains("include_png") && !args.value("include_png").isBool()))
            return Failure("WORLD_VIEWPORT_ARGUMENT", "截图身份或include_png无效。");
    }
    if (!host.invoke) return Failure("WORLD_VIEWPORT_UNSUPPORTED", "此宿主没有活动世界视口；请连接编辑器MCP端口。");
    return host.invoke(name, args);
}
void RegisterWorldViewportTools(McpServer& server) {
    for (auto tool : WorldViewportToolDefinitions()) {
        const auto name = tool.name;
        tool.handler = [name](const QJsonObject& args) { return InvokeWorldViewportTool(name, args, {}); };
        server.RegisterTool(std::move(tool));
    }
}
} // namespace War3Map::Mcp
