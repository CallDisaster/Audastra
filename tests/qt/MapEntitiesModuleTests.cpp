#include "MapEntitiesModule.h"

#include "EditorProject.h"
#include "ObjectNames.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "../MapDataFixtures.h"

#include <QApplication>
#include <QCheckBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QInputDialog>
#include <QPushButton>
#include <QJsonArray>
#include <QScrollBar>
#include <QTabWidget>
#include <QTimer>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <bit>
#include <numbers>
#include <span>

using namespace War3Map;

class MapEntitiesModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void EditsUseOneOverlayAndUndoAcrossCategories();
    void DoodadRegionCameraFieldsRoundTripThroughCommands();
    void TypePresentationDefaultsAndDegreesKeepRawStorage();
    void CurrentStringTableNamesAndStartLocationsPreserveStoredValues();
    void MovingStartLocationUndoesBothMembersToTheirOriginalBytes();
    void IndexedRevealDistinguishesDuplicateRegionNames();
    void NavigationBookmarkPreservesTypedIdentityAndScroll();
    void NavigationBookmarkBlocksModalDraft();
    void AuxiliaryCrudUsesSharedUndoAndReferences();
    void AuxiliaryDialogsKeepCapturedProjectBinding();
};

void MapEntitiesModuleTests::IndexedRevealDistinguishesDuplicateRegionNames() {
    MapData::RegionRecord region{}; region.name = "Shared"; region.right = 512; region.top = 256;
    const auto bytes = MapData::RegionsDocument{5, {region, region}, {}}.Serialize();
    QVERIFY(bytes.Succeeded() && bytes.value);
    QtProject::EditorProject project;
    const std::vector<MapArchiveMember> members{{"war3map.w3r", *bytes.value}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::MapEntitiesModule module(project);
    auto* table = module.Widget()->findChild<QTableWidget*>("mapEntitiesTable2");
    QVERIFY(table && table->rowCount() == 2);
    QVERIFY(module.Reveal({"entities", "region", "Shared", {{"sourceIndex", QStringLiteral("1")}}}));
    QCOMPARE(table->currentRow(), 1);
    for (const auto& bad : {QVariant(QStringLiteral("+1")), QVariant(QStringLiteral("-1")), QVariant(QStringLiteral(" 1")), QVariant(QStringLiteral("1x")), QVariant(QStringLiteral("18446744073709551616")), QVariant(QStringLiteral("2")), QVariant(1)})
        QVERIFY(!module.Reveal({"entities", "region", "Shared", {{"sourceIndex", bad}}}));
    QCOMPARE(table->currentRow(), 1);
}

void MapEntitiesModuleTests::EditsUseOneOverlayAndUndoAcrossCategories() {
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    QVERIFY(units.Succeeded());
    MapData::UnitPlacement unit;
    unit.rawcode = "hfoo";
    unit.scale = {1, 1, 1};
    unit.position = {100, 200, 0};
    unit.owner = 0;
    unit.creationId = 5;
    units.value->units.push_back(unit);
    const auto unitBytes = units.value->Serialize();
    QVERIFY(unitBytes.Succeeded());
    std::vector<MapArchiveMember> members{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", *unitBytes.value}};
    QtProject::EditorProject project;
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::MapEntitiesModule module(project);
    auto* page = module.Widget();
    QVERIFY(page != nullptr);
    auto* table = page->findChild<QTableWidget*>("mapEntitiesTable0");
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("所属玩家"));
    auto* raw = page->findChild<QCheckBox*>("mapEntitiesRawFields");
    QVERIFY(raw != nullptr);
    raw->setChecked(true);
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("owner"));
    raw->setChecked(false);
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("Human slot（玩家 1）"));
    QVERIFY(module.EditCell(MapDataKind::Units, 0, "owner", "2").Succeeded());
    QVERIFY(module.EditCell(MapDataKind::Units, 0, "x", "512.5").Succeeded());
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].owner, 2);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], 512.5f);
    QCOMPARE(module.UndoStack()->count(), 2);
    module.UndoStack()->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], 100.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].owner, 2);
    module.UndoStack()->undo();
    QVERIFY(!project.mapEdits_->IsDirty());
    module.UndoStack()->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].owner, 2);
    QVERIFY(project.mapEdits_->IsDirty());
    const auto count = module.UndoStack()->count();
    QVERIFY(!module.EditCell(MapDataKind::Units, 0, "x", "not a number").Succeeded());
    QVERIFY(!module.EditCell(MapDataKind::Units, 4, "owner", "1").Succeeded());
    QCOMPARE(module.UndoStack()->count(), count);
    QCOMPARE(project.mapDocument_->Units()->units[0].owner, 0); // 源文档仍只读。
}

void MapEntitiesModuleTests::DoodadRegionCameraFieldsRoundTripThroughCommands() {
    auto emptyDoodads = MapData::ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    QVERIFY(emptyDoodads.Succeeded());
    MapData::DoodadPlacement doodad{};
    doodad.rawcode = "LTlt";
    doodad.scale = {1.0f, 1.0f, 1.0f};
    doodad.lifePercent = 100;
    doodad.creationId = 18;
    emptyDoodads.value->doodads.push_back(doodad);
    const auto doodadBytes = emptyDoodads.value->Serialize();
    QVERIFY(doodadBytes.Succeeded());
    MapData::RegionRecord region{};
    region.name = "OriginalRegion";
    region.right = 512.0f;
    region.top = 256.0f;
    region.creationNumber = 4;
    MapData::RegionsDocument regions{5, {region}, {}};
    const auto regionBytes = regions.Serialize();
    QVERIFY(regionBytes.Succeeded());
    MapData::CameraRecord camera{};
    camera.name = "OriginalCamera";
    camera.fieldOfView = 70.0f;
    MapData::CamerasDocument cameras{0, {camera}, {}};
    const auto cameraBytes = cameras.Serialize();
    QVERIFY(cameraBytes.Succeeded());
    std::vector<MapArchiveMember> members{
        {"war3map.doo", *doodadBytes.value}, {"war3map.w3r", *regionBytes.value},
        {"war3map.w3c", *cameraBytes.value}};
    // OriginalCamera 的前 12 字节也能作为三个有限 float；无来源时必须拒绝歧义。
    QVERIFY(!MapDocument::FromMembers(members).Cameras());
    members.push_back({"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes});
    QtProject::EditorProject project;
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::MapEntitiesModule module(project);
    auto* page = module.Widget();
    QVERIFY(page != nullptr);
    QVERIFY(module.EditCell(MapDataKind::Doodads, 0, "life", "60").Succeeded());
    QVERIFY(module.EditCell(MapDataKind::Regions, 0, "name", "NewRegion").Succeeded());
    QVERIFY(module.EditCell(MapDataKind::Cameras, 0, "fov", "80.5").Succeeded());
    const auto changed = project.mapEdits_->Snapshot();
    QCOMPARE(changed.Doodads()->doodads[0].lifePercent, std::uint8_t{60});
    QCOMPARE(changed.Regions()->entries[0].name, std::string("NewRegion"));
    QCOMPARE(changed.Cameras()->entries[0].fieldOfView, 80.5f);
    QCOMPARE(project.mapEdits_->Replacements().size(), std::size_t{3});
    QVERIFY(!module.EditCell(MapDataKind::Doodads, 0, "life", "101").Succeeded());
    QVERIFY(!module.EditCell(MapDataKind::Cameras, 0, "fov", "nan").Succeeded());
    QVERIFY(!module.EditCell(MapDataKind::Cameras, 0, "targetX", "1000001").Succeeded());
    QVERIFY(!module.EditCell(MapDataKind::Regions, 0, "left", "1000001").Succeeded());
    QCOMPARE(module.UndoStack()->count(), 3);
    module.UndoStack()->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Cameras()->entries[0].fieldOfView, 70.0f);
    module.UndoStack()->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries[0].name, std::string("OriginalRegion"));
    module.UndoStack()->undo();
    QVERIFY(!project.mapEdits_->IsDirty());
    module.UndoStack()->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Doodads()->doodads[0].lifePercent, std::uint8_t{60});
}

void MapEntitiesModuleTests::AuxiliaryCrudUsesSharedUndoAndReferences() {
    QtProject::EditorProject project;
    const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
    Modules::MapEntitiesModule module(project);
    auto* page = module.Widget();
    auto* tabs = page->findChild<QTabWidget*>();
    auto* add = page->findChild<QPushButton*>("mapEntitiesAddAuxiliary");
    auto* remove = page->findChild<QPushButton*>("mapEntitiesDeleteAuxiliary");
    QVERIFY(tabs && add && remove);
    tabs->setCurrentIndex(2); QVERIFY(!add->isHidden() && !remove->isHidden());
    MapData::RegionRecord region{}; region.name = "Area"; region.creationNumber = 1; region.right = region.top = 128;
    QVERIFY(module.CreateAuxiliary(region).Succeeded());
    QCOMPARE(module.UndoStack()->count(), 1);
    QVERIFY(module.EditCell(MapDataKind::Regions, 0, "weather", "Rain").Succeeded());
    QVERIFY(module.EditCell(MapDataKind::Regions, 0, "color", "112233").Succeeded());
    QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries[0].colorBgra[3], std::uint8_t{0});
    QVERIFY(module.DeleteAuxiliary(MapDataKind::Regions, 0).Succeeded());
    QVERIFY(project.mapEdits_->Snapshot().Regions()->entries.empty());
    module.UndoStack()->undo(); QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries.size(), std::size_t{1});
    const auto count = module.UndoStack()->count();
    Trigger::ArgumentNode arg; arg.type = 1; arg.value.text = "gg_rct_Area";
    project.triggerDocument_->argumentNodes.push_back(arg);
    QVERIFY(!module.DeleteAuxiliary(MapDataKind::Regions, 0).Succeeded());
    QCOMPARE(module.UndoStack()->count(), count);
    QVERIFY(!module.EditCell(MapDataKind::Regions, 0, "name", "Renamed").Succeeded());
    QVERIFY(module.EditCell(MapDataKind::Regions, 0, "left", "-64").Succeeded());
    tabs->setCurrentIndex(0); QVERIFY(add->isHidden() && remove->isHidden());
}

void MapEntitiesModuleTests::AuxiliaryDialogsKeepCapturedProjectBinding() {
    // 真正进入 QInputDialog 的嵌套事件循环；两个工程使用相同成员字节，
    // 新建两例的 W3R/W3C 都缺席，只有公共工程/会话绑定能区分来源。
    for (const int category : {2, 3}) for (const bool creating : {false, true}) for (const bool switching : {false, true}) {
        std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
        if (!creating) {
            MapData::RegionRecord region{}; region.name = "SharedRegion"; region.creationNumber = 7;
            region.right = region.top = 128;
            MapData::CameraRecord camera{}; camera.name = "SharedCamera"; camera.fieldOfView = 70;
            camera.targetDistance = 1650; camera.farClippingPlane = 5000; camera.nearClippingPlane = 100;
            const auto regions = MapData::RegionsDocument{5, {region}, {}}.Serialize();
            const auto cameras = MapData::CamerasDocument{0, {camera}, {}}.Serialize();
            QVERIFY(regions.Succeeded() && cameras.Succeeded());
            members.push_back({"war3map.w3r", *regions.value}); members.push_back({"war3map.w3c", *cameras.value});
        }
        QtProject::EditorProject project;
        project.projectRoot_ = "original-dialog-project";
        project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
        project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
        project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
        const auto originalSession = project.mapEdits_->SessionIdentity();
        Modules::MapEntitiesModule module(project); auto* page = module.Widget();
        auto* tabs = page->findChild<QTabWidget*>();
        auto* table = page->findChild<QTableWidget*>(QStringLiteral("mapEntitiesTable%1").arg(category));
        auto* add = page->findChild<QPushButton*>("mapEntitiesAddAuxiliary");
        QVERIFY(tabs && table && add); tabs->setCurrentIndex(category);
        bool visited = false;
        QTimer::singleShot(0, page, [&] {
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            visited = dialog != nullptr;
            if (!dialog) return;
            if (switching) {
                // 与真实 LoadMapDocument 相同：清撤销栈并创建新会话，成员字节故意不变。
                project.mapDataUndoStack_->clear(); project.projectRoot_ = "other-dialog-project";
                project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
                project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
                emit project.MapDocumentChanged();
            }
            dialog->setTextValue(creating ? QStringLiteral("NewFromDialog") : QStringLiteral("-64"));
            dialog->accept();
        });
        if (creating) add->click();
        else QVERIFY(QMetaObject::invokeMethod(table, "cellDoubleClicked", Qt::DirectConnection,
            Q_ARG(int, 0), Q_ARG(int, 1)));
        QVERIFY(visited);
        if (switching) {
            QVERIFY(project.mapEdits_->SessionIdentity() != originalSession);
            QVERIFY(!project.mapEdits_->IsDirty()); QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
            QCOMPARE(module.UndoStack()->count(), 0);
        } else {
            QCOMPARE(project.mapEdits_->SessionIdentity(), originalSession);
            QVERIFY(project.mapEdits_->IsDirty()); QCOMPARE(module.UndoStack()->count(), 1);
            const auto changed = project.mapEdits_->Snapshot();
            if (category == 2) {
                QVERIFY(changed.Regions());
                if (creating) QCOMPARE(changed.Regions()->entries[0].name, std::string("NewFromDialog"));
                else QCOMPARE(changed.Regions()->entries[0].left, -64.0f);
            } else {
                QVERIFY(changed.Cameras());
                if (creating) QCOMPARE(changed.Cameras()->entries[0].name, std::string("NewFromDialog"));
                else QCOMPARE(changed.Cameras()->entries[0].targetX, -64.0f);
            }
            module.UndoStack()->undo(); QVERIFY(!project.mapEdits_->IsDirty());
            module.UndoStack()->redo(); QVERIFY(project.mapEdits_->IsDirty());
        }
    }
}

void MapEntitiesModuleTests::TypePresentationDefaultsAndDegreesKeepRawStorage() {
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    QVERIFY(units.Succeeded());
    MapData::UnitPlacement standard;
    standard.rawcode = "hfoo";
    standard.scale = {1, 1, 1};
    standard.rotation = std::numbers::pi_v<float> / 2.0f;
    standard.hitPoints = -1;
    standard.mana = -1;
    standard.creationId = 1;
    units.value->units.push_back(standard);
    MapData::UnitPlacement custom = standard;
    custom.rawcode = "H001";
    custom.creationId = 2;
    units.value->units.push_back(custom);
    const auto unitBytes = units.value->Serialize();
    QVERIFY(unitBytes.Succeeded());

    QtProject::EditorProject project;
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *unitBytes.value}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const std::string slk = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\nC;X3;K\"Art\"\n"
        "C;X1;Y2;K\"hfoo\"\nC;X2;K\"步兵\"\nC;X3;K\"Icons\\Footman.blp\"\nE\n";
    ObjectData::CatalogLoadOptions options;
    options.sources = {{ObjectData::ObjectType::Unit, {"unit.slk"}, {}, {"name"}, {"art"}}};
    options.stringFiles.clear();
    auto catalog = ObjectData::StandardObjectCatalog::Load([&slk](const std::string& path) {
        if (path != "unit.slk") return Result<std::vector<std::byte>>::Failure("Test", "missing", path);
        const auto* begin = reinterpret_cast<const std::byte*>(slk.data());
        return Result<std::vector<std::byte>>::Success(std::vector<std::byte>(begin, begin + slk.size()));
    }, options);
    QVERIFY(catalog.Succeeded() && catalog.value);
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*catalog.value));
    const std::string lni = "[H001]\n_parent=\"hfoo\"\nunam=\"自定义步兵\"\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(
        std::as_bytes(std::span(lni.data(), lni.size())));
    QVERIFY(parsed.Succeeded());
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));

    Modules::MapEntitiesModule module(project);
    auto* page = module.Widget();
    auto* table = page->findChild<QTableWidget*>("mapEntitiesTable0");
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 2);
    QVERIFY(table->item(0, 0)->text().contains(QStringLiteral("步兵")));
    QCOMPARE(table->item(0, 0)->data(Qt::UserRole).toString(), QStringLiteral("hfoo"));
    QCOMPARE(table->item(0, 0)->data(Qt::UserRole + 1).toString(), QStringLiteral("Icons\\Footman.blp"));
    QVERIFY(table->item(1, 0)->text().contains(QStringLiteral("自定义步兵")));
    QCOMPARE(table->item(0, 6)->text(), QStringLiteral("默认"));
    QCOMPARE(table->item(0, 7)->text(), QStringLiteral("默认"));
    QVERIFY(std::abs(table->item(0, 5)->text().toFloat() - 90.0f) < 0.01f);
    QVERIFY(table->columnWidth(8) < 180); // 金币列不吞掉右半个表格。
    QVERIFY(module.Reveal({"entities", QStringLiteral("unit"), QStringLiteral("H001"), {}}));
    auto* raw = page->findChild<QCheckBox*>("mapEntitiesRawFields");
    raw->setChecked(true);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("hfoo"));
    raw->setChecked(false);
    QVERIFY(module.EditCell(MapDataKind::Units, 0, "rotation", "180").Succeeded());
    QVERIFY(std::abs(project.mapEdits_->Snapshot().Units()->units[0].rotation - std::numbers::pi_v<float>) < 0.0001f);
    QVERIFY(!module.EditCell(MapDataKind::Units, 0, "rotation", "360").Succeeded());
    QVERIFY(module.EditCell(MapDataKind::Units, 0, "health", "60").Succeeded());
    QVERIFY(module.EditCell(MapDataKind::Units, 0, "health", "默认").Succeeded());
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].hitPoints, -1);
    QCOMPARE(table->item(0, 6)->text(), QStringLiteral("默认"));
}

void MapEntitiesModuleTests::CurrentStringTableNamesAndStartLocationsPreserveStoredValues() {
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    QVERIFY(units.Succeeded() && units.value);
    MapData::UnitPlacement unit;
    unit.rawcode = "H001";
    unit.scale = {1, 1, 1};
    unit.rotation = 3.1415925f;
    unit.owner = 0;
    units.value->units.push_back(unit);
    unit.rawcode = "sloc";
    unit.owner = 12;
    unit.creationId = 1;
    units.value->units.push_back(unit);
    unit.rawcode = "H002";
    unit.creationId = 2;
    units.value->units.push_back(unit);
    const auto bytes = units.value->Serialize();
    QVERIFY(bytes.Succeeded() && bytes.value);
    auto settings = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    QVERIFY(settings.Succeeded() && settings.value);
    settings.value->players[0].name = "TRIGSTR_1550";
    const auto settingsBytes = settings.value->Serialize();
    QVERIFY(settingsBytes.Succeeded() && settingsBytes.value);
    const std::string wts = "STRING 1549\n{\n地图自定义步兵\n}\nSTRING 1550\n{\n联盟\n}\n";
    const auto wtsSpan = std::as_bytes(std::span(wts.data(), wts.size()));
    const std::vector<MapArchiveMember> members{
        {"war3mapUnits.doo", *bytes.value}, {"war3map.w3i", *settingsBytes.value},
        {"war3map.wts", {wtsSpan.begin(), wtsSpan.end()}}};
    QtProject::EditorProject project;
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const std::string lni = "[H001]\n_parent=\"hfoo\"\nunam=\"TRIGSTR_1549\"\n"
        "[H002]\n_parent=\"hfoo\"\nunam=\"TRIGSTR_9999\"\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(
        std::as_bytes(std::span(lni.data(), lni.size())));
    QVERIFY(parsed.Succeeded());
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    QVERIFY(project.objectDocuments_[0]);
    const auto originalObjects = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Serialize(*project.objectDocuments_[0]);
    QVERIFY(originalObjects.Succeeded() && originalObjects.value);
    Modules::MapEntitiesModule module(project);
    auto* page = module.Widget();
    auto* table = page->findChild<QTableWidget*>("mapEntitiesTable0");
    QVERIFY(table);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("地图自定义步兵  ·  H001"));
    QVERIFY(table->item(0, 0)->toolTip().contains(QStringLiteral("TRIGSTR_1549")));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("联盟（玩家 1）"));
    QCOMPARE(table->item(0, 1)->data(Qt::UserRole).toString(), QStringLiteral("0"));
    QCOMPARE(table->item(0, 5)->text(), QStringLiteral("180"));
    QCOMPARE(table->item(1, 0)->text(), QStringLiteral("起始位置  ·  sloc"));
    QCOMPARE(table->item(1, 1)->text(), QStringLiteral("中立敌对"));
    QVERIFY(!project.mapEdits_->IsDirty());
    const auto originalRotation = std::bit_cast<std::uint32_t>(unit.rotation);
    QCOMPARE(std::bit_cast<std::uint32_t>(project.mapEdits_->Snapshot().Units()->units[0].rotation), originalRotation);

    auto strings = *project.mapEdits_->Snapshot().Strings();
    strings.entries[0].value = "会话中的名称";
    QVERIFY(project.mapEdits_->EditStrings(strings).Succeeded());
    module.Refresh();
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("会话中的名称  ·  H001"));
    // 其他实例编辑与撤销刷新都必须继续读取当前覆盖层中的 WTS。
    QVERIFY(module.EditCell(MapDataKind::Units, 0, "owner", "15").Succeeded());
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("中立被动"));
    module.UndoStack()->undo();
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("会话中的名称  ·  H001"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("联盟（玩家 1）"));
    QCOMPARE(std::bit_cast<std::uint32_t>(project.mapEdits_->Snapshot().Units()->units[0].rotation), originalRotation);
    // 对象身份的原值是 H002；缺 WTS 名称不能冒充已解析名称。原名称引用和原因保留在提示中。
    // 基线已有缺失引用，不删除受保护的 WTS 项来伪造此场景。
    const auto missing = ResolveProjectName(project, "H002", "unitcode");
    QCOMPARE(missing.state, Services::NameResolutionState::MissingString);
    QCOMPARE(missing.rawValue, std::string("H002"));
    QCOMPARE(missing.displayName, missing.rawValue);
    QVERIFY(missing.reason.find("TRIGSTR_9999") != std::string::npos);
    QVERIFY(missing.reason.find("缺少条目") != std::string::npos);
    QCOMPARE(table->item(2, 0)->text(), QStringLiteral("H002"));
    QCOMPARE(table->item(2, 0)->data(Qt::UserRole).toString(), QStringLiteral("H002"));
    QVERIFY(table->item(2, 0)->toolTip().contains(QString::fromUtf8(missing.reason)));
    QVERIFY(table->item(2, 0)->toolTip().contains(QStringLiteral("TRIGSTR_9999")));
    const auto& original = *project.mapDocument_->Strings();
    QVERIFY(project.mapEdits_->EditStrings(original).Succeeded());
    module.Refresh();
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("地图自定义步兵  ·  H001"));
    auto* raw = page->findChild<QCheckBox*>("mapEntitiesRawFields");
    QVERIFY(raw);
    raw->setChecked(true);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("H001"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("0"));
    QVERIFY(!project.mapEdits_->IsDirty());
    const auto retainedObjects = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Serialize(*project.objectDocuments_[0]);
    QVERIFY(retainedObjects.Succeeded() && retainedObjects.value);
    QVERIFY(*retainedObjects.value == *originalObjects.value);
    QCOMPARE(project.mapDocument_->ResolveString("TRIGSTR_1549").value, std::optional(std::string("地图自定义步兵")));
    QVERIFY(!project.mapDocument_->ResolveString("TRIGSTR_9999").Succeeded());
}

void MapEntitiesModuleTests::MovingStartLocationUndoesBothMembersToTheirOriginalBytes() {
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    QVERIFY(units.Succeeded() && units.value);
    MapData::UnitPlacement start;
    start.rawcode = "sloc";
    start.scale = {1, 1, 1};
    start.position = {100, 200, 0};
    units.value->units.push_back(start);
    const auto bytes = units.value->Serialize();
    QVERIFY(bytes.Succeeded() && bytes.value);
    const std::vector<MapArchiveMember> members{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"war3mapUnits.doo", *bytes.value}};
    QtProject::EditorProject project;
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::MapEntitiesModule module(project);
    (void)module.Widget();
    QVERIFY(module.EditCell(MapDataKind::Units, 0, "x", "750").Succeeded());
    QCOMPARE(project.mapEdits_->Replacements().size(), std::size_t{2});
    QCOMPARE(project.mapEdits_->Snapshot().Settings()->players[0].startX, 750.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Settings()->players[0].startY, 200.0f);
    QCOMPARE(module.UndoStack()->count(), 1);
    module.UndoStack()->undo();
    QVERIFY(!project.mapEdits_->IsDirty());
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], 100.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Settings()->players[0].startX,
        project.mapDocument_->Settings()->players[0].startX);
    module.UndoStack()->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], 750.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Settings()->players[0].startX, 750.0f);
    const auto count = module.UndoStack()->count();
    QVERIFY(!module.EditCell(MapDataKind::Units, 0, "owner", "1").Succeeded());
    QCOMPARE(module.UndoStack()->count(), count);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].owner, 0);
}

void MapEntitiesModuleTests::NavigationBookmarkPreservesTypedIdentityAndScroll() {
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    QVERIFY(units.Succeeded() && units.value);
    for (int row = 0; row < 80; ++row) {
        MapData::UnitPlacement unit; unit.rawcode = "hfoo"; unit.scale = {1, 1, 1}; unit.creationId = 100 + row;
        units.value->units.push_back(unit);
    }
    MapData::RegionRecord first{}; first.name = "Shared"; first.creationNumber = 17; first.right = first.top = 128;
    auto second = first; second.creationNumber = 18;
    const auto unitBytes = units.value->Serialize(), regions = MapData::RegionsDocument{5, {first, second}, {}}.Serialize();
    QVERIFY(unitBytes.Succeeded() && regions.Succeeded());
    QtProject::EditorProject project;
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3mapUnits.doo", *unitBytes.value}, {"war3map.w3r", *regions.value}}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::MapEntitiesModule module(project);
    auto* page = module.Widget(); page->resize(640, 320); page->show();
    auto* table = page->findChild<QTableWidget*>("mapEntitiesTable0");
    auto* raw = page->findChild<QCheckBox*>("mapEntitiesRawFields");
    auto* tabs = page->findChild<QTabWidget*>();
    QVERIFY(table && raw && tabs);
    table->setCurrentCell(40, 2); table->scrollToItem(table->item(40, 2));
    QTRY_VERIFY(table->verticalScrollBar()->maximum() > 0);
    table->verticalScrollBar()->setValue(table->verticalScrollBar()->maximum() / 2);
    const auto saved = module.CaptureNavigationBookmark(); QVERIFY(saved);
    const auto revision = project.mapEdits_->Revision();
    QVERIFY(module.Reveal({"entities", "unit", "hfoo", {{"sourceIndex", QStringLiteral("2")}}}));
    raw->setChecked(true);
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Restored);
    QCOMPARE(table->currentRow(), 40); QCOMPARE(table->currentColumn(), 2);
    QCOMPARE(table->verticalScrollBar()->value(), saved->value("tables").toArray()[0].toObject().value("vertical").toInt());
    QVERIFY(!raw->isChecked()); QCOMPARE(project.mapEdits_->Revision(), revision); QCOMPARE(module.UndoStack()->count(), 0);
    auto* regionsTable = page->findChild<QTableWidget*>("mapEntitiesTable2"); QVERIFY(regionsTable);
    tabs->setCurrentIndex(2); regionsTable->setCurrentCell(1, 0);
    const auto duplicateName = module.CaptureNavigationBookmark(); QVERIFY(duplicateName);
    regionsTable->setCurrentCell(0, 0);
    QCOMPARE(module.RestoreNavigationBookmark(*duplicateName), Shell::NavigationRestoreResult::Restored);
    QCOMPARE(regionsTable->currentRow(), 1);
    auto invalid = *duplicateName;
    auto entries = invalid.value("tables").toArray(); auto entry = entries[2].toObject();
    auto identity = entry.value("selection").toObject(); identity.insert("creation", 17);
    entry.insert("selection", identity); entries[2] = entry; invalid.insert("tables", entries);
    const auto unchanged = module.CaptureNavigationBookmark(); QVERIFY(unchanged);
    QCOMPARE(module.RestoreNavigationBookmark(invalid), Shell::NavigationRestoreResult::Invalid);
    QVERIFY(module.CaptureNavigationBookmark() == unchanged);
    auto changed = *project.mapEdits_->Snapshot().Regions(); changed.entries[1].creationNumber = 99;
    QVERIFY(project.mapEdits_->EditRegions(changed).Succeeded()); module.Refresh();
    const auto staleView = module.CaptureNavigationBookmark();
    QCOMPARE(module.RestoreNavigationBookmark(*duplicateName), Shell::NavigationRestoreResult::Invalid);
    QVERIFY(module.CaptureNavigationBookmark() == staleView); // 同名兄弟仍在，但不能代替旧完整身份。
}

void MapEntitiesModuleTests::NavigationBookmarkBlocksModalDraft() {
    MapData::RegionRecord region{}; region.name = "DraftRegion"; region.right = region.top = 128;
    const auto bytes = MapData::RegionsDocument{5, {region}, {}}.Serialize(); QVERIFY(bytes.Succeeded());
    QtProject::EditorProject project;
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3map.w3r", *bytes.value}}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::MapEntitiesModule module(project); auto* page = module.Widget();
    auto* table = page->findChild<QTableWidget*>("mapEntitiesTable2"); QVERIFY(table);
    table->setCurrentCell(0, 1); const auto saved = module.CaptureNavigationBookmark(); QVERIFY(saved);
    bool visited = false, blocked = false;
    QTimer::singleShot(0, page, [&] {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        visited = dialog != nullptr;
        blocked = !module.PrepareNavigationLeave() && module.RestoreNavigationBookmark(*saved) == Shell::NavigationRestoreResult::Blocked;
        if (dialog) dialog->reject();
    });
    // 原有真实弹窗入口；不设置文本，取消后不写入地图，也不产生 undo。
    QVERIFY(QMetaObject::invokeMethod(table, "cellDoubleClicked", Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 1)));
    QVERIFY(visited && blocked); QVERIFY(module.PrepareNavigationLeave()); QVERIFY(!project.mapEdits_->IsDirty());
    QCOMPARE(module.UndoStack()->count(), 0);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    MapEntitiesModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "MapEntitiesModuleTests.moc"
