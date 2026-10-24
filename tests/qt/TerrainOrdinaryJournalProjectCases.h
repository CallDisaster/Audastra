#pragma once

#include "War3MapCore/Services/TerrainEditService.h"

// 在EditorProjectTests类声明后包含。普通物在此使用真实空占地以隔离冷恢复；
// 非空且非对称的普通占地与special两阶段正式保存另由service专项核对。
namespace {
std::vector<MapArchiveMember> OrdinaryJournalQtMembers() {
    auto members = TerrainJournalWaterMembers();
    const auto replace = [&](std::string_view name, std::vector<std::byte> bytes) {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& entry) { return entry.name == name; });
        if (found == members.end()) members.push_back({std::string(name), std::move(bytes)});
        else found->data = std::move(bytes);
    };
    MapData::DoodadPlacements placed;
    MapData::DoodadPlacement ordinary; ordinary.rawcode = "Ord0"; ordinary.position = {512, 512, 37};
    ordinary.scale = {1, 1.25f, 0.75f}; ordinary.rotation = 0.625f;
    ordinary.flags = 2; ordinary.lifePercent = 83; ordinary.creationId = 912;
    MapData::PlacementItemSet drops; drops.items = {{"gold", 70}, {"lmbr", 30}};
    ordinary.itemSets.push_back(std::move(drops)); placed.doodads.push_back(std::move(ordinary));
    placed.specialDoodads = {{"Doo0", 0, {2, 2}}, {"Doo1", 0, {5, 5}}, {"Doo2", 0, {6, 5}}};
    replace("war3map.doo", Tests::WaterTake(placed.Serialize()));
    replace("war3map.shd", std::vector<std::byte>(32 * 32, std::byte{0x7d}));
    replace("war3map.mmp", std::vector<std::byte>(8, std::byte{0}));
    replace("war3mapMap.tga", Tests::WaterText("authored ordinary-journal minimap"));
    replace("Units/DestructableData.slk", NativeDoodadQtTable(
        {"destructableID", "code", "walkable"}, {{"Dst0", "Dst0", "0"}}));
    replace("Units/DestructableMetaData.slk", NativeDoodadQtTable(
        {"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"p001", "walkable", "DestructableData", "bool", "0", "0", "-1"}}));
    constexpr auto textureName = "PathTextures/ordinary-journal-special.tga";
    replace("Doodads/Doodads.slk", NativeDoodadQtTable(
        {"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", textureName, "C", "45"}, {"Doo1", "Doo1", textureName, "C", "45"},
         {"Doo2", "Doo2", textureName, "C", "45"}, {"Ord0", "Ord0", "_", "E", "-1"}}));
    std::vector<std::byte> texture(18); texture[2] = std::byte{2}; texture[12] = texture[14] = std::byte{4};
    texture[16] = std::byte{32}; texture[17] = std::byte{40};
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
        for (const unsigned channel : {x == 1 ? 255u : 0u, x == 2 ? 255u : 0u, x == 3 ? 255u : 0u, 0u})
            texture.push_back(static_cast<std::byte>(channel));
    replace(textureName, std::move(texture));
    auto& pathing = std::find_if(members.begin(), members.end(), [](const auto& entry) { return entry.name == "war3map.wpm"; })->data;
    // 原创顶原点4列黑/蓝/绿/红special占地，独立字面G0；不调用待测生成器造来源。
    constexpr std::array<unsigned, 4> dry{0x11, 0x19, 0x15, 0xd3};
    for (unsigned y = 8; y < 12; ++y) for (unsigned x = 8; x < 12; ++x)
        pathing[16 + y * 32 + x] = static_cast<std::byte>(dry[x - 8]);
    for (unsigned y = 20; y < 24; ++y) for (unsigned x = 20; x < 28; ++x)
        pathing[16 + y * 32 + x] = static_cast<std::byte>(dry[(x - 20) % 4]);
    return members;
}
}

void EditorProjectTests::OrdinaryWaterJournalProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("moveFirst"); QTest::addColumn<bool>("inPlace");
    QTest::newRow("ordinary-water-save-as") << true << false;
    QTest::newRow("water-ordinary-save-as") << false << false;
    QTest::newRow("ordinary-water-in-place") << true << true;
    QTest::newRow("water-ordinary-in-place") << false << true;
}

void EditorProjectTests::OrdinaryWaterJournalProjectColdRestoreAndNativeSave() {
    QFETCH(bool, moveFirst); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    const auto members = OrdinaryJournalQtMembers();
    const auto member = [&](std::string_view name) -> const std::vector<std::byte>& {
        return std::find_if(members.begin(), members.end(), [&](const auto& entry) { return entry.name == name; })->data;
    };
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto sourceDoodads = MapData::ParseDoodadPlacements(member("war3map.doo")); QVERIFY(sourceDoodads.Succeeded());
    const auto sourceTerrain = MapData::ParseMapTerrain(member("war3map.w3e")); QVERIFY(sourceTerrain.Succeeded());
    auto moved = *sourceDoodads.value; moved.doodads.front().position[0] = 640;
    const auto movedWire = moved.Serialize(); QVERIFY(movedWire.Succeeded());
    auto deleted = *sourceDoodads.value;
    deleted.specialDoodads = {sourceDoodads.value->specialDoodads[2], sourceDoodads.value->specialDoodads[1]};
    const auto deletedWire = deleted.Serialize(); QVERIFY(deletedWire.Succeeded());
    auto combined = deleted; combined.doodads.front().position[0] = 640;
    const auto combinedWire = combined.Serialize(); QVERIFY(combinedWire.Succeeded());
    const auto forge = std::filesystem::path(root.filePath("ordinary-water.forge").toStdWString());
    std::map<std::string, std::vector<std::byte>> finalDraft;
    std::vector<std::byte> waterWire;
    {
        EditorProject hot; QVERIFY(BindTerrainSaveProject(hot, source)); hot.war3GameRoot_ = game;
        QUndoStack initialUndo; hot.mapDataUndoStack_ = &initialUndo;
        const auto moveOrdinary = [&]() {
            const auto current = hot.mapEdits_->Snapshot();
            if (!current.Doodads()) return false;
            auto placements = *current.Doodads(); placements.doodads.front().position[0] = 640;
            if (!hot.mapEdits_->EditDoodads(placements).Succeeded() || !hot.ObserveMapDataEdit("移动混合恢复普通装饰物")) return false;
            PushMapDataEdit(hot, "Move ordinary doodad", "移动普通装饰物"); return true;
        };
        const auto paintWater = [&]() -> Status {
            const auto current = hot.mapEdits_->Snapshot();
            if (!current.Doodads()) return Status::Failure("Test", "DOO_REQUIRED", "原创输入必须有完整DOO。");
            const auto rules = Services::TerrainPathingSave::CaptureWaterBrushRules(*hot.archiveSession_,
                *sourceTerrain.value->Snapshot(), *hot.mapEdits_->Terrain(), *current.Doodads(), game, true, 64u * 1024u * 1024u);
            if (!rules.Succeeded() || !rules.value) return {rules.diagnostics};
            const Services::MapDataQueryBinding document{"ordinary-qt-journal", 1, 0, hot.archiveSession_->SourceSha256()};
            const Services::TerrainEditBinding binding{document, hot.mapEdits_->SessionIdentity(), hot.mapEdits_->Revision(), hot.mapEdits_->TerrainSourceRevision()};
            MapData::TerrainWaterStrokeOptions water; water.shape = MapData::TerrainBrushShape::Square; water.randomSeed = 1;
            const MapData::TerrainStrokePoint point{256, 256};
            const auto prepared = Services::PrepareTerrainWaterEdit(*hot.mapEdits_, document, binding, *rules.value,
                water, std::span(&point, 1), false);
            if (!prepared.Succeeded() || !prepared.value) return {prepared.diagnostics};
            const auto applied = Services::ApplyTerrainEdit(*hot.mapEdits_, document, *prepared.value);
            if (!applied.Succeeded()) return {applied.diagnostics};
            if (!hot.ObserveMapDataEdit("真实水刷删除命中特殊装饰物")) return Status::Failure("Test", "OBSERVE_FAILED", "水刷观察失败。");
            PushMapDataEdit(hot, "Water and special doodad", "水刷与特殊装饰物"); return Status::Ok();
        };
        if (moveFirst) QVERIFY(moveOrdinary());
        const auto water = paintWater(); QVERIFY2(water.Succeeded(), ReadDiagnosticText(water).c_str());
        if (!moveFirst) QVERIFY(moveOrdinary());
        QCOMPARE(initialUndo.count(), 2); QCOMPARE(hot.mapEdits_->Snapshot().Doodads()->Serialize().value, combinedWire.value);
        const auto serialized = MapData::SerializeMapTerrain(*hot.mapEdits_->Terrain()); QVERIFY(serialized.Succeeded()); waterWire = *serialized.value;
        finalDraft = TerrainJournalDraftBytes(*hot.mapEdits_);
        const auto saved = hot.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
        const auto archive = MapArchiveSession::Open(forge); QVERIFY(archive.Succeeded());
        const auto journal = archive.value->ReadMember("overlay/editor/terrain-strokes.bin"); QVERIFY(journal.Succeeded());
        QCOMPARE(journal.value->at(3), std::byte{'7'});
        const auto decoded = MapData::DecodeTerrainStrokeJournal(*journal.value); QVERIFY(decoded.Succeeded());
        QCOMPARE(decoded.value->strokes.size(), std::size_t{2});
        QVERIFY(decoded.value->strokes[moveFirst ? 0 : 1].ordinaryDoodads);
        initialUndo.clear();
    } // 原热会话与其全部Undo命令已销毁；恢复必须从真实.forge及源归档重新取得来源。
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QVERIFY((*opened.value)->terrainJournalWarning.empty()); QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{2});
    QCOMPARE(TerrainJournalDraftBytes(*(*opened.value)->mapDataEdits), finalDraft);
    const auto secondForge = std::filesystem::path(root.filePath("ordinary-water-again.forge").toStdWString());
    {
        EditorProject cold; QUndoStack undo; cold.mapDataUndoStack_ = &undo;
        QVERIFY(BindPreparedTerrainJournal(cold, **opened.value, game)); QCOMPARE(undo.count(), 2);
        const auto scope = cold.mapEdits_->TerrainJournalStateFingerprint();
        undo.undo(); QCOMPARE(cold.mapEdits_->Snapshot().Doodads()->Serialize().value, moveFirst ? movedWire.value : deletedWire.value);
        undo.undo(); QVERIFY(!cold.mapEdits_->IsDirty());
        QCOMPARE(cold.mapEdits_->Snapshot().Doodads()->Serialize().value, std::optional(member("war3map.doo")));
        undo.redo(); undo.redo(); QCOMPARE(TerrainJournalDraftBytes(*cold.mapEdits_), finalDraft);
        QCOMPARE(cold.mapEdits_->TerrainJournalStateFingerprint(), scope);
        const auto saved = cold.SaveProjectToPath(secondForge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY")); undo.clear();
    }
    auto reopened = PrepareProjectOpen(secondForge, game); QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QVERIFY(HasReadDiagnostic(reopened, "TERRAIN_JOURNAL_RESTORED")); QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t{2});
    EditorProject final; QUndoStack undo; final.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(final, **reopened.value, game)); QCOMPARE(undo.count(), 2);
    const auto session = final.mapEdits_->SessionIdentity();
    for (unsigned phase = 0; phase < 3; ++phase) {
        if (phase == 1) undo.undo();
        if (phase == 2) undo.redo();
        const bool water = phase != 1 || !moveFirst;
        const auto& expectedDoodads = phase != 1 ? *combinedWire.value : moveFirst ? *movedWire.value : *deletedWire.value;
        const auto& expectedTerrain = water ? waterWire : member("war3map.w3e");
        auto expectedPathing = member("war3map.wpm");
        if (water) {
            for (unsigned y = 4; y < 12; ++y) for (unsigned x = 4; x < 12; ++x) expectedPathing[16 + y * 32 + x] = std::byte{0xdb};
            for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x) expectedPathing[16 + y * 32 + x] = std::byte{0x1b};
        }
        const auto beforeSha = Sha256File(final.archiveSession_->SourcePath()); QVERIFY(beforeSha.Succeeded());
        const auto target = inPlace ? final.archiveSession_->SourcePath()
            : std::filesystem::path(root.filePath(QString("ordinary-water-saved-%1.w3x").arg(phase)).toStdWString());
        auto job = FreezeNativePathingQtJob(final, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = final.mapEdits_->FreezeForSave();
        if (inPlace) {
            job->inPlace = true; job->inPlaceOptions.backupDirectory =
                std::filesystem::path(root.filePath(QString("backups-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
        QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto archive = MapArchiveSession::Open(target); QVERIFY(archive.Succeeded());
        QCOMPARE(archive.value->ReadMember("war3map.w3e").value, std::optional(expectedTerrain));
        QCOMPARE(archive.value->ReadMember("war3map.wpm").value, std::optional(expectedPathing));
        QCOMPARE(archive.value->ReadMember("war3map.doo").value, std::optional(expectedDoodads));
        for (const auto& original : members) if (original.name != "war3map.w3e" && original.name != "war3map.wpm" && original.name != "war3map.doo")
            QCOMPARE(archive.value->ReadMember(original.name).value, std::optional(original.data));
        QVERIFY(final.CommitMapSave(*job)); QVERIFY(!final.HasUnsavedMapChanges());
        QCOMPARE(final.mapEdits_->SessionIdentity(), session); QCOMPARE(undo.count(), 2); QCOMPARE(undo.index(), phase == 1 ? 1 : 2);
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, beforeSha.value); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
    }
}
