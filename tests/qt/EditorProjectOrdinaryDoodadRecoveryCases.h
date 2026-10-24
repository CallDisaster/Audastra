#pragma once

// 在EditorProjectTests类声明之后包含；复用其原创MPQ与真实工程/地图保存入口。
// 明确制作旧版raw-only工程，验证兼容恢复不会伪造已经丢失的撤销历史。
namespace {
void AddOrdinaryRecoveryPattern(std::vector<std::byte>& pathing, unsigned x) {
    // 独立字面矩阵：原创3x2顶原点TGA(黑蓝绿/红青紫)，角0投影为2x3。
    constexpr std::array<unsigned, 6> masks{0, 0xc2, 8, 0x0c, 4, 0xca};
    for (unsigned i = 0; i < masks.size(); ++i)
        pathing[16 + (15 + i / 2) * 32 + x + i % 2] |= static_cast<std::byte>(masks[i]);
}
}

void EditorProjectTests::OrdinaryDoodadProjectRawColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("ordinary-raw-project-save-as") << false;
    QTest::newRow("ordinary-raw-project-save-in-place") << true;
}

void EditorProjectTests::OrdinaryDoodadProjectRawColdRestoreAndNativeSave() {
    QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    const std::array<MapArchiveMember, 1> gameMembers{{
        {"fixture.txt", Tests::WaterText("owned ordinary recovery game source")}}};
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", gameMembers).Succeeded());

    auto members = NativePathingQtMembers(); QVERIFY(!members.empty());
    const auto member = [&](std::string_view name) {
        return std::find_if(members.begin(), members.end(), [&](const auto& value) { return value.name == name; });
    };
    const auto doo = member("war3map.doo"), wpm = member("war3map.wpm");
    const auto shd = member("war3map.shd"), mmp = member("war3map.mmp");
    QVERIFY(doo != members.end() && wpm != members.end() && shd != members.end() && mmp != members.end());
    MapData::DoodadPlacements originalDoodads;
    MapData::DoodadPlacement placed; placed.rawcode = "Doo0"; placed.position = {512, 512, 37};
    placed.scale = {1, 1, 1}; placed.flags = 2; placed.creationId = 17;
    MapData::PlacementItemSet drops; drops.items = {{"gold", 70}, {"lmbr", 30}};
    placed.itemSets.push_back(std::move(drops)); originalDoodads.doodads.push_back(std::move(placed));
    originalDoodads.specialDoodads = {{"Spc0", 3, {1, 1}}, {"Spc0", 7, {6, 6}}};
    const auto originalWire = originalDoodads.Serialize(); QVERIFY(originalWire.Succeeded());
    doo->data = *originalWire.value; AddOrdinaryRecoveryPattern(wpm->data, 16);
    shd->data.assign(32 * 32, std::byte{0x7d}); mmp->data.assign(8, std::byte{0});
    const auto emptyUnits = MapData::UnitPlacements{}.Serialize(); QVERIFY(emptyUnits.Succeeded());
    members.push_back({"war3mapUnits.doo", *emptyUnits.value});
    members.push_back({"war3mapMap.tga", Tests::WaterText("authored ordinary recovery minimap")});
    members.push_back({"Units/DestructableData.slk", NativeDoodadQtTable(
        {"destructableID", "code", "walkable"}, {{"Dst0", "Dst0", "0"}})});
    members.push_back({"Units/DestructableMetaData.slk", NativeDoodadQtTable(
        {"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"p001", "walkable", "DestructableData", "bool", "0", "0", "-1"}})});
    constexpr auto textureName = "PathTextures/ordinary-recovery.tga";
    members.push_back({"Doodads/Doodads.slk", NativeDoodadQtTable(
        {"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", textureName, "E", "-1"}, {"Spc0", "Spc0", "_", "C", "-1"}})});
    std::vector<std::byte> texture(18); texture[2] = std::byte{2};
    texture[12] = std::byte{3}; texture[14] = std::byte{2};
    texture[16] = std::byte{24}; texture[17] = std::byte{32};
    constexpr std::array<std::uint8_t, 18> pixels{0,0,0, 1,0,0, 0,1,0, 0,0,1, 1,1,0, 1,0,1};
    for (const auto channel : pixels) texture.push_back(static_cast<std::byte>(channel));
    members.push_back({textureName, std::move(texture)});
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());

    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); project.war3GameRoot_ = game;
    QUndoStack initialUndo; project.mapDataUndoStack_ = &initialUndo;
    auto moved = originalDoodads; moved.doodads.front().position[0] = 640;
    const auto coldWire = moved.Serialize(); QVERIFY(coldWire.Succeeded());
    QVERIFY(project.mapEdits_->EditDoodads(moved).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("工程冷恢复前移动普通装饰物"));
    PushMapDataEdit(project, "Move ordinary doodad", "移动普通装饰物");
    MapData::TerrainBrush water; water.kind = MapData::TerrainBrushKind::WaterHeight;
    water.shape = MapData::TerrainBrushShape::Square; water.radius = 8; water.waterHeight = 80;
    const MapData::TerrainStrokePoint center{512, 512};
    QVERIFY(project.mapEdits_->EditTerrain(water, std::span(&center, 1), false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("工程冷恢复前修改整幅浅水高度"));
    PushMapDataEdit(project, "Water height", "浅水改为深水");
    MapData::TerrainBrush restriction; restriction.kind = MapData::TerrainBrushKind::PathingRestriction;
    restriction.size = 1; restriction.pathingRestrictions = 0x04;
    const MapData::TerrainStrokePoint restrictionPoint{48, 48};
    QVERIFY(project.mapEdits_->EditTerrain(restriction, std::span(&restrictionPoint, 1), false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("工程冷恢复前追加独立通行限制"));
    PushMapDataEdit(project, "Pathing restriction", "保留独立通行限制");
    QCOMPARE(initialUndo.count(), 3);
    const auto journal = project.mapEdits_->ExportTerrainStrokeJournal();
    QVERIFY(journal.Succeeded() && journal.value && journal.value->strokes.size() == 3);
    QVERIFY(journal.value->strokes.front().frozenDoodads);
    const auto draft = TerrainJournalDraftBytes(*project.mapEdits_);
    const auto restrictions = project.mapEdits_->SerializePathingRestrictionDraft();
    QVERIFY(restrictions.Succeeded() && !restrictions.value->empty());
    QCOMPARE(draft.size(), std::size_t{3});
    QCOMPARE(draft.at("war3map.doo"), *coldWire.value);
    const auto parsedTerrain = MapData::ParseMapTerrain(member("war3map.w3e")->data);
    QVERIFY(parsedTerrain.Succeeded() && parsedTerrain.value);
    auto expectedTerrain = *parsedTerrain.value->Snapshot();
    for (auto& vertex : expectedTerrain.vertices) vertex.waterHeightAndFlagsRaw = 8192 + 80 * 4;
    MapData::RefreshTerrainDerived(expectedTerrain, 0, 0, 9, 9);
    const auto deepWire = MapData::SerializeMapTerrain(expectedTerrain); QVERIFY(deepWire.Succeeded());
    QCOMPARE(draft.at("war3map.w3e"), *deepWire.value);
    auto expectedDraftPathing = member("war3map.wpm")->data;
    expectedDraftPathing[16 + 1 * 32 + 1] |= std::byte{0x04};
    QCOMPARE(draft.at("war3map.wpm"), expectedDraftPathing);

    const auto completeForge = std::filesystem::path(root.filePath("ordinary-ftj7.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(completeForge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
    // 旧版确实没有前向记录：从实际新工程同时删除声明与载荷，raw覆盖逐字保留。
    const auto complete = MapArchiveSession::Open(completeForge); QVERIFY(complete.Succeeded());
    QVERIFY(complete.value->HasMember("overlay/editor/terrain-strokes.bin"));
    auto legacyFiles = std::vector<MapArchiveMember>(complete.value->Members().begin(), complete.value->Members().end());
    std::erase_if(legacyFiles, [](const auto& entry) { return entry.name == "overlay/editor/terrain-strokes.bin"; });
    for (auto& entry : legacyFiles) if (entry.name == "project.json") {
        auto legacyManifest = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(entry.data.data()),
            static_cast<qsizetype>(entry.data.size()))).object();
        legacyManifest.remove("terrainStrokeJournal");
        legacyManifest["terrainJournalWarning"] = QStringLiteral("草稿已完整保留；旧版工程没有前向记录，普通摆放与水刷历史未恢复。");
        const auto json = QJsonDocument(legacyManifest).toJson(QJsonDocument::Compact);
        const auto bytes = std::as_bytes(std::span(json.data(), static_cast<std::size_t>(json.size())));
        entry.data.assign(bytes.begin(), bytes.end());
    }
    const auto forge = std::filesystem::path(root.filePath("ordinary.forge").toStdWString());
    QVERIFY(WriteVerifiedArchive(forge, legacyFiles).Succeeded());
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_DRAFT_ONLY"));
    QVERIFY(!HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QVERIFY((*opened.value)->terrainReceipts.empty() && !(*opened.value)->terrainJournalWarning.empty());
    QCOMPARE(TerrainJournalDraftBytes(*(*opened.value)->mapDataEdits), draft);
    QCOMPARE((*opened.value)->mapDataEdits->SerializePathingRestrictionDraft().value, restrictions.value);
    QCOMPARE(Sha256File(source).value, sourceSha.value);

    EditorProject coldProject; QUndoStack coldUndo; coldProject.mapDataUndoStack_ = &coldUndo;
    QVERIFY(BindPreparedTerrainJournal(coldProject, **opened.value, game));
    // raw载荷保留当前状态；没有FTJ的旧三条UI命令不能被捏造成可恢复的历史。
    QCOMPARE(coldUndo.count(), 0); QVERIFY(!coldUndo.canUndo() && !coldUndo.canRedo());
    const auto warning = coldProject.terrainJournalWarning_;
    const auto forgeAs = std::filesystem::path(root.filePath("ordinary-copy.forge").toStdWString());
    const auto savedAs = coldProject.SaveProjectToPath(forgeAs);
    QVERIFY2(savedAs.Succeeded(), ReadDiagnosticText(savedAs).c_str());
    QVERIFY(HasReadDiagnostic(savedAs, "TERRAIN_JOURNAL_DRAFT_ONLY"));
    const auto raw = MapArchiveSession::Open(forgeAs); QVERIFY(raw.Succeeded());
    QVERIFY(!raw.value->HasMember("overlay/editor/terrain-strokes.bin"));
    const auto manifestBytes = raw.value->ReadMember("project.json"); QVERIFY(manifestBytes.Succeeded());
    const auto manifest = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(manifestBytes.value->data()),
        static_cast<qsizetype>(manifestBytes.value->size()))).object();
    QCOMPARE(manifest["terrainJournalWarning"].toString().toStdString(), warning);
    for (const auto& [name, bytes] : draft) QCOMPARE(raw.value->ReadMember("overlay/map/" + name).value, std::optional(bytes));
    QCOMPARE(raw.value->ReadMember("overlay/editor/pathing-restrictions.bin").value, restrictions.value);
    auto reopened = PrepareProjectOpen(forgeAs, game); QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QCOMPARE((*reopened.value)->terrainJournalWarning, warning);
    QVERIFY((*reopened.value)->terrainReceipts.empty());
    QCOMPARE(TerrainJournalDraftBytes(*(*reopened.value)->mapDataEdits), draft);
    QCOMPARE((*reopened.value)->mapDataEdits->SerializePathingRestrictionDraft().value, restrictions.value);

    // 再次冷打开刚另存的工程；下面的正式地图保存不复用第一次恢复的内存状态。
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **reopened.value, game));
    QCOMPARE(undo.count(), 0); QCOMPARE(restored.terrainJournalWarning_, warning);
    const auto session = restored.mapEdits_->SessionIdentity();
    moved.doodads.front().position[0] = 768;
    const auto nextWire = moved.Serialize(); QVERIFY(nextWire.Succeeded());
    for (int phase = 0; phase < 4; ++phase) {
        if (phase == 1) {
            QVERIFY(restored.mapEdits_->EditDoodads(moved).Succeeded());
            QVERIFY(restored.ObserveMapDataEdit("冷恢复后再次移动普通装饰物"));
            PushMapDataEdit(restored, "Move after recovery", "冷恢复后的普通装饰物移动");
        } else if (phase == 2) undo.undo();
        else if (phase == 3) undo.redo();
        const bool movedAgain = phase == 1 || phase == 3;
        const auto& expectedDoodads = movedAgain ? *nextWire.value : *coldWire.value;
        QCOMPARE(restored.mapEdits_->Snapshot().Doodads()->Serialize().value, std::optional(expectedDoodads));
        QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, deepWire.value);
        // G1完全重建，不能把旧512中心的占地图混入冷恢复后的640/768中心。
        auto expectedPathing = member("war3map.wpm")->data;
        std::fill(expectedPathing.begin() + 16, expectedPathing.end(), std::byte{0x1b});
        AddOrdinaryRecoveryPattern(expectedPathing, movedAgain ? 24 : 20);
        expectedPathing[16 + 1 * 32 + 1] |= std::byte{0x04};
        const auto beforeSource = restored.sourceArchivePath_;
        const auto beforeSha = Sha256File(beforeSource); QVERIFY(beforeSha.Succeeded());
        const auto target = inPlace ? beforeSource
            : std::filesystem::path(root.filePath(QString("ordinary-saved-%1.w3x").arg(phase)).toStdWString());
        auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave(); job->minimapGameRoot = game;
        if (inPlace) {
            job->inPlace = true;
            job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath(QString("backups-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
        QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value, deepWire.value);
        QCOMPARE(actual.value->ReadMember("war3map.wpm").value, std::optional(expectedPathing));
        QCOMPARE(actual.value->ReadMember("war3map.doo").value, std::optional(expectedDoodads));
        for (const auto& original : members) if (original.name != "war3map.w3e" && original.name != "war3map.wpm"
            && original.name != "war3map.doo")
            QCOMPARE(actual.value->ReadMember(original.name).value, std::optional(original.data));
        QVERIFY(restored.CommitMapSave(*job));
        QCOMPARE(restored.mapEdits_->SessionIdentity(), session);
        QVERIFY(!restored.HasUnsavedMapChanges() && restored.mapEdits_->Replacements().empty());
        QCOMPARE(restored.mapEdits_->InspectTerrainPoint(restrictionPoint).value->pathingFlags, 0x1f);
        QCOMPARE(restored.mapEdits_->InspectTerrainPoint(restrictionPoint).value->addedPathingRestrictions, 0x04);
        QCOMPARE(undo.count(), phase == 0 ? 0 : 1);
        QCOMPARE(undo.index(), movedAgain ? 1 : 0);
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, beforeSha.value); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
    }
}
