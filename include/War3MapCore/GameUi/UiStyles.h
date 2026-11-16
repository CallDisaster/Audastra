#pragma once

#include "War3MapCore/GameUi/FrameLibrary.h"
#include "War3MapCore/GameUi/UiDescription.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

// 原版样式：从游戏进图时就读入的界面模板（FrameDef.toc 及其 IncludeFile）里挑出的常用控件，配中文名、用途与默认大小。
// 新建出来是模板控件（UiFrameType::Template），外观由游戏模板决定。界面工作区的创建卡片与助手工具共用一份。
// verified 为真的已在经典版 1.27a 实际进图看过：创建得出来、显示正常、能写文字的写上了文字。没核实的不在创建卡片里列出。
namespace War3Map::GameUi {

struct UiStyle {
    std::string_view id;            // 稳定标识（ASCII）
    std::string_view group;         // 面板、按钮、复选框……
    std::string_view label;         // 中文名
    std::string_view purpose;       // 用途
    std::string_view templateName;  // 游戏模板名
    std::string_view prefix;        // 新控件编号的前缀
    double width = 0.0;             // 0 为用模板自带的大小
    double height = 0.0;
    std::string_view text;  // 默认文字（模板能写文字时）
    bool verified = false;
};

// 按分组顺序排列。
[[nodiscard]] std::span<const UiStyle> UiStyles();
[[nodiscard]] const UiStyle* FindUiStyle(std::string_view id);
// 新建时的控件：模板控件，居中于父控件，带默认大小与文字。
[[nodiscard]] UiFrame StyleUiFrame(const UiStyle& style, std::string id);

// 模板能显示文字时文字所在的框架：空字符串为模板自身（TEXT），否则为 ButtonText 指定的子框架名（沿 INHERITS 查找）。
// 不能显示文字（或模板不在目录里）时为空。
[[nodiscard]] std::optional<std::string> UiTemplateTextTarget(const FrameLibrary& library, std::string_view templateName);

} // namespace War3Map::GameUi
