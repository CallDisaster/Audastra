#pragma once

#include "War3MapCore/GameUi/FrameTree.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// 多个控件的对齐、等间距分布与同宽同高：只算新矩形，不改界面；界面工作区与助手工具共用。
namespace War3Map::GameUi {

enum class UiArrangeMode : std::uint8_t {
    AlignLeft,
    AlignCenterX,
    AlignRight,
    AlignTop,
    AlignCenterY,
    AlignBottom,
    DistributeX,  // 横向等间距：最左、最右不动
    DistributeY,  // 纵向等间距：最上、最下不动
    SameWidth,    // 宽度取第一个，左边不动
    SameHeight,   // 高度取第一个，上边不动
};

// 名称：left、center_x、right、top、center_y、bottom、distribute_x、distribute_y、same_width、same_height。
[[nodiscard]] std::optional<UiArrangeMode> ParseUiArrangeMode(std::string_view name);
[[nodiscard]] std::string_view UiArrangeModeName(UiArrangeMode mode);
// 至少要几个控件：对齐 1 个（以 reference 为准，如父控件），分布 3 个，同宽同高 2 个。
[[nodiscard]] std::size_t UiArrangeMinimum(UiArrangeMode mode);

// 按顺序返回各矩形的新位置。对齐：两个以上时以合起来的范围为准，只有一个时以 reference 为准；
// 个数不够或只有一个且没有 reference 时原样返回。
[[nodiscard]] std::vector<FrameRect> ArrangeUiRects(std::span<const FrameRect> rects, UiArrangeMode mode,
    std::optional<FrameRect> reference = std::nullopt);

} // namespace War3Map::GameUi
