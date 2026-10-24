#pragma once

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 读条系统：生成自然 T（功能写法，Trigger/TriggerFeatureText.h），写进触发器编辑器的“读条系统”类别。
//
// 逻辑层不依赖界面库。按施法事件给每个施法者记一份读条（施法单位、技能、等级、目标、阶段、开始时刻、时长、跳数），
// 在三个时机运行该技能登记的效果触发器：完成（读条结束，或引导走完）、引导每跳、打断（读条或引导中途停下、施法者死亡）。
// 参数用 YDWE“运行触发器（带参数）”的参数块传入，效果触发器里用“获取逆天局部变量”按名字取：
//   施法单位、目标单位（单位）；目标X、目标Y（实数）；技能、等级、阶段、跳数（整数）。阶段：1 完成、2 每跳、3 打断。
// 读条时间与引导时间以物编为准（魔法施放时间、持续时间），登记里的秒数用于计时与显示，须与物编一致；
// 系统不下命令、不打断单位，移动或停止是否打断由游戏决定。引导每跳由周期刷新判定，最后一跳在引导正常结束时补齐。
//
// 可选地绑定一份已生成界面触发器的界面描述（读条条）：显示触发器只在本机按本机玩家最近一次读条写入界面数值，
// 再运行界面的更新与状态触发器；同步数据只在施法事件里改。
namespace War3Map::GameUi {

inline constexpr std::string_view kCastTriggerCategory = "读条系统";
inline constexpr int kCastPhaseFinish = 1;
inline constexpr int kCastPhaseTick = 2;
inline constexpr int kCastPhaseInterrupt = 3;
// 刷新周期；跳间隔不得小于它。
inline constexpr double kCastRefreshSeconds = 0.03;

struct CastAbility {
    std::string ability;          // 技能四字码，例如 A000
    double castSeconds = 0.0;     // 读条秒数；0 为瞬发（不显示读条）
    double channelSeconds = 0.0;  // 引导秒数；0 为不引导（读条结束即完成）
    double tickSeconds = 0.0;     // 引导每跳间隔；0 为不跳
    std::string effectTrigger;    // 效果触发器名（地图里已有的触发器）
};

// 读条条：界面描述的数值与状态编号。界面触发器须已用 GenerateUiTriggers 生成（更新触发器与数值变量按同一规则命名）。
struct CastBarBinding {
    const UiDescription* description = nullptr;
    std::string visible;      // 布尔：是否显示（必填）
    std::string progress;     // 实数：进度 0..1，引导时从满到空（必填）
    std::string text;         // 字符串：技能名；打断时为技能名（可空）
    std::string remaining;    // 实数：剩余秒数（可空）
    std::string channel;      // 状态：引导中（可空）
    std::string interrupted;  // 状态：打断提示（可空）
    double flashSeconds = 0.5;  // 打断提示显示多久
};

struct CastSystemOptions {
    std::string name = "读条";  // 触发器名前缀
    std::vector<CastAbility> abilities;  // 登记触发器里的初始登记
    // 为假时不生成登记触发器：地图里已有登记触发器时保留作者写的登记。
    bool includeRegistration = true;
    std::optional<CastBarBinding> bar;
};

struct CastSystemPlan {
    std::string featureText;
    std::vector<std::string> triggers;   // 按文本顺序
    std::vector<std::string> variables;  // 不带 udg_，按文本顺序
    std::string registrationTrigger;     // 登记触发器名（未生成时也给出，便于查找已有的）
};

// 选项不合法（技能码、秒数、效果触发器名、界面绑定的数值类型或状态不存在）时一次列出全部问题并失败，不给部分结果。
[[nodiscard]] Result<CastSystemPlan> GenerateCastSystem(const CastSystemOptions& options);

// 触发器名：“<前缀> <用途> Cast_<键>”，例如“读条 开始 Cast_Begin”；末尾 ASCII 部分让脚本名 gg_trg_ 互不相同。
[[nodiscard]] std::string CastTriggerName(std::string_view name, std::string_view role, std::string_view key);

} // namespace War3Map::GameUi
