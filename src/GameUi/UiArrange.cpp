#include "War3MapCore/GameUi/UiArrange.h"

#include <algorithm>
#include <array>
#include <numeric>

namespace War3Map::GameUi {
namespace {

constexpr std::array<std::string_view, 10> kNames{
    "left", "center_x", "right", "top", "center_y", "bottom", "distribute_x", "distribute_y", "same_width", "same_height"};

FrameRect Moved(const FrameRect& rect, float dx, float dy) {
    return {rect.left + dx, rect.bottom + dy, rect.right + dx, rect.top + dy};
}

// 等间距：按左（下）边排序，两端不动，中间的按相等的空隙排开（重叠时空隙为负）。
void Distribute(std::vector<FrameRect>& out, bool horizontal) {
    std::vector<std::size_t> order(out.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    const auto start = [horizontal](const FrameRect& rect) { return horizontal ? rect.left : rect.bottom; };
    const auto length = [horizontal](const FrameRect& rect) { return horizontal ? rect.Width() : rect.Height(); };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return start(out[a]) < start(out[b]); });
    const FrameRect& first = out[order.front()];
    const FrameRect& last = out[order.back()];
    float total = 0.0f;
    for (const std::size_t index : order) total += length(out[index]);
    const float span = (horizontal ? last.right : last.top) - start(first);
    const float gap = (span - total) / static_cast<float>(order.size() - 1);
    float cursor = start(first);
    for (const std::size_t index : order) {
        FrameRect& rect = out[index];
        const float shift = cursor - start(rect);
        rect = horizontal ? Moved(rect, shift, 0.0f) : Moved(rect, 0.0f, shift);
        cursor += length(rect) + gap;
    }
}

} // namespace

std::optional<UiArrangeMode> ParseUiArrangeMode(std::string_view name) {
    const auto found = std::find(kNames.begin(), kNames.end(), name);
    if (found == kNames.end()) return std::nullopt;
    return static_cast<UiArrangeMode>(found - kNames.begin());
}

std::string_view UiArrangeModeName(UiArrangeMode mode) {
    return kNames[static_cast<std::size_t>(mode)];
}

std::size_t UiArrangeMinimum(UiArrangeMode mode) {
    switch (mode) {
    case UiArrangeMode::DistributeX:
    case UiArrangeMode::DistributeY: return 3;
    case UiArrangeMode::SameWidth:
    case UiArrangeMode::SameHeight: return 2;
    default: return 1;
    }
}

std::vector<FrameRect> ArrangeUiRects(std::span<const FrameRect> rects, UiArrangeMode mode, std::optional<FrameRect> reference) {
    std::vector<FrameRect> out(rects.begin(), rects.end());
    if (out.size() < UiArrangeMinimum(mode)) return out;
    switch (mode) {
    case UiArrangeMode::DistributeX:
    case UiArrangeMode::DistributeY: Distribute(out, mode == UiArrangeMode::DistributeX); return out;
    case UiArrangeMode::SameWidth:
        for (FrameRect& rect : out) rect.right = rect.left + out.front().Width();
        return out;
    case UiArrangeMode::SameHeight:
        for (FrameRect& rect : out) rect.bottom = rect.top - out.front().Height();
        return out;
    default: break;
    }
    // 对齐：两个以上以合起来的范围为准，只有一个以 reference 为准。
    FrameRect bounds;
    if (out.size() == 1) {
        if (!reference) return out;
        bounds = *reference;
    } else {
        bounds = out.front();
        for (const FrameRect& rect : out) {
            bounds.left = std::min(bounds.left, rect.left);
            bounds.bottom = std::min(bounds.bottom, rect.bottom);
            bounds.right = std::max(bounds.right, rect.right);
            bounds.top = std::max(bounds.top, rect.top);
        }
    }
    for (FrameRect& rect : out) {
        switch (mode) {
        case UiArrangeMode::AlignLeft: rect = Moved(rect, bounds.left - rect.left, 0.0f); break;
        case UiArrangeMode::AlignRight: rect = Moved(rect, bounds.right - rect.right, 0.0f); break;
        case UiArrangeMode::AlignCenterX:
            rect = Moved(rect, (bounds.left + bounds.right) / 2.0f - (rect.left + rect.right) / 2.0f, 0.0f);
            break;
        case UiArrangeMode::AlignTop: rect = Moved(rect, 0.0f, bounds.top - rect.top); break;
        case UiArrangeMode::AlignBottom: rect = Moved(rect, 0.0f, bounds.bottom - rect.bottom); break;
        case UiArrangeMode::AlignCenterY:
            rect = Moved(rect, 0.0f, (bounds.bottom + bounds.top) / 2.0f - (rect.bottom + rect.top) / 2.0f);
            break;
        default: break;
        }
    }
    return out;
}

} // namespace War3Map::GameUi
