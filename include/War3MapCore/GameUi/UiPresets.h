#pragma once

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

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
// party：团队框架（5 格，每格名字、血条、蓝条、死亡与过远标记，按数组取值；点格子发 select 事件）。
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
} // namespace PartyPreset

// 按模板新建描述；label 为空时用模板名。模板不存在或编号无效时失败。
[[nodiscard]] Result<UiDescription> NewUiDescriptionFromPreset(std::string_view preset, std::string_view id, std::string_view label);

} // namespace War3Map::GameUi
