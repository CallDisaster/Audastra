#pragma once

void TextureSlotFacadeUsesRealOwnersAndAtomicPublication() {
    namespace P = AudastraTest::TextureSlot;
    P::Fixture native;
    auto source = P::Take(MapArchiveSession::Open(native.source.path));
    MapDataEditSession session(MapDocument::FromMembers(source.Members()));
    const auto terrain = P::Take(source.ReadMember("war3map.w3e"));
    Expect(session.AttachTerrain(terrain).Succeeded() && session.AttachPathing(P::Take(source.ReadMember("war3map.wpm"))).Succeeded(), "owned palette session attaches");
    const auto original = session.Terrain();
    const auto document = [&] { return MapDataQueryBinding{"palette-facade", 1, session.Revision(), source.SourceSha256()}; };
    std::optional<TerrainPathingResources> owner;
    unsigned preparations = 0;
    TerrainEditToolHost host;
    host.identity = [&] { return QJsonObject{{"project_identity", "palette-facade"}, {"revision", QString::number(session.Revision())}}; };
    host.context = [&] { return Result<TerrainEditToolContext>::Success({&session, document(), false, session.IsDirty()}); };
    host.prepareTextureSlot = [&](const auto& context, const auto& binding, const auto& command) -> Result<PreparedTerrainEdit> {
        ++preparations;
        const auto preview = PreviewTerrainTextureSlotCommand(*context.session, command);
        if (!preview.Succeeded()) return {{}, preview.diagnostics};
        auto captured = TerrainPathingSave::CaptureWaterBrushRules(source, *original, **preview.value,
            *session.Snapshot().Doodads(), native.inputs.game, true, 64u * 1024u * 1024u);
        if (!captured.Succeeded()) return {{}, captured.diagnostics};
        owner = std::move(*captured.value);
        return PrepareTerrainTextureSlotEdit(session, document(), binding, *owner, command);
    };
    host.verifyWater = [&] { return owner ? owner->VerifySourceInputs(source, 64u * 1024u * 1024u) : Status::Failure("Test", "NO_OWNER", "missing owner"); };
    host.apply = [&](const auto& plan, const auto& validate) { return ApplyTerrainEdit(session, document(), plan, validate); };
    const QJsonObject append{{"kind", "texture_slot_append"}, {"table", "ground"}, {"expected_count", 1}, {"rawcode", "Tg02"}};
    QJsonObject request{{"project_identity", "palette-facade"}, {"expected_revision", "0"},
        {"terrain_binding", InvokeTerrainEditTool("get_terrain_edit_state", {}, host).value("terrain_binding")},
        {"command", append}, {"dry_run", true}};
    const auto preview = InvokeTerrainEditTool("edit_terrain_texture_slot", request, host);
    Expect(preview.value("ok").toBool() && !preview.value("applied").toBool() && !session.IsDirty()
        && session.Revision() == 0 && session.Terrain() == original, "actual owner preview is pure");
    const auto before = preparations;
    for (const char* key : {"project_identity", "expected_revision", "terrain_binding"}) {
        auto invalid = request; invalid[key] = "stale";
        Expect(!InvokeTerrainEditTool("edit_terrain_texture_slot", invalid, host).value("ok").toBool(), "every binding dimension rejects stale requests");
    }
    auto invalid = request; invalid.remove("dry_run");
    Expect(!InvokeTerrainEditTool("edit_terrain_texture_slot", invalid, host).value("ok").toBool(), "dry_run required");
    invalid = request; invalid["dry_run"] = "true";
    Expect(!InvokeTerrainEditTool("edit_terrain_texture_slot", invalid, host).value("ok").toBool(), "dry_run typed");
    invalid = request; auto command = append; command["expected_count"] = 2; invalid["command"] = command;
    Expect(!InvokeTerrainEditTool("edit_terrain_texture_slot", invalid, host).value("ok").toBool(), "length CAS before capture");
    Expect(preparations == before && session.Revision() == 0, "invalid requests do not prepare or modify the session");
    const auto verify = host.verifyWater; unsigned verifications = 0;
    host.verifyWater = [&] { return ++verifications == 2 ? Status::Failure("Test", "PREPUBLISH_REJECT", "atomic rejection") : verify(); };
    auto write = request; write["dry_run"] = false;
    const auto rejected = InvokeTerrainEditTool("edit_terrain_texture_slot", write, host);
    Expect(!rejected.value("ok").toBool() && session.Terrain() == original && session.Revision() == 0 && !session.CanUndo(), "prepublication rejection has no prefix/undo");
    host.verifyWater = verify;
    const auto applied = InvokeTerrainEditTool("edit_terrain_texture_slot", write, host);
    Expect(applied.value("ok").toBool() && applied.value("applied").toBool() && applied.value("dirty_after").toBool()
        && applied.value("terrain_binding_after").isObject() && session.CanUndo(), "sealed slot plan publishes with real identity and undo");
    Expect(session.Terrain()->header.groundTilesets == std::vector<std::string>{"Tg01", "Tg02"}, "actual append persisted in session");
    const auto journal = P::Take(session.ExportTerrainStrokeJournal());
    Expect(journal.strokes.size() == 1 && journal.strokes[0].intent.palette
        && journal.strokes[0].intent.palette->algorithm == 2 && journal.strokes[0].intent.palette->slot == 1
        && journal.strokes[0].intent.palette->expectedRawcode.empty()
        && journal.strokes[0].intent.palette->replacementRawcode == "Tg02", "active publication records the same canonical palette intent as independent replay");
    Expect(!InvokeTerrainEditTool("edit_terrain_texture_slot", write, host).value("ok").toBool(), "old binding cannot replay");
    Expect(session.Undo() && session.Terrain()->header.groundTilesets == std::vector<std::string>{"Tg01"}, "undo restores old table");
    Expect(session.Redo() && session.Terrain()->header.groundTilesets.size() == 2, "redo restores appended table");
    Expect(P::Take(Sha256File(native.source.path)) == native.source.sha256, "facade never saved source");
    const auto definitions = TerrainEditToolDefinitions();
    const auto definition = std::find_if(definitions.begin(), definitions.end(), [](const auto& value) { return value.name == "edit_terrain_texture_slot"; });
    Expect(definition != definitions.end() && definition->permission == ToolPermission::Edit
        && definition->inputSchema.value("required").toArray().contains("dry_run")
        && definition->inputSchema.value("properties").toObject().value("command").toObject().value("oneOf").toArray().size() == 2,
        "active tool publishes shared command schema and explicit preview/write decision");
}

void RegisteredTextureSlotHostPreviewsAndRoutesSavesExplicitly() {
    namespace P = AudastraTest::TextureSlot;
    P::Fixture native; Fixture fixture;
    auto source = P::Take(MapArchiveSession::Open(native.source.path));
    for (const auto& member : source.Members()) {
        if (member.name == "war3map.w3e" || member.name == "war3map.wpm" || member.name == "war3map.w3i"
            || member.name.ends_with(".doo") || member.name.ends_with(".slk") || member.name.starts_with("UI/"))
            Fixture::Write(fixture.source / member.name, member.data);
    }
    fixture.terrain = P::Take(source.ReadMember("war3map.w3e")); fixture.pathing = P::Take(source.ReadMember("war3map.wpm"));
    ScopedWaterGameRoot environment(native.inputs.game); McpServer server; OpenWorkspace(fixture, server);
    const auto state = Current(server);
    QJsonObject request{{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")},
        {"terrain_binding", state.value("terrain_binding")}, {"dry_run", true},
        {"command", QJsonObject{{"kind", "texture_slot_append"}, {"table", "cliff"}, {"expected_count", 1}, {"rawcode", "Cl02"}}}};
    const auto preview = Call(server, "edit_terrain_texture_slot", request);
    Expect(preview.value("ok").toBool() && !preview.value("applied").toBool()
        && Current(server) == state && !server.Workspace()->HasActiveTransaction(), "registered directory preview is pure and captures new cliff metadata");
    request["dry_run"] = false;
    const auto rejected = Call(server, "edit_terrain_texture_slot", request);
    Expect(!rejected.value("ok").toBool() && !server.Workspace()->HasActiveTransaction() && Current(server) == state,
        "directory host refuses publication rather than creating a nonreplayable palette overlay");
    const auto diagnostics = rejected.value("diagnostics").toArray();
    Expect(!diagnostics.isEmpty() && diagnostics[0].toObject().value("code") == "TERRAIN_TEXTURE_SLOT_ACTIVE_MAP_REQUIRED",
        "directory host directs actual map saving to the common candidate service");
}
