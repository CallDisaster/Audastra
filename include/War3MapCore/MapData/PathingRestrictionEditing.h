#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"

namespace War3Map::MapData {

inline constexpr std::uint8_t kPathingRestrictionBits = 0x0e; // 不可走、不可飞、不可建。
inline constexpr double kPathingCellSize = 32.0;

// 通行格使用半开边界；尺寸 1..8 对应格半径 0..7，落点吸附格中心。
[[nodiscard]] std::optional<TerrainBrushFootprint> GetPathingRestrictionFootprint(
    const TerrainHeader& header, const TerrainBrush& brush, const TerrainStrokePoint& point);

struct PathingRestrictionStroke {
    std::vector<std::byte> bytes;
    TerrainStrokeResult changed; // 涉及的地形顶点范围，W3E 本身不改变。
};

struct PathingRestrictionCell {
    std::uint32_t x = 0, y = 0;
    std::uint8_t flags = 0;
};
struct PathingRestrictionPreview {
    std::size_t totalCells = 0;
    std::vector<PathingRestrictionCell> cells; // 有界、均匀抽样；只列本工程新增限制。
};

// value=true 仅追加已知限制；false 仅撤回来源中没有的附加限制。保留全部来源位。
[[nodiscard]] Result<PathingRestrictionStroke> ApplyPathingRestrictionStroke(
    const TerrainHeader& terrain, std::span<const std::byte> original, std::span<const std::byte> current,
    const TerrainBrush& brush, std::span<const TerrainStrokePoint> points);

// 工程专用声明：精确绑定原 W3E/WPM 并保存每格附加掩码。空结果表示没有附加限制。
// 声明不证明原生地图通行生成正确；旧的任意 WPM 覆盖恢复检查仍然有效。
[[nodiscard]] Result<std::vector<std::byte>> EncodePathingRestrictionDraft(
    const TerrainGridSnapshot& originalTerrain, std::span<const std::byte> original,
    std::span<const std::byte> current);
// 验证声明与完整候选逐格相符，再剥离声明过的附加位；调用方还须检查剩余荒芜差分。
[[nodiscard]] Result<std::vector<std::byte>> StripPathingRestrictionDraft(
    const TerrainGridSnapshot& originalTerrain, std::span<const std::byte> original,
    std::span<const std::byte> current, std::span<const std::byte> draft);

} // namespace War3Map::MapData
