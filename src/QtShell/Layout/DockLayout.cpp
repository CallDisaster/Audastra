#include "Layout/DockLayout.h"

#include <algorithm>
#include <set>

namespace War3Map::Shell {
namespace {

bool Contains(const std::vector<ModuleId>& list, const ModuleId& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

auto FindFloat(std::vector<FloatWindow>& floats, const ModuleId& id) {
    return std::find_if(floats.begin(), floats.end(), [&](const FloatWindow& item) { return item.id == id; });
}

auto FindFloat(const std::vector<FloatWindow>& floats, const ModuleId& id) {
    return std::find_if(floats.begin(), floats.end(), [&](const FloatWindow& item) { return item.id == id; });
}

DockLayout PresetBase(const DockLayout& current) {
    DockLayout layout = DefaultLayout();
    layout.sideWidth = current.sideWidth;
    layout.sizes = current.sizes;
    return layout;
}

DockLayout WithSide(DockLayout layout, const ModuleId& main, const ModuleId& side) {
    layout.mainActive = main;
    layout.side = {side};
    layout.sideActive = side;
    return layout;
}

} // namespace

DockLayout DefaultLayout() {
    return {};
}

Area WhereIs(const DockLayout& layout, const ModuleId& id) {
    if (Contains(layout.side, id)) {
        return Area::Side;
    }
    if (FindFloat(layout.floats, id) != layout.floats.end()) {
        return Area::Float;
    }
    return Area::Main;
}

std::vector<ModuleId> MainModules(const DockLayout& layout, const std::vector<ModuleId>& all) {
    std::vector<ModuleId> result;
    for (const auto& id : all) {
        if (WhereIs(layout, id) == Area::Main) {
            result.push_back(id);
        }
    }
    return result;
}

std::optional<ModuleId> MainShown(const DockLayout& layout, const std::vector<ModuleId>& all) {
    if (WhereIs(layout, layout.mainActive) == Area::Main && Contains(all, layout.mainActive)) {
        return layout.mainActive;
    }
    const auto main = MainModules(layout, all);
    return main.empty() ? std::nullopt : std::optional<ModuleId>(main.front());
}

std::vector<ModuleId> VisibleModules(const DockLayout& layout, const std::vector<ModuleId>& all) {
    std::vector<ModuleId> result;
    if (const auto shown = MainShown(layout, all)) {
        result.push_back(*shown);
    }
    if (layout.sideActive) {
        result.push_back(*layout.sideActive);
    }
    for (const auto& window : layout.floats) {
        result.push_back(window.id);
    }
    return result;
}

DockLayout Detach(const DockLayout& layout, const ModuleId& id) {
    DockLayout next = layout;
    const auto sideIt = std::find(next.side.begin(), next.side.end(), id);
    if (sideIt != next.side.end()) {
        const auto index = static_cast<std::size_t>(sideIt - next.side.begin());
        next.side.erase(sideIt);
        // 移走右侧活动标签时激活相邻标签（优先右侧）。
        if (next.sideActive == id) {
            next.sideActive = next.side.empty()
                ? std::nullopt
                : std::optional<ModuleId>(next.side[std::min(index, next.side.size() - 1)]);
        }
    }
    const auto floatIt = FindFloat(next.floats, id);
    if (floatIt != next.floats.end()) {
        next.sizes[id] = {floatIt->w, floatIt->h};
        next.floats.erase(floatIt);
    }
    return next;
}

DockLayout Place(const DockLayout& layout, const ModuleId& id, const Placement& target) {
    DockLayout next = Detach(layout, id);
    switch (target.area) {
    case Area::Main:
        next.mainActive = id;
        return next;
    case Area::Float: {
        const auto remembered = next.sizes.find(id);
        FloatWindow window;
        window.id = id;
        window.x = target.x;
        window.y = target.y;
        window.w = target.w.value_or(remembered != next.sizes.end() ? remembered->second.w : 900);
        window.h = target.h.value_or(remembered != next.sizes.end() ? remembered->second.h : 600);
        next.floats.push_back(window);
        return next;
    }
    case Area::Side: {
        const int size = static_cast<int>(next.side.size());
        const int index = std::clamp(target.index.value_or(size), 0, size);
        next.side.insert(next.side.begin() + index, id);
        next.sideActive = id;
        return next;
    }
    }
    return next;
}

DockLayout Activate(const DockLayout& layout, const ModuleId& id, bool reveal) {
    DockLayout next = layout;
    switch (WhereIs(layout, id)) {
    case Area::Float: {
        const auto it = FindFloat(next.floats, id);
        FloatWindow window = *it;
        if (reveal) {
            window.collapsed = false;
        }
        next.floats.erase(it);
        next.floats.push_back(window);
        return next;
    }
    case Area::Side:
        next.sideActive = id;
        return next;
    case Area::Main:
        next.mainActive = id;
        return next;
    }
    return next;
}

DockLayout UpdateFloat(const DockLayout& layout, const FloatWindow& window) {
    DockLayout next = layout;
    const auto it = FindFloat(next.floats, window.id);
    if (it != next.floats.end()) {
        *it = window;
    }
    return next;
}

FloatWindow ClampFloat(const FloatWindow& window, const Bounds& bounds) {
    FloatWindow result = window;
    result.w = std::max(kMinFloatWidth, std::min(window.w, bounds.width));
    result.h = std::max(kMinFloatHeight, std::min(window.h, bounds.height));
    result.x = std::max(bounds.x + 80 - result.w, std::min(window.x, bounds.x + bounds.width - 80));
    result.y = std::max(bounds.y, std::min(window.y, bounds.y + bounds.height - 36));
    return result;
}

const std::vector<LayoutPreset>& LayoutPresets() {
    static const std::vector<LayoutPreset> presets{
        {"single", "单编辑区", "全部放回主编辑区"},
        {"triggers-manager", "触发器 + 物体管理器", "右侧查引用"},
        {"objects-manager", "物体 + 物体管理器", "改物编时看引用"},
        {"triggers-variables", "触发器 + 变量", "写触发时管理变量"},
    };
    return presets;
}

std::optional<DockLayout> MakePreset(const std::string& presetId, const DockLayout& current) {
    if (presetId == "single") {
        DockLayout layout = PresetBase(current);
        layout.mainActive = current.mainActive;
        return layout;
    }
    if (presetId == "triggers-manager") {
        return WithSide(PresetBase(current), "triggers", "manager");
    }
    if (presetId == "objects-manager") {
        return WithSide(PresetBase(current), "objects", "manager");
    }
    if (presetId == "triggers-variables") {
        return WithSide(PresetBase(current), "triggers", "variables");
    }
    return std::nullopt;
}

DockLayout Sanitize(const DockLayout& layout, const std::vector<ModuleId>& known) {
    const DockLayout base = DefaultLayout();
    std::set<ModuleId> seen;
    DockLayout result;
    result.mainActive = Contains(known, layout.mainActive) ? layout.mainActive : base.mainActive;
    for (const auto& id : layout.side) {
        if (Contains(known, id) && seen.insert(id).second) {
            result.side.push_back(id);
        }
    }
    for (const auto& window : layout.floats) {
        if (Contains(known, window.id) && window.w > 0 && window.h > 0 && seen.insert(window.id).second) {
            result.floats.push_back(window);
        }
    }
    result.sideActive = layout.sideActive && Contains(result.side, *layout.sideActive)
        ? layout.sideActive
        : (result.side.empty() ? std::nullopt : std::optional<ModuleId>(result.side.front()));
    result.sideWidth = std::max(kMinSideWidth, layout.sideWidth);
    for (const auto& [id, size] : layout.sizes) {
        if (Contains(known, id) && size.w > 0 && size.h > 0) {
            result.sizes.emplace(id, size);
        }
    }
    return result;
}

} // namespace War3Map::Shell
