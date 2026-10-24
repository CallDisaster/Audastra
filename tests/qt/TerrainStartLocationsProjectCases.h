#pragma once

#include "War3MapCore/MapData/PlacementEditing.h"

// 在TerrainUnitsJournalProjectCases.h之后包含，所有输入均为同一套原创小图。
namespace {
std::vector<MapArchiveMember> StartLocationsJournalQtMembers() {
    auto members = UnitsJournalQtMembers();
    auto unitsMember = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "war3mapUnits.doo"; });
    auto settingsMember = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "war3map.w3i"; });
    if (unitsMember == members.end() || settingsMember == members.end()) throw std::runtime_error("start fixture requires Units and W3I");
    auto units = Tests::WaterTake(MapData::ParseUnitPlacements(unitsMember->data));
    if (units.units.empty()) throw std::runtime_error("start fixture requires one classic unit");
    units.units.front().rawcode = "sloc"; units.units.front().owner = 2;
    unitsMember->data = Tests::WaterTake(units.Serialize());
    auto settings = Tests::WaterTake(MapData::MapSettings::Parse(settingsMember->data));
    auto player = std::find_if(settings.players.begin(), settings.players.end(), [](const auto& value) { return value.number == 2; });
    if (player == settings.players.end()) {
        MapData::MapPlayerSettings added; added.number = 2; added.controller = 1;
        settings.players.push_back(added); player = settings.players.end() - 1;
    }
    player->startX = units.units.front().position[0]; player->startY = units.units.front().position[1];
    settingsMember->data = Tests::WaterTake(settings.Serialize());
    return members;
}
}

void EditorProjectTests::StartLocationsWaterJournalProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("moveFirst"); QTest::addColumn<bool>("inPlace");
    QTest::newRow("starts-water-save-as") << true << false;
    QTest::newRow("water-starts-save-as") << false << false;
    QTest::newRow("starts-water-in-place") << true << true;
    QTest::newRow("water-starts-in-place") << false << true;
}

void EditorProjectTests::StartLocationsWaterJournalProjectColdRestoreAndNativeSave() {
    QFETCH(bool, moveFirst); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    const auto members = StartLocationsJournalQtMembers();
    const auto member = [&](std::string_view name) -> const std::vector<std::byte>& {
        return std::find_if(members.begin(), members.end(), [&](const auto& entry) { return entry.name == name; })->data;
    };
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto sourceDoodads = MapData::ParseDoodadPlacements(member("war3map.doo")); QVERIFY(sourceDoodads.Succeeded());
    const auto sourceTerrain = MapData::ParseMapTerrain(member("war3map.w3e")); QVERIFY(sourceTerrain.Succeeded());
    const auto sourceUnits = MapData::ParseUnitPlacements(member("war3mapUnits.doo")); QVERIFY(sourceUnits.Succeeded());
    const auto sourceSettings = MapData::MapSettings::Parse(member("war3map.w3i")); QVERIFY(sourceSettings.Succeeded());
    auto moved = *sourceUnits.value; moved.units.front().position[0] = 640; moved.units.front().position[1] = 512;
    const auto movedWire = moved.Serialize(); QVERIFY(movedWire.Succeeded());
    auto movedSettings = *sourceSettings.value;
    auto player = std::find_if(movedSettings.players.begin(), movedSettings.players.end(), [](const auto& entry) { return entry.number == 2; });
    QVERIFY(player != movedSettings.players.end()); player->startX = 640; player->startY = 512;
    const auto movedSettingsWire = movedSettings.Serialize(); QVERIFY(movedSettingsWire.Succeeded());
    auto deleted = *sourceDoodads.value;
    deleted.specialDoodads = {sourceDoodads.value->specialDoodads[2], sourceDoodads.value->specialDoodads[1]};
    const auto deletedWire = deleted.Serialize(); QVERIFY(deletedWire.Succeeded());
    const auto forge = std::filesystem::path(root.filePath("starts-water.forge").toStdWString());
    std::map<std::string, std::vector<std::byte>> finalDraft;
    std::vector<std::byte> waterWire;
    {
        EditorProject hot; QVERIFY(BindTerrainSaveProject(hot, source)); hot.war3GameRoot_ = game;
        QUndoStack initialUndo; hot.mapDataUndoStack_ = &initialUndo;
        const auto moveUnits = [&]() {
            const auto current = hot.mapEdits_->Snapshot();
            if (!current.Units() || current.Units()->units.empty()) return false;
            const MapData::PlacementMove move{
                MapData::MakePlacementId(MapData::PlacementFile::Units, 0, current.Units()->units.front()),
                moved.units.front().position, {}};
            if (!MapData::MovePlacements(*hot.mapEdits_, std::span(&move, 1)).Succeeded()
                || !hot.ObserveMapDataEdit("经世界共用摆放入口移动起点")) return false;
            PushMapDataEdit(hot, "Move start location", "移动起始位置"); return true;
        };
        const auto paintWater = [&]() -> Status {
            const auto current = hot.mapEdits_->Snapshot();
            if (!current.Doodads()) return Status::Failure("Test", "DOO_REQUIRED", "原创输入必须有完整DOO。");
            const auto rules = Services::TerrainPathingSave::CaptureWaterBrushRules(*hot.archiveSession_,
                *sourceTerrain.value->Snapshot(), *hot.mapEdits_->Terrain(), *current.Doodads(), game, true, 64u * 1024u * 1024u);
            if (!rules.Succeeded() || !rules.value) return {rules.diagnostics};
            const Services::MapDataQueryBinding document{"starts-qt-journal", 1, 0, hot.archiveSession_->SourceSha256()};
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
        if (moveFirst) QVERIFY(moveUnits());
        const auto water = paintWater(); QVERIFY2(water.Succeeded(), ReadDiagnosticText(water).c_str());
        if (!moveFirst) QVERIFY(moveUnits());
        QCOMPARE(initialUndo.count(), 2); QCOMPARE(hot.mapEdits_->Snapshot().Doodads()->Serialize().value, deletedWire.value);
        QCOMPARE(hot.mapEdits_->Snapshot().Units()->Serialize().value, movedWire.value);
        QCOMPARE(hot.mapEdits_->Snapshot().Settings()->Serialize().value, movedSettingsWire.value);
        const auto serialized = MapData::SerializeMapTerrain(*hot.mapEdits_->Terrain()); QVERIFY(serialized.Succeeded()); waterWire = *serialized.value;
        finalDraft = TerrainJournalDraftBytes(*hot.mapEdits_);
        const auto saved = hot.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
        const auto archive = MapArchiveSession::Open(forge); QVERIFY(archive.Succeeded());
        const auto journal = archive.value->ReadMember("overlay/editor/terrain-strokes.bin"); QVERIFY(journal.Succeeded());
        QCOMPARE(journal.value->at(3), std::byte{'A'});
        const auto decoded = MapData::DecodeTerrainStrokeJournal(*journal.value); QVERIFY(decoded.Succeeded());
        QCOMPARE(decoded.value->strokes.size(), std::size_t{2});
        QVERIFY(decoded.value->strokes[moveFirst ? 0 : 1].startLocations);
        initialUndo.clear();
    } // 原热会话与其全部Undo命令已销毁；恢复必须从真实.forge及源归档重新取得来源。
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QVERIFY((*opened.value)->terrainJournalWarning.empty()); QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{2});
    QCOMPARE(TerrainJournalDraftBytes(*(*opened.value)->mapDataEdits), finalDraft);
    const auto secondForge = std::filesystem::path(root.filePath("starts-water-again.forge").toStdWString());
    {
        EditorProject cold; QUndoStack undo; cold.mapDataUndoStack_ = &undo;
        QVERIFY(BindPreparedTerrainJournal(cold, **opened.value, game)); QCOMPARE(undo.count(), 2);
        const auto scope = cold.mapEdits_->TerrainJournalStateFingerprint();
        undo.undo(); QCOMPARE(cold.mapEdits_->Snapshot().Units()->Serialize().value, moveFirst ? movedWire.value : std::optional(member("war3mapUnits.doo")));
        QCOMPARE(cold.mapEdits_->Snapshot().Settings()->Serialize().value, moveFirst ? movedSettingsWire.value : std::optional(member("war3map.w3i")));
        QCOMPARE(cold.mapEdits_->Snapshot().Doodads()->Serialize().value, moveFirst ? std::optional(member("war3map.doo")) : deletedWire.value);
        undo.undo(); QVERIFY(!cold.mapEdits_->IsDirty());
        QCOMPARE(cold.mapEdits_->Snapshot().Settings()->Serialize().value, std::optional(member("war3map.w3i")));
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
        const auto& expectedDoodads = water ? *deletedWire.value : member("war3map.doo");
        const auto& expectedUnits = phase != 1 || moveFirst ? *movedWire.value : member("war3mapUnits.doo");
        const auto& expectedTerrain = water ? waterWire : member("war3map.w3e");
        const auto& expectedSettings = phase != 1 || moveFirst ? *movedSettingsWire.value : member("war3map.w3i");
        auto expectedPathing = member("war3map.wpm");
        if (water) {
            for (unsigned y = 4; y < 12; ++y) for (unsigned x = 4; x < 12; ++x) expectedPathing[16 + y * 32 + x] = std::byte{0xdb};
            for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x) expectedPathing[16 + y * 32 + x] = std::byte{0x1b};
        }
        const auto beforeSha = Sha256File(final.archiveSession_->SourcePath()); QVERIFY(beforeSha.Succeeded());
        const auto target = inPlace ? final.archiveSession_->SourcePath()
            : std::filesystem::path(root.filePath(QString("starts-water-saved-%1.w3x").arg(phase)).toStdWString());
        const bool derive = MapData::TerrainSavePlan::HasTerrainChanges(*final.mapEdits_);
        auto job = FreezeNativePathingQtJob(final, target); QVERIFY(job); QCOMPARE(job->terrainDerivationRequired, derive);
        job->terrainSessionSnapshot = final.mapEdits_->FreezeForSave();
        if (inPlace) {
            job->inPlace = true; job->inPlaceOptions.backupDirectory =
                std::filesystem::path(root.filePath(QString("backups-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        if (derive) {
            QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
            QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        } else QVERIFY(!job->terrainSavePlan); // 起点仅写Units与W3I，不为它伪造静态通行重建。
        const auto archive = MapArchiveSession::Open(target); QVERIFY(archive.Succeeded());
        QCOMPARE(archive.value->ReadMember("war3map.w3e").value, std::optional(expectedTerrain));
        QCOMPARE(archive.value->ReadMember("war3map.wpm").value, std::optional(expectedPathing));
        QCOMPARE(archive.value->ReadMember("war3map.doo").value, std::optional(expectedDoodads));
        QCOMPARE(archive.value->ReadMember("war3mapUnits.doo").value, std::optional(expectedUnits));
        QCOMPARE(archive.value->ReadMember("war3map.w3i").value, std::optional(expectedSettings));
        for (const auto& original : members) if (original.name != "war3map.w3e" && original.name != "war3map.wpm" && original.name != "war3map.doo" && original.name != "war3mapUnits.doo" && original.name != "war3map.w3i")
            QCOMPARE(archive.value->ReadMember(original.name).value, std::optional(original.data));
        QVERIFY(final.CommitMapSave(*job)); QVERIFY(!final.HasUnsavedMapChanges());
        QCOMPARE(final.mapEdits_->SessionIdentity(), session); QCOMPARE(undo.count(), 2); QCOMPARE(undo.index(), phase == 1 ? 1 : 2);
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, beforeSha.value); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
    }
}

void EditorProjectTests::StartLocationsDescriptionsProjectColdRestore_data() {
    QTest::addColumn<bool>("descriptionFirst"); QTest::addColumn<bool>("settingsRoute");
    QTest::newRow("descriptions-before-world-start") << true << false;
    QTest::newRow("descriptions-after-world-start") << false << false;
    QTest::newRow("descriptions-before-settings-start") << true << true;
    QTest::newRow("descriptions-after-settings-start") << false << true;
}

void EditorProjectTests::StartLocationsDescriptionsProjectColdRestore() {
    QFETCH(bool, descriptionFirst); QFETCH(bool, settingsRoute);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    const auto members = StartLocationsJournalQtMembers();
    const auto member = [&](std::string_view name) -> const std::vector<std::byte>& {
        return std::find_if(members.begin(), members.end(), [&](const auto& entry) { return entry.name == name; })->data;
    };
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto forge = std::filesystem::path(root.filePath("starts-descriptions.forge").toStdWString());
    std::map<std::string, std::vector<std::byte>> finalDraft;
    const auto originalSettings = Tests::WaterTake(MapData::MapSettings::Parse(member("war3map.w3i")));
    auto moved = Tests::WaterTake(MapData::ParseUnitPlacements(member("war3mapUnits.doo")));
    moved.units.front().position[0] = 640; moved.units.front().position[1] = 512;
    const auto movedWire = Tests::WaterTake(moved.Serialize());
    auto movedSettings = originalSettings;
    auto player = std::find_if(movedSettings.players.begin(), movedSettings.players.end(), [](const auto& value) { return value.number == 2; });
    QVERIFY(player != movedSettings.players.end()); player->startX = 640; player->startY = 512;
    const auto onlyMovedSettings = Tests::WaterTake(movedSettings.Serialize());
    auto finalSettings = movedSettings;
    finalSettings.mapName = "Independently edited map name"; finalSettings.author = "Edited author";
    finalSettings.description = "A different and longer description"; finalSettings.recommendedPlayers = "2-8";
    const auto finalSettingsWire = Tests::WaterTake(finalSettings.Serialize());
    {
        EditorProject hot; QVERIFY(BindTerrainSaveProject(hot, source)); hot.war3GameRoot_ = game;
        QUndoStack initialUndo; hot.mapDataUndoStack_ = &initialUndo;
        const auto editDescriptions = [&]() {
            auto settings = *hot.mapEdits_->Snapshot().Settings();
            settings.mapName = finalSettings.mapName; settings.author = finalSettings.author;
            settings.description = finalSettings.description; settings.recommendedPlayers = finalSettings.recommendedPlayers;
            if (!hot.mapEdits_->EditSettings(settings).Succeeded() || !hot.ObserveMapDataEdit("独立修改四说明")) return false;
            PushMapDataEdit(hot, "Map descriptions", "修改地图四说明"); return true;
        };
        const auto moveStart = [&]() {
            const auto current = hot.mapEdits_->Snapshot();
            if (!current.Units() || current.Units()->units.empty() || !current.Settings()) return false;
            Status changed;
            if (settingsRoute) {
                auto settings = *current.Settings();
                const auto target = std::find_if(settings.players.begin(), settings.players.end(), [](const auto& value) { return value.number == 2; });
                if (target == settings.players.end()) return false;
                target->startX = 640; target->startY = 512;
                changed = hot.mapEdits_->EditSettings(settings);
            } else {
                const MapData::PlacementMove move{
                    MapData::MakePlacementId(MapData::PlacementFile::Units, 0, current.Units()->units.front()),
                    moved.units.front().position, {}};
                changed = MapData::MovePlacements(*hot.mapEdits_, std::span(&move, 1));
            }
            if (!changed.Succeeded() || !hot.ObserveMapDataEdit("经世界或玩家设置入口移动真实起点")) return false;
            PushMapDataEdit(hot, "Move start", "移动起始位置"); return true;
        };
        if (descriptionFirst) QVERIFY(editDescriptions());
        QVERIFY(moveStart());
        if (!descriptionFirst) QVERIFY(editDescriptions());
        QCOMPARE(initialUndo.count(), 2);
        QCOMPARE(hot.mapEdits_->Snapshot().Units()->Serialize().value, std::optional(movedWire));
        QCOMPARE(hot.mapEdits_->Snapshot().Settings()->Serialize().value, std::optional(finalSettingsWire));
        finalDraft = TerrainJournalDraftBytes(*hot.mapEdits_);
        const auto saved = hot.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
        const auto archive = MapArchiveSession::Open(forge); QVERIFY(archive.Succeeded());
        const auto wire = archive.value->ReadMember("overlay/editor/terrain-strokes.bin"); QVERIFY(wire.Succeeded());
        QCOMPARE(wire.value->at(3), std::byte{'A'});
        const auto journal = MapData::DecodeTerrainStrokeJournal(*wire.value); QVERIFY(journal.Succeeded());
        QCOMPARE(journal.value->strokes.size(), std::size_t{1}); QVERIFY(journal.value->strokes.front().startLocations);
        initialUndo.clear();
    }
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QVERIFY((*opened.value)->terrainJournalWarning.empty());
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{2});
    QCOMPARE(TerrainJournalDraftBytes(*(*opened.value)->mapDataEdits), finalDraft);
    const auto secondForge = std::filesystem::path(root.filePath("starts-descriptions-again.forge").toStdWString());
    {
        EditorProject cold; QUndoStack undo; cold.mapDataUndoStack_ = &undo;
        QVERIFY(BindPreparedTerrainJournal(cold, **opened.value, game)); QCOMPARE(undo.count(), 2);
        const auto scope = cold.mapEdits_->TerrainJournalStateFingerprint();
        // 冷恢复先重放完整起点，再合并最终四说明；两者都必须是可撤销的真实事务。
        undo.undo(); QCOMPARE(cold.mapEdits_->Snapshot().Units()->Serialize().value, std::optional(movedWire));
        QCOMPARE(cold.mapEdits_->Snapshot().Settings()->Serialize().value, std::optional(onlyMovedSettings));
        undo.undo(); QVERIFY(!cold.mapEdits_->IsDirty());
        QCOMPARE(cold.mapEdits_->Snapshot().Units()->Serialize().value, std::optional(member("war3mapUnits.doo")));
        QCOMPARE(cold.mapEdits_->Snapshot().Settings()->Serialize().value, std::optional(member("war3map.w3i")));
        undo.redo(); undo.redo(); QCOMPARE(TerrainJournalDraftBytes(*cold.mapEdits_), finalDraft);
        QCOMPARE(cold.mapEdits_->TerrainJournalStateFingerprint(), scope);
        const auto saved = cold.SaveProjectToPath(secondForge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY")); undo.clear();
    }
    auto reopened = PrepareProjectOpen(secondForge, game); QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QVERIFY(HasReadDiagnostic(reopened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t{2});
    QCOMPARE(TerrainJournalDraftBytes(*(*reopened.value)->mapDataEdits), finalDraft);
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}
