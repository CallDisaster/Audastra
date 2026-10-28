#pragma once
void TestCustomSettingsSaveAndQuery() {
    namespace C = AudastraTest::CustomSettings;
    C::Fixture fixture; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots;
    qputenv("FORGE_MCP_YDWE_ROOT", QString::fromStdWString(fixture.base.inputs.tool.wstring()).toUtf8());
    qputenv("FORGE_MCP_WAR3_ROOT", QString::fromStdWString(fixture.base.inputs.game.wstring()).toUtf8());
    QJsonObject source{{"source_map_path", QString::fromStdWString(fixture.source.path.wstring())}, {"expected_source_sha256", QString::fromStdString(fixture.source.sha256)}, {"section", "summary"}};
    const auto summary = Call(server, "query_map_custom_settings", source);
    Expect(summary.value("ok").toBool(), QJsonDocument(summary).toJson().constData());
    source["section"] = "upgrades"; source["settings_binding"] = summary.value("settings_binding");
    Expect(Call(server, "query_map_custom_settings", source).value("rows").toArray().isEmpty(), "independent table query uses exact binding");
    auto stale = source; auto binding = stale.value("settings_binding").toObject(); binding["member_sha256"] = QString(64, 'a'); stale["settings_binding"] = binding;
    Code(Call(server, "query_map_custom_settings", stale), "SETTINGS_QUERY_STALE");
    auto direct = fixture.Request("direct-mcp-tech"); direct.settingsCustomEdits = fixture.AppendUpgrade();
    C::Take(Services::SaveMapEditCandidate(direct));
    const auto candidate = fixture.base.root / "mcp-tech.w3x";
    QJsonObject args{{"source_map_path", QString::fromStdWString(fixture.source.path.wstring())}, {"expected_source_sha256", QString::fromStdString(fixture.source.sha256)},
        {"output_path", QString::fromStdWString(candidate.wstring())}, {"expected_settings_sha256", QString::fromStdString(direct.settingsCustomEdits->expectedMemberSha256)},
        {"settings_custom_edits", QJsonArray{QJsonObject{{"section", "upgrades"}, {"op", "append"}, {"expected_count", 0},
            {"value", QJsonObject{{"player_mask", 1}, {"id_hex", "52303031"}, {"level", 1}, {"availability", 2}}}}}}};
    const auto saved = Call(server, "save_map_edit_candidate", args); Expect(saved.value("ok").toBool(), QJsonDocument(saved).toJson().constData());
    auto actual = C::Take(MapArchiveSession::Open(candidate)); auto expected = C::Take(MapArchiveSession::Open(direct.targetCandidate));
    for (const auto* member : {"war3map.w3i", "war3map.j", "war3map.w3e", "war3map.wpm", "unchanged.bin"})
        Expect(C::Take(actual.ReadMember(member)) == C::Take(expected.ReadMember(member)), "MCP and common normal save agree");
    auto commands = args.value("settings_custom_edits").toArray(); auto suffix = commands.first().toObject(); suffix["expected_count"] = 0; commands.append(suffix);
    args["settings_custom_edits"] = commands; args["output_path"] = QString::fromStdWString((fixture.base.root / "mcp-no-prefix.w3x").wstring());
    Code(Call(server, "save_map_edit_candidate", args), "SETTINGS_COUNT_STALE");
    Expect(!std::filesystem::exists(fixture.base.root / "mcp-no-prefix.w3x"), "MCP failed suffix has no candidate");
    if (Script::RandomMapInitializationAvailable()) {
        auto random = fixture.Request("direct-mcp-random"); random.settingsCustomEdits = fixture.AppendRandomUnits();
        C::Take(Services::SaveMapEditCandidate(random));
        args["settings_custom_edits"] = QJsonDocument::fromJson(QByteArray::fromStdString(C::Fixture::RandomUnitOperationsJson())).array();
        args["output_path"] = QString::fromStdWString((fixture.base.root / "mcp-random.w3x").wstring());
        const auto result = Call(server, "save_map_edit_candidate", args); Expect(result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
        auto mcpRandom = C::Take(MapArchiveSession::Open(fixture.base.root / "mcp-random.w3x"));
        auto directRandom = C::Take(MapArchiveSession::Open(random.targetCandidate));
        for (const auto* member : {"war3map.w3i", "war3map.j", "unchanged.bin"})
            Expect(C::Take(mcpRandom.ReadMember(member)) == C::Take(directRandom.ReadMember(member)), "MCP random table normal save matches the same service");
    }
}
