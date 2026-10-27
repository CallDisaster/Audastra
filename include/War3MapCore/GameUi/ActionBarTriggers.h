#pragma once

#include "War3MapCore/GameUi/TargetTriggers.h"
#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 技能栏的自然 T：一组技能格（数值都是数组，格数即格数），显示本机玩家“当前单位”的技能。每 0.1 秒按“登记”里该单位类型
// 的技能表把每格的显示、图标、热键、等级写进界面数值；开了技能状态时再用 YDWE 的技能属性（JAPI）读剩余冷却与法力消耗，
// 写进冷却比例、冷却文字与法力不足；然后运行该界面的“更新”触发器。生成一段功能写法，写进“技能栏”类别。
// - 当前单位同目标框架：来源为“选中”时跟玩家选中的单位，为“焦点”时由“设定”触发器指定（例如玩家的英雄）。
// - 点格子只在本机发生：选中的就是当前单位时按下这一格登记的热键（同玩家在命令面板按键：选目标、冷却、法力都由游戏判断），
//   否则先选中当前单位，再点一次放技能。热键要和物编里这个技能的热键一致。
// - “登记”触发器列出单位类型、第几格、技能、图标与热键（作者可改，重新生成时保留）。
// 同一张图可生成多份，键不同即可。触发器名为“<名字> <用途> <键>_<用途键>”，变量为 AUA_<键>_*。
namespace War3Map::GameUi {

inline constexpr std::string_view kActionBarTriggerCategory = "技能栏";
inline constexpr double kActionBarRefreshSeconds = 0.1;
inline constexpr std::uint32_t kActionBarMaxPlayers = 12;
inline constexpr std::uint32_t kActionBarMaxSlots = 24;
inline constexpr std::size_t kActionBarMaxEntries = 256;

// 绑定技能栏界面：每格的数值是数组，格数都与 shown 相同。shown 必填，其余可空。
struct ActionBarFrameBinding {
    const UiDescription* description = nullptr;
    std::string shown;         // 布尔[N]：这一格有技能（当前单位有登记的技能且等级大于 0）
    std::string barShown;      // 布尔：有当前单位
    std::string icon;          // 字符串[N]：图标路径
    std::string key;           // 字符串[N]：热键
    std::string level;         // 整数[N]：技能等级
    std::string cooldown;      // 实数[N] 0..1：剩余冷却占总冷却的比例，就绪时为 0（要开技能状态）
    std::string cooldownText;  // 字符串[N]：剩余冷却秒数，就绪时为空（要开技能状态）
    std::string noMana;        // 布尔[N]：法力不够（要开技能状态）
    std::string use;           // 点击事件编号：事件须是点击，框架在格数为 N 的重复框架里（按实例下标对应格）
};

// 登记的一格：单位类型、第几格（1 起）、技能、图标路径、热键（可空，空时点格子只选中单位）。
struct ActionBarEntry {
    std::string unit;
    std::uint32_t slot = 1;
    std::string ability;
    std::string icon;
    std::string key;
};

struct ActionBarSystemOptions {
    std::string name = "技能栏";  // 触发器名前缀与说明里的显示名
    std::string key = "Bar";       // ASCII 标识符：触发器名末尾与变量前缀，同图多份时须不同
    TargetSource source = TargetSource::Selection;
    ActionBarFrameBinding frame;
    std::uint32_t players = kActionBarMaxPlayers;  // 记录前几个玩家（1..12）的当前单位
    bool abilityState = true;                       // 用 YDWE 技能属性（JAPI）读冷却与法力消耗
    std::vector<ActionBarEntry> entries;            // 写进“登记”触发器，最多 256 格
    bool includeRegistration = true;                // 地图里已有登记触发器时为假：不生成它，保留作者的登记
};

struct ActionBarSystemPlan {
    std::string featureText;
    std::vector<std::string> triggers;   // 按文本顺序
    std::vector<std::string> variables;  // 不带 udg_，按文本顺序
    std::uint32_t slotCount = 0;         // 格数
    std::string memberVariable;          // 焦点：“设定”读取的单位变量（不带 udg_），选中为空
    std::string playerVariable;          // 焦点：“设定”读取的玩家变量（不带 udg_），选中为空
    std::string setTrigger;              // 焦点：设定触发器名，选中为空
    std::string registrationTrigger;     // 登记触发器名
};

// 选项或绑定不合法时一次列出全部错误并失败（AUA_OPTION_INVALID、AUA_FRAME_INVALID、AUA_ENTRY_INVALID），不给部分结果。
[[nodiscard]] Result<ActionBarSystemPlan> GenerateActionBarSystem(const ActionBarSystemOptions& options);

} // namespace War3Map::GameUi
