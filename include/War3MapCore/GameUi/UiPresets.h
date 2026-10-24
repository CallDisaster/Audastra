#pragma once

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

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
// party：团队框架（5 格，每格血条、蓝条、名字，按数组取值）。
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

// 按模板新建描述；label 为空时用模板名。模板不存在或编号无效时失败。
[[nodiscard]] Result<UiDescription> NewUiDescriptionFromPreset(std::string_view preset, std::string_view id, std::string_view label);

} // namespace War3Map::GameUi
