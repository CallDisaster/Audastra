#pragma once

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

// 界面描述的起始模板：新建时直接得到一份可用的描述（框架、数值、状态都已连好），作者再改。
// 只用游戏自带的贴图，数值编号与读条系统、团队框架的生成器默认绑定的编号一致。
namespace War3Map::GameUi {

struct UiPreset {
    std::string_view id;
    std::string_view label;
};

// castbar：读条条（显示、进度、技能名、剩余秒数；引导、打断两个状态）。
// party：团队框架（5 格，每格名字、血条、蓝条、死亡与过远标记、右侧 4 个增减益图标，按数组取值；点格子发 select 事件）。
// talents：统一天赋树（技能栏上方的入口按钮，中间偏左的面板：5 层 × 3 选项，已选描边、不可选变灰，悬停在右侧显示说明）。
[[nodiscard]] const std::vector<UiPreset>& UiPresets();

// castbar 模板的数值与状态编号（绑定读条系统用）。
namespace CastBarPreset {
inline constexpr std::string_view kVisible = "shown";
inline constexpr std::string_view kProgress = "progress";
inline constexpr std::string_view kText = "text";
inline constexpr std::string_view kRemaining = "remaining";
inline constexpr std::string_view kChannel = "channel";
inline constexpr std::string_view kInterrupted = "interrupt";
} // namespace CastBarPreset

// party 模板的数值与事件编号（绑定团队框架系统用）；数值都是数组，格数为 kSlots。
namespace PartyPreset {
inline constexpr std::uint32_t kSlots = 5;
inline constexpr std::string_view kShown = "shown";
inline constexpr std::string_view kHealth = "hp";
inline constexpr std::string_view kMana = "mp";
inline constexpr std::string_view kName = "name";
inline constexpr std::string_view kDead = "dead";
inline constexpr std::string_view kFar = "far";
inline constexpr std::string_view kSelect = "select";
inline constexpr std::array<std::string_view, 4> kBuffs{"buff1", "buff2", "buff3", "buff4"};  // 增减益图标路径（文字）
} // namespace PartyPreset

// talents 模板（统一天赋树）的数值与事件编号（绑定天赋系统用）。每层一行（重复框架），行内每列一个选项；
// 每列的图标、名字、已选为数组，格数为层数；点击、移入、移出事件按行号区分层。
namespace TalentPreset {
inline constexpr std::uint32_t kTiers = 5;
inline constexpr std::uint32_t kOptions = 3;
inline constexpr std::string_view kShown = "shown";
inline constexpr std::string_view kEntryShown = "entry_shown";
inline constexpr std::string_view kEntry = "entry";
inline constexpr std::string_view kTitle = "title";
inline constexpr std::string_view kPoints = "points";
inline constexpr std::string_view kLevel = "level";
inline constexpr std::array<std::string_view, kOptions> kIcons{"icon1", "icon2", "icon3"};
inline constexpr std::array<std::string_view, kOptions> kNames{"name1", "name2", "name3"};
inline constexpr std::array<std::string_view, kOptions> kPicked{"picked1", "picked2", "picked3"};
inline constexpr std::array<std::string_view, kOptions> kPicks{"pick1", "pick2", "pick3"};
inline constexpr std::array<std::string_view, kOptions> kEnters{"enter1", "enter2", "enter3"};
inline constexpr std::array<std::string_view, kOptions> kLeaves{"leave1", "leave2", "leave3"};
inline constexpr std::string_view kOpen = "open";
inline constexpr std::string_view kClose = "close";
inline constexpr std::string_view kTipShown = "tip_shown";
inline constexpr std::string_view kTipTitle = "tip_title";
inline constexpr std::string_view kTipText = "tip_text";
} // namespace TalentPreset

// 按模板新建描述；label 为空时用模板名。模板不存在或编号无效时失败。
[[nodiscard]] Result<UiDescription> NewUiDescriptionFromPreset(std::string_view preset, std::string_view id, std::string_view label);

} // namespace War3Map::GameUi
