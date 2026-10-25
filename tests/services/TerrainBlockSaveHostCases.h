#pragma once

#include "../../src/Services/MapEditSaveValidation.h"

#include <array>
#include <stdexcept>

namespace War3Map::Tests {

inline void TerrainBlockSaveHostValidation() {
    using namespace MapData;
    using namespace Services;
    MapEditTerrainStroke copied;
    copied.blockPaste = TerrainBlockPasteIntent{1, {8, 8, 10, 10}, {16, 16}, {}};
    const auto valid = [&](const MapEditTerrainStroke& stroke, bool expected) {
        const auto result = MapEditValidation::ValidateStrokes(std::span(&stroke, 1));
        if (result.Succeeded() != expected || (!expected && (result.diagnostics.size() != 1
            || result.diagnostics.front().code != "SAVE_TERRAIN_BLOCK_REQUEST_INVALID")))
            throw std::runtime_error("block save request must be an independent typed command before source access");
    };
    valid(copied, true); // 空摘要必须由实际捕获补齐，不能从请求取得来源资格。
    auto bound = copied;
    bound.blockPaste->expectedBlockSha256 = std::string(64, 'a');
    valid(bound, true);
    bound.followDoodads = true;
    valid(bound, true);
    auto mixed = copied;
    mixed.bounds.emplace(); valid(mixed, false);
    mixed = copied; mixed.waterBrush.emplace(); valid(mixed, false);
    mixed = copied; mixed.points.push_back({1, 1}); valid(mixed, false);
    mixed = copied; mixed.brush.kind = TerrainBrushKind::Raise; valid(mixed, false);
    mixed = copied; mixed.brush.variation = 0; valid(mixed, false);
    mixed = copied; mixed.pointSpace = MapEditPointSpace::TerrainVertex; valid(mixed, false);
    mixed = copied; mixed.blockPaste->algorithm = 2; valid(mixed, false);
    mixed = copied; mixed.blockPaste->expectedBlockSha256 = std::string(64, 'A'); valid(mixed, false);
    const std::array two{copied, copied};
    const auto batch = MapEditValidation::ValidateStrokes(two);
    if (batch.Succeeded() || batch.diagnostics.front().code != "SAVE_TERRAIN_BLOCK_REQUEST_INVALID")
        throw std::runtime_error("public block save must not resolve later source coordinates against an earlier grid");
}

} // namespace War3Map::Tests
