#pragma once

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/GameUi/UnitDisplay.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 目标框架的自然 T：一个单位框架（数值都是单值），显示本机玩家的“目标”。每 0.1 秒把目标的显示、生命、法力、生命与法力的数字、
// 名字、等级、死亡、头像、增减益写进界面数值，再运行该界面的“更新”触发器。生成一段功能写法，写进“目标框架”类别。
// - 目标是同步数据，每个玩家一份：来源为“选中”时，玩家选中单位（游戏同步的选择事件）就记下它，取消选择时清掉；来源为“焦点”时，
//   设好“<键> 单位”“<键> 玩家”变量后运行“设定”触发器（单位为空时清除）。单位被移除后自动清除。本机只显示本机玩家那一份。
// - 点框架只在本机发生：本机举着单位目标技能或物品时用 AudastraUIClickUnit 把点击交给游戏作为目标，否则选中目标。
// - 增减益与头像同团队框架：“登记”“头像”触发器列出增减益、单位类型与图标（作者可改，重新生成时保留）。
// 同一张图可生成多份（例如“目标”与“焦点”），键不同即可。触发器名为“<名字> <用途> <键>_<用途键>”，变量为 AUF_<键>_*。
namespace War3Map::GameUi {

inline constexpr std::string_view kTargetTriggerCategory = "目标框架";
inline constexpr double kTargetRefreshSeconds = 0.1;
inline constexpr std::uint32_t kTargetMaxPlayers = 12;

enum class TargetSource : std::uint8_t {
    Selection,  // 玩家选中的单位
    Focus,      // 触发器设定的焦点单位
};

// 绑定目标框架界面：数值都是单值。shown 必填，其余可空。
struct TargetFrameBinding {
    const UiDescription* description = nullptr;
    std::string shown;      // 布尔：有目标
    std::string health;     // 实数 0..1：生命比例
    std::string mana;       // 实数 0..1：法力比例（没有法力为 0）
    std::string healthNow;  // 实数：当前生命
    std::string healthMax;  // 实数：最大生命
    std::string manaNow;    // 实数：当前法力
    std::string manaMax;    // 实数：最大法力
    std::string name;       // 字符串
    std::string level;      // 整数：英雄为英雄等级，其他单位为单位等级
    std::string dead;       // 布尔：死亡
    std::string portrait;   // 字符串：单位类型的头像图标路径，没登记时为空
    std::string select;     // 点击事件编号：事件须是点击，框架不在重复框架里
    std::vector<std::string> buffIcons;  // 字符串：第 1、2…个增减益的图标路径，没有时为空
};

struct TargetSystemOptions {
    std::string name = "目标";  // 触发器名前缀与说明里的显示名
    std::string key = "Target";  // ASCII 标识符：触发器名末尾与变量前缀，同图多份时须不同
    TargetSource source = TargetSource::Selection;
    TargetFrameBinding frame;
    UnitNameSource nameSource = UnitNameSource::Unit;
    std::uint32_t players = kTargetMaxPlayers;  // 记录前几个玩家（1..12）的目标
    std::vector<UnitBuff> buffs;                // 写进“登记”触发器，最多 64 个
    bool includeRegistration = true;            // 地图里已有登记触发器时为假：不生成它，保留作者的登记
    std::vector<UnitPortrait> portraits;        // 写进“头像”触发器，最多 256 个
    bool includePortraitRegistration = true;    // 地图里已有头像触发器时为假：不生成它，保留作者的头像表
};

struct TargetSystemPlan {
    std::string featureText;
    std::vector<std::string> triggers;   // 按文本顺序
    std::vector<std::string> variables;  // 不带 udg_，按文本顺序
    std::string memberVariable;          // 焦点：“设定”读取的单位变量（不带 udg_），选中为空
    std::string playerVariable;          // 焦点：“设定”读取的玩家变量（不带 udg_），选中为空
    std::string setTrigger;              // 焦点：设定触发器名，选中为空
    std::string registrationTrigger;     // 绑定了图标时的登记触发器名，否则为空
    std::string portraitTrigger;         // 绑定了头像时的头像触发器名，否则为空
};

// 选项或绑定不合法时一次列出全部错误并失败（AUF_OPTION_INVALID、AUF_FRAME_INVALID、AUF_BUFF_INVALID、AUF_PORTRAIT_INVALID），
// 不给部分结果。
[[nodiscard]] Result<TargetSystemPlan> GenerateTargetSystem(const TargetSystemOptions& options);

// “<名字> <用途> <键>_<用途键>”，例如“目标 刷新 Target_Refresh”。
[[nodiscard]] std::string TargetTriggerName(std::string_view name, std::string_view key, std::string_view role, std::string_view roleKey);

} // namespace War3Map::GameUi
