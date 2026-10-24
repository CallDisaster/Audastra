#pragma once

#include "../../src/Services/MapEditSaveValidation.h"

#include <array>
#include <limits>
#include <stdexcept>

namespace War3Map::Tests {

inline void TerrainStraightRampHostValidation() {
    using namespace MapData;
    using namespace Services;
    std::array<MapEditTerrainStroke, 1> strokes;
    auto& stroke = strokes.front();
    stroke.brush.kind = TerrainBrushKind::StraightRamp;
    stroke.points.push_back({-992, -928});
    const auto verify = [&](bool success, const char* code = nullptr) {
        const auto result = MapEditValidation::ValidateStrokes(strokes);
        if (result.Succeeded() != success || (code && (result.diagnostics.size() != 1 || result.diagnostics.front().code != code)))
            throw std::runtime_error("straight save request must preserve its dedicated kind, world point and neutral brush gate");
    };
    for (const bool enabled : {true, false}) { stroke.brush.value = enabled; verify(true); }
    const auto canonical = stroke;
    stroke.pointSpace = MapEditPointSpace::TerrainVertex;
    verify(false, "SAVE_STROKE_STRAIGHT_RAMP");
    stroke = canonical; stroke.points.clear(); verify(false, "SAVE_STROKE_STRAIGHT_RAMP");
    stroke = canonical; stroke.points.push_back({-928, -928}); verify(false, "SAVE_STROKE_STRAIGHT_RAMP");
    stroke = canonical; stroke.brush.size = 1; verify(false, "SAVE_STROKE_STRAIGHT_RAMP");
    stroke = canonical; stroke.brush.radius = 0; verify(false, "SAVE_STROKE_STRAIGHT_RAMP");
    stroke = canonical; stroke.brush.strength = 1; verify(false, "SAVE_STROKE_STRAIGHT_RAMP");
    stroke = canonical; stroke.brush.groundTexture = "Ldrt"; verify(false, "SAVE_STROKE_STRAIGHT_RAMP");
    stroke = canonical; stroke.brush.shape = TerrainBrushShape::Square; verify(false, "SAVE_STROKE_STRAIGHT_RAMP");
    stroke = canonical; stroke.points[0].x = std::numeric_limits<double>::infinity(); verify(false, "SAVE_STROKE_POINTS");
}

} // namespace War3Map::Tests
