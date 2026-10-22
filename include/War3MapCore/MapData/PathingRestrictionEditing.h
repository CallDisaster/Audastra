#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"

namespace War3Map::MapData {

inline constexpr std::uint8_t kPathingRestrictionBits = 0x0e; // 不可走、不可飞、不可建。
inline constexpr double kPathingCellSize = 32.0;

// 通行格使用半开边界；尺寸 1..8 对应格半径 0..7，落点吸附格中心。
[[nodiscard]] std::optional<TerrainBrushFootprint> GetPathingRestrictionFootprint(
    const TerrainHeader& header, const TerrainBrush& brush, const TerrainStrokePoint& point);

struct PathingRestrictionStroke {
    std::vector<std::byte> bytes; // 完整意图重载在 WPM 不变时为空，避免 A-only 复制 WPM。
    std::vector<std::byte> intent; // 每格完整的本工程限制意图，可以与来源限制重叠。
    TerrainStrokeResult changed; // 涉及的地形顶点范围，W3E 本身不改变。
};

struct PathingRestrictionCell {
    std::uint32_t x = 0, y = 0;
    std::uint8_t flags = 0;
};
struct PathingRestrictionPreview {
    std::size_t totalCells = 0;
    std::vector<PathingRestrictionCell> cells; // 有界、均匀抽样；列出本工程声明，包括来源已有的位。
};

// 来源中不能撤回的限制、上次写入地图的本工程意图、当前意图彼此独立。
// source = protectedSource | savedIntent；current = protectedSource | intent。
struct PathingRestrictionLayers {
    std::vector<std::byte> protectedSource, savedIntent, intent;
};

[[nodiscard]] Status VerifyPathingRestrictionLayers(const TerrainHeader& terrain,
    std::span<const std::byte> original, std::span<const std::byte> current,
    std::span<const std::byte> protectedSource, std::span<const std::byte> savedIntent,
    std::span<const std::byte> intent);

// value=true 声明限制意图；false 只撤回本工程意图。合成结果保留全部受保护来源位。
[[nodiscard]] Result<PathingRestrictionStroke> ApplyPathingRestrictionStroke(
    const TerrainHeader& terrain, std::span<const std::byte> original, std::span<const std::byte> current,
    const TerrainBrush& brush, std::span<const TerrainStrokePoint> points);
[[nodiscard]] Result<PathingRestrictionStroke> ApplyPathingRestrictionStroke(
    const TerrainHeader& terrain, std::span<const std::byte> original, std::span<const std::byte> current,
    std::span<const std::byte> intent, const TerrainBrush& brush, std::span<const TerrainStrokePoint> points);
[[nodiscard]] Result<PathingRestrictionStroke> ApplyPathingRestrictionStroke(
    const TerrainHeader& terrain, std::span<const std::byte> original, std::span<const std::byte> current,
    std::span<const std::byte> protectedSource, std::span<const std::byte> savedIntent,
    std::span<const std::byte> intent, const TerrainBrush& brush, std::span<const TerrainStrokePoint> points);

// 工程专用声明：精确绑定原 W3E/WPM。当前或已保存意图仍存在时必须保留声明。
// 声明不证明原生地图通行生成正确；旧的任意 WPM 覆盖恢复检查仍然有效。
[[nodiscard]] Result<std::vector<std::byte>> EncodePathingRestrictionDraft(
    const TerrainGridSnapshot& originalTerrain, std::span<const std::byte> original,
    std::span<const std::byte> current);
// APR2 保存独立意图，不从合成 WPM 反推；APR1 仍由旧重载产生并可严格恢复。
[[nodiscard]] Result<std::vector<std::byte>> EncodePathingRestrictionDraft(
    const TerrainGridSnapshot& originalTerrain, std::span<const std::byte> original,
    std::span<const std::byte> current, std::span<const std::byte> intent);
// APR3 在地图保存后保留来源分层。首次未保存的意图仍写 APR2，避免无谓扩大工程。
[[nodiscard]] Result<std::vector<std::byte>> EncodePathingRestrictionDraft(
    const TerrainGridSnapshot& originalTerrain, std::span<const std::byte> original,
    std::span<const std::byte> current, std::span<const std::byte> protectedSource,
    std::span<const std::byte> savedIntent, std::span<const std::byte> intent);
[[nodiscard]] Result<PathingRestrictionLayers> DecodePathingRestrictionLayers(
    const TerrainGridSnapshot& originalTerrain, std::span<const std::byte> original,
    std::span<const std::byte> current, std::span<const std::byte> draft);
[[nodiscard]] Result<std::vector<std::byte>> DecodePathingRestrictionDraft(
    const TerrainGridSnapshot& originalTerrain, std::span<const std::byte> original,
    std::span<const std::byte> current, std::span<const std::byte> draft);
// 验证声明与完整候选逐格相符，再恢复来源限制位；调用方还须检查剩余荒芜差分。
// APR1/2不匹配定位声明差分；APR3分层不匹配定位实际WPM格，来源身份不符单独报错。
// 可同时接回完整 A，避免联合恢复时重复解码；仅成功时写入输出。
[[nodiscard]] Result<std::vector<std::byte>> StripPathingRestrictionDraft(
    const TerrainGridSnapshot& originalTerrain, std::span<const std::byte> original,
    std::span<const std::byte> current, std::span<const std::byte> draft,
    std::vector<std::byte>* decodedIntent = nullptr);

} // namespace War3Map::MapData
