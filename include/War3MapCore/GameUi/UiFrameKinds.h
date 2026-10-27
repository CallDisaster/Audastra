#pragma once

#include "War3MapCore/GameUi/UiDescription.h"

#include <span>
#include <string>
#include <string_view>

// 可新建的界面类型：按用途分组，带作用、用法、作者可换的项与默认外观。界面工作区的右键创建与助手工具共用一份；
// 类型只决定新建时的默认框架，存进描述的仍是 UiFrameType 与字段，不另存“类型”。
namespace War3Map::GameUi {

enum class UiFrameSlotKind : std::uint8_t {
    Texture,  // 贴图路径（可写文字模板按数值换图）
    Icon,     // 图标贴图（命令按钮图标一类）
    Text,     // 文字模板
    Number,   // 数值或取值
    Flag,     // 显示开关或取值
};

// 作者可换的一项：字段名与描述文件一致。
struct UiFrameSlot {
    std::string_view field;
    std::string_view label;
    UiFrameSlotKind kind = UiFrameSlotKind::Texture;
    bool bindable = false;  // 可取自数值
};

struct UiFrameKind {
    std::string_view id;
    std::string_view group;
    std::string_view label;
    std::string_view purpose;  // 作用
    std::string_view usage;    // 用法
    UiFrameType type = UiFrameType::Panel;
    std::span<const UiFrameSlot> swappable;
};

// 按分组顺序排列。
[[nodiscard]] std::span<const UiFrameKind> UiFrameKinds();
[[nodiscard]] const UiFrameKind* FindUiFrameKind(std::string_view id);
// 新建时的默认框架：居中对齐父框架，带能看见的默认外观；模板类型用 templateName。
[[nodiscard]] UiFrame DefaultUiFrame(const UiFrameKind& kind, std::string id, std::string templateName = {});
// 新框架编号的前缀（同一描述里按前缀取下一个空号）。
[[nodiscard]] std::string_view UiFrameIdPrefix(const UiFrameKind& kind);

} // namespace War3Map::GameUi
