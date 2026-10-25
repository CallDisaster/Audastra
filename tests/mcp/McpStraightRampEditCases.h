#pragma once

// 这里只验传输与宿主路由；实际含水坡道的结构和 WPM 结果由 Core 测试核对。
void StraightRampFacadeRequiresItsOwnCapturedOwner() {
    Fixture fixture;
    auto request = Stroke(State(fixture));
    request["brush"] = QJsonObject{{"kind", "straight_ramp"}, {"value", false}};
    request["points"] = QJsonArray{QJsonObject{{"x", -992}, {"y", -928}}};
    const auto rejected = [](const QJsonObject& result, const char* code) {
        const auto diagnostics = result.value("diagnostics").toArray();
        Expect(!result.value("ok").toBool() && diagnostics.size() == 1
            && diagnostics[0].toObject().value("code") == code,
            "straight facade must identify the exact rejected transport or owner gate");
    };
    rejected(InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host()),
        "TERRAIN_STRAIGHT_RAMP_HOST_UNAVAILABLE");
    for (const char* field : {"size", "shape", "radius", "anchor", "strength", "seed", "enforce_water_limits",
            "algorithm", "straight_ramp_algorithm", "loaded_rules", "source_identity"}) {
        auto invalid = request; auto brush = invalid.value("brush").toObject();
        brush[field] = 1; invalid["brush"] = brush;
        rejected(InvokeTerrainEditTool("edit_terrain_stroke", invalid, fixture.Host()), "TERRAIN_ARGUMENT_UNKNOWN");
    }
    auto doubled = request; auto points = request.value("points").toArray(); points.append(points[0]); doubled["points"] = points;
    rejected(InvokeTerrainEditTool("edit_terrain_stroke", doubled, fixture.Host()), "TERRAIN_STRAIGHT_RAMP_POINT_COUNT");

    auto host = fixture.Host(); unsigned prepares = 0, verifies = 0;
    host.prepareCornerRamp = [](const auto&, const auto&, const auto&, const auto&, bool) {
        return Result<PreparedTerrainEdit>::Failure("Test", "WRONG_CORNER_ROUTE", "不得把直坡分派到角坡。");
    };
    host.prepareStraightRamp = [&](const auto& context, const auto& binding, const TerrainBrush& brush,
                                  std::span<const TerrainStrokePoint> worldPoints, bool follow,
                                  TerrainStraightRampAlgorithm algorithm) {
        ++prepares;
        const TerrainBrush neutral;
        Expect(context.session == &fixture.edits && binding.terrainRevision == fixture.edits.Revision()
            && brush.kind == TerrainBrushKind::StraightRamp && !brush.value
            && brush.shape == neutral.shape && brush.radius == neutral.radius
            && brush.strength == neutral.strength && brush.groundTexture.empty()
            && !brush.size && !brush.anchor && !brush.variation
            && worldPoints.size() == 1 && worldPoints[0].x == -992 && worldPoints[0].y == -928 && !follow
            && algorithm == TerrainStraightRampAlgorithm::Connected,
            "straight host receives exact canonical request and defaults without synthesizing ordinary brush fields");
        return Result<PreparedTerrainEdit>::Failure("Test", "STRAIGHT_CAPTURE_SENTINEL", "实际资源捕获由宿主负责。");
    };
    rejected(InvokeTerrainEditTool("edit_terrain_stroke", request, host), "TERRAIN_STRAIGHT_RAMP_HOST_UNAVAILABLE");
    Expect(prepares == 0, "prepare must not run without prepublish source verification");
    host.verifyWater = [&] { ++verifies; return Status::Ok(); };
    rejected(InvokeTerrainEditTool("edit_terrain_stroke", request, host), "STRAIGHT_CAPTURE_SENTINEL");
    Expect(prepares == 1 && verifies == 0 && fixture.edits.Revision() == 0
        && !fixture.edits.IsDirty() && !fixture.edits.CanUndo(),
        "failed straight owner preparation must not verify or mutate the existing transaction");
}
