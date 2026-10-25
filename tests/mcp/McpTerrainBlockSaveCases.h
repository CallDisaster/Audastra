#pragma once

void TestTerrainBlockSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots; roots.Set(fixture.ui);
    const QJsonObject block{{"kind", "block_copy"}, {"source_x", 4}, {"source_y", 4}, {"width", 4}, {"height", 4},
        {"destination_x", 20}, {"destination_y", 20}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "block-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](const QJsonArray& commands) {
        args["terrain_strokes"] = commands; return Call(server, "save_map_edit_candidate", args);
    };
    Code(call({block}), "SOURCE_LEASE_FAILED");
    for (const char* field : {"raw", "source_bytes", "algorithm", "rules_path", "expected_block_sha256", "points", "follow_doodads", "verified"}) {
        auto invalid = block; invalid[field] = 1; Code(call({invalid}), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    for (const char* field : {"source_x", "source_y", "width", "height", "destination_x", "destination_y"}) {
        auto invalid = block; invalid[field] = -1; Code(call({invalid}), "MCP_MAP_EDIT_BLOCK_INVALID");
    }
    auto invalid = block; invalid["width"] = 0; Code(call({invalid}), "MCP_MAP_EDIT_BLOCK_INVALID");
    invalid = block; invalid["destination_x"] = 2047; Code(call({invalid}), "MCP_MAP_EDIT_BLOCK_INVALID");
    const QJsonObject bounds{{"kind", "playable_bounds"}, {"west", 1}, {"south", 1}, {"east", 8}, {"north", 8}};
    Code(call({block, bounds}), "MCP_MAP_EDIT_BLOCK_ORDER");
    Code(call({bounds, block}), "MCP_MAP_EDIT_BLOCK_ORDER");
    Code(call({block, block}), "MCP_MAP_EDIT_BLOCK_ORDER");
}
