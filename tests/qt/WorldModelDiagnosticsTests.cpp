#include "WorldAnimationDiagnostics.h"
#include "WorldPlacementModels.h"
#include "WorldModule.h"
#include "EditorProject.h"
#include "Theme/ThemeManager.h"
#include "Fonts/FontLoader.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Services/WorldInstanceRegistry.h"
#include "../MapDataFixtures.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>

using namespace War3Map;
using namespace War3Map::Modules;
namespace {
std::vector<std::byte> Bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span(text.data(), text.size())); return {view.begin(), view.end()};
}
bool InstallCatalog(QtProject::EditorProject& project) {
    const std::string slk = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"file\"\nC;X3;K\"uberSplat\"\n"
        "C;X1;Y2;K\"hfoo\"\nC;X2;K\"\"\nC;X3;K\"\"\n"
        "C;X1;Y3;K\"hkee\"\nC;X2;K\"Units\\\\Human\\\\Keep.mdl\"\nC;X3;K\"HSMA\"\nE\n";
    ObjectData::CatalogLoadOptions options; options.stringFiles.clear();
    options.sources = {{ObjectData::ObjectType::Unit, {"original-unit.slk"}, {}, {"name"}, {"art"}}};
    auto loaded = ObjectData::StandardObjectCatalog::Load([&](const std::string&) {
        return Result<std::vector<std::byte>>::Success(Bytes(slk));
    }, options);
    if (!loaded.Succeeded()) return false;
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*loaded.value));
    const std::string lni = "[U001]\n_parent=\"hfoo\"\n[U002]\n_parent=\"ZZZZ\"\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(Bytes(lni));
    if (!parsed.Succeeded()) return false;
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    return project.objectDocuments_[0] != nullptr;
}
bool InstallApplicabilityCatalog(QtProject::EditorProject& project) {
    const std::string slk = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"isbldg\"\nC;X3;K\"uberSplat\"\n"
        "C;X4;K\"hero\"\nC;X5;K\"creep\"\n"
        "C;X1;Y2;K\"hfoo\"\nC;X2;K0\nC;X4;K0\nC;X5;K0\n"
        "C;X1;Y3;K\"hkee\"\nC;X2;K1\nC;X4;K0\nC;X5;K0\n"
        "C;X1;Y4;K\"hemp\"\nC;X2;K0\nC;X3;K\"\"\n"
        "C;X1;Y5;K\"hund\"\nC;X2;K0\nC;X3;K\"_\"\n"
        "C;X1;Y6;K\"hunk\"\nC;X2;K0\nC;X3;K\"ZZZZ\"\n"
        "C;X1;Y7;K\"hmis\"\n"
        "C;X1;Y8;K\"hbad\"\nC;X2;K\"unknown\"\n"
        "C;X1;Y9;K\"hher\"\nC;X2;K0\nC;X4;K1\nC;X5;K0\n"
        "C;X1;Y10;K\"hcre\"\nC;X2;K0\nC;X4;K0\nC;X5;K1\n"
        "C;X1;Y11;K\"hcat\"\nC;X2;K0\nE\n";
    ObjectData::CatalogLoadOptions options; options.stringFiles.clear();
    options.sources = {{ObjectData::ObjectType::Unit, {"original-applicability.slk"}, {}, {"name"}, {"art"}}};
    auto catalog = ObjectData::StandardObjectCatalog::Load([&](const std::string&) {
        return Result<std::vector<std::byte>>::Success(Bytes(slk));
    }, options);
    if (!catalog.Succeeded()) return false;
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*catalog.value));
    const std::string lni = "[UMIS]\n_parent=\"ZZZZ\"\n[UOPA]\n_parent=\"hfoo\"\nuubs={\n}\n"
        "[UBOP]\n_parent=\"hfoo\"\nubdg={\n}\n";
    auto objects = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(Bytes(lni));
    if (!objects.Succeeded()) return false;
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(objects.document.release()));
    return project.objectDocuments_[0] != nullptr;
}
bool InstallApplicabilitySchema(QtProject::EditorProject& project, std::optional<std::string> useUnit = "0",
    std::string useBuilding = "1", std::string useHero = "0", std::optional<std::string> useCreep = {}) {
    std::string slk = "ID;PWXL\nC;X1;Y1;K\"ID\"\nC;X2;K\"field\"\nC;X3;K\"slk\"\n"
        "C;X4;K\"useUnit\"\nC;X5;K\"useBuilding\"\nC;X6;K\"useHero\"\nC;X7;K\"useCreep\"\n"
        "C;X1;Y2;K\"uubs\"\nC;X2;K\"uberSplat\"\nC;X3;K\"UnitUI\"\n";
    if (useUnit) slk += "C;X4;K\"" + *useUnit + "\"\n";
    slk += "C;X5;K\"" + useBuilding + "\"\nC;X6;K\"" + useHero + "\"\n";
    if (useCreep) slk += "C;X7;K\"" + *useCreep + "\"\n";
    slk += "E\n";
    ObjectData::ObjectFieldSchemaOptions options; options.sources = {{ObjectData::ObjectType::Unit, "original-metadata.slk"}};
    options.stringFiles.clear(); options.worldEditDataFile.clear(); options.unitEditorDataFile.clear();
    auto schema = ObjectData::ObjectFieldSchema::Load([&](const std::string&) {
        return Result<std::vector<std::byte>>::Success(Bytes(slk));
    }, options);
    if (!schema.Succeeded()) return false;
    project.objectFieldSchema_ = std::make_unique<ObjectData::ObjectFieldSchema>(std::move(*schema.value));
    return true;
}
std::vector<WorldModelDiagnosticRow> VisibleRows() {
    return {{QStringLiteral("Units/Human/<Keep>.mdx"), QStringLiteral("单位 · 城镇大厅 [hkee]"), QStringLiteral("WORLD_MODEL_UNAVAILABLE"),
        QStringLiteral("地图导入覆盖来源无法读取；原文件保持"), QStringLiteral("地图导入 · SHA ") + QString(64, 'a'),
        {{MapData::PlacementFile::Units, 3, "hkee", 104}, {MapData::PlacementFile::Units, 7, "hkee", 108}}, 3, true},
        {QString{}, QStringLiteral("定义未知 · X999"), QStringLiteral("WORLD_OBJECT_DEFINITION_MISSING"),
        QStringLiteral("当前物编/原版目录没有定义；未猜模型"), QStringLiteral("来源未知"),
        {{MapData::PlacementFile::Units, 4, "X999", 105}}, 1, false},
        {QStringLiteral("Doodads/Environment/Ruins.mdx"), QStringLiteral("装饰物 · 遗迹 [D001]"), QStringLiteral("GEOA_KEYS_INVALID"),
        QStringLiteral("网格局部隔离，其他网格继续显示"), QStringLiteral("游戏来源 · SHA ") + QString(64, 'b'),
        {{MapData::PlacementFile::Doodads, 2, "D001", 21}}, 1, true}};
}
}
class WorldModelDiagnosticsTests final : public QObject {
    Q_OBJECT
private slots:
    void ActualRegistrySpecsBindExactOwnersAndRejectAlternateHostOrIdentity() {
        auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11()); QVERIFY(units.Succeeded());
        auto doodads = MapData::ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11()); QVERIFY(doodads.Succeeded());
        for (int i = 0; i < 2; ++i) {
            auto unit = std::get<MapData::UnitPlacement>(MakeNewPlacement(ObjectData::ObjectType::Unit, "hfoo", 0, {0, 0, 0}));
            unit.creationId = 100 + i; units.value->units.push_back(std::move(unit));
        }
        const auto unitBytes = units.value->Serialize(); const auto doodadBytes = doodads.value->Serialize();
        QVERIFY(unitBytes.Succeeded()); QVERIFY(doodadBytes.Succeeded());
        const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *unitBytes.value}, {"war3map.doo", *doodadBytes.value}};
        const auto document = MapDocument::FromMembers(members);
        MapData::MapDataEditSession edits(document);
        std::vector<MapData::PlacementId> owners;
        std::vector<Render::ModelInstanceRequest> requests;
        for (std::uint32_t i = 0; i < 2; ++i) {
            owners.push_back(MapData::MakePlacementId(MapData::PlacementFile::Units, i, document.Units()->units[i]));
            Render::ModelInstanceRequest request; request.modelPath = "original-fixture.mdx"; requests.push_back(std::move(request));
        }
        const auto registry = Services::WorldInstanceRegistry::Create(document,
            {17, 0, edits.SessionIdentity(), edits.Revision(), "owned-source-SHA"}, requests, owners);
        QVERIFY(registry.Succeeded()); QVERIFY(*registry.value);
        const auto& specs = (*registry.value)->CapturedSpecs(); QCOMPARE(specs.size(), owners.size());
        QVERIFY(specs[0].hostKey != WorldPlacementHostKey(owners[0])); // 原失败必须真正存在于成功registry路径。
        Render::AnimationInstanceState state; state.identity = specs[0].identity;
        QVERIFY(WorldDiagnosticPlacementMatches(owners[0], 0, specs[0], state, registry.value->get()));
        QVERIFY(!WorldDiagnosticPlacementMatches(owners[1], 0, specs[0], state, registry.value->get()));
        auto wrongOwner = owners[0]; ++wrongOwner.creationId;
        QVERIFY(!WorldDiagnosticPlacementMatches(wrongOwner, 0, specs[0], state, registry.value->get()));
        auto wrongHost = specs[0]; wrongHost.hostKey = WorldPlacementHostKey(owners[0]);
        QVERIFY(!WorldDiagnosticPlacementMatches(owners[0], 0, wrongHost, state, registry.value->get()));
        auto wrongState = state; wrongState.identity = specs[1].identity;
        QVERIFY(!WorldDiagnosticPlacementMatches(owners[0], 0, specs[0], wrongState, registry.value->get()));
        QVERIFY(!WorldDiagnosticPlacementMatches(owners[0], specs.size(), specs[0], state, registry.value->get()));
        QVERIFY(WorldDiagnosticPlacementMatches(owners[0], 0, wrongHost, state, nullptr));
        QVERIFY(!WorldDiagnosticPlacementMatches(owners[1], 0, wrongHost, state, nullptr));
    }
    // 物体管理的放置引用：完整身份吻合才选中并定位；创建编号或序号不符、格式错误都拒绝，不猜替代实例。
    void RevealPlacementSelectsExactIdentityOnly() {
        QtProject::EditorProject project; QVERIFY(InstallCatalog(project));
        MapData::UnitPlacements units;
        for (const auto rawcode : {"hfoo", "hkee"}) {
            auto unit = std::get<MapData::UnitPlacement>(MakeNewPlacement(ObjectData::ObjectType::Unit, rawcode, 0,
                {static_cast<float>(units.units.size()) * 256, 64, 0}));
            unit.creationId = static_cast<std::int32_t>(units.units.size() + 200); units.units.push_back(std::move(unit));
        }
        const auto encoded = units.Serialize(); QVERIFY(encoded.Succeeded());
        project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
            std::vector<MapArchiveMember>{{"war3mapUnits.doo", *encoded.value}}));
        project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
        WorldModule module(project); (void)module.Widget();
        const auto keep = QString::fromLatin1(QByteArray("hkee").toHex());
        QVERIFY(!module.Reveal({"world", "placement", QStringLiteral("units:1:999:%1").arg(keep), {}}));
        QVERIFY(!module.Reveal({"world", "placement", QStringLiteral("units:0:201:%1").arg(keep), {}}));
        QVERIFY(!module.Reveal({"world", "placement", QStringLiteral("units:1:201:zz"), {}}));
        QVERIFY(!module.Reveal({"world", "placement", QStringLiteral("doodads:1:201:%1").arg(keep), {}}));
        QVERIFY(module.Reveal({"world", "placement", QStringLiteral("units:1:201:%1").arg(keep), {}}));
    }
    void UnboundModuleCannotPublishRealSourceCapture() {
        QtProject::EditorProject project; WorldModule module(project);
        QVERIFY(!module.ModelDiagnosticsReady());
        const auto capture = module.CaptureModelDiagnosticsView(); QVERIFY(!capture.Succeeded()); QVERIFY(!capture.value);
        QVERIFY(std::any_of(capture.diagnostics.begin(), capture.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "WORLD_DIAGNOSTIC_CAPTURE_NOT_READY";
        }));
    }
    void CaptureKeepsEachUnresolvedIdentityAndFieldPresence() {
        QtProject::EditorProject project; QVERIFY(InstallCatalog(project));
        MapData::UnitPlacements units;
        for (const auto rawcode : {"hfoo", "U001", "U002", "X999", "hkee"}) {
            auto unit = std::get<MapData::UnitPlacement>(MakeNewPlacement(ObjectData::ObjectType::Unit, rawcode, 0,
                {static_cast<float>(units.units.size()) * 128, 64, 0}));
            unit.creationId = static_cast<std::int32_t>(units.units.size() + 100); units.units.push_back(std::move(unit));
        }
        const auto encoded = units.Serialize(); QVERIFY(encoded.Succeeded());
        const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *encoded.value}};
        const auto document = MapDocument::FromMembers(members);
        const auto capture = CaptureWorldPlacements(project, document);
        QCOMPARE(capture.unresolved, std::size_t(4)); QCOMPARE(capture.unresolvedRows.size(), std::size_t(4));
        QCOMPARE(capture.requests.size(), std::size_t(1));
        for (std::size_t i = 0; i < capture.unresolvedRows.size(); ++i) {
            const auto& row = capture.unresolvedRows[i]; QCOMPARE(row.owner.index, static_cast<std::uint32_t>(i));
            QCOMPARE(row.owner.creationId, static_cast<std::int32_t>(100 + i));
            QCOMPARE(row.position[0], static_cast<float>(i * 128)); QVERIFY(MapData::FindPlacement(document, row.owner).Succeeded());
        }
        QCOMPARE(capture.unresolvedRows[0].reasonCode, std::string("WORLD_MODEL_FIELD_EMPTY"));
        QVERIFY(!capture.unresolvedRows[0].origins.empty());
        QCOMPARE(capture.unresolvedRows[2].reasonCode, std::string("WORLD_MODEL_SOURCE_UNAVAILABLE"));
        QCOMPARE(capture.unresolvedRows[3].reasonCode, std::string("WORLD_OBJECT_DEFINITION_MISSING"));
        const auto splats = CaptureWorldBuildingSplats(project, document, {}); QVERIFY(splats.Succeeded());
        QCOMPARE(splats.value->fieldDiagnostics.size(), std::size_t(5));
        const auto& empty = splats.value->fieldDiagnostics[0];
        QCOMPARE(empty.uubs.state, Services::PlacementFieldState::Known); QVERIFY(empty.uubs.value.empty());
        QVERIFY(!empty.origins.empty()); QCOMPARE(empty.origins[0].sourcePath, std::string("17:original-unit.slk"));
        // 原生产无贴画统计保持到真实副本证据确认后；此回归先证明presence与空值可取证。
        QCOMPARE(splats.value->units.size(), std::size_t(5));
        QCOMPARE(splats.value->fieldDiagnostics[2].uubs.state, Services::PlacementFieldState::Unavailable);
        QCOMPARE(splats.value->fieldDiagnostics[4].uubs.value, std::string("HSMA"));
        const auto reencoded = document.Units()->Serialize(); QVERIFY(reencoded.Succeeded()); QCOMPARE(*reencoded.value, *encoded.value);
    }
    void OnlyAbsentUubsWithExplicitNonBuildingSchemaBecomesNotApplicable() {
        QtProject::EditorProject project; QVERIFY(InstallApplicabilityCatalog(project)); QVERIFY(InstallApplicabilitySchema(project, "0", "1", "1"));
        const auto sourceObjects = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Serialize(*project.objectDocuments_[0]); QVERIFY(sourceObjects.Succeeded());
        MapData::UnitPlacements units;
        for (const auto rawcode : {"hfoo", "hkee", "hemp", "hund", "hunk", "hmis", "hbad", "UMIS", "UOPA", "UBOP", "hher", "hcre", "hcat"}) {
            auto unit = std::get<MapData::UnitPlacement>(MakeNewPlacement(ObjectData::ObjectType::Unit, rawcode, 0, {0, 0, 0}));
            unit.creationId = static_cast<std::int32_t>(units.units.size() + 10); units.units.push_back(std::move(unit));
        }
        const auto encoded = units.Serialize(); QVERIFY(encoded.Succeeded());
        const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *encoded.value}};
        const auto document = MapDocument::FromMembers(members);
        const auto captured = CaptureWorldBuildingSplats(project, document, {}); QVERIFY(captured.Succeeded());
        QCOMPARE(captured.value->units.size(), std::size_t(13));
        const auto& first = captured.value->fieldDiagnostics[0];
        QCOMPARE(first.uubs.state, Services::PlacementFieldState::Absent); QVERIFY(first.uubs.value.empty());
        QVERIFY(first.origins.empty()); QVERIFY(!first.uubs.reason.empty());
        QCOMPARE(first.configuration, Services::BuildingSplatConfiguration::NotApplicableToNonBuilding);
        QCOMPARE(first.applicabilityOrigins.size(), std::size_t(4));
        QCOMPARE(first.applicabilityOrigins[0].field, std::string("ubdg"));
        QCOMPARE(first.applicabilityOrigins[1].field, std::string("hero"));
        QCOMPARE(first.applicabilityOrigins[2].field, std::string("creep"));
        QCOMPARE(first.applicabilityOrigins[3].field, std::string("uubs-applicability-schema"));
        QVERIFY(!first.applicabilityReason.empty()); QVERIFY(captured.value->units[0].fields.complete);
        for (std::size_t i = 1; i < captured.value->units.size(); ++i)
            QCOMPARE(captured.value->units[i].fields.configuration, Services::BuildingSplatConfiguration::EffectiveField);
        QCOMPARE(captured.value->fieldDiagnostics[1].uubs.state, Services::PlacementFieldState::Absent); // 真建筑不可猜空。
        QCOMPARE(captured.value->fieldDiagnostics[2].uubs.state, Services::PlacementFieldState::Known);
        QVERIFY(captured.value->fieldDiagnostics[2].uubs.value.empty());
        QCOMPARE(captured.value->fieldDiagnostics[3].uubs.value, std::string("_"));
        QCOMPARE(captured.value->fieldDiagnostics[4].uubs.value, std::string("ZZZZ"));
        QCOMPARE(captured.value->fieldDiagnostics[7].uubs.state, Services::PlacementFieldState::Unavailable);
        QCOMPARE(captured.value->fieldDiagnostics[8].uubs.state, Services::PlacementFieldState::Opaque);
        QVERIFY(!captured.value->units[9].fields.complete); // ubdg opaque也不能授权不适用。
        QVERIFY(!captured.value->units[10].fields.complete); // useHero1允许实际hero1，不误转Absent。
        QVERIFY(!captured.value->units[11].fields.complete); // useCreep缺项默认true，实际creep1仍适用。
        QVERIFY(!captured.value->units[12].fields.complete); // 无真实类别信息，即使ubdg0也不能猜普通单位。
        const auto* definition = project.objectFieldSchema_->FindNative(ObjectData::ObjectType::Unit, "uubs"); QVERIFY(definition);
        QVERIFY(definition->useCreep); QVERIFY(!definition->metadata.contains("usecreep"));
        QVERIFY(definition->useHero);
        const auto strictIdentity = captured.value->inputIdentity;
        QVERIFY(InstallApplicabilitySchema(project, "0", "1", "1", "0"));
        const auto creepRejected = CaptureWorldBuildingSplats(project, document, {}); QVERIFY(creepRejected.Succeeded());
        QCOMPARE(creepRejected.value->units[11].fields.configuration, Services::BuildingSplatConfiguration::NotApplicableToNonBuilding);
        QCOMPARE(creepRejected.value->fieldDiagnostics[11].uubs.state, Services::PlacementFieldState::Absent);
        QVERIFY(creepRejected.value->inputIdentity != strictIdentity);
        for (const auto flag : {std::optional<std::string>{"1"}, std::optional<std::string>{}, std::optional<std::string>{"unknown"}}) {
            QVERIFY(InstallApplicabilitySchema(project, flag));
            const auto refused = CaptureWorldBuildingSplats(project, document, {}); QVERIFY(refused.Succeeded());
            QCOMPARE(refused.value->units[0].fields.configuration, Services::BuildingSplatConfiguration::EffectiveField);
            QVERIFY(!refused.value->units[0].fields.complete); QVERIFY(refused.value->inputIdentity != strictIdentity);
            QCOMPARE(refused.value->fieldDiagnostics[0].uubs.state, Services::PlacementFieldState::Absent);
            QVERIFY(refused.value->fieldDiagnostics[0].origins.empty());
        }
        QVERIFY(InstallApplicabilitySchema(project, "0", "0", "1"));
        const auto buildingRejected = CaptureWorldBuildingSplats(project, document, {}); QVERIFY(buildingRejected.Succeeded());
        QVERIFY(!buildingRejected.value->units[0].fields.complete);
        QCOMPARE(buildingRejected.value->units[0].fields.configuration, Services::BuildingSplatConfiguration::EffectiveField);
        project.objectFieldSchema_.reset();
        const auto noSchema = CaptureWorldBuildingSplats(project, document, {}); QVERIFY(noSchema.Succeeded());
        QVERIFY(!noSchema.value->units[0].fields.complete);
        const auto sourceAfter = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Serialize(*project.objectDocuments_[0]); QVERIFY(sourceAfter.Succeeded());
        QCOMPARE(*sourceAfter.value, *sourceObjects.value);
        const auto placementAfter = document.Units()->Serialize(); QVERIFY(placementAfter.Succeeded());
        QCOMPARE(*placementAfter.value, *encoded.value);
    }
    void ListLocatesExactOwnerAndRejectsStaleCallback() {
        WorldModelDiagnosticsDialog dialog; auto rows = VisibleRows();
        std::optional<MapData::PlacementId> located;
        dialog.SetRows(rows, [&](const auto& owner) { located = owner; return true; });
        auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("worldModelDiagnosticsTable")); QVERIFY(table);
        QCOMPARE(table->rowCount(), 3); QCOMPARE(table->item(0, 3)->text(), QStringLiteral("2"));
        QCOMPARE(table->columnCount(), 7); QCOMPARE(table->item(0, 5)->text(), QStringLiteral("未请求占位"));
        QVERIFY(table->item(0, 0)->text().contains("<Keep>")); QVERIFY(table->item(0, 0)->toolTip().contains("&lt;Keep&gt;"));
        dialog.show(); QCoreApplication::processEvents();
        const auto target = table->visualItemRect(table->item(0, 0)).center();
        QSignalSpy doubleClicked(table, &QTableWidget::cellDoubleClicked); QVERIFY(doubleClicked.isValid());
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        QVERIFY(!located); // 首次press/release只选择，下一double-click必须走真实view signal。
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, target); QCoreApplication::processEvents();
        QCOMPARE(doubleClicked.count(), 1);
        QVERIFY(located); QVERIFY(*located == rows[0].owners[0]);
        QVERIFY(dialog.LocateRow(0)); QVERIFY(located); QVERIFY(*located == rows[0].owners[0]);
        dialog.SetRows(rows, [&](const auto&) { return false; });
        QVERIFY(!dialog.LocateRow(0)); QVERIFY(!dialog.LocateRow(99));
        QCOMPARE(dialog.Rows()[0].owners.size(), std::size_t(2));
    }
    void ExportUsesAllRowsAndNeverWritesMapTarget() {
        QTemporaryDir root; QVERIFY(root.isValid()); auto rows = VisibleRows();
        rows[0].placeholderState = QStringLiteral("已准备 1/2 紫黑占位；显示待核验");
        rows[0].reason += QStringLiteral("\n第二行\t原文");
        const auto target = root.filePath("models.txt"); QVERIFY(ExportWorldModelDiagnostics(rows, target).Succeeded());
        QFile text(target); QVERIFY(text.open(QIODevice::ReadOnly)); const auto actual = text.readAll();
        QVERIFY(actual == WorldModelDiagnosticText(rows).toUtf8()); QVERIFY(actual.contains("\\n")); QVERIFY(actual.contains("\\t"));
        QVERIFY(actual.contains("104")); QVERIFY(actual.contains("108")); QVERIFY(actual.contains("X999"));
        QVERIFY(actual.contains(rows[0].placeholderState.toUtf8()));
        QFile map(root.filePath("owned-original.w3x")); QVERIFY(map.open(QIODevice::WriteOnly)); QCOMPARE(map.write("original"), qint64(8)); map.close();
        QVERIFY(!ExportWorldModelDiagnostics(rows, map.fileName()).Succeeded()); QVERIFY(map.open(QIODevice::ReadOnly)); QCOMPARE(map.readAll(), QByteArray("original"));
        QVERIFY(!ExportWorldModelDiagnostics(rows, root.filePath("missing-parent/models.txt")).Succeeded());
    }
    void LocateCallbackRebindingOrDestroyingDialogCannotPublishStaleStatus() {
        auto rows = VisibleRows();
        WorldModelDiagnosticsDialog dialog;
        std::optional<MapData::PlacementId> located;
        dialog.SetRows(rows, [&](const auto& owner) {
            located = owner; dialog.SetRows({}, {}); return true;
        });
        QVERIFY(!dialog.LocateRow(0)); QVERIFY(located); QVERIFY(*located == rows[0].owners[0]);
        QVERIFY(dialog.Rows().empty());
        auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("worldModelDiagnosticsTable")); QVERIFY(table);
        QCOMPARE(table->rowCount(), 0);
        auto* destroyed = new WorldModelDiagnosticsDialog;
        const QPointer<WorldModelDiagnosticsDialog> observer(destroyed);
        destroyed->SetRows(rows, [destroyed](const auto&) { delete destroyed; return true; });
        QVERIFY(!destroyed->LocateRow(0)); QVERIFY(observer.isNull());
    }
    void ExportChooserUsesFrozenRowsAndRejectsRebindingOrDestruction() {
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto rows = VisibleRows();
        WorldModelDiagnosticsDialog dialog; dialog.SetRows(rows, {});
        const auto reboundTarget = root.filePath("rebound.txt");
        QVERIFY(!dialog.ExportWithChooser([&](QWidget*) {
            dialog.SetRows({}, {}); return reboundTarget;
        }));
        QVERIFY(!QFileInfo::exists(reboundTarget)); QVERIFY(dialog.Rows().empty());
        auto* destroyed = new WorldModelDiagnosticsDialog; destroyed->SetRows(rows, {});
        const QPointer<WorldModelDiagnosticsDialog> observer(destroyed);
        const auto destroyedTarget = root.filePath("destroyed.txt");
        bool originalParent = false;
        QVERIFY(!destroyed->ExportWithChooser([&](QWidget* parent) {
            originalParent = parent == destroyed; delete destroyed; return destroyedTarget;
        }));
        QVERIFY(originalParent); QVERIFY(observer.isNull()); QVERIFY(!QFileInfo::exists(destroyedTarget));
        dialog.SetRows(rows, {});
        const auto successTarget = root.filePath("complete");
        QVERIFY(dialog.ExportWithChooser([&](QWidget* parent) { return parent == &dialog ? successTarget : QString{}; }));
        QFile file(successTarget + QStringLiteral(".txt")); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), WorldModelDiagnosticText(rows).toUtf8());
    }
    void CaptureChangedDialogOnlyWhenExplicitlyRequested() {
        const auto output = qEnvironmentVariable("FORGE_WORLD_DIAGNOSTICS_CAPTURE_DIR");
        if (output.isEmpty()) QSKIP("Set FORGE_WORLD_DIAGNOSTICS_CAPTURE_DIR for offscreen diagnostic-list evidence.");
        QVERIFY(QFileInfo(output).isAbsolute()); QVERIFY(QDir().mkpath(output));
        const auto imagePath = QDir(output).filePath("model-list.png"); const auto receiptPath = QDir(output).filePath("model-list.geometry.json");
        QVERIFY(!QFileInfo::exists(imagePath)); QVERIFY(!QFileInfo::exists(receiptPath));
        UiKit::ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
        WorldModelDiagnosticsDialog dialog; dialog.SetRows(VisibleRows(), [](const auto&) { return false; });
        dialog.show(); QCoreApplication::processEvents();
        const auto image = dialog.grab(); QVERIFY(!image.isNull()); QVERIFY(image.save(imagePath));
        const QJsonObject receipt{{"dpr", image.devicePixelRatio()}, {"logicalWidth", dialog.width()}, {"logicalHeight", dialog.height()},
            {"pixelWidth", image.width()}, {"pixelHeight", image.height()}, {"rows", static_cast<double>(dialog.Rows().size())},
            {"syntheticFixture", true}, {"gpuCompletionProven", false}, {"authorMapViewed", false}};
        QSaveFile file(receiptPath); file.setDirectWriteFallback(false); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(receipt).toJson(); QCOMPARE(file.write(bytes), qint64(bytes.size())); QVERIFY(file.commit());
    }
};
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (const auto fonts = qEnvironmentVariable("FORGE_TEST_FONTS"); !fonts.isEmpty()) UiKit::ApplyApplicationFont(UiKit::LoadFonts(fonts));
    WorldModelDiagnosticsTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "WorldModelDiagnosticsTests.moc"
