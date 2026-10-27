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
             "game_ui_description_triggers", "game_ui_cast_system", "game_ui_party_system", "game_ui_target_system", "game_ui_talent_system"}) {
        ToolDefinition tool; tool.name = name;
        const bool query = tool.name == "game_ui_view_read" || tool.name == "game_ui_nodes_query" || tool.name == "game_ui_render"
                           || tool.name == "game_ui_description_read";
        tool.permission = query ? ToolPermission::Query : ToolPermission::Edit;
        QJsonObject properties{{"project_identity", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
            {"expected_revision", IdSchema()}};
        QJsonArray required{"project_identity", "expected_revision"};
        if (tool.name == "game_ui_view_read") {
            tool.description = "读取界面工作区的预览设置（种族、版本、宽高比、根界面、是否显示隐藏框架）、读取状态、问题、地图自带界面列表、"
                "地图脚本创建的界面数量与预览未还原的界面调用，以及地图自己的界面描述（AudastraUI，含数值、状态、事件与当前预览取值）";
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
            tool.description = "新建一份地图界面描述（编号为 ASCII 标识符），作为导入文件放进地图的待保存修改；与导入文件同一撤销与保存流程。"
                "preset 可选起始模板：castbar 读条条（数值 shown、progress、text、remaining，状态 channel、interrupt，"
                "与 game_ui_cast_system 的读条条绑定对应）；party 团队框架（5 格，数组数值 shown、hp、mp、name、dead、far、hp_now、hp_max、portrait、buff1..buff4，点击事件 select，与 game_ui_party_system 的绑定对应）；target 目标框架（单值数值 shown、hp、mp、hp_now、hp_max、name、level、dead、portrait、buff1..buff6，点击事件 select，与 game_ui_target_system 的绑定对应）；talents 统一天赋树（入口按钮与面板，5 层 × 3 列，单值 shown、entry_shown、entry、title、points、"
                "tip_shown、tip_title、tip_text，数组 level、icon1..3、name1..3、picked1..3，事件 open、close、pick1..3、enter1..3、leave1..3，"
                "与 game_ui_talent_system 的默认绑定对应）；省略为空白";
            properties.insert("description", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxUiIdLength}});
            properties.insert("label", QJsonObject{{"type", "string"}, {"maxLength", 256}});
            properties.insert("preset", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"castbar", "party", "target", "talents"}}});
            required.append("description");
        } else if (tool.name == "game_ui_description_edit") {
            tool.description = "按顺序作用一组编辑到地图界面描述，整组检查，任何一步或结果不合法都不改。每步一个 op："
                "set_description{label}；add_frame{parent?,index?,frame}；set_frame{frame,fields}（null 删除字段，改 id 同步引用）；"
                "move_frame{frame,parent,index?}；remove_frame{frame}；rename_value{from,to}（同步框架取值、文字与贴图模板）；set_values{values}、set_states{states}、set_events{events}（整体替换）。"
                "框架与字段按 audastra.ui 写法：type 为 panel/backdrop/text/bar/button/template，anchors 为 {point,to,toPoint,x,y}，"
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
        } else if (tool.name == "game_ui_cast_system") {
            tool.description = "生成读条系统的自然 T，整批写进触发器编辑器的“读条系统”类别，走触发器撤销栈：登记（技能、读条秒数、引导秒数、"
                "跳间隔、效果触发器）、按施法事件记录每个施法者、引导每跳与打断判定，并在完成、每跳、打断时用 YDWE 运行触发器参数块"
                "运行效果触发器，参数名：施法单位、目标单位、目标X、目标Y、技能、等级、阶段（1 完成 2 每跳 3 打断）、跳数。"
                "地图里已有登记触发器时保留它，abilities 不写入（结果 registration_kept 为 true）。"
                "bar 绑定一份已生成界面触发器的界面描述作读条条：visible（布尔）与 progress（实数）必填，text（字符串）、remaining（实数）、"
                "channel 与 interrupted（状态）可空。需要 YDWE 函数库。preview 为 true 时只返回生成的功能写法，不改地图";
            properties.insert("abilities", QJsonObject{{"type", "array"}, {"maxItems", 64}, {"items", QJsonObject{{"type", "object"},
                {"properties", QJsonObject{{"ability", QJsonObject{{"type", "string"}, {"minLength", 4}, {"maxLength", 4}}},
                    {"cast", QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 3600}}},
                    {"channel", QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 3600}}},
                    {"tick", QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 3600}}},
                    {"effect", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}}}},
                {"required", QJsonArray{"ability", "effect"}}, {"additionalProperties", false}}}});
            QJsonObject barProperties;
            for (const char* key : {"description", "visible", "progress", "text", "remaining", "channel", "interrupted"})
                barProperties.insert(key, QJsonObject{{"type", "string"}, {"maxLength", kMaxUiIdLength}});
            properties.insert("bar", QJsonObject{{"type", "object"}, {"properties", barProperties},
                {"required", QJsonArray{"description", "visible", "progress"}}, {"additionalProperties", false}});
            properties.insert("preview", QJsonObject{{"type", "boolean"}});
        } else if (tool.name == "game_ui_party_system") {
            tool.description = "生成团队框架的自然 T，整批写进触发器编辑器的“团队框架”类别（同名触发器整体替换），走触发器撤销栈：队员名单"
                "（设 AUP_Member 后运行“团队 加入/移出”触发器；auto_heroes 默认 true，自动加入正在游戏的用户玩家的英雄，不含幻象，单位被移除后自动移出），"
                "每 0.1 秒把每格的显示、生命与法力比例、名字、死亡、过远写进界面数组数值并运行该界面的“更新”触发器；点击事件：本机举着单位目标技能或物品时用 AudastraUIClickUnit 把点击交给游戏作为目标，否则选中该队员（只改本机选择）。"
                "frame 绑定一份已生成界面触发器的界面描述：shown（布尔数组，格数即队伍上限）必填；health、mana（实数数组）、name（字符串数组）、"
                "dead、out_of_range（布尔数组）可空，格数须与 shown 相同；select 为点击事件编号，事件框架须在重复次数等于格数的重复框架里。"
                "health_now、health_max、mana_now、mana_max（实数数组）为生命、法力的当前与最大值，界面文字可写 {hp_now[]:0}/{hp_max[]:0}。"
                "party 模板对应 shown、hp、mp、name、dead、far、select、hp_now、hp_max、portrait。name_source：player 玩家名（默认）或 unit 英雄名/单位名；"
                "far_range 为过远距离（默认 1200）。过远与点选需要 YDWE 的 GetLocalPlayer、RemoveLocation。"
                "frame.buff_icons 为增减益图标数值（字符串数组，按显示顺序，party 模板为 buff1..buff4）；buffs 为要显示的增减益码，"
                "写进“团队 登记 Party_Register”触发器，图标取自物编；地图里已有登记触发器时保留它，buffs 不写入（结果 registration_kept 为 true）。"
                "frame.portrait 为头像图标数值（字符串数组）；portraits 为登记头像的单位类型，写进“团队 头像 Party_Portrait”触发器，"
                "图标取自物编，没登记的单位头像为空；地图里已有头像触发器时保留它，portraits 不写入（结果 portrait_kept 为 true）。"
                "preview 为 true 时只返回生成的功能写法，不改地图";
            QJsonObject frameProperties;
            for (const char* key : {"description", "shown", "health", "mana", "name", "dead", "out_of_range", "select", "health_now", "health_max",
                     "mana_now", "mana_max", "portrait"})
                frameProperties.insert(key, QJsonObject{{"type", "string"}, {"maxLength", kMaxUiIdLength}});
            frameProperties.insert("buff_icons", QJsonObject{{"type", "array"}, {"maxItems", 16},
                {"items", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxUiIdLength}}}});
            properties.insert("frame", QJsonObject{{"type", "object"}, {"properties", frameProperties},
                {"required", QJsonArray{"description", "shown"}}, {"additionalProperties", false}});
            properties.insert("name_source", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"player", "unit"}}});
            properties.insert("far_range", QJsonObject{{"type", "number"}, {"minimum", 100}, {"maximum", 100000}});
            properties.insert("auto_heroes", QJsonObject{{"type", "boolean"}});
            properties.insert("buffs", QJsonObject{{"type", "array"}, {"maxItems", 64},
                {"items", QJsonObject{{"type", "string"}, {"minLength", 4}, {"maxLength", 4}}}});
            properties.insert("portraits", QJsonObject{{"type", "array"}, {"maxItems", 256},
                {"items", QJsonObject{{"type", "string"}, {"minLength", 4}, {"maxLength", 4}}}});
            properties.insert("preview", QJsonObject{{"type", "boolean"}});
            required.append("frame");
        } else if (tool.name == "game_ui_target_system") {
            tool.description = "生成目标框架的自然 T，整批写进触发器编辑器的“目标框架”类别（同名触发器整体替换），走触发器撤销栈：每个玩家记一个目标"
                "（同步数据）。source 为 selection（默认）时玩家选中单位就记下、取消选择时清掉；为 focus 时设好 AUF_<键>_Member（单位）与 AUF_<键>_Player"
                "（玩家）后运行“<名字> 设定 <键>_Set”触发器（单位为空时清除）。单位被移除后自动清除。每 0.1 秒把本机玩家目标的显示、生命与法力比例、"
                "当前与最大生命和法力、名字、等级、死亡、头像、增减益写进界面数值并运行该界面的“更新”触发器；点击事件：本机举着单位目标技能或物品时用 "
                "AudastraUIClickUnit 把点击交给游戏作为目标，否则选中目标（只改本机选择）。frame 绑定一份已生成界面触发器的界面描述，数值都是单值："
                "shown（布尔）必填；health、mana、health_now、health_max、mana_now、mana_max（实数）、name、portrait（字符串）、level（整数）、"
                "dead（布尔）可空；select 为点击事件编号，事件框架不能在重复框架里。target 模板对应 shown、hp、mp、hp_now、hp_max、name、level、dead、"
                "portrait、buff1..buff6、select。name 与 key（ASCII 标识符，默认 目标/Target，focus 时 焦点/Focus）区分同图的多份；players 为记录目标的"
                "玩家数（1..12，默认 12）；name_source：unit 英雄名/单位名（默认）或 player 玩家名。frame.buff_icons、buffs、frame.portrait、portraits "
                "同 game_ui_party_system，写进“<名字> 登记 <键>_Register”“<名字> 头像 <键>_Portrait”，已有时保留（registration_kept、portrait_kept）。"
                "需要 YDWE 的 GetLocalPlayer；preview 为 true 时只返回生成的功能写法，不改地图";
            QJsonObject frameProperties;
            for (const char* key : {"description", "shown", "health", "mana", "health_now", "health_max", "mana_now", "mana_max", "name", "level", "dead",
                     "portrait", "select"})
                frameProperties.insert(key, QJsonObject{{"type", "string"}, {"maxLength", kMaxUiIdLength}});
            frameProperties.insert("buff_icons", QJsonObject{{"type", "array"}, {"maxItems", 16},
                {"items", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxUiIdLength}}}});
            properties.insert("frame", QJsonObject{{"type", "object"}, {"properties", frameProperties},
                {"required", QJsonArray{"description", "shown"}}, {"additionalProperties", false}});
            properties.insert("source", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"selection", "focus"}}});
            properties.insert("name", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 32}});
            properties.insert("key", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 24}});
            properties.insert("players", IntegerSchema(1, 12));
            properties.insert("name_source", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"unit", "player"}}});
            properties.insert("buffs", QJsonObject{{"type", "array"}, {"maxItems", 64},
                {"items", QJsonObject{{"type", "string"}, {"minLength", 4}, {"maxLength", 4}}}});
            properties.insert("portraits", QJsonObject{{"type", "array"}, {"maxItems", 256},
                {"items", QJsonObject{{"type", "string"}, {"minLength", 4}, {"maxLength", 4}}}});
            properties.insert("preview", QJsonObject{{"type", "boolean"}});
            required.append("frame");
        } else if (tool.name == "game_ui_talent_system") {
            tool.description = "生成统一天赋树的自然 T，整批写进触发器编辑器的“天赋系统”类别（同名触发器整体替换），走触发器撤销栈。"
                "所有英雄共用一套界面与触发器：英雄达到某层的解锁等级（unlock_levels，默认 5、10、15…）就开放这一层，每层只能选一次、选定不可改，"
                "已开放未选的层都可选。选择结果是同步数据：AUT_Picked[玩家编号×8+层] 为选中的列号（1 起），0 为未选，英雄技能触发器按它改变行为。"
                "界面点击只在本机发生，用选择单位同步：开局为每个用户玩家建 层数×列数 个同步单位（sync_unit 类型，默认 hfoo，须可被选中，"
                "透明、无敌、暂停、不占人口，放在地图左下角之外），点选项时把对应同步单位加进本机选择，“选择”触发器在所有玩家上校验后写入结果、"
                "按登记学会技能，再运行 after_pick 触发器（须已在地图里，运行前设好 AUT_PickUnit、AUT_PickTier、AUT_PickOption）。"
                "英雄绑定：开局已在地图上与之后进入地图的用户玩家英雄（不含幻象），类型已登记、该玩家还没有英雄时自动绑定；也可设 AUT_Unit 后运行“天赋 绑定 Talent_Bind”。"
                "heroes 写进“天赋 登记 Talent_Register”触发器：每个英雄 unit（物编编号）与 talents（tier、option 从 1 起，name、text 说明、learn 学会的技能，"
                "icon 为空时取 learn 技能的物编图标，disabled_icon 为空时按 BTN→DISBTN 推出）；地图里已有登记触发器时保留它、heroes 不写入"
                "（结果 registration_kept 为 true），replace_registration 为 true 时按 heroes 重写。"
                "frame 绑定一份已生成界面触发器的界面描述，只给 description 时按 talents 模板的编号绑定；单个字段写空字符串为不绑定。"
                "level（字符串数组，格数即层数）与 columns（每列 icon、name 字符串数组，picked 布尔数组，pick 点击事件，enter、leave 移入移出事件，"
                "事件框架须在重复层数次的框架里）决定层数与列数。需要 YDWE 的 GetLocalPlayer、RemoveLocation。preview 为 true 时只返回生成的功能写法，不改地图";
            QJsonObject frameProperties;
            for (const char* key : {"description", "shown", "entry_shown", "entry", "title", "points", "level", "open", "close", "tip_shown",
                     "tip_title", "tip_text"})
                frameProperties.insert(key, QJsonObject{{"type", "string"}, {"maxLength", kMaxUiIdLength}});
            QJsonObject columnProperties;
            for (const char* key : {"icon", "name", "picked", "pick", "enter", "leave"})
                columnProperties.insert(key, QJsonObject{{"type", "string"}, {"maxLength", kMaxUiIdLength}});
            frameProperties.insert("columns", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 4},
                {"items", QJsonObject{{"type", "object"}, {"properties", columnProperties},
                    {"required", QJsonArray{"icon", "name", "picked", "pick"}}, {"additionalProperties", false}}}});
            properties.insert("frame", QJsonObject{{"type", "object"}, {"properties", frameProperties},
                {"required", QJsonArray{"description"}}, {"additionalProperties", false}});
            properties.insert("name", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 32}});
            properties.insert("unlock_levels", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 7},
                {"items", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}}});
            properties.insert("sync_unit", QJsonObject{{"type", "string"}, {"minLength", 4}, {"maxLength", 4}});
            properties.insert("players", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 24}});
            properties.insert("after_pick", QJsonObject{{"type", "string"}, {"maxLength", 256}});
            QJsonObject talentProperties{{"tier", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 7}}},
                {"option", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
                {"name", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                {"icon", QJsonObject{{"type", "string"}, {"maxLength", 260}}}, {"disabled_icon", QJsonObject{{"type", "string"}, {"maxLength", 260}}},
                {"text", QJsonObject{{"type", "string"}, {"maxLength", 1024}}},
                {"learn", QJsonObject{{"type", "string"}, {"maxLength", 4}}}};
            QJsonObject heroProperties{{"unit", QJsonObject{{"type", "string"}, {"minLength", 4}, {"maxLength", 4}}},
                {"label", QJsonObject{{"type", "string"}, {"maxLength", 128}}},
                {"talents", QJsonObject{{"type", "array"}, {"maxItems", 28}, {"items", QJsonObject{{"type", "object"}, {"properties", talentProperties},
                    {"required", QJsonArray{"tier", "option", "name"}}, {"additionalProperties", false}}}}}};
            properties.insert("heroes", QJsonObject{{"type", "array"}, {"maxItems", 64}, {"items", QJsonObject{{"type", "object"},
                {"properties", heroProperties}, {"required", QJsonArray{"unit", "talents"}}, {"additionalProperties", false}}}});
            properties.insert("replace_registration", QJsonObject{{"type", "boolean"}});
            properties.insert("preview", QJsonObject{{"type", "boolean"}});
            required.append("frame");
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

namespace {
bool ValidText(const QJsonValue& value, qsizetype minimum, qsizetype maximum) {
    return value.isString() && value.toString().size() >= minimum && value.toString().size() <= maximum && !value.toString().contains(QChar::Null);
}

// 天赋树的登记与选项（只在 game_ui_talent_system 里出现）。
bool TalentArgumentsValid(const QJsonObject& args) {
    if (args.contains("unlock_levels")) {
        const QJsonArray levels = args.value("unlock_levels").toArray();
        if (!args.value("unlock_levels").isArray() || levels.isEmpty() || levels.size() > 7
            || !std::all_of(levels.begin(), levels.end(), [](const QJsonValue& item) { return ValidInteger(item, 1, 10000); }))
            return false;
    }
    if (args.contains("sync_unit") && !ValidText(args.value("sync_unit"), 4, 4)) return false;
    if (args.contains("players") && !ValidInteger(args.value("players"), 1, 24)) return false;
    if (args.contains("name") && !ValidText(args.value("name"), 1, 32)) return false;
    if (args.contains("after_pick") && !ValidText(args.value("after_pick"), 0, 256)) return false;
    if (!args.contains("heroes")) return true;
    const QJsonArray heroes = args.value("heroes").toArray();
    if (!args.value("heroes").isArray() || heroes.size() > 64) return false;
    const auto validTalent = [](const QJsonValue& item) {
        const QJsonObject talent = item.toObject();
        if (!item.isObject() || !talent.contains("tier") || !talent.contains("option") || !talent.contains("name")) return false;
        for (auto it = talent.begin(); it != talent.end(); ++it) {
            const auto key = it.key();
            const bool ok = key == QLatin1String("tier") ? ValidInteger(it.value(), 1, 7)
                : key == QLatin1String("option") ? ValidInteger(it.value(), 1, 4)
                : key == QLatin1String("name") ? ValidText(it.value(), 1, 128)
                : key == QLatin1String("icon") || key == QLatin1String("disabled_icon") ? ValidText(it.value(), 0, 260)
                : key == QLatin1String("text") ? ValidText(it.value(), 0, 1024)
                : key == QLatin1String("learn") ? ValidText(it.value(), 0, 4) && it.value().toString().size() != 1
                    && it.value().toString().size() != 2 && it.value().toString().size() != 3
                : false;
            if (!ok) return false;
        }
        return true;
    };
    return std::all_of(heroes.begin(), heroes.end(), [&](const QJsonValue& item) {
        const QJsonObject hero = item.toObject();
        if (!item.isObject() || !hero.contains("unit") || !hero.contains("talents")) return false;
        for (auto it = hero.begin(); it != hero.end(); ++it) {
            if (it.key() == QLatin1String("unit")) {
                if (!ValidText(it.value(), 4, 4)) return false;
            } else if (it.key() == QLatin1String("label")) {
                if (!ValidText(it.value(), 0, 128)) return false;
            } else if (it.key() == QLatin1String("talents")) {
                const QJsonArray talents = it.value().toArray();
                if (!it.value().isArray() || talents.size() > 28 || !std::all_of(talents.begin(), talents.end(), validTalent)) return false;
            } else {
                return false;
            }
        }
        return true;
    });
}
} // namespace

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
    for (const auto* key : {"race", "aspect", "preset", "name_source", "source"}) {
        if (!args.contains(key)) continue;
        const auto allowed = properties.value(key).toObject().value("enum").toArray();
        if (!allowed.contains(args.value(key))) return Failure("GAME_UI_ARGUMENT", std::string(key) + " 不在可选范围内。");
    }
    for (const auto* key : {"expansion", "show_hidden", "preview", "auto_heroes", "replace_registration"}) {
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
    for (const auto& key : {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("players")}) {
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
    if (args.contains("abilities")) {
        const auto value = args.value("abilities");
        const QJsonArray abilities = value.toArray();
        const auto validAbility = [](const QJsonValue& item) {
            if (!item.isObject()) return false;
            const QJsonObject ability = item.toObject();
            for (auto it = ability.begin(); it != ability.end(); ++it) {
                if (it.key() == QLatin1String("ability") || it.key() == QLatin1String("effect")) {
                    const QString text = it.value().toString();
                    if (!it.value().isString() || text.contains(QChar::Null)
                        || (it.key() == QLatin1String("ability") ? text.size() != 4 : text.isEmpty() || text.size() > 256)) return false;
                } else if (it.key() == QLatin1String("cast") || it.key() == QLatin1String("channel") || it.key() == QLatin1String("tick")) {
                    const double number = it.value().toDouble(-1.0);
                    if (!it.value().isDouble() || !std::isfinite(number) || number < 0.0 || number > 3600.0) return false;
                } else {
                    return false;
                }
            }
            return ability.contains("ability") && ability.contains("effect");
        };
        if (!value.isArray() || abilities.size() > 64 || !std::all_of(abilities.begin(), abilities.end(), validAbility))
            return Failure("GAME_UI_ARGUMENT", "abilities 最多 64 项；每项有 4 字符 ability、非空 effect，秒数为 0 到 3600 的数值。");
    }
    // 绑定对象（读条条 bar、团队框架 frame）：必填项齐全，各项为编号。
    for (const auto* key : {"bar", "frame"}) {
        if (!args.contains(key)) continue;
        const auto value = args.value(key);
        const QJsonObject binding = value.toObject();
        const auto schema = properties.value(key).toObject();
        const auto bindingProperties = schema.value("properties").toObject();
        const auto requiredKeys = schema.value("required").toArray();
        bool valid = value.isObject()
            && std::all_of(requiredKeys.begin(), requiredKeys.end(), [&](const QJsonValue& item) { return binding.contains(item.toString()); });
        const auto validId = [](const QJsonValue& item, qsizetype minimum) {
            return item.isString() && item.toString().size() >= minimum && item.toString().size() <= kMaxUiIdLength
                && !item.toString().contains(QChar::Null);
        };
        for (auto it = binding.begin(); valid && it != binding.end(); ++it) {
            const auto property = bindingProperties.value(it.key()).toObject();
            if (it.key() == QLatin1String("columns")) {
                // 天赋树的每列绑定：对象，图标、名字、已选、点击必填，各项为编号。
                const auto columnSchema = property.value("items").toObject();
                const auto columnProperties = columnSchema.value("properties").toObject();
                const auto columnRequired = columnSchema.value("required").toArray();
                const QJsonArray columns = it.value().toArray();
                valid = it.value().isArray() && !columns.isEmpty() && columns.size() <= property.value("maxItems").toInt()
                    && std::all_of(columns.begin(), columns.end(), [&](const QJsonValue& item) {
                           const QJsonObject column = item.toObject();
                           if (!item.isObject()) return false;
                           for (const auto& required : columnRequired)
                               if (!column.contains(required.toString())) return false;
                           for (auto field = column.begin(); field != column.end(); ++field)
                               if (!columnProperties.contains(field.key()) || !validId(field.value(), 0)) return false;
                           return true;
                       });
            } else if (property.value("type").toString() == QLatin1String("array")) {
                const QJsonArray items = it.value().toArray();
                valid = it.value().isArray() && items.size() <= property.value("maxItems").toInt()
                    && std::all_of(items.begin(), items.end(), [&](const QJsonValue& item) { return validId(item, 1); });
            } else {
                valid = bindingProperties.contains(it.key()) && validId(it.value(), 0);
            }
        }
        if (!valid) {
            QStringList names;
            for (const auto& item : requiredKeys) names.append(item.toString());
            return Failure("GAME_UI_ARGUMENT", std::string(key) + " 需要 " + names.join(QStringLiteral("、")).toStdString()
                + "，各项为不超过 64 字的编号。");
        }
    }
    if (args.contains("buffs")) {
        const QJsonArray buffs = args.value("buffs").toArray();
        const bool valid = args.value("buffs").isArray() && buffs.size() <= 64 && std::all_of(buffs.begin(), buffs.end(), [](const QJsonValue& item) {
            return item.isString() && item.toString().size() == 4 && !item.toString().contains(QChar::Null);
        });
        if (!valid) return Failure("GAME_UI_ARGUMENT", "buffs 最多 64 项，每项为 4 字符增减益码。");
    }
    if (args.contains("portraits")) {
        const QJsonArray units = args.value("portraits").toArray();
        const bool valid = args.value("portraits").isArray() && units.size() <= 256 && std::all_of(units.begin(), units.end(), [](const QJsonValue& item) {
            return item.isString() && item.toString().size() == 4 && !item.toString().contains(QChar::Null);
        });
        if (!valid) return Failure("GAME_UI_ARGUMENT", "portraits 最多 256 项，每项为 4 字符单位类型。");
    }
    if (args.contains("key")) {
        const QString key = args.value("key").toString();
        const bool valid = args.value("key").isString() && !key.isEmpty() && key.size() <= 24 && (key.front().isLetter() || key.front() == QLatin1Char('_'))
            && std::all_of(key.begin(), key.end(), [](QChar c) { return c.unicode() < 0x80 && (c.isLetterOrNumber() || c == QLatin1Char('_')); });
        if (!valid) return Failure("GAME_UI_ARGUMENT", "key 必须是字母或下划线开头、最长 24 的 ASCII 标识符。");
    }
    if (args.contains("far_range")) {
        const double range = args.value("far_range").toDouble(-1.0);
        if (!args.value("far_range").isDouble() || !std::isfinite(range) || range < 100.0 || range > 100000.0)
            return Failure("GAME_UI_ARGUMENT", "far_range 必须是 100 到 100000 的数值。");
    }
    if (!TalentArgumentsValid(args)) return Failure("GAME_UI_ARGUMENT", "天赋树参数无效：unlock_levels 为 1 到 7 个 1..10000 的整数；sync_unit 为 4 字符物编编号；"
        "players 为 1..24；name 为 1..32 字；after_pick 为不超过 256 字的触发器名；heroes 最多 64 个，每个有 4 字符 unit 与最多 28 项 talents，"
        "每项 tier 1..7、option 1..4、name 1..128 字，learn 为空或 4 字符。");
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
