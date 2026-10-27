#include "McpGameUiTools.h"
#include "McpToolInternal.h"
#include <algorithm>
#include <cmath>

namespace War3Map::Mcp {
namespace {
constexpr int kMaxRoots = 16;
constexpr int kMaxNameLength = 128;
constexpr int kMaxFunctionLength = 256;
constexpr int kMaxSimulationValues = 200;
constexpr int kMaxSimulationKey = 512;
constexpr int kMaxSimulationText = 4096;
constexpr int kMaxUiIdLength = 64;

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

// 模拟输入：键为输入标识，值为文字、有限数值或布尔值。
bool ValidSimulationValues(const QJsonValue& value) {
    if (!value.isObject() || value.toObject().size() > kMaxSimulationValues) return false;
    const auto values = value.toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (it.key().isEmpty() || it.key().size() > kMaxSimulationKey || it.key().contains(QChar::Null)) return false;
        const auto& item = it.value();
        if (item.isString()) {
            if (item.toString().size() > kMaxSimulationText || item.toString().contains(QChar::Null)) return false;
        } else if (item.isDouble()) {
            if (!std::isfinite(item.toDouble())) return false;
        } else if (!item.isBool()) {
            return false;
        }
    }
    return true;
}
} // namespace

std::vector<ToolDefinition> GameUiToolDefinitions() {
    std::vector<ToolDefinition> tools;
    for (const auto* name : {"game_ui_view_read", "game_ui_view_set", "game_ui_nodes_query", "game_ui_render", "game_ui_simulate",
             "game_ui_preview_set", "game_ui_description_read", "game_ui_description_create", "game_ui_description_edit",
             "game_ui_description_triggers", "game_ui_trigger_simulation"}) {
        ToolDefinition tool; tool.name = name;
        const bool query = tool.name == "game_ui_view_read" || tool.name == "game_ui_nodes_query" || tool.name == "game_ui_render"
                           || tool.name == "game_ui_description_read";
        tool.permission = query ? ToolPermission::Query : ToolPermission::Edit;
        QJsonObject properties{{"project_identity", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
            {"expected_revision", IdSchema()}};
        QJsonArray required{"project_identity", "expected_revision"};
        if (tool.name == "game_ui_view_read") {
            tool.description = "读取界面工作区的预览设置（种族、版本、宽高比、根界面、是否显示隐藏框架）、读取状态、问题、地图自带界面列表、"
                "地图脚本创建的界面数量与预览未还原的界面调用，地图自己的界面描述（AudastraUI，含数值、状态、事件与当前预览取值），"
                "以及可新建的界面类型 frame_kinds（作用、用法、可换项与默认框架，与编辑器右键“创建界面”同一份）";
        } else if (tool.name == "game_ui_view_set") {
            tool.description = "设置界面预览：种族、版本、宽高比、增删根界面（GameUI 为游戏代码区域，MapScriptUI 为地图脚本创建的界面）、只显示某节点；只改预览，不改地图";
            properties.insert("race", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"Human", "Orc", "NightElf", "Undead"}}});
            properties.insert("expansion", QJsonObject{{"type", "boolean"}});
            properties.insert("aspect", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"16:9", "16:10", "4:3"}}});
            properties.insert("add_roots", NameListSchema());
            properties.insert("remove_roots", NameListSchema());
            properties.insert("show_only", NameSchema());
            properties.insert("show_hidden", QJsonObject{{"type", "boolean"}});
        } else if (tool.name == "game_ui_nodes_query") {
            tool.description = "按名称或中文名分页查询界面节点：游戏内名称、类型、位置（屏幕单位与 1080p 像素）、锚点、贴图与文字来源；脚本创建的节点附创建位置、变量与未还原的设置";
            properties.insert("name_contains", QJsonObject{{"type", "string"}, {"maxLength", kMaxNameLength}});
            properties.insert("offset", IntegerSchema(0, 1000000));
            properties.insert("limit", IntegerSchema(1, 200));
        } else if (tool.name == "game_ui_simulate") {
            tool.description = "界面预览模拟：选一个运行时改变脚本框架的函数（见 game_ui_view_read 的 simulation_functions），"
                "按输入值执行其中的赋值、条件、循环与界面设置；之后的节点查询与绘图显示模拟结果。function 为空字符串时停止模拟；"
                "values 按输入键给值（文字、数值或布尔）。只改预览，不执行脚本其他效果，不改地图";
            properties.insert("function", QJsonObject{{"type", "string"}, {"maxLength", kMaxFunctionLength}});
            properties.insert("values", QJsonObject{{"type", "object"}, {"maxProperties", kMaxSimulationValues}});
        } else if (tool.name == "game_ui_description_read") {
            tool.description = "读取地图界面描述的规范 JSON（audastra.ui，含未保存的修改）与它在地图里的路径 AudastraUI\\<编号>.json";
            properties.insert("description", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxUiIdLength}});
            required.append("description");
        } else if (tool.name == "game_ui_description_create") {
            tool.description = "新建一份空白的地图界面描述（编号为 ASCII 标识符），作为导入文件放进地图的待保存修改；与导入文件同一撤销与保存流程";
            properties.insert("description", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxUiIdLength}});
            properties.insert("label", QJsonObject{{"type", "string"}, {"maxLength", 256}});
            required.append("description");
        } else if (tool.name == "game_ui_description_edit") {
            tool.description = "按顺序作用一组编辑到地图界面描述，整组检查，任何一步或结果不合法都不改。每步一个 op："
                "set_description{label}；add_frame{parent?,index?,frame}；set_frame{frame,fields}（null 删除字段，改 id 同步引用）；"
                "move_frame{frame,parent,index?}；remove_frame{frame}；rename_value{from,to}（同步框架取值、文字与贴图模板）；set_values{values}、set_states{states}、set_events{events}（整体替换）；"
                "set_simulation{simulation:{timers:[{id,label,period,repeat}],values:{编号或编号[下标]:起始值}}}（编辑器模拟用，游戏不读）。"
                "框架与字段按 audastra.ui 写法（新框架可照 game_ui_view_read 的 frame_kinds[].frame 默认外观写）：type 为 panel/backdrop/text/bar/button/template，anchors 为 {point,to,toPoint,x,y}，"
                "to 为 parent、screen、框架编号或 game:区域标识；value/visible 可写 编号、编号[] 或 编号[下标]（visible 取文字数值时非空为显示），"
                "文字可写 {编号:小数位}，贴图也可写 {编号} 按数值换图。"
                "结果作为导入文件进入待保存修改";
            properties.insert("description", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxUiIdLength}});
            properties.insert("edits", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"items", QJsonObject{{"type", "object"}}}});
            required.append("edits");
            required.append("description");
        } else if (tool.name == "game_ui_description_triggers") {
            tool.description = "由地图界面描述生成自然 T：框架、数值与状态的全局变量，以及创建（每 0.2 秒试到界面库可用，最多 30 秒）、更新（按数值刷新绑定的属性，界面建好前不执行）、"
                "每个状态、回到基础状态的触发器，整批写进触发器编辑器的 AudastraUI 类别（同名触发器整体替换），走触发器撤销栈；"
                "生成的触发器用到的基础模板 AudastraUI\\AudastraUI.toc/.fdf 随同放进待保存修改。需要函数库已启用 AudastraUI。"
                "preview 为 true 时只返回生成的功能写法，不改地图。生成不了的部分全部列出并整体拒绝。"
                "写入后 runtime 给出本工程是否已批准运行库及本机可用包；未批准时测试运行和保存会拒绝，须经作者同意后用 map_runtime_set 启用";
            properties.insert("description", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxUiIdLength}});
            properties.insert("preview", QJsonObject{{"type", "boolean"}});
            required.append("description");
        } else if (tool.name == "game_ui_trigger_simulation") {
            tool.description = "界面模拟：在编辑器里执行作者自己的触发器。界面描述的模拟计时器（set_simulation）每次到期，执行用事件 "
                "AudastraUIRegisterSimulationTimer(界面编号, 计时器编号)“界面模拟计时器到期”订阅它的触发器（游戏里不会发生），"
                "再把这份界面的数值与状态变量读回预览；生成的创建、更新与状态触发器不执行，改为读回数值、切换状态。"
                "action：status 只读状态；start 按周期运行；step 推进 steps 次到期；pause；reset 丢掉脚本，预览回到起始值。"
                "第一次 start/step 在后台按当前触发器（含未保存修改）生成整图脚本，wait_ms 内等它准备好；改了触发器要先 reset。"
                "触发器碰到模拟里不能执行的游戏函数（单位、玩家、计时器、等待等）时停下并暂停，problems 给出函数名与位置。只改预览，不改地图";
            properties.insert("action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"status", "start", "step", "pause", "reset"}}});
            properties.insert("description", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxUiIdLength}});
            properties.insert("steps", IntegerSchema(1, 1000));
            properties.insert("wait_ms", IntegerSchema(0, 60000));
            required.append("action");
        } else if (tool.name == "game_ui_preview_set") {
            tool.description = "地图界面描述的预览：切换状态（空字符串为基础状态）、按 编号 或 编号[下标] 给数值（文字、数值或布尔），"
                "之后的节点查询与绘图显示这组取值。整批检查，任何一项不合法都不改。只改预览，不改地图";
            properties.insert("description", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxUiIdLength}});
            properties.insert("state", QJsonObject{{"type", "string"}, {"maxLength", kMaxUiIdLength}});
            properties.insert("values", QJsonObject{{"type", "object"}, {"maxProperties", kMaxSimulationValues}});
            required.append("description");
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
    for (const auto* key : {"race", "aspect", "action"}) {
        if (!args.contains(key)) continue;
        const auto allowed = properties.value(key).toObject().value("enum").toArray();
        if (!allowed.contains(args.value(key))) return Failure("GAME_UI_ARGUMENT", std::string(key) + " 不在可选范围内。");
    }
    for (const auto* key : {"expansion", "show_hidden", "preview"}) {
        if (args.contains(key) && !args.value(key).isBool()) return Failure("GAME_UI_ARGUMENT", std::string(key) + " 必须是布尔值。");
    }
    if (args.contains("label") && (!args.value("label").isString() || args.value("label").toString().size() > 256))
        return Failure("GAME_UI_ARGUMENT", "label 必须是不超过 256 字的文字。");
    if (args.contains("edits")) {
        const auto value = args.value("edits");
        const QJsonArray edits = value.toArray();
        if (!value.isArray() || edits.isEmpty() || edits.size() > 256
            || !std::all_of(edits.begin(), edits.end(), [](const QJsonValue& item) { return item.isObject(); }))
            return Failure("GAME_UI_ARGUMENT", "edits 必须是 1 到 256 个编辑对象。");
    }
    for (const auto* key : {"description", "state"}) {
        if (!args.contains(key)) continue;
        const auto value = args.value(key);
        if (!value.isString() || value.toString().size() > kMaxUiIdLength || value.toString().contains(QChar::Null)
            || (QLatin1String(key) == QLatin1String("description") && value.toString().isEmpty()))
            return Failure("GAME_UI_ARGUMENT", std::string(key) + " 必须是不超过 64 字的编号。");
    }
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
    for (const auto& key : {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("width"), QStringLiteral("height"),
             QStringLiteral("steps"), QStringLiteral("wait_ms")}) {
        if (!args.contains(key)) continue;
        const auto schema = properties.value(key).toObject();
        if (!ValidInteger(args.value(key), schema.value("minimum").toInt(), schema.value("maximum").toInt()))
            return Failure("GAME_UI_ARGUMENT", "数值参数超出范围。");
    }
    if (args.contains("function")) {
        const auto value = args.value("function");
        if (!value.isString() || value.toString().size() > kMaxFunctionLength || value.toString().contains(QChar::Null))
            return Failure("GAME_UI_ARGUMENT", "function 必须是不超过 256 字的函数名。");
    }
    if (args.contains("values") && !ValidSimulationValues(args.value("values")))
        return Failure("GAME_UI_ARGUMENT", "values 最多 200 项；键为非空输入标识，值为文字、有限数值或布尔值。");
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
