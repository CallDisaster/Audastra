#pragma once

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 伤害统计的自然 T：用 YDWE 的“任意单位伤害事件”累计每个玩家的单位造成的伤害（同步数据），每 0.5 秒按伤害从高到低
// 把前几名写进一份重复行界面的数值，再运行该界面的“更新”触发器。生成一段功能写法，写进“伤害统计”类别。
// - 伤害来源单位的所有者记为造成伤害的玩家（召唤物、马甲的伤害算在主人头上）；默认只记打敌人的伤害。
// - 秒数从清零后第一次记到伤害开始，每 0.5 秒加一次；每秒伤害 = 伤害 ÷ 秒数（不足 1 秒按 1 秒）。
// - 运行“<名字> 清零 <键>_Reset”触发器把所有玩家的伤害与秒数清零（例如首领战开始时）。
// - 颜色条按玩家编号取默认队伍颜色（TeamColor00..11）；地图改过玩家颜色时不跟着变。
// - 原版没有治疗事件，不统计治疗。
// 同一张图可生成多份，键不同即可。触发器名为“<名字> <用途> <键>_<用途键>”，变量为 AUM_<键>_*。
namespace War3Map::GameUi {

inline constexpr std::string_view kMeterTriggerCategory = "伤害统计";
inline constexpr double kMeterRefreshSeconds = 0.5;
inline constexpr std::uint32_t kMeterMaxPlayers = 12;
inline constexpr std::uint32_t kMeterMaxRows = 12;

// 绑定伤害统计界面：shown 为布尔数组，格数就是行数（1..12）；其余每行的数值是同样格数的数组，总量与秒数是单值，都可空。
struct MeterFrameBinding {
    const UiDescription* description = nullptr;
    std::string shown;       // 布尔数组：这一行有人
    std::string name;        // 字符串数组：玩家名
    std::string amount;      // 实数数组：伤害
    std::string amountText;  // 字符串数组：伤害的文字，一万以上写作“1.2万”
    std::string share;       // 实数数组 0..1：占第一名的比例（进度条用）
    std::string percent;     // 实数数组 0..100：占全部伤害的百分比
    std::string rate;        // 实数数组：每秒伤害
    std::string color;       // 字符串数组：玩家的队伍颜色贴图路径
    std::string total;       // 实数：全部伤害
    std::string time;        // 实数：秒数
};

struct MeterSystemOptions {
    std::string name = "伤害";   // 触发器名前缀与说明里的显示名
    std::string key = "Damage";  // ASCII 标识符：触发器名末尾与变量前缀，同图多份时须不同
    MeterFrameBinding frame;
    std::uint32_t players = kMeterMaxPlayers;  // 统计前几个玩家（1..12）
    bool enemiesOnly = true;                   // 只记打敌人的伤害
};

struct MeterSystemPlan {
    std::string featureText;
    std::vector<std::string> triggers;   // 按文本顺序
    std::vector<std::string> variables;  // 不带 udg_，按文本顺序
    std::uint32_t rows = 0;
    std::string resetTrigger;            // 清零触发器名
    std::string amountVariable;          // 每个玩家的伤害（实数数组，下标为玩家编号 1 起，不带 udg_）
};

// 选项或绑定不合法时一次列出全部错误并失败（AUM_OPTION_INVALID、AUM_FRAME_INVALID），不给部分结果。
[[nodiscard]] Result<MeterSystemPlan> GenerateMeterSystem(const MeterSystemOptions& options);

// “<名字> <用途> <键>_<用途键>”，例如“伤害 刷新 Damage_Refresh”。
[[nodiscard]] std::string MeterTriggerName(std::string_view name, std::string_view key, std::string_view role, std::string_view roleKey);

} // namespace War3Map::GameUi
