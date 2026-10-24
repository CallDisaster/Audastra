#pragma once

void TestStraightRampSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server);
    ScopedSaveRoots roots; roots.Set(fixture.ui);
    const QJsonObject stroke{{"kind", "straight_ramp"}, {"point_space", "world"}, {"set_ramp", true},
        {"points", QJsonArray{QJsonObject{{"x", -992}, {"y", -928}}}}, {"follow_doodads", false}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "straight-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](const QJsonObject& value) {
        args["terrain_strokes"] = QJsonArray{value}; return Call(server, "save_map_edit_candidate", args);
    };
    for (const bool enabled : {true, false}) {
        auto valid = stroke; valid["set_ramp"] = enabled;
        Code(call(valid), "SOURCE_LEASE_FAILED");
    }
    auto invalid = stroke; invalid["point_space"] = "terrain_vertex";
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_SPACE_INVALID");
    invalid = stroke; invalid["set_ramp"] = "true";
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_RAMP_INVALID");
    invalid = stroke; invalid.remove("set_ramp");
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_RAMP_INVALID");
    invalid = stroke; invalid["follow_doodads"] = true;
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_FOLLOW_UNSUPPORTED");
    invalid = stroke; invalid["points"] = QJsonArray{};
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_POINTS_INVALID");
    invalid = stroke; invalid["points"] = QJsonArray{QJsonObject{{"x", -992}, {"y", -928}},
        QJsonObject{{"x", -928}, {"y", -928}}};
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_POINTS_INVALID");
    for (const char* field : {"shape", "radius", "size", "strength", "anchor", "seed", "loaded_rules", "enforce_water_limits"}) {
        invalid = stroke; invalid[field] = 1;
        Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    Expect(!std::filesystem::exists(fixture.root / "straight-candidate.w3x"),
        "straight transport rejection must never create a partial output map");
}
