#pragma once
#include "AnimationInternal.h"

namespace War3Map::Render {
// 共享 MDX 数据当前不保留 SEQS extent；只读固定 v800 来源布局，不改变解析/求值语义。
void ReadAnimationVisibilityBounds(AnimationAsset& asset, std::span<const std::byte> bytes) noexcept;
[[nodiscard]] Status CheckAnimationView(const AnimationView& view);
// 按资产缓存全动作/粒子保守包络。未知范围放行；空间 BVH 仅在首次 worldView 创建。
// 变换编辑仅更新叶子和祖先；取消候选后的下一次查询会按已接受帧回滚缓存叶子。
[[nodiscard]] Status PrepareAnimationVisibility(std::shared_ptr<AnimationVisibilityIndex>& index,
    const AnimatedModelScene& scene, std::span<const AnimationAsset> assets,
    std::span<const std::uint32_t> requestAssets, AnimationFrame& frame,
    std::span<const AnimationEdit> edits, std::uint64_t maxVisibilityBytes);
} // namespace War3Map::Render
