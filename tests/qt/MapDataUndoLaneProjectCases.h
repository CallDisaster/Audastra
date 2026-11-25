#pragma once

#include "War3MapCore/MapData/MapDataEditLane.h"
#include "War3MapCore/MapData/PlacementEditing.h"

// 在TerrainStartLocationsProjectCases.h之后包含：世界、地图设置、声音三个撤销栈共用同一编辑会话，
// 一个栈的撤销/重做只回退自己的修改。所有输入为同一套原创小图加一条原创声音定义。
namespace {
using UndoLane = MapData::MapDataEditLane;

std::vector<MapArchiveMember> UndoLaneQtMembers() {
    auto members = StartLocationsJournalQtMembers();
    MapData::SoundRecord sound{};
    sound.name = "LaneThunder"; sound.filePath = "Sounds\\lane.wav"; sound.eaxSetting = "DefaultEAXON";
    sound.volume = 90; sound.pitch = 1.0f; sound.channel = 5;
    sound.minDistance = 100.0f; sound.maxDistance = 4000.0f; sound.distanceCutoff = 2000.0f;
    sound.coneAngleInside = 360.0f; sound.coneAngleOutside = 360.0f; sound.coneOutsideVolume = 127;
    members.push_back({"war3map.w3s", Tests::WaterTake(MapData::SoundsDocument{1, {sound}, {}}.Serialize())});
    return members;
}

// W3I、单位、声音与地形的当前完整字节。
std::map<std::string, std::vector<std::byte>> UndoLaneState(const EditorProject& project) {
    std::map<std::string, std::vector<std::byte>> state;
    const auto snapshot = project.mapEdits_->Snapshot();
    const std::array<std::pair<const char*, MapDataKind>, 3> members{{{"war3map.w3i", MapDataKind::Settings},
        {"war3mapUnits.doo", MapDataKind::Units}, {"war3map.w3s", MapDataKind::Sounds}}};
    for (const auto& [name, kind] : members) {
        const auto& member = snapshot.Member(kind);
        state[name] = member.sources.empty() ? std::vector<std::byte>{} : member.sources.front().data;
    }
    state["war3map.w3e"] = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    return state;
}

std::array<float, 2> UndoLaneStart(const EditorProject& project) {
    const auto settings = project.mapEdits_->Snapshot().Settings();
    const auto player = std::find_if(settings->players.begin(), settings->players.end(), [](const auto& value) { return value.number == 2; });
    return player == settings->players.end() ? std::array<float, 2>{-1, -1} : std::array<float, 2>{player->startX, player->startY};
}

std::array<float, 2> UndoLaneUnit(const EditorProject& project) {
    const auto snapshot = project.mapEdits_->Snapshot();
    const auto& unit = snapshot.Units()->units.front();
    return {unit.position[0], unit.position[1]};
}

bool UndoLaneSettings(EditorProject& project, const std::function<void(MapData::MapSettings&)>& change, const char* label) {
    auto settings = *project.mapEdits_->Snapshot().Settings(); change(settings);
    if (!project.mapEdits_->EditSettings(settings).Succeeded() || !project.ObserveMapDataEdit(label)) return false;
    PushMapDataEdit(project, UndoLane::Settings, QString::fromUtf8(label), label);
    return true;
}

bool UndoLaneVolume(EditorProject& project, std::int32_t volume) {
    auto sounds = *project.mapEdits_->Snapshot().Sounds(); sounds.entries.front().volume = volume;
    if (!project.mapEdits_->EditSounds(sounds).Succeeded() || !project.ObserveMapDataEdit("声音页修改音量")) return false;
    PushMapDataEdit(project, UndoLane::Sounds, QStringLiteral("音量"), "声音页修改音量");
    return true;
}

// 世界页移动起始位置：单位与W3I玩家起点一起改，属于世界撤销栈。
bool UndoLaneMoveStart(EditorProject& project, float x, float y) {
    const auto snapshot = project.mapEdits_->Snapshot();
    const auto& unit = snapshot.Units()->units.front();
    auto position = unit.position; position[0] = x; position[1] = y;
    const MapData::PlacementMove move{MapData::MakePlacementId(MapData::PlacementFile::Units, 0, unit), position, {}};
    if (!MapData::MovePlacements(*project.mapEdits_, std::span(&move, 1)).Succeeded()
        || !project.ObserveMapDataEdit("世界页移动起始位置")) return false;
    PushMapDataEdit(project, UndoLane::World, QStringLiteral("移动起点"), "世界页移动起始位置");
    return true;
}
} // namespace

void EditorProjectTests::MapDataUndoLanesKeepEachPageIndependent() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("lanes.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, UndoLaneQtMembers()).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    auto* world = project.mapDataUndoStack_; auto* settings = project.mapSettingsUndoStack_; auto* sounds = project.soundUndoStack_;
    QVERIFY(world && settings && sounds && world != settings && settings != sounds && world != sounds);
    QCOMPARE(project.MapDataUndoStack(UndoLane::World), world);
    QCOMPARE(project.MapDataUndoStack(UndoLane::Settings), settings);
    QCOMPARE(project.MapDataUndoStack(UndoLane::Sounds), sounds);
    const auto original = UndoLaneState(project);
    const auto originalStart = UndoLaneStart(project);
    const auto originalName = project.mapEdits_->Snapshot().Settings()->mapName;
    const auto originalAuthor = project.mapEdits_->Snapshot().Settings()->author;

    // 交错：世界地形 → 设置地图名 → 声音音量 → 世界起点（同写W3I） → 设置作者。
    QVERIFY(RaiseTerrainSaveProject(project)); PushMapDataEdit(project, UndoLane::World, QStringLiteral("升高"), "世界页升高地形");
    const auto raisedTerrain = UndoLaneState(project).at("war3map.w3e");
    QVERIFY(raisedTerrain != original.at("war3map.w3e"));
    QVERIFY(UndoLaneSettings(project, [](auto& value) { value.mapName = "Lane map name"; }, "设置页修改地图名"));
    QVERIFY(UndoLaneVolume(project, 42));
    QVERIFY(UndoLaneMoveStart(project, 640, 512));
    QVERIFY(UndoLaneSettings(project, [](auto& value) { value.author = "Lane author"; }, "设置页修改作者"));
    QCOMPARE(world->count(), 2); QCOMPARE(settings->count(), 2); QCOMPARE(sounds->count(), 1);
    const auto edited = UndoLaneState(project);
    QCOMPARE(UndoLaneStart(project), (std::array<float, 2>{640, 512}));

    const auto expectOthers = [&](const char* name, const char* author, std::int32_t volume) {
        const auto snapshot = project.mapEdits_->Snapshot();
        return snapshot.Settings()->mapName == name && snapshot.Settings()->author == author
            && snapshot.Sounds()->entries.front().volume == volume;
    };
    // 世界页撤销起点：W3I只退回起点坐标，后来的作者和地图名、声音都保留。
    world->undo();
    QCOMPARE(UndoLaneStart(project), originalStart); QCOMPARE(UndoLaneState(project).at("war3mapUnits.doo"), original.at("war3mapUnits.doo"));
    QVERIFY(expectOthers("Lane map name", "Lane author", 42));
    QCOMPARE(UndoLaneState(project).at("war3map.w3e"), raisedTerrain);
    // 世界页撤销地形：设置与声音仍不变。
    world->undo();
    QCOMPARE(UndoLaneState(project).at("war3map.w3e"), original.at("war3map.w3e"));
    QVERIFY(expectOthers("Lane map name", "Lane author", 42));
    QCOMPARE(world->index(), 0); QCOMPARE(settings->index(), 2); QCOMPARE(sounds->index(), 1);
    // 设置页撤销：只退回作者、地图名，不碰地形、单位或声音。
    settings->undo();
    QVERIFY(expectOthers("Lane map name", originalAuthor.c_str(), 42));
    settings->undo();
    QVERIFY(expectOthers(originalName.c_str(), originalAuthor.c_str(), 42));
    QCOMPARE(UndoLaneState(project).at("war3map.w3i"), original.at("war3map.w3i"));
    // 声音页撤销：只退回音量。
    sounds->undo();
    QCOMPARE(UndoLaneState(project), original);
    QVERIFY(!project.mapEdits_->IsDirty());

    // 换一种顺序重做：结果与编辑完成时逐字节相同。
    sounds->redo(); QVERIFY(expectOthers(originalName.c_str(), originalAuthor.c_str(), 42));
    world->redo(); world->redo();
    QCOMPARE(UndoLaneStart(project), (std::array<float, 2>{640, 512}));
    QVERIFY(expectOthers(originalName.c_str(), originalAuthor.c_str(), 42));
    settings->redo(); settings->redo();
    QCOMPARE(UndoLaneState(project), edited);
    QCOMPARE(world->index(), 2); QCOMPARE(settings->index(), 2); QCOMPARE(sounds->index(), 1);
    QCOMPARE(world->count(), 2); QCOMPARE(settings->count(), 2); QCOMPARE(sounds->count(), 1);
    QVERIFY(!ModelHasCode(*project.diagnosticsModel_, QStringLiteral("MAP_UNDO_REFUSED")));
    QVERIFY(!ModelHasCode(*project.diagnosticsModel_, QStringLiteral("MAP_REDO_REFUSED")));
}

void EditorProjectTests::MapDataUndoLaneRefusesOverlappingEdit() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("lane-conflict.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, UndoLaneQtMembers()).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    auto* world = project.mapDataUndoStack_; auto* settings = project.mapSettingsUndoStack_;
    const auto original = UndoLaneState(project);
    // 设置页玩家起点联动单位；之后世界页又移动同一单位。两页改的是同一份单位数据。
    QVERIFY(UndoLaneSettings(project, [](auto& value) {
        for (auto& player : value.players) if (player.number == 2) { player.startX = 700; player.startY = 600; }
    }, "设置页移动玩家起点"));
    QCOMPARE(UndoLaneUnit(project), (std::array<float, 2>{700, 600}));
    QVERIFY(UndoLaneMoveStart(project, 640, 512));
    QCOMPARE(settings->count(), 1); QCOMPARE(world->count(), 1);
    const auto moved = UndoLaneState(project);
    // 设置页的撤销不能覆盖世界页后来的单位位置：拒绝、地图不变、该步移出设置栈。
    settings->undo();
    QCOMPARE(UndoLaneState(project), moved);
    QCOMPARE(settings->count(), 0); QVERIFY(!settings->canRedo());
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, QStringLiteral("MAP_UNDO_REFUSED")));
    QCOMPARE(world->count(), 1); QCOMPARE(world->index(), 1);
    // 世界页自己的撤销/重做照常。
    world->undo(); QCOMPARE(UndoLaneUnit(project), (std::array<float, 2>{700, 600}));
    QCOMPARE(UndoLaneStart(project), (std::array<float, 2>{700, 600}));
    world->redo(); QCOMPARE(UndoLaneState(project), moved);
    world->undo(); world->redo();
    QVERIFY(UndoLaneState(project) != original);
}

void EditorProjectTests::MapDataUndoLanesSaveUndoSaveAndRestore() {
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    QTemporaryDir projectOutput; QVERIFY(projectOutput.isValid());
    const auto source = std::filesystem::path(root.filePath("source/lanes.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    const auto members = UndoLaneQtMembers();
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto forge = std::filesystem::path(projectOutput.filePath("lanes.forge").toStdWString());
    std::map<std::string, std::vector<std::byte>> original, edited, soundUndone, finalDraft;
    std::array<float, 2> originalStart{};
    {
        EditorProject hot; QVERIFY(BindTerrainSaveProject(hot, source)); hot.war3GameRoot_ = game;
        auto* world = hot.mapDataUndoStack_; auto* settings = hot.mapSettingsUndoStack_; auto* sounds = hot.soundUndoStack_;
        original = UndoLaneState(hot); originalStart = UndoLaneStart(hot);
        QVERIFY(UndoLaneSettings(hot, [](auto& value) { value.mapName = "Saved lane name"; }, "设置页修改地图名"));
        QVERIFY(UndoLaneVolume(hot, 42));
        QVERIFY(UndoLaneMoveStart(hot, 640, 512));
        QVERIFY(UndoLaneSettings(hot, [](auto& value) { value.author = "Saved lane author"; }, "设置页修改作者"));
        edited = UndoLaneState(hot);
        QCOMPARE(world->count(), 1); QCOMPARE(settings->count(), 2); QCOMPARE(sounds->count(), 1);

        const auto saveMap = [&](const char* name) -> std::optional<std::filesystem::path> {
            const auto target = std::filesystem::path(root.filePath(name).toStdWString());
            auto job = FreezeNativePathingQtJob(hot, target);
            if (!job) return std::nullopt;
            const auto lease = hot.mapEdits_->PrepareTerrainHistoryForSave();
            if (!lease.Succeeded() || !lease.value) { qWarning() << ReadDiagnosticText(lease).c_str(); return std::nullopt; }
            job->terrainHistorySaveLease = *lease.value;
            job->terrainSessionSnapshot = hot.mapEdits_->FreezeForSave();
            InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
            if (!job->succeeded) { qWarning() << ReadDiagnosticText(*job).c_str(); return std::nullopt; }
            if (!job->terrainHistoryWarning.empty()) { qWarning() << job->terrainHistoryWarning.c_str(); return std::nullopt; }
            if (!hot.CommitMapSave(*job) || !hot.terrainJournalWarning_.empty()) return std::nullopt;
            return target;
        };
        const auto savedMatches = [&](const std::filesystem::path& target, const std::map<std::string, std::vector<std::byte>>& expected) {
            const auto archive = MapArchiveSession::Open(target);
            if (!archive.Succeeded()) return false;
            for (const auto* name : {"war3map.w3i", "war3mapUnits.doo", "war3map.w3s"})
                if (archive.value->ReadMember(name).value != std::optional(expected.at(name))) return false;
            return true;
        };

        // 保存：三个栈都在保存点。
        const auto first = saveMap("saved-1.w3x"); QVERIFY(first);
        QVERIFY(savedMatches(*first, edited));
        QVERIFY(world->isClean() && settings->isClean() && sounds->isClean()); QVERIFY(!hot.HasUnsavedMapChanges());
        // 撤销：只撤声音（之后还有世界与设置的修改），其他栈仍在保存点。
        sounds->undo();
        QVERIFY(!sounds->isClean() && world->isClean() && settings->isClean());
        soundUndone = UndoLaneState(hot);
        QCOMPARE(soundUndone.at("war3map.w3s"), original.at("war3map.w3s"));
        QCOMPARE(soundUndone.at("war3map.w3i"), edited.at("war3map.w3i"));
        QCOMPARE(soundUndone.at("war3mapUnits.doo"), edited.at("war3mapUnits.doo"));
        QVERIFY(hot.HasUnsavedMapChanges());
        // 再保存：地图只少了音量修改；历史仍可冻结。
        const auto second = saveMap("saved-2.w3x"); QVERIFY(second);
        QVERIFY(savedMatches(*second, soundUndone));
        QVERIFY(world->isClean() && settings->isClean() && sounds->isClean()); QVERIFY(sounds->canRedo());
        QCOMPARE(Sha256File(source).value, sourceSha.value);

        finalDraft = TerrainJournalDraftBytes(*hot.mapEdits_);
        const auto written = hot.SaveProjectToPath(forge); QVERIFY2(written.Succeeded(), ReadDiagnosticText(written).c_str());
        QVERIFY(!HasReadDiagnostic(written, "TERRAIN_JOURNAL_DRAFT_ONLY"));
        world->clear(); settings->clear(); sounds->clear();
    }
    // 冷恢复：回执按触及成员分回三个栈，各自的当前与保存位置保持。
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QVERIFY((*opened.value)->terrainJournalWarning.empty());
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{4});
    QCOMPARE((*opened.value)->terrainCurrentIndex, std::size_t{3});
    QCOMPARE((*opened.value)->terrainSavedIndex, std::optional<std::size_t>(3));
    QCOMPARE(TerrainJournalDraftBytes(*(*opened.value)->mapDataEdits), finalDraft);
    EditorProject cold;
    auto* world = cold.mapDataUndoStack_; auto* settings = cold.mapSettingsUndoStack_; auto* sounds = cold.soundUndoStack_;
    QVERIFY(BindPreparedTerrainJournal(cold, **opened.value, game));
    QCOMPARE(world->count(), 1); QCOMPARE(world->index(), 1); QVERIFY(world->isClean());
    QCOMPARE(settings->count(), 2); QCOMPARE(settings->index(), 2); QVERIFY(settings->isClean());
    QCOMPARE(sounds->count(), 1); QCOMPARE(sounds->index(), 0); QVERIFY(sounds->isClean()); QVERIFY(sounds->canRedo());
    QCOMPARE(UndoLaneState(cold), soundUndone);
    sounds->redo(); QCOMPARE(UndoLaneState(cold), edited);
    // 世界页撤销起点：作者与地图名（之后的设置修改）保留。
    world->undo();
    QCOMPARE(UndoLaneStart(cold), originalStart);
    QCOMPARE(cold.mapEdits_->Snapshot().Settings()->author, std::string("Saved lane author"));
    QCOMPARE(cold.mapEdits_->Snapshot().Settings()->mapName, std::string("Saved lane name"));
    settings->undo(); settings->undo(); sounds->undo();
    QCOMPARE(UndoLaneState(cold), original);
    world->redo(); settings->redo(); settings->redo(); sounds->redo();
    QCOMPARE(UndoLaneState(cold), edited);
    QVERIFY(!ModelHasCode(*cold.diagnosticsModel_, QStringLiteral("MAP_UNDO_REFUSED")));
    world->clear(); settings->clear(); sounds->clear();
}
