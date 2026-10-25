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
        invalid = resize; invalid[field] = 2147483648.0; Code(call(invalid), "MCP_MAP_EDIT_RESIZE_INVALID");
        invalid = resize; invalid[field] = -2147483649.0; Code(call(invalid), "MCP_MAP_EDIT_RESIZE_INVALID");
    }
    for (const char* field : {"raw", "algorithm", "rules", "rules_sha256", "verified", "move_objects", "clip", "points", "follow_doodads"}) {
        auto invalid = resize; invalid[field] = true; Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    auto invalid = resize; invalid["east"] = -32; Code(call(invalid), "MCP_MAP_EDIT_RESIZE_INVALID");
    invalid = resize; invalid["north"] = -33; Code(call(invalid), "MCP_MAP_EDIT_RESIZE_INVALID");
    Expect(!std::filesystem::exists(fixture.root / "resize-candidate.w3x"), "resize transport cannot publish on rejected or unavailable sources");
}
