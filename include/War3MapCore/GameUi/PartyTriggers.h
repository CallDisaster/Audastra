#pragma once

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 团队框架的自然 T：一份队员名单，定时把每个队员的显示、生命、法力、名字、死亡、过远写进团队框架界面的数组数值，
// 再运行该界面的“更新”触发器；点格子选中该队员。生成一段功能写法（Trigger/TriggerFeatureText.h），写进“团队框架”类别。
// - 名单是同步数据，所有玩家相同：设好“团队 队员”变量后运行“加入”或“移出”；可选自动加入用户玩家的英雄（不含幻象），
//   开局已在地图上的与之后进入地图的都算。单位被移除后自动移出。名单顺序即格子顺序，移出时后面的格子前移。
// - “过远”按本机玩家在名单里的第一个队员量距离；点格子只在本机发生，只改本机的选择（游戏自己同步选择）。
// 触发器名为“<名字> <用途> Party_<键>”。
namespace War3Map::GameUi {

inline constexpr std::string_view kPartyTriggerCategory = "团队框架";
inline constexpr double kPartyRefreshSeconds = 0.1;

enum class PartyNameSource : std::uint8_t {
    Player,  // 所属玩家的名字
    Unit,    // 英雄用英雄名，其他单位用单位名
};

// 绑定团队框架界面：数值都是数组，格数相同，即队伍上限。shown 必填，其余可空。
struct PartyFrameBinding {
    const UiDescription* description = nullptr;
    std::string shown;       // 布尔：这一格有队员
    std::string health;      // 实数 0..1：生命比例
    std::string mana;        // 实数 0..1：法力比例（没有法力为 0）
    std::string name;        // 字符串
    std::string dead;        // 布尔：死亡
    std::string outOfRange;  // 布尔：离本机玩家的队员超过 farRange
    std::string select;      // 点击事件编号：事件须是点击，框架是重复框架（或其中的子框架），重复次数等于格数
};

struct PartySystemOptions {
    std::string name = "团队";  // 触发器名前缀与说明里的显示名
    PartyFrameBinding frame;
    PartyNameSource nameSource = PartyNameSource::Player;
    double farRange = 1200.0;  // 绑定 outOfRange 时用，100..100000
    bool autoHeroes = true;
};

struct PartySystemPlan {
    std::string featureText;
    std::vector<std::string> triggers;   // 按文本顺序
    std::vector<std::string> variables;  // 不带 udg_，按文本顺序
    std::uint32_t slotCount = 0;
    std::string memberVariable;  // “加入”“移出”读取的单位变量（不带 udg_）
    std::string joinTrigger;
    std::string leaveTrigger;
};

// 选项或绑定不合法时一次列出全部错误并失败（AUP_OPTION_INVALID、AUP_FRAME_INVALID），不给部分结果。
[[nodiscard]] Result<PartySystemPlan> GeneratePartySystem(const PartySystemOptions& options);
// “<名字> <用途> Party_<键>”，例如“团队 加入 Party_Join”。
[[nodiscard]] std::string PartyTriggerName(std::string_view name, std::string_view role, std::string_view key);

} // namespace War3Map::GameUi
