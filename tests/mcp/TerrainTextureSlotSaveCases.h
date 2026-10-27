#pragma once

void TestTextureSlotActualSave() {
    namespace P = AudastraTest::TextureSlot;
    P::Fixture fixture; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots;
    qputenv("FORGE_MCP_YDWE_ROOT", QString::fromStdWString(fixture.inputs.tool.wstring()).toUtf8());
    qputenv("FORGE_MCP_WAR3_ROOT", QString::fromStdWString(fixture.inputs.game.wstring()).toUtf8());
    const auto direct = fixture.Request("direct");
    auto request = direct;
    request.terrainStrokes = {P::Command(true, MapData::TerrainTextureTable::Ground, 1, "", "Tg02"),
        P::Command(false, MapData::TerrainTextureTable::Ground, 1, "Tg02", "Tg03"),
        P::Command(true, MapData::TerrainTextureTable::Cliff, 1, "", "Cl02")};
    (void)P::Take(Services::SaveMapEditCandidate(request));
    const auto candidate = fixture.root / "mcp.w3x";
    QJsonObject args{{"source_map_path", QString::fromStdWString(fixture.source.path.wstring())},
        {"output_path", QString::fromStdWString(candidate.wstring())},
        {"expected_source_sha256", QString::fromStdString(fixture.source.sha256)},
        {"expected_terrain_sha256", QString::fromStdString(fixture.terrainSha)},
        {"terrain_strokes", QJsonArray{
            QJsonObject{{"kind", "texture_slot_append"}, {"table", "ground"}, {"expected_count", 1}, {"rawcode", "Tg02"}},
            QJsonObject{{"kind", "texture_slot_replace"}, {"table", "ground"}, {"slot", 1}, {"expected_rawcode", "Tg02"}, {"rawcode", "Tg03"}},
            QJsonObject{{"kind", "texture_slot_append"}, {"table", "cliff"}, {"expected_count", 1}, {"rawcode", "Cl02"}}}}};
    const auto saved = Call(server, "save_map_edit_candidate", args);
    Expect(saved.value("ok").toBool(), QJsonDocument(saved).toJson().constData());
    auto expected = P::Take(MapArchiveSession::Open(request.targetCandidate));
    auto actual = P::Take(MapArchiveSession::Open(candidate));
    for (const char* member : {"war3map.w3e", "war3map.wpm", "war3map.w3i", "war3map.doo", "unchanged.bin"})
        Expect(P::Take(expected.ReadMember(member)) == P::Take(actual.ReadMember(member)), "MCP/common-service independent readback agrees");
    args["output_path"] = QString::fromStdWString((fixture.root / "no-prefix.w3x").wstring());
    auto commands = args.value("terrain_strokes").toArray(); auto stale = commands.last().toObject(); stale["expected_count"] = 2;
    commands[2] = stale; args["terrain_strokes"] = commands;
    Code(Call(server, "save_map_edit_candidate", args), "TERRAIN_JOURNAL_PALETTE_DRIFT");
    Expect(!std::filesystem::exists(fixture.root / "no-prefix.w3x"), "MCP CAS suffix failure cannot publish prefix");
    Expect(P::Take(Sha256File(fixture.source.path)) == fixture.source.sha256, "MCP leaves source unchanged");
}

void TestTextureSlotSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots; roots.Set(fixture.ui);
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "palette.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](QJsonObject command) { args["terrain_strokes"] = QJsonArray{command};
        return Call(server, "save_map_edit_candidate", args); };
    for (const auto table : {"ground", "cliff"}) for (const bool append : {false, true}) {
        const char* index = append ? "expected_count" : "slot";
        QJsonObject command{{"kind", append ? "texture_slot_append" : "texture_slot_replace"},
            {"table", table}, {index, 1}, {"rawcode", "New1"}};
        if (!append) command["expected_rawcode"] = "Old1";
        Code(call(command), "SOURCE_LEASE_FAILED");
        for (const char* field : {"points", "algorithm", "rules", "follow_doodads", "host_path", append ? "slot" : "expected_count"}) {
            auto bad = command; bad[field] = true; Code(call(bad), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
        }
        for (const char* field : {"table", "rawcode", append ? "rawcode" : "expected_rawcode"}) {
            auto bad = command; bad.remove(field); Code(call(bad), "MCP_TEXTURE_SLOT_TEXT");
        }
        auto bad = command; bad.remove(index); Code(call(bad), "MCP_INTEGER_REQUIRED");
        bad = command; bad[index] = 1.5; Code(call(bad), "MCP_INTEGER_INVALID");
        bad = command; bad[index] = -1; Code(call(bad), "TERRAIN_TILESET_SLOT");
        bad = command; bad[index] = table == std::string_view("ground") ? 16 : 15; Code(call(bad), "TERRAIN_TILESET_SLOT");
        bad = command; bad["table"] = "water"; Code(call(bad), "TERRAIN_TILESET_TABLE");
        bad = command; bad["rawcode"] = "长字符a"; Code(call(bad), "TERRAIN_TEXTURE_SLOT_RAWCODE");
        if (append) { bad = command; bad["expected_rawcode"] = "Old1"; Code(call(bad), "MCP_MAP_EDIT_FIELD_UNSUPPORTED"); }
        Services::MapEditTerrainStroke typed;
        typed.textureSlot = Services::TerrainTextureSlotCommand{append ? Services::TerrainTextureSlotOperation::Append : Services::TerrainTextureSlotOperation::Replace,
            table == std::string_view("ground") ? MapData::TerrainTextureTable::Ground : MapData::TerrainTextureTable::Cliff,
            1, append ? "" : "Old1", "New1"};
        Expect(Services::MapEditValidation::ValidateStrokes(std::span(&typed, 1)).Succeeded(), "point-free typed palette accepted");
        typed.points.push_back({0, 0});
        Expect(!Services::MapEditValidation::ValidateStrokes(std::span(&typed, 1)).Succeeded(), "palette cannot hide a brush");
    }
    Expect(!std::filesystem::exists(fixture.root / "palette.w3x"), "invalid/unavailable source produces no prefix candidate");
    TestTextureSlotActualSave();
}
