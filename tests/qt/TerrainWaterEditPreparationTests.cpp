#include "TerrainWaterEditPreparation.h"
#include "TerrainWaterEditFixture.h"

#include <QTemporaryDir>
#include <QTest>

using namespace War3Map;
using namespace War3Map::QtProject;
using namespace War3Map::Tests;

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
        auto captured = WaterTake(LoadTerrainWaterEditResources(frozen));
        const MapData::TerrainStrokePoint first{384, 384};
        MapData::TerrainWaterBrush brush{MapData::TerrainWaterBrushKind::Deep, MapData::TerrainBrushShape::Circle, 1, 0, 0,
            WaterTake(MapData::CaptureTerrainWaterStrokeAnchor(*frozen->terrain, first))};
        auto candidate = MapData::PrepareTerrainWaterStroke(*frozen->terrain, *captured.resources.WaterBrushRules(),
            brush, std::span(&first, 1), frozen->doodads);
        QVERIFY(!candidate.Succeeded());
        QVERIFY(!specialProject.mapEdits_->PrepareTerrainWaterEdit(captured.resources, brush, std::span(&first, 1)).Succeeded());
        QCOMPARE(specialProject.mapEdits_->Revision(), std::uint64_t{0});
    }
};

QTEST_GUILESS_MAIN(TerrainWaterEditPreparationTests)
#include "TerrainWaterEditPreparationTests.moc"
