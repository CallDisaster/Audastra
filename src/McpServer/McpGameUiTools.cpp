#include "McpGameUiTools.h"
#include "McpToolInternal.h"
#include <algorithm>
#include <cmath>

namespace War3Map::Mcp {
namespace {
constexpr int kMaxRoots = 16;
constexpr int kMaxNameLength = 128;

QJsonObject IdSchema() { return {{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}; }
QJsonObject NameSchema() { return {{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxNameLength}}; }
QJsonObject NameListSchema() { return {{"type", "array"}, {"items", NameSchema()}, {"maxItems", kMaxRoots}}; }
QJsonObject IntegerSchema(int minimum, int maximum) {
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

bool ValidName(const QJsonValue& value) {
    return value.isString() && !value.toString().isEmpty() && value.toString().size() <= kMaxNameLength
        && !value.toString().contains(QChar::Null);
}

bool ValidNameList(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() > kMaxRoots) return false;
    const auto items = value.toArray();
    return std::all_of(items.begin(), items.end(), [](const QJsonValue& item) { return ValidName(item); });
}

bool ValidInteger(const QJsonValue& value, int minimum, int maximum) {
    const double number = value.toDouble();
    return value.isDouble() && std::isfinite(number) && std::floor(number) == number && number >= minimum && number <= maximum;
}
} // namespace

std::vector<ToolDefinition> GameUiToolDefinitions() {
    std::vector<ToolDefinition> tools;
    for (const auto* name : {"game_ui_view_read", "game_ui_view_set", "game_ui_nodes_query", "game_ui_render"}) {
        ToolDefinition tool; tool.name = name;
        tool.permission = tool.name == "game_ui_view_set" ? ToolPermission::Edit : ToolPermission::Query;
        QJsonObject properties{{"project_identity", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
            {"expected_revision", IdSchema()}};
        QJsonArray required{"project_identity", "expected_revision"};
        if (tool.name == "game_ui_view_read") {
            tool.description = "读取界面工作区的预览设置（种族、版本、宽高比、根界面）、读取状态、问题与地图自带界面列表";
        } else if (tool.name == "game_ui_view_set") {
            tool.description = "设置界面预览：种族、版本、宽高比、增删根界面（GameUI 为游戏代码区域）、只显示某节点；只改预览，不改地图";
            properties.insert("race", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"Human", "Orc", "NightElf", "Undead"}}});
            properties.insert("expansion", QJsonObject{{"type", "boolean"}});
            properties.insert("aspect", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"16:9", "16:10", "4:3"}}});
            properties.insert("add_roots", NameListSchema());
            properties.insert("remove_roots", NameListSchema());
            properties.insert("show_only", NameSchema());
        } else if (tool.name == "game_ui_nodes_query") {
            tool.description = "按名称或中文名分页查询界面节点：游戏内名称、类型、位置（屏幕单位与 1080p 像素）、锚点、贴图与文字来源";
            properties.insert("name_contains", QJsonObject{{"type", "string"}, {"maxLength", kMaxNameLength}});
            properties.insert("offset", IntegerSchema(0, 1000000));
            properties.insert("limit", IntegerSchema(1, 200));
        } else {
            tool.description = "按当前预览设置绘制游戏界面 PNG（与界面工作区同一画法，不含边框与名称标记）";
            properties.insert("width", IntegerSchema(320, 2560));
            properties.insert("height", IntegerSchema(240, 1440));
        }
        tool.inputSchema = BasicSchema(properties, required);
        tool.maxRequestIdBytes = 128;
        tool.maxResponseBytes = tool.name == "game_ui_render" ? 6 * 1024 * 1024 : 1024 * 1024;
        tool.pngImageResult = tool.name == "game_ui_render";
        tools.push_back(std::move(tool));
    }
    return tools;
}

QJsonObject InvokeGameUiTool(const QString& name, const QJsonObject& args, const GameUiToolHost& host) {
    const auto definitions = GameUiToolDefinitions();
    const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) { return value.name == name; });
    if (found == definitions.end()) return Failure("GAME_UI_TOOL_UNKNOWN", "未知界面工具。");
    const auto properties = found->inputSchema.value("properties").toObject();
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!properties.contains(it.key())) return Failure("GAME_UI_ARGUMENT", "界面工具不接受未知参数。");
    for (const auto& key : found->inputSchema.value("required").toArray())
        if (!args.contains(key.toString())) return Failure("GAME_UI_ARGUMENT", "界面工具缺少必需参数。");
    const auto identity = args.value("project_identity");
    if (!identity.isString() || identity.toString().isEmpty() || identity.toString().size() > 128 || identity.toString().contains(QChar::Null)
        || !ParseUnsignedId(args, "expected_revision", true).Succeeded())
        return Failure("GAME_UI_ARGUMENT", "工程身份或修订无效。");
    for (const auto* key : {"race", "aspect"}) {
        if (!args.contains(key)) continue;
        const auto allowed = properties.value(key).toObject().value("enum").toArray();
        if (!allowed.contains(args.value(key))) return Failure("GAME_UI_ARGUMENT", "种族或宽高比不在可选范围内。");
    }
    if (args.contains("expansion") && !args.value("expansion").isBool())
        return Failure("GAME_UI_ARGUMENT", "expansion 必须是布尔值。");
    for (const auto* key : {"add_roots", "remove_roots"}) {
        if (args.contains(key) && !ValidNameList(args.value(key)))
            return Failure("GAME_UI_ARGUMENT", "根界面列表最多 16 项，每项为非空名称。");
    }
    if (args.contains("show_only") && !ValidName(args.value("show_only")))
        return Failure("GAME_UI_ARGUMENT", "show_only 必须是非空名称。");
    if (args.contains("name_contains")) {
        const auto value = args.value("name_contains");
        if (!value.isString() || value.toString().size() > kMaxNameLength || value.toString().contains(QChar::Null))
            return Failure("GAME_UI_ARGUMENT", "name_contains 无效。");
    }
    for (const auto& key : {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("width"), QStringLiteral("height")}) {
        if (!args.contains(key)) continue;
        const auto schema = properties.value(key).toObject();
        if (!ValidInteger(args.value(key), schema.value("minimum").toInt(), schema.value("maximum").toInt()))
            return Failure("GAME_UI_ARGUMENT", "数值参数超出范围。");
    }
    if (!host.invoke) return Failure("GAME_UI_UNSUPPORTED", "此宿主没有界面工作区；请连接编辑器MCP端口。");
    return host.invoke(name, args);
}

void RegisterGameUiTools(McpServer& server) {
    for (auto tool : GameUiToolDefinitions()) {
        const auto name = tool.name;
        tool.handler = [name](const QJsonObject& args) { return InvokeGameUiTool(name, args, {}); };
        server.RegisterTool(std::move(tool));
    }
}
} // namespace War3Map::Mcp
