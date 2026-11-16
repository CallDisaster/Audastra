#pragma once

#include "War3MapCore/GameUi/UiDescription.h"

#include <span>
#include <string_view>

// 布局片段：常用的一组控件摆好的位置（窗口、图标格子、分页标签栏、列表行），只有布局，不带任何逻辑。
// 编辑器“添加控件”的创建卡片与助手工具共用同一份；放进界面时编号按界面里已有的换掉冲突的。
namespace War3Map::GameUi {

struct UiSnippet {
    std::string_view id;
    std::string_view label;
    std::string_view purpose;
};

[[nodiscard]] std::span<const UiSnippet> UiSnippets();
[[nodiscard]] const UiSnippet* FindUiSnippet(std::string_view id);
// 片段的根控件（带子控件），根居中于父控件。
[[nodiscard]] UiFrame SnippetUiFrame(const UiSnippet& snippet);

} // namespace War3Map::GameUi
