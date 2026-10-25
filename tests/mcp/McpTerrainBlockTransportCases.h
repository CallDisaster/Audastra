#pragma once

void TerrainBlockFacadeCapturesItsOwnSourceDigest() {
    Fixture fixture;
    const auto state = State(fixture);
    QJsonObject request{{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")},
        {"terrain_binding", state.value("terrain_binding")}, {"source_x", 4}, {"source_y", 5}, {"width", 3}, {"height", 2},
        {"destination_x", 10}, {"destination_y", 11}, {"dry_run", true}};
    const auto code = [](const QJsonObject& result, const char* expected) {
        const auto diagnostics = result.value("diagnostics").toArray();
        Expect(!result.value("ok").toBool() && diagnostics.size() == 1
            && diagnostics[0].toObject().value("code") == expected, "block transport must preserve the exact rejection");
    };
    code(InvokeTerrainEditTool("edit_terrain_block", request, fixture.Host()), "TERRAIN_BLOCK_HOST_UNAVAILABLE");
    for (const char* field : {"raw", "source_bytes", "algorithm", "rules_path", "expected_block_sha256", "points", "brush", "verified"}) {
        auto invalid = request; invalid[field] = 1;
        code(InvokeTerrainEditTool("edit_terrain_block", invalid, fixture.Host()), "TERRAIN_ARGUMENT_UNKNOWN");
    }
    auto host = fixture.Host(); unsigned calls = 0;
    host.prepareBlock = [&](const auto& context, const auto& binding, const TerrainBlockPasteIntent& intent, bool follow) {
        ++calls;
        Expect(context.session == &fixture.edits && binding.terrainRevision == 0 && intent.algorithm == 1
            && intent.sourceCells.x0 == 4 && intent.sourceCells.y0 == 5 && intent.sourceCells.x1 == 7 && intent.sourceCells.y1 == 7
            && intent.destinationVertex == std::array<std::uint32_t, 2>{10, 11} && !follow,
            "block intent must be a same-session, absolute source rectangle and destination");
        const auto fingerprint = CaptureTerrainBlockFingerprint(*fixture.edits.Terrain(), intent.sourceCells);
        Expect(fingerprint.Succeeded() && *fingerprint.value == intent.expectedBlockSha256 && intent.expectedBlockSha256.size() == 64,
            "service must compute the fingerprint from its actual bound terrain before host preparation");
        return Result<PreparedTerrainEdit>::Failure("Test", "BLOCK_PREPARE_SENTINEL", "actual captured owner sentinel");
    };
    code(InvokeTerrainEditTool("edit_terrain_block", request, host), "TERRAIN_BLOCK_HOST_UNAVAILABLE");
    Expect(calls == 0, "block must not prepare without prepublish verification");
    host.verifyWater = [] { return Status::Ok(); };
    code(InvokeTerrainEditTool("edit_terrain_block", request, host), "BLOCK_PREPARE_SENTINEL");
    Expect(calls == 1 && fixture.edits.Revision() == 0 && !fixture.edits.CanUndo(), "failed block prepare cannot publish a transaction");
    auto invalid = request; invalid["width"] = 0;
    Expect(!InvokeTerrainEditTool("edit_terrain_block", invalid, host).value("ok").toBool() && calls == 1, "empty blocks reject before preparation");
    invalid = request; invalid["source_x"] = 1.5;
    Expect(!InvokeTerrainEditTool("edit_terrain_block", invalid, host).value("ok").toBool() && calls == 1, "fractional block cells reject before preparation");
    invalid = request; invalid["destination_x"] = 2048;
    code(InvokeTerrainEditTool("edit_terrain_block", invalid, host), "TERRAIN_BLOCK_RECTANGLE");
    invalid = request; invalid["expected_revision"] = "1";
    code(InvokeTerrainEditTool("edit_terrain_block", invalid, host), "TERRAIN_EDIT_STALE");
    Expect(calls == 1, "bad rectangle and stale identity cannot capture a source or prepare");
}
