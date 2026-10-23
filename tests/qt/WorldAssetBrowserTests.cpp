#include "WorldAssetBrowser.h"
#include "WorldPlacementModels.h"
#include "WorldSelectionPresentation.h"
#include "EditorProject.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Workspace.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QComboBox>
#include <QToolButton>
#include <QScrollBar>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

using namespace War3Map;
using namespace War3Map::Modules;

namespace {
bool InstallObjects(QtProject::EditorProject& project) {
    const std::string slk = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\n"
        "C;X1;Y2;K\"hfoo\"\nC;X2;K\"Original\"\nC;X1;Y3;K\"hpea\"\nC;X2;K\"Worker\"\nE\n";
    ObjectData::CatalogLoadOptions options; options.stringFiles.clear();
    options.sources = {{ObjectData::ObjectType::Unit, {"unit.slk"}, {}, {"name"}, {"art"}}};
    auto catalog = ObjectData::StandardObjectCatalog::Load([&](const std::string&) {
        const auto bytes = std::as_bytes(std::span(slk.data(), slk.size()));
        return Result<std::vector<std::byte>>::Success({bytes.begin(), bytes.end()});
    }, options);
    if (!catalog.Succeeded()) return false;
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*catalog.value));
    const std::string lni = "[hfoo]\nunam=\"Map name\"\nuico=\"Synthetic/Map.blp\"\n[U001]\n_parent=\"hfoo\"\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(), lni.size())));
    if (!parsed.Succeeded()) return false;
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    return project.objectDocuments_[0] != nullptr;
}
}
class WorldAssetBrowserTests final : public QObject {
    Q_OBJECT
private slots:
    void PagesKeepMapOverridesAndParentPresentation();
    void CapturedAnimationProperties();
    void CapturedFlightHeightAndCircleFieldsKeepRawPlacements();
    void SelectionCircleBatchCachesNativeTextureAndKeepsDisplayHeightSeparate();
    void SelectionOutlineUsesRendererEdges();
    void EffectiveClassificationsFollowStandardParentAndMapOverrides();
    void DragIdentityIsBoundedAndDecimal();
    void BrowserUsesPlacementSignalAndRejectsStaleContext();
    void CompactGroupedCatalogKeepsRowIdentity();
    void ObservedPlacementsRebindWithoutReloadButCatalogChangesReload();
    void MissingBackendNeverProducesModelImage();
    void RealGameThumbnailEvidence();
};
void WorldAssetBrowserTests::PagesKeepMapOverridesAndParentPresentation() {
    QtProject::EditorProject project; QVERIFY(InstallObjects(project));
    QtApp::ObjectViewQuery query; query.kind = "unit"; query.limit = 1; query.maximumExamined = 1;
    QStringList ids; int calls = 0, preparationCalls = 0;
    do {
        QVERIFY(++calls <= 16);
        const auto page = QtApp::QueryObjectViewEntries(project, query);
        QVERIFY(page.examined <= 1); QVERIFY(page.rows.size() <= 1); QVERIFY(page.issue.isEmpty());
        if (page.preparing) { ++preparationCalls; QVERIFY(page.hasMore); QCOMPARE(page.nextOffset, query.offset); continue; }
        for (const auto& row : page.rows) {
            ids.push_back(row.entry.id);
            if (row.mapObject) { QCOMPARE(row.entry.name, QString("Map name")); QCOMPARE(row.entry.iconPath, QString("Synthetic/Map.blp")); }
        }
        if (!page.hasMore) break;
        QVERIFY(page.nextOffset > query.offset); query.offset = page.nextOffset;
    } while (ids.size() < 5);
    QCOMPARE(ids, QStringList({"hfoo", "U001", "hpea"}));
    QVERIFY(preparationCalls >= 4);
    query.offset = 1;
    const auto warmPage = QtApp::QueryObjectViewEntries(project, query);
    QVERIFY(!warmPage.preparing); QCOMPARE(warmPage.examined, std::size_t{1});
    QCOMPARE(warmPage.rows.front().entry.id, QString("U001"));
    QVERIFY(project.editRevision_.Observe({{"objects/unit", "changed"}}, "fixture accepted revision").Succeeded());
    const auto revisedPage = QtApp::QueryObjectViewEntries(project, query);
    QVERIFY(revisedPage.preparing); QCOMPARE(revisedPage.examined, std::size_t{1}); QVERIFY(revisedPage.rows.isEmpty());
    query.maximumIndexBytes = 16;
    const auto budgetPage = QtApp::QueryObjectViewEntries(project, query);
    QVERIFY(budgetPage.partial); QVERIFY(!budgetPage.issue.isEmpty()); QVERIFY(budgetPage.rows.isEmpty());
    query.maximumIndexBytes = 64 * 1024 * 1024;
    auto native = ObjectData::NativeObjectDocument::CreateEmpty(ObjectData::ObjectType::Unit); QVERIFY(native.Succeeded());
    QVERIFY(native.value->AddOriginalObject("hfoo").Succeeded());
    QVERIFY(native.value->AddCustomObject("U002", "hfoo").Succeeded());
    QVERIFY(native.value->AddModification({ObjectData::ObjectType::Unit, "hfoo"}, {"unam"},
        ObjectData::NativeValueType::String, std::string("TRIGSTR_1")).Succeeded());
    const std::string wts = "STRING 1\n{\nNative name\n}\n";
    const auto wtsBytes = std::as_bytes(std::span(wts.data(), wts.size()));
    const std::vector<MapArchiveMember> members{{"war3map.wts", {wtsBytes.begin(), wtsBytes.end()}}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    project.nativeObjectDocuments_[0] = std::make_unique<ObjectData::NativeObjectDocument>(std::move(*native.value));
    query.offset = 0; query.limit = 4; query.maximumExamined = 4; query.source = QtApp::ObjectViewSource::Map;
    const auto lniPage = QtApp::QueryObjectViewEntries(project, query);
    QCOMPARE(lniPage.rows.size(), 2); QCOMPARE(lniPage.rows[1].entry.id, QString("U001"));
    QCOMPARE(lniPage.rows[0].entry.name, QString("Map name")); // 当前 LNI 不被旧 native 基线覆盖。
    project.objectDocuments_[0].reset();
    const auto nativePage = QtApp::QueryObjectViewEntries(project, query); QVERIFY(!nativePage.partial);
    QCOMPARE(nativePage.rows.size(), 2); QVERIFY(nativePage.rows[1].mapObject);
    QCOMPARE(nativePage.rows[1].entry.id, QString("U002")); QCOMPARE(nativePage.rows[1].entry.name, QString("Native name"));
    auto editedStrings = *project.mapDocument_->Strings(); editedStrings.entries.front().value = "Current name";
    QVERIFY(project.mapEdits_->EditStrings(editedStrings).Succeeded());
    const auto stringsPage = QtApp::QueryObjectViewEntries(project, query);
    QCOMPARE(stringsPage.rows.front().entry.name, QString("Current name")); // WTS 修订独立绑定，不沿用旧名称。
    QVERIFY(project.mapEdits_->Undo());
    const auto undoPage = QtApp::QueryObjectViewEntries(project, query);
    QCOMPARE(undoPage.rows.front().entry.name, QString("Native name"));
    QCOMPARE(QtApp::BuildObjectViewEntries(project).size(), 2); // 旧完整目录入口行为保留。
    query.offset = 0; query.limit = 129; QVERIFY(QtApp::QueryObjectViewEntries(project, query).partial);
}
void WorldAssetBrowserTests::CapturedAnimationProperties() {
    QtProject::EditorProject project; QVERIFY(InstallObjects(project));
    const std::string lni="[hfoo]\nfile=\"Synthetic/Tower.mdl\"\nuani=\"upgrade,first\"\n[U001]\n_parent=\"hfoo\"\n";
    auto parsed=ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(),lni.size())));
    QVERIFY(parsed.Succeeded()); project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    MapData::PlacementTransform placement; placement.rawcode="U001";
    auto preview=CapturePlacementPreview(project,ObjectData::ObjectType::Unit,placement,0);
    QVERIFY(preview); QCOMPARE(preview->animationProperties,std::string("upgrade,first"));
    auto native=ObjectData::NativeObjectDocument::CreateEmpty(ObjectData::ObjectType::Unit); QVERIFY(native.Succeeded());
    QVERIFY(native.value->AddOriginalObject("hfoo").Succeeded()); QVERIFY(native.value->AddCustomObject("U001","hfoo").Succeeded());
    QVERIFY(native.value->AddModification({ObjectData::ObjectType::Unit,"hfoo"},{"umdl"},ObjectData::NativeValueType::String,std::string("Synthetic/Tower.mdl")).Succeeded());
    QVERIFY(native.value->AddModification({ObjectData::ObjectType::Unit,"hfoo"},{"uani"},ObjectData::NativeValueType::String,std::string("upgrade,second")).Succeeded());
    project.nativeObjectDocuments_[0]=std::make_unique<ObjectData::NativeObjectDocument>(std::move(*native.value));
    QCOMPARE(CapturePlacementPreview(project,ObjectData::ObjectType::Unit,placement,0)->animationProperties,std::string("upgrade,first"));
    project.objectDocuments_[0].reset();
    preview=CapturePlacementPreview(project,ObjectData::ObjectType::Unit,placement,0);
    QVERIFY(preview); QCOMPARE(preview->animationProperties,std::string("upgrade,second"));
    MapData::UnitPlacements units;
    units.units.push_back(std::get<MapData::UnitPlacement>(MakeNewPlacement(ObjectData::ObjectType::Unit,"U001",0,{0,0,0})));
    const auto encoded=units.Serialize(); QVERIFY(encoded.Succeeded());
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo",*encoded.value}};
    const auto captured=CaptureWorldPlacements(project,MapDocument::FromMembers(members));
    QCOMPARE(captured.requests.size(),std::size_t{1});
    QCOMPARE(captured.requests.front().animationProperties,preview->animationProperties);
}
void WorldAssetBrowserTests::CapturedFlightHeightAndCircleFieldsKeepRawPlacements() {
    QtProject::EditorProject project; QVERIFY(InstallObjects(project));
    const std::string lni = "[hfoo]\numdl=\"Synthetic/Eye.mdl\"\numvt=\"fly\"\numvh=875\nussc=3\nuslz=600\nusew=1\nuscb=1\n[U001]\n_parent=\"hfoo\"\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(), lni.size())));
    QVERIFY(parsed.Succeeded()); project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    MapData::UnitPlacements units;
    units.units.push_back(std::get<MapData::UnitPlacement>(MakeNewPlacement(ObjectData::ObjectType::Unit, "U001", 0, {10,20,25})));
    const auto encoded = units.Serialize(); QVERIFY(encoded.Succeeded());
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *encoded.value}};
    const auto document = MapDocument::FromMembers(members);
    const auto captured = CaptureWorldPlacements(project, document);
    QCOMPARE(captured.requests.size(), std::size_t{1}); QCOMPARE(captured.flightHeightPreviewUnits, std::size_t{1});
    const auto preview = CapturePlacementPreview(project, ObjectData::ObjectType::Unit, units.units.front(), 0);
    QVERIFY(preview); QCOMPARE(preview->position, captured.requests.front().position); QCOMPARE(preview->position[2], 900.0f);
    QVERIFY(preview->selection); QCOMPARE(preview->selection->rawPlacementZ, 25.0f);
    QCOMPARE(preview->selection->circleScale, 3.0f); QCOMPARE(preview->selection->circleHeight, 600.0f);
    QVERIFY(preview->selection->circleOnWater); QVERIFY(preview->selection->scalesProjectiles);
    QCOMPARE(WorldPlacementDisplayPosition(*preview, {30,40,100})[2], 975.0f);
    QCOMPARE(document.Units()->units.front().position[2], 25.0f); QCOMPARE(*units.Serialize().value, *encoded.value);
    const std::string grounded = lni + "umvt=\"foot\"\n"; // 子对象覆盖父fly，不能从umvh单独推飞行。
    auto changed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(grounded.data(), grounded.size())));
    QVERIFY(changed.Succeeded()); project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(changed.document.release()));
    const auto foot = CapturePlacementPreview(project, ObjectData::ObjectType::Unit, units.units.front(), 0);
    QVERIFY(foot); QCOMPARE(foot->position[2], 25.0f); QVERIFY(!foot->selection->heightPreviewApproximate);
    auto native = ObjectData::NativeObjectDocument::CreateEmpty(ObjectData::ObjectType::Unit); QVERIFY(native.Succeeded());
    QVERIFY(native.value->AddOriginalObject("hfoo").Succeeded()); QVERIFY(native.value->AddCustomObject("U001", "hfoo").Succeeded());
    for (const auto& [field,value] : std::array<std::pair<const char*,const char*>,2>{{{"umdl","Synthetic/Eye.mdl"}, {"umvt","fly"}}})
        QVERIFY(native.value->AddModification({ObjectData::ObjectType::Unit,"hfoo"},{field},ObjectData::NativeValueType::String,std::string(value)).Succeeded());
    QVERIFY(native.value->AddModification({ObjectData::ObjectType::Unit,"hfoo"},{"umvh"},ObjectData::NativeValueType::Real,475.0f).Succeeded());
    project.objectDocuments_[0].reset(); project.nativeObjectDocuments_[0] = std::make_unique<ObjectData::NativeObjectDocument>(std::move(*native.value));
    const auto nativePreview = CapturePlacementPreview(project, ObjectData::ObjectType::Unit, units.units.front(), 0);
    QVERIFY(nativePreview); QCOMPARE(nativePreview->position[2], 500.0f); QCOMPARE(document.Units()->units.front().position[2],25.0f);
}
void WorldAssetBrowserTests::SelectionCircleBatchCachesNativeTextureAndKeepsDisplayHeightSeparate() {
    Image::DecodedImage image{2,2, std::vector<std::byte>(16, std::byte{255})}; image.rgba[3] = std::byte{0};
    const auto texture = Image::EncodeBlpPaletted(image); QVERIFY(texture.Succeeded());
    int textureReads = 0;
    const auto read = [&](std::string_view path, std::uint64_t) -> Result<Render::AnimationResource> {
        Render::AnimationResource source; source.sourceLayer = "fixture-original";
        const std::string profile = path == "UI\\MiscData.txt" ?
            "[SelectionCircle]\nNumSizes=3\nScaleFactor=72\nSize00=100\nSize01=300\nTexture00=Original/Circle\nTexture01=Original/Circle\nTexture02=Original/Circle\nColorFriend=255,0,255,0\n" : "[Misc]\nSelectionCircleBaseZ=16\n";
        if (path == "Original/Circle.blp") { ++textureReads; source.bytes = *texture.value; }
        else { const auto bytes = std::as_bytes(std::span(profile.data(), profile.size())); source.bytes.assign(bytes.begin(), bytes.end()); }
        const auto hash = Sha256(source.bytes); if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
        source.digest = *hash.value; return Result<Render::AnimationResource>::Success(std::move(source));
    };
    const auto assets = LoadWorldSelectionCircleAssets({1,7,"fixture-map"}, read); QVERIFY(assets.Succeeded());
    QCOMPARE(textureReads, 1); QCOMPARE(assets.value->sources.size(), std::size_t{3});
    QCOMPARE(assets.value->scene->geometry.textures.size(), std::size_t{1}); QCOMPARE(assets.value->scene->models.size(), std::size_t{3});
    std::array<Render::ModelInstanceRequest,2> requests;
    requests[0].position = {10,20,900}; requests[0].selection = Render::ModelSelectionDescription{3,600,true,true,true,25,875,true};
    requests[1].position = {30,40,25}; requests[1].selection = Render::ModelSelectionDescription{1,0,true};
    const auto frame = BuildWorldSelectionCircleFrame(*assets.value, requests, nullptr, 1); QVERIFY(frame.Succeeded());
    QCOMPARE(frame.value->requestIndices, std::vector<std::uint32_t>({1,0})); QCOMPARE(frame.value->frame->groups.size(), std::size_t{8});
    QCOMPARE(frame.value->frame->instances[1].instance.transform[11], 641.0f); // 圈rawZ+selZ+base，不能重复加飞行高度。
    QCOMPARE(frame.value->frame->instances[1].instance.transform[0], 216.0f); // uscb不改变圈尺寸。
    QCOMPARE(frame.value->waterSemanticsPending, std::size_t{1}); QVERIFY(frame.value->previewApproximate);
    requests[0].position[0] = 50;
    const auto moved = BuildWorldSelectionCircleFrame(*assets.value, requests, nullptr, 2); QVERIFY(moved.Succeeded());
    QCOMPARE(moved.value->frame->instances[1].instance.transform[3], 50.0f); QCOMPARE(textureReads, 1);
    const auto empty = BuildWorldSelectionCircleFrame(*assets.value, {}, nullptr, 3); QVERIFY(empty.Succeeded()); QVERIFY(empty.value->frame->instances.empty());
    requests[0].selection->circleScale = std::numeric_limits<float>::quiet_NaN();
    QVERIFY(!BuildWorldSelectionCircleFrame(*assets.value, requests, nullptr, 4).Succeeded());
}
void WorldAssetBrowserTests::SelectionOutlineUsesRendererEdges() {
    const Render::ViewProjectionFrame view{{1,2,3,4}, {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, 64,48};
    const auto edges = BuildWorldSelectionOutline(view, {64,48,1}, {4,6}, {60,42});
    QVERIFY(edges.Succeeded()); QCOMPARE(edges.value->size(), std::size_t{4});
    for (std::size_t i = 0; i < edges.value->size(); ++i) {
        const auto& edge = edges.value->at(i); QVERIFY(edge.color[1] > edge.color[0]);
        for (std::size_t axis = 0; axis < 3; ++axis)
            QCOMPARE(edge.to[axis], edges.value->at((i + 1) % 4).from[axis]);
    }
}
void WorldAssetBrowserTests::EffectiveClassificationsFollowStandardParentAndMapOverrides() {
    QtProject::EditorProject project;
    const std::string slk = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\nC;X3;K\"race\"\nC;X4;K\"campaign\"\n"
        "C;X1;Y2;K\"hfoo\"\nC;X2;K\"Footman\"\nC;X3;K\"human\"\nC;X4;K0\n"
        "C;X1;Y3;K\"Hpal\"\nC;X2;K\"Paladin\"\nC;X3;K\"human\"\nC;X4;K0\nE\n";
    ObjectData::CatalogLoadOptions options; options.stringFiles.clear();
    options.sources = {{ObjectData::ObjectType::Unit, {"unit.slk"}, {}, {"name"}, {"art"}}};
    auto catalog = ObjectData::StandardObjectCatalog::Load([&](const std::string&) {
        const auto bytes = std::as_bytes(std::span(slk.data(), slk.size()));
        return Result<std::vector<std::byte>>::Success({bytes.begin(), bytes.end()});
    }, options);
    QVERIFY(catalog.Succeeded());
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*catalog.value));
    const std::string lni = "[u001]\n_parent=\"Hpal\"\n[U002]\n_parent=\"hfoo\"\n"
        "[U003]\n_parent=\"U002\"\nurac=\"orc\"\nucam=1\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(), lni.size())));
    QVERIFY(parsed.Succeeded());
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    QVERIFY(project.objectDocuments_[0]);
    const auto hero = QtApp::ResolveObjectViewClassification(project, {ObjectData::ObjectType::Unit, "u001"});
    QVERIFY(hero.issue.isEmpty()); QCOMPARE(hero.hero, std::optional<bool>{true});
    QCOMPARE(hero.folders, QStringList({"人族", "对战", "英雄"}));
    const auto soldier = QtApp::ResolveObjectViewClassification(project, {ObjectData::ObjectType::Unit, "U002"});
    QCOMPARE(soldier.hero, std::optional<bool>{false}); QCOMPARE(soldier.folders, QStringList({"人族", "对战", "单位"}));
    const auto overridden = QtApp::ResolveObjectViewClassification(project, {ObjectData::ObjectType::Unit, "U003"});
    QCOMPARE(overridden.folders, QStringList({"兽族", "战役", "单位"}));
    QtApp::ObjectViewQuery query; query.kind = "unit"; query.source = QtApp::ObjectViewSource::Map;
    query.group = "兽族"; query.catalogSet = "战役";
    const auto page = QtApp::QueryObjectViewEntries(project, query);
    QVERIFY(!page.partial); QCOMPARE(page.rows.size(), 1); QCOMPARE(page.rows.front().entry.id, QString("U003"));
    QVERIFY(page.rows.front().customObject); QCOMPARE(page.rows.front().entry.race, QString("orc"));
    query.group.clear(); query.catalogSet.clear(); query.catalogCategory = "英雄";
    const auto heroes = QtApp::QueryObjectViewEntries(project, query);
    QCOMPARE(heroes.rows.size(), 1); QCOMPARE(heroes.rows.front().entry.id, QString("u001"));
}
void WorldAssetBrowserTests::DragIdentityIsBoundedAndDecimal() {
    QJsonObject request{{"schema", "forge-world-asset-v1"}, {"mapKey", "map-A"}, {"generation", "18446744073709551615"},
        {"revision", "7"}, {"type", 0}, {"rawcode", "hfoo"}};
    const auto parse = [&] { return WorldAssetBrowser::ParseDrag(QJsonDocument(request).toJson(QJsonDocument::Compact)); };
    auto good = parse(); QVERIFY(good.Succeeded()); QCOMPARE(good.value->binding.generation, UINT64_MAX);
    QCOMPARE(good.value->object.rawcode, std::string("hfoo"));
    for (const auto& value : {QJsonValue(7), QJsonValue("-1"), QJsonValue(" 7"), QJsonValue("18446744073709551616")}) {
        request["revision"] = value; QVERIFY(!parse().Succeeded());
    }
    request["revision"] = "7"; request["type"] = 4; QVERIFY(!parse().Succeeded());
    for (const int type : {5, 6}) { request["type"] = type; QVERIFY(parse().Succeeded()); }
    for (const int type : {1, 3}) { request["type"] = type; QVERIFY(!parse().Succeeded()); }
    QVERIFY(!WorldAssetBrowser::ParseDrag(QByteArray(4097, 'x')).Succeeded());
}
void WorldAssetBrowserTests::BrowserUsesPlacementSignalAndRejectsStaleContext() {
    QtProject::EditorProject project; QVERIFY(InstallObjects(project)); bool current = true; int placements = 0;
    WorldAssetBrowser browser(nullptr, nullptr); browser.resize(700, 300);
    browser.SetContext(project, {"map-A", 1, 7}, Result<WorldTerrainSources>::Failure("Test", "missing", "fixture"), [&] { return current; }, {});
    connect(&browser, &WorldAssetBrowser::PlacementRequested, this, [&](const QtApp::ObjectViewEntry& entry, const WorldAssetBinding& binding) {
        QCOMPARE(entry.id, QString("hfoo")); QCOMPARE(binding.revision, std::uint64_t(7)); ++placements;
    });
    browser.show(); QApplication::processEvents(); auto* rows = browser.findChild<QListWidget*>("worldAssetRows"); QVERIFY(rows);
    QTRY_COMPARE(rows->count(), 4); // U001 按原版父对象 hfoo 分类：三个单位与一个分类标题。
    QVERIFY(!(rows->item(0)->flags() & Qt::ItemIsSelectable));
    QCOMPARE(rows->item(0)->text(), QString("单位"));
    QStringList ids;
    for (int i = 1; i < rows->count(); ++i) ids << rows->item(i)->data(Qt::UserRole).toString();
    ids.sort(); // 此处核对完整身份集合；显示顺序由本地化名称排序决定。
    QCOMPARE(ids, QStringList({"U001", "hfoo", "hpea"}));
    QCOMPARE(rows->item(1)->data(Qt::UserRole).toString(), QString("hfoo"));
    const auto point = rows->visualItemRect(rows->item(1)).center(); QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(placements, 1); current = false; QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, point); QCOMPARE(placements, 1);
    browser.SetCollapsed(true); QVERIFY(browser.Collapsed()); QVERIFY(!rows->isVisible());
}
void WorldAssetBrowserTests::CompactGroupedCatalogKeepsRowIdentity() {
    QtProject::EditorProject project; QVERIFY(InstallObjects(project));
    const std::string lni = "[U001]\n_parent=\"hfoo\"\nubdg=1\n[U002]\n_parent=\"hfoo\"\nuspe=1\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(), lni.size())));
    QVERIFY(parsed.Succeeded()); project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    WorldAssetBrowser browser(nullptr, nullptr); browser.resize(280, 600); browser.SetVerticalDock(true);
    browser.SetContext(project, {"map-A", 1, 7}, Result<WorldTerrainSources>::Success({}), [] { return true; }, {});
    browser.show(); auto* rows = browser.findChild<QListWidget*>("worldAssetRows"); QVERIFY(rows);
    QTRY_COMPARE(rows->count(), 7); // 四个对象：单位两个、建筑一个、特殊一个；三个分类标题。
    QVERIFY(browser.Compact()); QCOMPARE(rows->iconSize(), QSize(32, 32));
    QStringList headings, ids;
    for (int i = 0; i < rows->count(); ++i) {
        const auto* item = rows->item(i);
        if (!(item->flags() & Qt::ItemIsSelectable)) headings << item->text();
        else { ids << item->data(Qt::UserRole).toString(); QVERIFY(item->text().isEmpty()); QVERIFY(!item->toolTip().isEmpty()); }
    }
    QCOMPARE(headings, QStringList({"单位", "建筑", "特殊"}));
    QCOMPARE(ids, QStringList({"hfoo", "hpea", "U001", "U002"}));
    QTRY_VERIFY(rows->visualItemRect(rows->item(0)).width() >= rows->viewport()->width() - 6);
    QVERIFY(rows->visualItemRect(rows->item(1)).top() >= rows->visualItemRect(rows->item(0)).bottom());
    browser.SetVerticalDock(false); QVERIFY(!browser.Compact());
    QVERIFY(!rows->item(1)->text().isEmpty());
    browser.SetCompact(true); browser.SetVerticalDock(false); QVERIFY(browser.Compact()); // 用户选择优先。
}
void WorldAssetBrowserTests::ObservedPlacementsRebindWithoutReloadButCatalogChangesReload() {
    QTemporaryDir root; QVERIFY(root.isValid());
    QtProject::EditorProject project; QVERIFY(InstallObjects(project));
    std::string lni = "[hfoo]\nunam=\"TRIGSTR_1\"\n";
    for (int i = 1; i <= 64; ++i)
        lni += "[" + QString("U%1").arg(i, 3, 16, QLatin1Char('0')).toUpper().toStdString() + "]\n_parent=\"hfoo\"\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(), lni.size())));
    QVERIFY(parsed.Succeeded());
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    QVERIFY(project.objectDocuments_[0]);
    MapData::UnitPlacements units;
    units.units.push_back(std::get<MapData::UnitPlacement>(MakeNewPlacement(ObjectData::ObjectType::Unit, "hfoo", 0, {0, 0, 0})));
    const auto encoded = units.Serialize(); QVERIFY(encoded.Succeeded());
    const std::string wts = "STRING 1\n{\nName before\n}\n";
    const auto wtsBytes = std::as_bytes(std::span(wts.data(), wts.size()));
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *encoded.value}, {"war3map.wts", {wtsBytes.begin(), wtsBytes.end()}}};
    for (const auto& member : members) {
        QFile file(root.filePath(QString::fromStdString(member.name))); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(reinterpret_cast<const char*>(member.data.data()), static_cast<qint64>(member.data.size())), static_cast<qint64>(member.data.size()));
    }
    auto opened = DirectoryWorkspace::Open(std::filesystem::path(root.path().toStdWString())); QVERIFY(opened.Succeeded());
    project.projectRoot_ = std::filesystem::path(root.path().toStdWString());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    QVERIFY(project.mapDocument_->Units() && project.mapDocument_->Strings());
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    project.ResetEditTracking(); QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
    WorldAssetBrowser browser(nullptr, nullptr); browser.resize(240, 320);
    const auto rebind = [&] {
        const WorldAssetBinding binding{root.path(), 1, project.mapEdits_->Revision()};
        browser.SetContext(project, binding, [&, binding] { return binding.revision == project.mapEdits_->Revision(); });
    };
    rebind(); browser.show();
    auto* rows = browser.findChild<QListWidget*>("worldAssetRows"); QVERIFY(rows);
    const auto find = [&]() -> QListWidgetItem* {
        for (int i = 0; i < rows->count(); ++i)
            if (rows->item(i)->data(Qt::UserRole).toString() == "U020") return rows->item(i);
        return nullptr;
    };
    QTRY_COMPARE(rows->count(), 67); QTRY_VERIFY(rows->verticalScrollBar()->maximum() > 0);
    auto* retained = find(); QVERIFY(retained); QCOMPARE(retained->text(), QString("Name before"));
    rows->setCurrentItem(retained); rows->verticalScrollBar()->setValue(rows->verticalScrollBar()->maximum() / 2);
    const auto scroll = rows->verticalScrollBar()->value(); QVERIFY(scroll > 0);
    constexpr int markerRole = Qt::UserRole + 90; retained->setData(markerRole, "retained");
    QVERIFY(!project.editRevision_.Current().contains("map/editor/terrain-journal"));
    units.units.front().rotation = 0.5f; QVERIFY(project.mapEdits_->EditUnits(units).Succeeded());
    QVERIFY(project.ObserveEditState("fixture placement edit"));
    // 单位命令使地形前向记录变为不可重放；这个实际工程scope仍不是目录内容。
    QVERIFY(project.editRevision_.Current().contains("map/editor/terrain-journal")); rebind();
    QCOMPARE(find(), retained); QCOMPARE(rows->currentItem(), retained); QCOMPARE(rows->verticalScrollBar()->value(), scroll);
    QCOMPARE(retained->data(markerRole).toString(), QString("retained"));
    project.terrainJournalWarning_ = "原创草稿的地形重放提示";
    QVERIFY(project.ObserveEditState("fixture terrain journal warning"));
    QVERIFY(project.editRevision_.Current().contains("map/editor/terrain-journal-warning")); rebind();
    QCOMPARE(find(), retained); QCOMPARE(rows->currentItem(), retained); QCOMPARE(rows->verticalScrollBar()->value(), scroll);
    QCOMPARE(retained->data(markerRole).toString(), QString("retained"));
    project.terrainJournalWarning_.clear(); QVERIFY(project.ObserveEditState("fixture clear terrain journal warning")); rebind();
    QCOMPARE(find(), retained); QCOMPARE(retained->data(markerRole).toString(), QString("retained"));
    auto pathingScopes = project.editRevision_.Current();
    pathingScopes["map/editor/pathing-restrictions"] = "synthetic changed pathing intent";
    QVERIFY(project.editRevision_.Observe(std::move(pathingScopes), "fixture pathing intent").Succeeded()); rebind();
    QCOMPARE(find(), retained); QCOMPARE(retained->data(markerRole).toString(), QString("retained"));
    int requests = 0; WorldAssetBinding received;
    connect(&browser, &WorldAssetBrowser::PlacementRequested, this, [&](const QtApp::ObjectViewEntry& entry, const WorldAssetBinding& binding) {
        QCOMPARE(entry.id, QString("U020")); received = binding; ++requests;
    });
    rows->scrollToItem(retained); QTRY_VERIFY(rows->viewport()->rect().contains(rows->visualItemRect(retained).center()));
    QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, rows->visualItemRect(retained).center());
    QCOMPARE(requests, 1); QCOMPARE(received.revision, project.mapEdits_->Revision()); QCOMPARE(received.mapKey, root.path());
    auto strings = *project.mapDocument_->Strings(); strings.entries.front().value = "Strings updated";
    QVERIFY(project.mapEdits_->EditStrings(strings).Succeeded()); QVERIFY(project.ObserveEditState("fixture WTS edit")); rebind();
    QTRY_VERIFY(find() && find()->text() == "Strings updated"); QVERIFY(!find()->data(markerRole).isValid());
    find()->setData(markerRole, "before object edit");
    QVERIFY(project.objectDocuments_[0]->SetModification({ObjectData::ObjectType::Unit, "U020"}, {"unam"}, std::string("Object updated")).Succeeded());
    QVERIFY(project.ObserveEditState("fixture object edit")); rebind();
    QTRY_VERIFY(find() && find()->text() == "Object updated"); QVERIFY(!find()->data(markerRole).isValid());
    find()->setData(markerRole, "before unobserved edit");
    const auto revision = project.editRevision_.Revision(); units.units.front().rotation = 1.0f;
    QVERIFY(project.mapEdits_->EditUnits(units).Succeeded()); rebind(); // 故意不调用 Observe。
    QCOMPARE(project.editRevision_.Revision(), revision);
    QTRY_VERIFY(find() && !find()->data(markerRole).isValid()); QCOMPARE(find()->text(), QString("Object updated"));
    // 未Observe会清除可复用上下文；先重建并证明能复用，未知scope的断言才有区分力。
    rebind(); QTRY_VERIFY(find() && find()->text() == "Object updated");
    find()->setData(markerRole, "before unknown scope");
    rebind(); QVERIFY(find()); QCOMPARE(find()->data(markerRole).toString(), QString("before unknown scope"));
    auto scopes = project.editRevision_.Current(); scopes["map/editor/unknown-content"] = "synthetic changed scope";
    QVERIFY(project.editRevision_.Observe(std::move(scopes), "fixture unknown editor scope").Succeeded()); rebind();
    QTRY_VERIFY(find() && !find()->data(markerRole).isValid()); QCOMPARE(find()->text(), QString("Object updated"));
}
void WorldAssetBrowserTests::MissingBackendNeverProducesModelImage() {
    WorldThumbnailCache cache(nullptr); cache.SetContext({"map-A", 1, 7}, Result<WorldTerrainSources>::Success({}), [] { return true; });
    Render::ModelInstanceRequest request; request.modelPath = "Synthetic/Only.mdx"; const auto key = cache.Request(request);
    QTRY_VERIFY(cache.Lookup(key).has_value()); QVERIFY(cache.Lookup(key)->image.isNull()); QVERIFY(!cache.Lookup(key)->issue.isEmpty());
    cache.SetContext({"map-B", 2, 0}, Result<WorldTerrainSources>::Success({}), [] { return true; }); QVERIFY(!cache.Lookup(key));
}
void WorldAssetBrowserTests::RealGameThumbnailEvidence() {
    const auto output = qEnvironmentVariable("FORGE_R08_THUMBNAIL_EVIDENCE_DIR");
    if (output.isEmpty()) { qInfo("real thumbnail evidence not enabled"); return; }
    const auto game = qEnvironmentVariable("FORGE_WAR3_ROOT"); QVERIFY2(!game.isEmpty(), "explicit evidence requires FORGE_WAR3_ROOT");
    QDir directory(output); QVERIFY(directory.mkpath(QStringLiteral(".")));
    WorldTerrainSources sources; sources.gameRoot = std::filesystem::path(game.toStdWString());
    sources.archivePath = std::filesystem::path(directory.filePath("thumbnail-empty-source.mpq").toStdWString());
    QVERIFY(Mpq::WriteMpqArchive(sources.archivePath, {}).Succeeded());
    const auto archiveSha = Sha256File(sources.archivePath); QVERIFY(archiveSha.Succeeded()); sources.archiveSha = *archiveSha.value;
    WorldThumbnailCache cache(nullptr, nullptr, true); cache.SetContext({"real-footman-evidence", 1, 7}, Result<WorldTerrainSources>::Success(sources), [] { return true; });
    Render::ModelInstanceRequest request; request.modelPath = "Units\\Human\\Footman\\Footman.mdx";
    const auto key = cache.Request(request); QTRY_VERIFY_WITH_TIMEOUT(cache.Lookup(key).has_value(), 60000);
    const auto result = *cache.Lookup(key); QVERIFY2(result.issue.isEmpty(), qPrintable(result.issue));
    QVERIFY(!result.image.isNull() && result.receipt && !result.sources.empty() && result.poses.size() == 1 && result.poses.front().animated);
    const auto png = directory.filePath("footman-stand-0.png"); QFile pngFile(png);
    QVERIFY(pngFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QVERIFY(result.image.save(&pngFile, "PNG")); pngFile.close();
    const auto pngSha = Sha256File(std::filesystem::path(png.toStdWString())); QVERIFY(pngSha.Succeeded());
    const auto& receipt = *result.receipt; const auto& pose = result.poses.front(); const auto decimal = [](std::uint64_t v) { return QString::number(v); };
    QJsonArray resources; for (const auto& source : result.sources) resources.append(QJsonObject{{"path", QString::fromStdString(source.virtualPath)},
        {"layer", QString::fromStdString(source.sourceLayer)}, {"sha256", QString::fromStdString(source.contentDigest)}});
    const auto& frame = receipt.frame;
    QJsonArray projection; for (const auto value : frame.view.viewProjection) projection.append(value);
    QJsonObject camera{{"target_x", frame.camera.targetX}, {"target_y", frame.camera.targetY}, {"target_z", frame.camera.targetZ},
        {"distance", frame.camera.distance}, {"yaw", frame.camera.yawRadians}, {"pitch", frame.camera.pitchRadians}};
    QJsonObject report{{"quality", "PreviewApproximate"}, {"scope", "model-thumbnail"}, {"runtime_verified", false}, {"complete_map", false},
        {"action", "Stand"}, {"elapsed_ms", "0"}, {"png_sha256", QString::fromStdString(*pngSha.value)},
        {"rgba_sha256", QString::fromStdString(result.imageSha256)}, {"source_archive_sha256", QString::fromStdString(sources.archiveSha)}, {"resources", resources},
        {"width", static_cast<int>(frame.capture.width)}, {"height", static_cast<int>(frame.capture.height)}, {"frame_count", decimal(frame.capture.frameCount)},
        {"capture_ticket", decimal(frame.capture.ticket)}, {"document_revision", decimal(frame.capture.documentRevision)}, {"surface_epoch", decimal(frame.view.surfaceEpoch)},
        {"scene_epoch", decimal(frame.view.sceneEpoch)}, {"view_epoch", decimal(frame.view.viewEpoch)}, {"scene_version", decimal(receipt.skin.sceneVersion)},
        {"content_version", decimal(receipt.skin.contentVersion)}, {"static_upload_count", decimal(receipt.skin.staticUploadCount)}, {"matrix_upload_count", decimal(receipt.skin.matrixUploadCount)},
        {"instance_id", decimal(pose.identity.value)}, {"instance_generation", decimal(pose.identity.generation)}, {"asset_id", decimal(pose.asset.value)},
        {"asset_generation", decimal(pose.asset.generation)}, {"pose_version", decimal(pose.poseVersion)}, {"evaluation_epoch", decimal(pose.evaluationEpoch)},
        {"matrix_hash", decimal(pose.matrixHash)}, {"alpha_hash", decimal(pose.alphaHash)}, {"color_hash", decimal(pose.colorHash)},
        {"camera", camera}, {"view_projection", projection}, {"view_frame_count", decimal(frame.view.frameCount)}, {"skin_frame_count", decimal(receipt.skin.frameCount)}};
    QFile file(directory.filePath("footman-receipt.json")); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented); QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close();
    cache.SetContext({"changed-map", 2, 0}, Result<WorldTerrainSources>::Success(sources), [] { return true; }); QVERIFY(!cache.Lookup(key));
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication application(argc, argv); WorldAssetBrowserTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "WorldAssetBrowserTests.moc"
