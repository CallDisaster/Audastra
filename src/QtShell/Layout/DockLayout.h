#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

// 工作台停靠布局（与 Web 原型 editor-ui/src/workbench/dockLayout.ts 同一套规则）。
// 所有模块常驻在顶栏切换器里，没有“打开 / 关闭”这一层；每个模块只在一处：
// 主编辑区（默认）、右侧停靠组或浮动窗口。主编辑区一次显示一个模块，其余主区模块保持存在但隐藏。
// 纯数据与纯函数，不依赖 Qt；界面只把拖动 / 点击翻译成这些操作。
namespace War3Map::Shell {

using ModuleId = std::string;

struct FloatSize {
    int w = 0;
    int h = 0;
    bool operator==(const FloatSize&) const = default;
};

// 浮动窗口是系统顶层窗口，坐标为屏幕坐标。collapsed：收起为标题栏。
struct FloatWindow {
    ModuleId id;
    int x = 0;
    int y = 0;
    int w = 900;
    int h = 600;
    bool maximized = false;
    bool collapsed = false;
    bool operator==(const FloatWindow&) const = default;
};

struct DockLayout {
    // 主编辑区当前显示的模块；若它已浮动或停靠到右侧，显示时回退到主区第一个模块。
    ModuleId mainActive = "triggers";
    std::vector<ModuleId> side;
    std::optional<ModuleId> sideActive;
    int sideWidth = 520;
    // 数组顺序即前后次序，最后一个在最前。
    std::vector<FloatWindow> floats;
    // 每个模块上次浮动时的尺寸：放回后再拖出时沿用。
    std::map<ModuleId, FloatSize> sizes;
    bool operator==(const DockLayout&) const = default;
};

enum class Area { Main, Side, Float };

struct Placement {
    Area area = Area::Main;
    std::optional<int> index; // Side：插入位置，缺省为末尾
    int x = 0;                // Float：屏幕坐标
    int y = 0;
    std::optional<int> w;     // Float：缺省沿用上次尺寸，再缺省 900 × 600
    std::optional<int> h;

    static Placement Main() { return {}; }
    static Placement Side(std::optional<int> index = std::nullopt) { return {Area::Side, index}; }
    static Placement Float(int x, int y, std::optional<int> w = std::nullopt, std::optional<int> h = std::nullopt) {
        return {Area::Float, std::nullopt, x, y, w, h};
    }
};

struct Bounds {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

inline constexpr int kMinSideWidth = 320;
inline constexpr int kMinFloatWidth = 360;
inline constexpr int kMinFloatHeight = 240;

[[nodiscard]] DockLayout DefaultLayout();
[[nodiscard]] Area WhereIs(const DockLayout& layout, const ModuleId& id);
[[nodiscard]] std::vector<ModuleId> MainModules(const DockLayout& layout, const std::vector<ModuleId>& all);
// 主编辑区实际显示的模块；全部模块都浮动或停靠到右侧时为空。
[[nodiscard]] std::optional<ModuleId> MainShown(const DockLayout& layout, const std::vector<ModuleId>& all);
// 当前可见的模块：主区显示的、右侧活动标签、全部浮动窗口。
[[nodiscard]] std::vector<ModuleId> VisibleModules(const DockLayout& layout, const std::vector<ModuleId>& all);

// 从右侧组与浮动窗口中移出（回到主编辑区，但不切换主区显示）；记住浮动尺寸。
[[nodiscard]] DockLayout Detach(const DockLayout& layout, const ModuleId& id);
[[nodiscard]] DockLayout Place(const DockLayout& layout, const ModuleId& id, const Placement& target);
// 切到模块：浮动窗口提到最前，右侧组中则激活，主区模块在主编辑区显示。
// reveal：从切换器、搜索或引用打开时同时展开收起的窗口；单纯点击窗口只调整前后次序。
[[nodiscard]] DockLayout Activate(const DockLayout& layout, const ModuleId& id, bool reveal = false);
[[nodiscard]] DockLayout UpdateFloat(const DockLayout& layout, const FloatWindow& window);
// 浮动窗口限制在屏幕可用区域内：至少保留标题栏可见，避免拖出屏幕后找不回来。
[[nodiscard]] FloatWindow ClampFloat(const FloatWindow& window, const Bounds& bounds);

struct LayoutPreset {
    std::string id;
    std::string label;
    std::string detail;
};
// 布局预设（类似 Photoshop 的“工作区”）：保留窗口尺寸记忆与右侧宽度。
[[nodiscard]] const std::vector<LayoutPreset>& LayoutPresets();
[[nodiscard]] std::optional<DockLayout> MakePreset(const std::string& presetId, const DockLayout& current);

// 丢弃未知模块、重复位置与非法尺寸，保证每个模块只在一处。
[[nodiscard]] DockLayout Sanitize(const DockLayout& layout, const std::vector<ModuleId>& known);

} // namespace War3Map::Shell
