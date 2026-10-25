#pragma once

void TestTerrainResizeSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots; roots.Set(fixture.ui);
    const QJsonObject resize{{"kind", "canvas_resize"}, {"west", -32}, {"south", -32}, {"east", 64}, {"north", 64}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "resize-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](QJsonObject command) {
        args["terrain_strokes"] = QJsonArray{command}; return Call(server, "save_map_edit_candidate", args);
    };
    Code(call(resize), "SOURCE_LEASE_FAILED");
    for (const char* field : {"west", "south", "east", "north"}) {
        auto invalid = resize; invalid.remove(field); Code(call(invalid), "MCP_INTEGER_REQUIRED");
        invalid = resize; invalid[field] = 1.5; Code(call(invalid), "MCP_INTEGER_INVALID");
        invalid = resize; invalid[field] = "32"; Code(call(invalid), "MCP_INTEGER_REQUIRED");
        invalid = resize; invalid[field] = 2147483648.0; Code(call(invalid), "MCP_INTEGER_INVALID");
        invalid = resize; invalid[field] = -2147483649.0; Code(call(invalid), "MCP_INTEGER_INVALID");
    }
    for (const char* field : {"raw", "algorithm", "rules", "rules_sha256", "verified", "move_objects", "clip", "points", "follow_doodads"}) {
        auto invalid = resize; invalid[field] = true; Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    auto invalid = resize; invalid["east"] = -32; Code(call(invalid), "MCP_MAP_EDIT_RESIZE_INVALID");
    invalid = resize; invalid["north"] = -33; Code(call(invalid), "MCP_MAP_EDIT_RESIZE_INVALID");

    // 协议成功解析之后仍须通过CLI/MCP共用批次门；只有canonical resize
    // 没有采样点，普通笔刷的非空及累计点数上限不能被一并放宽。
    Services::MapEditTerrainStroke canonical;
    canonical.resize = MapData::TerrainResizeIntent{1, {-32, -32, 64, 64}};
    const auto verify = [&](const Services::MapEditTerrainStroke& stroke, bool success, const char* code = nullptr) {
        const auto status = Services::MapEditValidation::ValidateStrokes(std::span(&stroke, 1));
        Expect(status.Succeeded() == success, "shared resize request validation preserves its dedicated point-free domain");
        if (code) Expect(!status.diagnostics.empty() && status.diagnostics.front().code == code,
            "shared resize validation keeps the expected rejection reason");
    };
    verify(canonical, true);
    auto mixed = canonical; mixed.points.push_back({0, 0}); verify(mixed, false, "SAVE_STROKE_RESIZE_INVALID");
    mixed = canonical; mixed.brush.kind = MapData::TerrainBrushKind::Lower; verify(mixed, false, "SAVE_STROKE_RESIZE_INVALID");
    mixed = canonical; mixed.pointSpace = Services::MapEditPointSpace::TerrainVertex; verify(mixed, false, "SAVE_STROKE_RESIZE_INVALID");
    mixed = canonical; mixed.followDoodads = true; verify(mixed, false, "SAVE_STROKE_RESIZE_INVALID");
    mixed = canonical; mixed.waterBrush.emplace(); verify(mixed, false, "SAVE_STROKE_RESIZE_INVALID");
    mixed = canonical; mixed.bounds.emplace(); verify(mixed, false, "SAVE_STROKE_RESIZE_INVALID");
    mixed = canonical; mixed.blockPaste.emplace(); verify(mixed, false, "SAVE_STROKE_RESIZE_INVALID");
    mixed = canonical; mixed.resize->algorithm = 2; verify(mixed, false, "SAVE_STROKE_RESIZE_INVALID");
    mixed = canonical; mixed.resize->canvas.east = mixed.resize->canvas.west; verify(mixed, false, "TERRAIN_RESIZE_INTENT");
    mixed = canonical; mixed.resize->canvas.east = 4096; verify(mixed, false, "TERRAIN_RESIZE_DIMENSION");
    Services::MapEditTerrainStroke ordinary; ordinary.brush.kind = MapData::TerrainBrushKind::Raise;
    verify(ordinary, false, "SAVE_STROKE_POINTS");
    ordinary.points.push_back({0, 0}); verify(ordinary, true);
    std::array ordered{canonical, ordinary};
    Expect(Services::MapEditValidation::ValidateStrokes(ordered).Succeeded(),
        "point-free resize can precede a real ordinary stroke in the same ordered save transaction");
    ordinary.points.assign(4097, {0, 0}); verify(ordinary, false, "SAVE_STROKE_POINTS");
    ordered[1] = ordinary;
    Expect(!Services::MapEditValidation::ValidateStrokes(ordered).Succeeded(),
        "a preceding resize cannot suppress the ordinary point budget");
    ordinary.points.assign(2048, {0, 0});
    auto second = ordinary; second.points.push_back({0, 0});
    const std::array cumulative{canonical, ordinary, second};
    Expect(!Services::MapEditValidation::ValidateStrokes(cumulative).Succeeded(),
        "resize preserves the accumulated budget across individually valid ordinary strokes");
    Expect(!std::filesystem::exists(fixture.root / "resize-candidate.w3x"), "resize transport cannot publish on rejected or unavailable sources");
}
