#include "War3MapCore/GameUi/UiSnippets.h"

#include "War3MapCore/GameUi/UiFrameKinds.h"

#include <algorithm>
#include <array>
#include <string>

namespace War3Map::GameUi {
namespace {

constexpr std::array<UiSnippet, 4> kSnippets{{
    {"window", "窗口", "带标题和关闭按钮的窗口，内容放进中间的“内容”容器。"},
    {"icon-grid", "图标格子", "4×3 的图标格子（一个图标控件按阵列排开），做背包、技能栏。"},
    {"tabs", "分页标签栏", "一排 4 个标签按钮加下面的页面区域。"},
    {"list-row", "列表行", "左边图标、右边文字的一行，做任务、成员、物品列表；要多行时给它设阵列。"},
}};

UiFrame Make(std::string_view kind, std::string id) {
    return DefaultUiFrame(*FindUiFrameKind(kind), std::move(id));
}

UiAnchor To(FramePoint point, FramePoint targetPoint, double x, double y) {
    UiAnchor anchor;
    anchor.point = point;
    anchor.target = UiAnchorTarget::Parent;
    anchor.targetPoint = targetPoint;
    anchor.x = x;
    anchor.y = y;
    return anchor;
}

UiFrame Sized(UiFrame frame, double width, double height, std::vector<UiAnchor> anchors) {
    frame.width = width;
    frame.height = height;
    frame.anchors = std::move(anchors);
    return frame;
}

// 两个对角锚点铺在父控件里，四周留出边距（不写宽高）。
UiFrame Inset(UiFrame frame, double left, double top, double right, double bottom) {
    frame.width.reset();
    frame.height.reset();
    frame.anchors = {To(FramePoint::TopLeft, FramePoint::TopLeft, left, -top), To(FramePoint::BottomRight, FramePoint::BottomRight, -right, bottom)};
    return frame;
}

UiFrame Window() {
    UiFrame root = Sized(Make("backdrop", "window"), 0.3, 0.22, {To(FramePoint::Center, FramePoint::Center, 0.0, 0.0)});
    root.label = "窗口";
    UiFrame title = Sized(Make("text", "title"), 0.2, 0.018, {To(FramePoint::Top, FramePoint::Top, 0.0, -0.01)});
    title.label = "标题";
    title.text = "标题";
    UiFrame close = Sized(Make("button", "close"), 0.022, 0.022, {To(FramePoint::TopRight, FramePoint::TopRight, -0.008, -0.008)});
    close.label = "关闭";
    close.text = "X";
    UiFrame body = Inset(Make("panel", "body"), 0.012, 0.036, 0.012, 0.012);
    body.label = "内容";
    root.children = {std::move(title), std::move(close), std::move(body)};
    return root;
}

UiFrame IconGrid() {
    UiFrame root = Sized(Make("panel", "grid"), 0.2, 0.158, {To(FramePoint::Center, FramePoint::Center, 0.0, 0.0)});
    root.label = "图标格子";
    UiFrame slot = Sized(Make("icon", "slot"), 0.036, 0.036, {To(FramePoint::TopLeft, FramePoint::TopLeft, 0.01, -0.01)});
    slot.label = "格子";
    slot.repeat = UiRepeat{12, 0.046, 0.0, 4, 0.0, -0.046};
    root.children = {std::move(slot)};
    return root;
}

UiFrame Tabs() {
    UiFrame root = Sized(Make("panel", "tabs"), 0.3, 0.2, {To(FramePoint::Center, FramePoint::Center, 0.0, 0.0)});
    root.label = "分页";
    UiFrame tab = Sized(Make("button", "tab"), 0.07, 0.026, {To(FramePoint::TopLeft, FramePoint::TopLeft, 0.004, 0.0)});
    tab.label = "标签";
    tab.text = "标签";
    tab.repeat = UiRepeat{4, 0.073, 0.0, 0, 0.0, 0.0};
    UiFrame page = Inset(Make("backdrop", "page"), 0.0, 0.03, 0.0, 0.0);
    page.label = "页面";
    root.children = {std::move(tab), std::move(page)};
    return root;
}

UiFrame ListRow() {
    UiFrame root = Sized(Make("panel", "row"), 0.22, 0.034, {To(FramePoint::Center, FramePoint::Center, 0.0, 0.0)});
    root.label = "列表行";
    UiFrame icon = Sized(Make("icon", "rowicon"), 0.028, 0.028, {To(FramePoint::Left, FramePoint::Left, 0.003, 0.0)});
    icon.label = "图标";
    UiFrame text = Sized(Make("text", "rowtext"), 0.17, 0.018, {To(FramePoint::Left, FramePoint::Left, 0.037, 0.0)});
    text.label = "文字";
    text.text = "名称";
    text.justifyH = JustifyH::Left;
    root.children = {std::move(icon), std::move(text)};
    return root;
}

} // namespace

std::span<const UiSnippet> UiSnippets() { return kSnippets; }

const UiSnippet* FindUiSnippet(std::string_view id) {
    const auto found = std::find_if(kSnippets.begin(), kSnippets.end(), [&](const UiSnippet& snippet) { return snippet.id == id; });
    return found == kSnippets.end() ? nullptr : &*found;
}

UiFrame SnippetUiFrame(const UiSnippet& snippet) {
    if (snippet.id == "icon-grid") return IconGrid();
    if (snippet.id == "tabs") return Tabs();
    if (snippet.id == "list-row") return ListRow();
    return Window();
}

} // namespace War3Map::GameUi
