#include "TerrainWaterEditPreparation.h"
#include "TerrainWaterEditFixture.h"
#include "MapSaveJob.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Services/TerrainPathingSave.h"

#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using namespace War3Map;
using namespace War3Map::QtProject;
using namespace War3Map::Tests;

namespace {
std::vector<std::byte> WaterSlk(const std::vector<std::string>& columns,
    std::initializer_list<std::vector<std::string>> rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto append = [&](const auto& cells, std::size_t row) {
        for (std::size_t x = 0; x < cells.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(row) + ";K\"" + cells[x] + "\"\n";
    };
    append(columns, 1); std::size_t row = 2;
    for (const auto& cells : rows) append(cells, row++);
    return WaterText(text + "E\n");
}

std::vector<MapArchiveMember> SpecialWaterSaveMembers() {
    auto members = TerrainWaterMembers();
    for (auto& member : members) {
        if (member.name == "TerrainArt/Water.slk")
            member.data = WaterSlk({"waterID", "height", "impassable"}, {{"LSha", "0", "0"}});
        if (member.name == "UI/MiscData.txt") member.data = WaterText(
            "[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n"
            "[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n");
    }
    auto pathing = WaterText("MP3W");
    for (const auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
        pathing.push_back(static_cast<std::byte>((value >> shift) & 255));
    // 干地禁止水行；两个纯黑特殊贴图仅在各自4x4通行区覆盖原生六位。
    pathing.resize(16 + 32 * 32, std::byte{0x51});
    for (const auto ground : {3u, 6u}) for (unsigned y = ground * 4; y < ground * 4 + 4; ++y)
        for (unsigned x = ground * 4; x < ground * 4 + 4; ++x) pathing[16 + y * 32 + x] = std::byte{0x11};
    MapData::MapSettings settings; settings.formatVersion = 25; settings.custom.emplace();
    settings.tileset = 'L'; settings.playableWidth = settings.playableHeight = 8; settings.flags |= 0x400;
    MapData::DoodadPlacements placements;
    placements.specialDoodads = {{"Doo0", 0, {3, 3}}, {"Doo0", 0, {6, 6}}};
    // 独立旧WPM预期保留0x11作者保护位；alpha0要求平层，TGA描述符明确8位alpha。
    std::vector<std::byte> texture(18 + 4 * 4 * 4);
    texture[2] = std::byte{2}; texture[12] = texture[14] = std::byte{4}; texture[16] = std::byte{32};
    texture[17] = std::byte{8};
    members.push_back({"war3map.wpm", std::move(pathing)});
    members.push_back({"war3map.w3i", WaterTake(settings.Serialize())});
    members.push_back({"war3map.doo", WaterTake(placements.Serialize())});
    members.push_back({"war3mapUnits.doo", WaterTake(MapData::UnitPlacements{}.Serialize())});
    members.push_back({"war3map.j", WaterText("// original Qt water save fixture\n")});
    members.push_back({"war3mapMap.tga", WaterText("preserved custom minimap fixture")});
    members.push_back({"PathTextures/qt-special.tga", std::move(texture)});
    members.push_back({"Units/DestructableData.slk", WaterSlk({"destructableID", "code", "walkable"}, {{"Dst0", "Dst0", "0"}})});
    members.push_back({"Doodads/Doodads.slk", WaterSlk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", "PathTextures/qt-special.tga", "C", "-1"}})});
    members.push_back({"Units/DestructableMetaData.slk", WaterSlk({"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"dwlk", "walkable", "DestructableData", "bool", "0", "0", "-1"}})});
    return members;
}
} // namespace

class TerrainWaterEditPreparationTests final : public QObject {
    Q_OBJECT
private slots:
    void ActualSourceAndFrozenPreview_data() {
        QTest::addColumn<bool>("archive");
        QTest::newRow("directory") << false;
        QTest::newRow("archive") << true;
    }
    void ActualSourceAndFrozenPreview() {
        QFETCH(bool, archive);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        EditorProject project;
        InstallTerrainWaterProject(project, directory.path().toStdWString(), archive);
        auto request = CaptureTerrainWaterEditRequest(project, true);
        QVERIFY(request.Succeeded());
        auto frozen = std::make_shared<TerrainWaterEditRequest>(std::move(*request.value));
        auto captured = LoadTerrainWaterEditResources(frozen);
        QVERIFY(captured.Succeeded());
        QVERIFY(VerifyTerrainWaterEditResources(*captured.value).Succeeded());
        QVERIFY(TerrainWaterEditRequestCurrent(project, *frozen, true));
        const auto original = WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
        const MapData::TerrainStrokePoint first{384, 384};
        const auto anchor = WaterTake(MapData::CaptureTerrainWaterStrokeAnchor(*frozen->terrain, first));
        const MapData::TerrainWaterBrush brush{MapData::TerrainWaterBrushKind::Shallow,
            MapData::TerrainBrushShape::Square, 1, 0, 17, anchor};
        const std::array points{first, MapData::TerrainStrokePoint{512, 384}};
        auto preview = MapData::PrepareTerrainWaterStroke(*frozen->terrain, *captured.value->resources.WaterBrushRules(),
            brush, points, frozen->doodads);
        QVERIFY(preview.Succeeded());
        QCOMPARE(WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), original);
        auto plan = project.mapEdits_->PrepareTerrainWaterEdit(captured.value->resources, brush, points);
        QVERIFY(plan.Succeeded());
        QVERIFY(project.mapEdits_->ApplyPreparedTerrainEdit(*plan.value).Succeeded());
        QCOMPARE(WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())),
            WaterTake(MapData::SerializeMapTerrain(preview.value->terrain)));
        QVERIFY(!TerrainWaterEditRequestCurrent(project, *frozen, true));
        QVERIFY(project.mapEdits_->Undo());
        QCOMPARE(WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), original);
        // 字节回到原值仍是新修订，旧水刷请求不能再次提交。
        QVERIFY(!TerrainWaterEditRequestCurrent(project, *frozen, true));
    }
    void IdentityAndPendingCoverage() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        EditorProject project; InstallTerrainWaterProject(project, directory.path().toStdWString(), false);
        auto frozen = WaterTake(CaptureTerrainWaterEditRequest(project, true));
        QVERIFY(!TerrainWaterEditRequestCurrent(project, frozen, false));
        project.pendingImports_.push_back({QStringLiteral("TerrainArt/Terrain.slk"), {}, 1,
            std::make_shared<const std::vector<std::byte>>(WaterText("changed"))});
        QVERIFY(!TerrainWaterEditRequestCurrent(project, frozen, true));
        const auto pending = CaptureTerrainWaterEditRequest(project, true);
        QVERIFY(!pending.Succeeded());
        QCOMPARE(pending.diagnostics.front().code, std::string("TERRAIN_WATER_PENDING_RESOURCES"));
        project.pendingImports_.clear();
        project.sourceChangeSuspected_ = true;
        QVERIFY(!CaptureTerrainWaterEditRequest(project, true).Succeeded());
        QVERIFY(!TerrainWaterEditRequestCurrent(project, frozen, true));
        project.sourceChangeSuspected_ = false;
        project.war3GameRoot_ /= "other";
        QVERIFY(!TerrainWaterEditRequestCurrent(project, frozen, true));
        project.war3GameRoot_ = frozen.gameRoot;
        project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
        QVERIFY(project.mapEdits_->AttachTerrain(TerrainWaterMembers().front().data).Succeeded());
        QVERIFY(!TerrainWaterEditRequestCurrent(project, frozen, true));
        QVERIFY(!CaptureTerrainWaterEditRequest(project, true, 1).Succeeded());
    }
    void SourcePathAliasesAreVerifiedInBackground_data() {
        QTest::addColumn<bool>("archive");
        QTest::newRow("directory-alias") << false;
        QTest::newRow("archive-alias") << true;
    }
    void SourcePathAliasesAreVerifiedInBackground() {
        QFETCH(bool, archive);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        EditorProject project; InstallTerrainWaterProject(project, directory.path().toStdWString(), archive);
        auto& openedPath = archive ? project.sourceArchivePath_ : project.projectRoot_;
        openedPath = openedPath.parent_path() / "." / openedPath.filename();
        auto request = std::make_shared<TerrainWaterEditRequest>(WaterTake(CaptureTerrainWaterEditRequest(project, true)));
        QVERIFY(request->sourcePath != openedPath);
        auto capture = LoadTerrainWaterEditResources(request);
        QVERIFY(capture.Succeeded());
        QVERIFY(TerrainWaterEditRequestCurrent(project, *request, true));
        QVERIFY(VerifyTerrainWaterEditResources(*capture.value).Succeeded());
        const auto other = std::filesystem::path(directory.path().toStdWString()) / "other";
        std::filesystem::create_directory(other);
        if (archive) {
            openedPath = other / "source.w3x";
            QVERIFY(WriteVerifiedArchive(openedPath, TerrainWaterMembers()).Succeeded());
        } else openedPath = other;
        QVERIFY(!TerrainWaterEditRequestCurrent(project, *request, true));
        auto wrong = std::make_shared<TerrainWaterEditRequest>(WaterTake(CaptureTerrainWaterEditRequest(project, true)));
        // GUI仅冻结；即使另一份文件字节相同，也不能借用原来源快照。
        QVERIFY(!LoadTerrainWaterEditResources(wrong).Succeeded());
    }
    void ChangedDirectorySourceRejectsBackgroundResult() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        EditorProject project; InstallTerrainWaterProject(project, directory.path().toStdWString(), false);
        auto frozen = std::make_shared<TerrainWaterEditRequest>(WaterTake(CaptureTerrainWaterEditRequest(project, true)));
        QVERIFY(IO::WriteFileBytes(project.projectRoot_ / "UI/MiscData.txt", WaterText("[Terrain]\nMaxSlope=1\n")).Succeeded());
        QVERIFY(!LoadTerrainWaterEditResources(frozen).Succeeded());
        QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
    }
    void ChangedGameInputsInvalidateCapturedOwner_data() {
        QTest::addColumn<bool>("patch");
        QTest::newRow("new-loose-file") << false;
        QTest::newRow("new-patch-winner") << true;
    }
    void ChangedGameInputsInvalidateCapturedOwner() {
        QFETCH(bool, patch);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        EditorProject project; InstallTerrainWaterProject(project, directory.path().toStdWString(), false);
        auto frozen = std::make_shared<TerrainWaterEditRequest>(WaterTake(CaptureTerrainWaterEditRequest(project, true)));
        auto captured = WaterTake(LoadTerrainWaterEditResources(frozen));
        QVERIFY(VerifyTerrainWaterEditResources(captured).Succeeded());
        if (patch) {
            const std::array members{MapArchiveMember{"UI/WorldEditGameStrings.txt", WaterText("[Terrain]\nMaxSlope=12\n")}};
            QVERIFY(WriteVerifiedArchive(project.war3GameRoot_ / "war3patch.mpq", members).Succeeded());
        } else {
            std::filesystem::create_directory(project.war3GameRoot_ / "UI");
            QVERIFY(IO::WriteFileBytes(project.war3GameRoot_ / "UI/MiscData.txt", WaterText("[Terrain]\nMaxSlope=12\n")).Succeeded());
        }
        // 同路径与同Session修订不代表实际规则来源仍相同；后台复核必须拒绝。
        QVERIFY(TerrainWaterEditRequestCurrent(project, *frozen, true));
        QVERIFY(!VerifyTerrainWaterEditResources(captured).Succeeded());
        QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
    }
    void InvalidAndSpecialDoodadsAreNotEmpty() {
        QTemporaryDir invalid, special; QVERIFY(invalid.isValid() && special.isValid());
        auto invalidMembers = TerrainWaterMembers();
        invalidMembers.push_back({"war3map.doo", WaterText("invalid")});
        EditorProject invalidProject; InstallTerrainWaterProject(invalidProject, invalid.path().toStdWString(), false, invalidMembers);
        QVERIFY(!CaptureTerrainWaterEditRequest(invalidProject, true).Succeeded());
        auto specialMembers = TerrainWaterMembers();
        MapData::DoodadPlacements doodads;
        MapData::SpecialDoodadPlacement placement;
        placement.rawcode = "D001";
        doodads.specialDoodads.push_back(placement);
        specialMembers.push_back({"war3map.doo", WaterTake(doodads.Serialize())});
        EditorProject specialProject; InstallTerrainWaterProject(specialProject, special.path().toStdWString(), false, specialMembers);
        auto frozen = std::make_shared<TerrainWaterEditRequest>(WaterTake(CaptureTerrainWaterEditRequest(specialProject, true)));
        QCOMPARE(frozen->doodads.specialDoodads.size(), std::size_t{1});
        // 旧测试能捕获空规则再拒绝编辑；现在实际特殊对象元数据在资源准备阶段就必须齐全。
        QVERIFY(!LoadTerrainWaterEditResources(frozen).Succeeded());
        QCOMPARE(specialProject.mapEdits_->Revision(), std::uint64_t{0});
    }
    void SpecialDeletionSurvivesTheQtWorker_data() {
        QTest::addColumn<bool>("inPlace");
        QTest::newRow("save-as") << false;
        QTest::newRow("save-in-place") << true;
    }
    void SpecialDeletionSurvivesTheQtWorker() try {
        QFETCH(bool, inPlace);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto members = SpecialWaterSaveMembers();
        EditorProject project; InstallTerrainWaterProject(project, directory.path().toStdWString(), true, members);
        const auto memberBytes = [&](std::string_view name) {
            const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
            if (found == members.end()) throw std::runtime_error("missing Qt special save fixture member");
            return found->data;
        };
        QVERIFY(project.mapEdits_->AttachPathing(memberBytes("war3map.wpm")).Succeeded());
        project.ResetEditTracking();
        auto frozen = std::make_shared<TerrainWaterEditRequest>(WaterTake(CaptureTerrainWaterEditRequest(project, true)));
        auto captured = WaterTake(LoadTerrainWaterEditResources(frozen));
        QCOMPARE(captured.resources.WaterSpecialShapes().size(), std::size_t{2});
        const MapData::TerrainStrokePoint point{384, 384};
        const MapData::TerrainWaterBrush brush{MapData::TerrainWaterBrushKind::Shallow,
            MapData::TerrainBrushShape::Square, 1, 0, 17,
            WaterTake(MapData::CaptureTerrainWaterStrokeAnchor(*frozen->terrain, point))};
        auto prepared = WaterTake(project.mapEdits_->PrepareTerrainWaterEdit(captured.resources, brush, std::span(&point, 1)));
        QVERIFY(project.mapEdits_->ApplyPreparedTerrainEdit(prepared).Succeeded());
        QVERIFY(project.ObserveMapDataEdit("水刷删除特殊模型"));
        const auto currentDoodads = [&] {
            return WaterTake(project.mapEdits_->Snapshot(MapDataKind::Doodads, 1024 * 1024)).Doodads().value();
        };
        QCOMPARE(currentDoodads().specialDoodads.size(), std::size_t{1});
        QCOMPARE(currentDoodads().specialDoodads.front().position, (std::array<std::int32_t, 2>{6, 6}));
        const auto changedDoo = WaterTake(currentDoodads().Serialize());
        const auto changedTerrain = WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
        const auto sessionIdentity = project.mapEdits_->SessionIdentity();
        {
            MapSaveJob untrusted;
            QVERIFY(untrusted.FreezeArchive(*project.archiveSession_).Succeeded());
            untrusted.target = std::filesystem::path(directory.path().toStdWString()) / "untrusted-must-not-exist.w3x";
            untrusted.minimapGameRoot = project.war3GameRoot_;
            untrusted.terrainDerivationRequired = true;
            untrusted.replacements = project.mapEdits_->Replacements();
            EditorProject::RunMapSave(untrusted);
            QVERIFY(!untrusted.succeeded && !std::filesystem::exists(untrusted.target));
            QVERIFY(std::any_of(untrusted.diagnostics.begin(), untrusted.diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.code == "TERRAIN_SPECIAL_PROVENANCE_REQUIRED";
            }));
        }
        for (unsigned step = 0; step < 3; ++step) {
            if (step == 1) QVERIFY(project.mapEdits_->Undo() && project.ObserveMapDataEdit("保存后撤销水刷"));
            if (step == 2) QVERIFY(project.mapEdits_->Redo() && project.ObserveMapDataEdit("再次保存后重做水刷"));
            const auto expectedTerrain = step == 1 ? memberBytes("war3map.w3e") : changedTerrain;
            const auto expectedDoo = step == 1 ? memberBytes("war3map.doo") : changedDoo;
            auto job = std::make_unique<MapSaveJob>();
            QVERIFY(job->FreezeArchive(*project.archiveSession_).Succeeded());
            job->target = inPlace ? project.archiveSession_->SourcePath()
                : std::filesystem::path(directory.path().toStdWString()) / ("saved-" + std::to_string(step) + ".w3x");
            job->inPlace = inPlace;
            job->inPlaceOptions.backupDirectory = std::filesystem::path(directory.path().toStdWString()) / "backups";
            std::filesystem::create_directories(job->inPlaceOptions.backupDirectory);
            job->minimapGameRoot = project.war3GameRoot_;
            job->replacements = project.mapEdits_->Replacements(); job->removals = project.mapEdits_->Removals();
            job->terrainDerivationRequired = true;
            job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
            QVERIFY(job->terrainSessionSnapshot && !job->terrainSessionSnapshot->CanUndo());
            QCOMPARE(job->terrainSessionSnapshot->SessionIdentity(), sessionIdentity);
            QCOMPARE(job->terrainSessionSnapshot->Revision(), project.mapEdits_->Revision());
            job->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(),
                project.editRevision_.Current(), project.luaMapTargetGeneration_};
            EditorProject::RunMapSave(*job);
            QVERIFY2(job->succeeded, job->diagnostics.empty() ? "Qt special save worker failed" : job->diagnostics.back().message.c_str());
            QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
            QCOMPARE(WaterTake(job->nextSession->ReadMember("war3map.w3e")), expectedTerrain);
            QCOMPARE(WaterTake(job->nextSession->ReadMember("war3map.doo")), expectedDoo);
            if (step == 1) QCOMPARE(WaterTake(job->nextSession->ReadMember("war3map.wpm")), memberBytes("war3map.wpm"));
            QVERIFY(project.CommitMapSave(*job));
            QCOMPARE(project.mapEdits_->SessionIdentity(), sessionIdentity);
            QVERIFY(!project.HasUnsavedMapChanges());
        }
    } catch (const std::exception& error) {
        QFAIL(error.what());
    }
};

QTEST_GUILESS_MAIN(TerrainWaterEditPreparationTests)
#include "TerrainWaterEditPreparationTests.moc"
