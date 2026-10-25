#pragma once

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 统一天赋树的自然 T：所有英雄共用一套界面与触发器，每个英雄只登记自己的天赋表。生成一段功能写法
// （Trigger/TriggerFeatureText.h），写进“天赋系统”类别。
// - 规则：英雄达到某层的解锁等级就开放这一层；每层只能选一次，选定后不能改；已开放、尚未选择的层都可以选。
//   可选数 = 已开放层数 − 已选层数。
// - 选择结果是同步数据，所有玩家相同：AUT_Picked[玩家编号×8 + 层] 存选中的选项号（1 起），0 为未选。
//   英雄技能触发器用条件判断它来改变行为。
// - 同步：界面点击只在本机发生。开局为每个用户玩家建“层数 × 选项数”个同步单位（透明、无敌、暂停、不占人口、
//   放在地图左下角之外），本机点了选项就把对应的同步单位加进本机选择；游戏同步选择，“选择”触发器在所有玩家上
//   收到选择单位事件，校验后写入结果，再把同步单位移出选择。不用平台或扩展接口。
// - 英雄绑定：每个玩家一名英雄。开局已在地图上和之后进入地图的用户玩家英雄（不含幻象），类型登记过、
//   该玩家还没有英雄时自动绑定；也可设好 AUT_Unit 后运行“绑定”。
// - 登记：每个英雄类型一组，每个选项一组名字、图标、灰色图标、说明和可选的学会技能（作者可改，重新生成时保留）。
// 触发器名为“<名字> <用途> Talent_<键>”。
namespace War3Map::GameUi {

inline constexpr std::string_view kTalentTriggerCategory = "天赋系统";
inline constexpr std::uint32_t kTalentMaxTiers = 7;
inline constexpr std::uint32_t kTalentMaxOptions = 4;
inline constexpr std::uint32_t kTalentMaxPlayers = 24;
inline constexpr std::uint32_t kTalentMaxHeroes = 64;

// 一列选项的绑定：数值都是数组，格数为层数；事件的框架须在重复“层数”次的框架里，点第几行即第几层。
struct TalentColumnBinding {
    std::string icon;    // 文字：图标路径；不可选时换成登记的灰色图标，没有登记该选项时为空
    std::string name;    // 文字：名字；不可选时为灰色
    std::string picked;  // 布尔：已选这一项
    std::string pick;    // 点击事件
    std::string enter;   // 移入事件（可空）
    std::string leave;   // 移出事件（可空）
};

// 绑定天赋树界面。shown、level、columns 必填，其余可空。
struct TalentFrameBinding {
    const UiDescription* description = nullptr;
    std::string shown;       // 布尔：面板显示
    std::string entryShown;  // 布尔：入口显示（本机玩家绑定了英雄）
    std::string entry;       // 文字：入口文字，有可选天赋时带个数
    std::string title;       // 文字：标题（英雄名）
    std::string points;      // 文字：可选数，没有可选时为空
    std::string level;       // 文字数组：每层的解锁等级，格数即层数
    std::vector<TalentColumnBinding> columns;  // 每列一组，列数即每层选项数
    std::string open;        // 点击事件：入口，开关面板
    std::string close;       // 点击事件：关闭面板
    std::string tipShown;    // 布尔：说明框显示
    std::string tipTitle;    // 文字：说明框标题
    std::string tipText;     // 文字：说明框正文
};

// 登记的一个选项：层、列从 1 起。
struct TalentEntry {
    std::uint32_t tier = 1;
    std::uint32_t option = 1;
    std::string name;
    std::string icon;
    std::string disabledIcon;  // 空时按 CommandButtons\BTN → CommandButtonsDisabled\DISBTN 推出，推不出时用 icon
    std::string text;          // 说明
    std::string learn;         // 选定后学会的技能（4 字物编编号），空表示只记录选择
};

struct TalentHero {
    std::string unitType;  // 英雄类型（4 字物编编号）
    std::string label;     // 登记触发器里的注释，可空
    std::vector<TalentEntry> entries;
};

struct TalentSystemOptions {
    std::string name = "天赋";  // 触发器名前缀与说明里的显示名
    TalentFrameBinding frame;
    std::vector<std::uint32_t> unlockLevels;  // 每层的解锁等级，个数等于层数；空为 5、10、15…
    std::string syncUnitType = "hfoo";        // 同步单位的类型：须可被选中（不能带蝗虫）
    std::uint32_t players = 12;               // 为 玩家 1..players 登记选择事件、建同步单位
    std::string afterPick;                    // 选定后运行的作者触发器名，可空；运行前设好 AUT_PickUnit/Tier/Option
    std::vector<TalentHero> heroes;           // 写进“登记”触发器，最多 kTalentMaxHeroes 个
    bool includeRegistration = true;          // 地图里已有登记触发器时为假：不生成它，保留作者的登记
};

struct TalentSystemPlan {
    std::string featureText;
    std::vector<std::string> triggers;   // 按文本顺序
    std::vector<std::string> variables;  // 不带 udg_，按文本顺序
    std::uint32_t tiers = 0;
    std::uint32_t options = 0;
    std::string pickedVariable;  // 选择结果数组（不带 udg_），下标 玩家编号×8 + 层
    std::string bindTrigger;
    std::string registrationTrigger;
};

// 选项或绑定不合法时一次列出全部错误并失败（AUT_OPTION_INVALID、AUT_FRAME_INVALID、AUT_ENTRY_INVALID），不给部分结果。
[[nodiscard]] Result<TalentSystemPlan> GenerateTalentSystem(const TalentSystemOptions& options);
// “<名字> <用途> Talent_<键>”，例如“天赋 绑定 Talent_Bind”。
[[nodiscard]] std::string TalentTriggerName(std::string_view name, std::string_view role, std::string_view key);
// CommandButtons\BTNxxx → CommandButtonsDisabled\DISBTNxxx（不分大小写）；不是这种路径时原样返回。
[[nodiscard]] std::string TalentDisabledIcon(std::string_view icon);

} // namespace War3Map::GameUi
