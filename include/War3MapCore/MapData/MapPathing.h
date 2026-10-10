#pragma once

#include "War3MapCore/MapData/MapTerrain.h"

#include <memory>
#include <optional>
#include <span>

namespace War3Map::MapData {

struct PathingReadLimits {
    // 2048×2048 个地形 tile 对应 8192×8192 个 32 世界单位通行格。
    std::size_t maxBytes = 64 * 1024 * 1024 + 16;
    std::size_t maxDimension = 8192;
    std::size_t maxCells = 64 * 1024 * 1024;
};

struct PathingGridSnapshot {
    std::uint32_t version = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // 西南原点、X 向东、Y 向北、行优先。保留全部原始位；不猜测各位的来源。
    std::vector<std::uint8_t> cells;

    [[nodiscard]] std::optional<std::uint8_t> Cell(std::uint32_t x, std::uint32_t y) const noexcept;
};

class MapPathing {
public:
    [[nodiscard]] const std::vector<std::byte>& OriginalBytes() const noexcept { return original_; }
    [[nodiscard]] std::shared_ptr<const PathingGridSnapshot> Snapshot() const noexcept { return snapshot_; }

private:
    std::vector<std::byte> original_;
    std::shared_ptr<const PathingGridSnapshot> snapshot_;
    friend Result<MapPathing> ParseMapPathing(std::span<const std::byte>, const TerrainHeader*,
        const PathingReadLimits&);
};

// 只读解析经典 MP3W v0。给出 W3E header 时验证 4×tile 尺寸关系；未知位逐字节保留。
// 此接口不生成 WPM，也不证明刷地形后旧 WPM 的寻路/建造语义仍然正确。
[[nodiscard]] Result<MapPathing> ParseMapPathing(std::span<const std::byte> bytes,
    const TerrainHeader* terrain = nullptr, const PathingReadLimits& limits = {});

} // namespace War3Map::MapData
