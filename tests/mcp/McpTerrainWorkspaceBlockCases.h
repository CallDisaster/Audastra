#pragma once

// 主测试在匿名命名空间外包含TerrainBlockFixtures/ConnectedRampFixtures；这里复用
// 原MCP注册、协议调用和隔离目录夹具，所有资源均为合成小样例。
template<class NativeFixture>
void InstallNativeTerrainWorkspace(Fixture& host, const NativeFixture& native) {
    for (const auto& member : native.members) Fixture::Write(host.source / member.name, member.data);
    host.terrain = native.members[0].data; host.pathing = native.members[1].data;
}

QJsonObject BlockCopy(const QJsonObject& state, bool dryRun = true) {
    return {{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")},
        {"terrain_binding", state.value("terrain_binding")}, {"source_x", 4}, {"source_y", 4}, {"width", 4}, {"height", 4},
        {"destination_x", 20}, {"destination_y", 20}, {"follow_doodads", true}, {"dry_run", dryRun}};
}

void RegisteredBlockHostPublishesCompleteTypedDirectoryAndColdReadback() {
    namespace B = War3Map::TerrainBlockFixtures;
    B::BlockFixture native;
    // 原Core撤销恢复完整before，而非反向刷回近似高度；和目录宿主使用同一密封plan。
    const auto shared = TerrainTake(native.edits->PrepareTerrainBlockPasteEdit(native.Rules(), native.Intent(), true));
    Expect(native.edits->ApplyPreparedTerrainEdit(shared).Succeeded(), "shared block command applies its actual resource plan");
    native.CheckState(*native.edits, true);
    Expect(native.edits->Undo(), "block command retains the original undo transaction"); native.CheckState(*native.edits, false);
    Expect(native.edits->Redo(), "block redo restores the same complete raw members"); native.CheckState(*native.edits, true);

    Fixture fixture; InstallNativeTerrainWorkspace(fixture, native);
    ScopedWaterGameRoot environment(native.game); McpServer server; OpenWorkspace(fixture, server);
    auto* workspace = server.Workspace();
    const auto initial = Current(server), dry = Call(server, "edit_terrain_block", BlockCopy(initial));
    Expect(dry.value("ok").toBool() && !dry.value("applied").toBool() && !workspace->HasActiveTransaction()
        && Current(server) == initial, "actual block preview captures rules without changing a revision or acquiring a writer");
    auto revision = workspace->Revision(); auto transaction = TerrainTake(workspace->BeginEdit(revision));
    const auto before = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
    const auto state = TerrainTake(CaptureTerrainEditState(*before.session, before.document));
    const auto resources = TerrainTake(workspace->CaptureTerrainWaterResources(true));
    auto intent = native.Intent();
    intent.expectedBlockSha256 = TerrainTake(CaptureTerrainBlockFingerprint(*before.session, before.document, state.binding, intent.sourceCells));
    const auto prepared = TerrainTake(PrepareTerrainBlockPasteEdit(*before.session, before.document, state.binding, resources, intent, true));
    bool beforePublish = false;
    const auto refused = workspace->ApplyTerrainPlan(prepared, [&](const auto&) {
        beforePublish = true;
        return Status::Failure("Test", "BLOCK_PUBLISH_REFUSED", "owned prepublish refusal");
    });
    Expect(beforePublish && !refused.Succeeded() && before.session->Revision() == 0 && !before.session->CanUndo()
        && TerrainTake(workspace->Session()->PreviewEdit(transaction)).changes.empty(),
        "prepublish rejection preserves the actual typed session and directory overlay atomically");
    const auto request = BlockCopy(Current(server), false);
    const auto applied = Call(server, "edit_terrain_block", request);
    Expect(applied.value("ok").toBool() && applied.value("applied").toBool() && !applied.value("map_saved").toBool(),
        "registered independent host applies the typed block instead of treating its empty points as an ordinary brush");
    const auto current = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
    Expect(current.session == before.session && current.session->Revision() == 1 && current.session->CanUndo(),
        "directory publication keeps the prepared session and original block command identity");
    native.CheckState(*current.session, true);
    Expect(!Call(server, "edit_terrain_block", request).value("ok").toBool(), "block request keeps the original CAS rejection");
    Expect(workspace->PreviewEdit(transaction, revision).Succeeded() && workspace->ValidateEdit(transaction, revision).satisfied,
        "block command reaches real independent preview and validation");
    Expect(workspace->Rollback(transaction, revision).Succeeded(), "block uses the original transaction rollback");
    auto retained = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
    native.CheckState(*retained.session, false);
    Expect(!workspace->HasActiveTransaction(), "rolled back block has no active writer");

    revision = workspace->Revision(); transaction = TerrainTake(workspace->BeginEdit(revision));
    const auto reapplied = Call(server, "edit_terrain_block", BlockCopy(Current(server), false));
    Expect(reapplied.value("ok").toBool() && reapplied.value("applied").toBool(), "rolled back block can be prepared again from the original source");
    Expect(workspace->PreviewEdit(transaction, revision).Succeeded() && workspace->ValidateEdit(transaction, revision).satisfied,
        "reapplied block has current preview and validation before archive handoff");
    const auto original = TerrainTake(DirectoryWorkspace::Open(fixture.source));
    std::vector<MapArchiveMember> archiveMembers;
    for (const auto& file : original.Snapshot().files)
        archiveMembers.push_back({IO::GenericPathUtf8(file.relativePath), TerrainTake(IO::ReadFileBytes(fixture.source / file.relativePath))});
    const auto archive = TerrainTake(WriteVerifiedArchive(std::filesystem::path(fixture.temporary.path().toStdWString()) / "block-source.w3x", archiveMembers));
    MapEditSaveRequest archiveRequest; archiveRequest.sourceMap = archive.path; archiveRequest.expectedSourceSha256 = archive.sha256;
    archiveRequest.targetCandidate = std::filesystem::path(fixture.temporary.path().toStdWString()) / "block-archive-candidate.w3x";
    archiveRequest.gameRoot = native.game; archiveRequest.toolRoot = std::filesystem::path(fixture.temporary.path().toStdWString()) / "missing-toolchain";
    archiveRequest.minimapChoice = TerrainMinimapSaveChoice::Preserve;
    const auto archiveResult = workspace->ExportArchiveCandidate(transaction, revision, archiveRequest);
    Expect(!archiveResult.Succeeded() && std::any_of(archiveResult.diagnostics.begin(), archiveResult.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "SCRIPT_TOOLCHAIN_MISSING"; })
        && !std::filesystem::exists(archiveRequest.targetCandidate) && workspace->HasActiveTransaction(),
        "archive handoff retains the block variant through actual save preparation and stops at the unchanged script toolchain gate");
    const auto output = std::filesystem::path(fixture.temporary.path().toStdWString()) / "block-directory-candidate";
    Expect(workspace->PreviewEdit(transaction, revision).Succeeded() && workspace->ValidateEdit(transaction, revision).satisfied
        && workspace->ExportCandidate(transaction, revision, output).Succeeded(), "ordinary directory exporter publishes the complete block recipe");
    Expect(TerrainTake(IO::ReadFileBytes(output / "war3map.w3e")) == B::TerrainLiteral(true)
        && TerrainTake(IO::ReadFileBytes(output / "war3map.wpm")) == B::PathingLiteral(true)
        && TerrainTake(IO::ReadFileBytes(output / "war3mapUnits.doo")) == TerrainTake(native.UnitsLiteral(true).Serialize())
        && TerrainTake(IO::ReadFileBytes(output / "war3map.doo")) == TerrainTake(native.DoodadsLiteral(true).Serialize()),
        "directory output matches independent W3E/WPM and complete placement literals");
    for (const auto* name : {"war3map.w3i", "war3map.j", "war3map.wtg", "war3map.wct"})
        Expect(TerrainTake(IO::ReadFileBytes(output / name)) == TerrainTake(IO::ReadFileBytes(fixture.source / name)),
            "block export preserves settings, authored script and trigger bytes");

    McpServer cold; cold.SetWorkspace(output, std::filesystem::path(fixture.temporary.path().toStdWString()) / "ui");
    RegisterAllTools(cold); Expect(cold.Workspace() != nullptr, "exported directory reopens through the real independent host");
    (void)Current(cold);
    const auto coldContext = TerrainTake(cold.Workspace()->TerrainContext(cold.DocumentId(), cold.DocumentGeneration()));
    native.CheckState(*coldContext.session, true);
    Expect(!coldContext.session->IsDirty() && !coldContext.session->CanUndo(),
        "cold directory is its actual saved baseline, without inventing persisted undo history");

    const auto nextRevision = workspace->Revision(); const auto next = TerrainTake(workspace->BeginEdit(nextRevision));
    auto raise = Stroke(Current(server), false); raise["points"] = QJsonArray{QJsonObject{{"x", 1536}, {"y", 1536}}};
    Expect(Call(server, "edit_terrain_stroke", raise).value("ok").toBool(),
        "new directory transaction can replay its retained block then append an ordinary command");
    Expect(workspace->PreviewEdit(next, nextRevision).Succeeded() && workspace->ValidateEdit(next, nextRevision).satisfied,
        "mixed directory recipe keeps the typed block across subsequent sealing");
    const auto mixedArchive = workspace->ExportArchiveCandidate(next, nextRevision, archiveRequest);
    Expect(!mixedArchive.Succeeded() && std::any_of(mixedArchive.diagnostics.begin(), mixedArchive.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "MCP_TERRAIN_BLOCK_ARCHIVE_BATCH_UNSUPPORTED"; })
        && !std::filesystem::exists(archiveRequest.targetCandidate),
        "unsupported mixed archive export refuses explicitly without weakening the single-block save request domain");
    Expect(workspace->Rollback(next, nextRevision).Succeeded(), "continuation rollback keeps the prior exported block");
    retained = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration())); native.CheckState(*retained.session, true);
    for (const auto& member : native.members)
        Expect(TerrainTake(IO::ReadFileBytes(fixture.source / member.name)) == member.data, "block preview, export and rollback never write the original directory");
}

void BlockDirectorySealRejectsUntypedOrConflictingEmptyCommands() {
    namespace B = War3Map::TerrainBlockFixtures;
    B::BlockFixture native; Fixture fixture; InstallNativeTerrainWorkspace(fixture, native);
    McpServer server; OpenWorkspace(fixture, server);
    const auto directory = TerrainTake(DirectoryWorkspace::Open(fixture.source));
    const auto original = TerrainTake(MapDocument::FromDirectory(directory));
    const auto plan = TerrainTake(native.edits->PrepareTerrainBlockPasteEdit(native.Rules(), native.Intent(), true));
    Expect(native.edits->ApplyPreparedTerrainEdit(plan).Succeeded(), "block seal rejection uses an actual prepared candidate");
    TerrainStrokeIntent command; command.blockPaste = native.Intent(); command.followDoodads = true;
    const auto core = TerrainPatchCodec::Seal(original, fixture.terrain, fixture.pathing, directory.Snapshot(),
        *native.edits, std::span(&command, 1), native.game);
    Expect(!core.Succeeded(), "public Core seal cannot authorize an empty-point block without actual Services replay");
    const auto seal = [&](const TerrainStrokeIntent& intent) {
        return TerrainPathingSave::Seal(original, fixture.terrain, fixture.pathing, directory.Snapshot(),
            *native.edits, std::span(&intent, 1), native.game);
    };
    Expect(seal(command).Succeeded(), "Services can seal the actual typed rectangle with its real native owner");
    auto bad = command; bad.points.push_back({512, 512}); Expect(!seal(bad).Succeeded(), "block plus brush points is rejected");
    bad = command; bad.brush.kind = TerrainBrushKind::Raise; Expect(!seal(bad).Succeeded(), "block cannot carry ordinary brush semantics");
    bad = command; bad.brush.variation = 0; Expect(!seal(bad).Succeeded(), "block cannot carry hidden brush variation");
    bad = command; bad.palette.emplace(); Expect(!seal(bad).Succeeded(), "block cannot carry a palette variant");
    bad = command; bad.bounds.emplace(); Expect(!seal(bad).Succeeded(), "block cannot carry map bounds");
    bad = command; bad.waterBrush.emplace(); Expect(!seal(bad).Succeeded(), "block cannot carry separate water options");
    bad = command; bad.cornerRampAlgorithm = TerrainCornerRampAlgorithm::WetTerrain; Expect(!seal(bad).Succeeded(), "block cannot borrow corner algorithm metadata");
    bad = command; bad.straightRampAlgorithm = TerrainStraightRampAlgorithm::Connected; Expect(!seal(bad).Succeeded(), "block cannot borrow connected algorithm metadata");
    bad = command; bad.blockPaste->expectedBlockSha256 = std::string(64, '0'); Expect(!seal(bad).Succeeded(), "valid-looking wrong source digest still rejects actual source mismatch");
    TerrainStrokeIntent empty; Expect(!seal(empty).Succeeded(), "untyped empty command remains rejected");
}

void RegisteredConnectedHostKeepsItsAlgorithmThroughDirectoryReplay() {
    namespace C = War3Map::TerrainConnectedRampFixtures;
    C::ConnectedFixture native(C::SurfaceCase::SurfaceEquality, false);
    Fixture fixture; InstallNativeTerrainWorkspace(fixture, native);
    ScopedWaterGameRoot environment(native.game); McpServer server; OpenWorkspace(fixture, server);
    auto* workspace = server.Workspace();
    const auto requestFor = [&](bool dryRun) {
        auto request = Stroke(Current(server), dryRun);
        request["brush"] = QJsonObject{{"kind", "straight_ramp"}, {"value", true}};
        request["points"] = QJsonArray{QJsonObject{{"x", native.point.x}, {"y", native.point.y}}};
        request["follow_doodads"] = true; return request;
    };
    const auto dry = Call(server, "edit_terrain_stroke", requestFor(true));
    Expect(dry.value("ok").toBool() && !dry.value("applied").toBool() && !workspace->HasActiveTransaction(),
        "actual Connected preview does not acquire a writer");
    const auto revision = workspace->Revision(); const auto transaction = TerrainTake(workspace->BeginEdit(revision));
    const auto result = Call(server, "edit_terrain_stroke", requestFor(false));
    Expect(result.value("ok").toBool() && result.value("applied").toBool(),
        "Connected candidate retains its algorithm when independent host replays and seals it");
    const auto context = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
    native.CheckState(*context.session, true);
    const auto output = std::filesystem::path(fixture.temporary.path().toStdWString()) / "connected-directory-candidate";
    Expect(workspace->PreviewEdit(transaction, revision).Succeeded() && workspace->ValidateEdit(transaction, revision).satisfied
        && workspace->ExportCandidate(transaction, revision, output).Succeeded(), "Connected recipe reaches ordinary directory export");
    Expect(TerrainTake(IO::ReadFileBytes(output / "war3map.w3e")) == native.TerrainLiteral(true)
        && TerrainTake(IO::ReadFileBytes(output / "war3map.wpm")) == C::ConnectedPathingLiteral(native.surface, true),
        "Connected output matches literal terrain and native pathing, without falling back to independent ramp semantics");
    const auto nextRevision = workspace->Revision(); const auto next = TerrainTake(workspace->BeginEdit(nextRevision));
    const auto seeded = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
    native.CheckState(*seeded.session, true);
    Expect(workspace->Rollback(next, nextRevision).Succeeded(), "seeded Connected history keeps normal rollback");
    for (const auto& member : native.members)
        Expect(TerrainTake(IO::ReadFileBytes(fixture.source / member.name)) == member.data, "Connected directory work preserves original source bytes");
}
