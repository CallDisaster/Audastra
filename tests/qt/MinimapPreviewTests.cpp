#include "WorldMinimapPreview.h"
#include "EditorProject.h"
#include "Commands/MapDataCommands.h"
#include "McpToolInternal.h"
#include "War3MapCore/Services/MinimapPreview.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "../TerrainCornerRampFixtures.h"
#include "../support/MinimapPreviewFixture.h"
#include <QApplication>
#include <QAction>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QTest>
#include <QTabBar>
#include <QUndoStack>

using namespace War3Map;
namespace F = TerrainCornerRampFixtures;
namespace {
auto ImageBytes(bool reverse = false) {
    Image::DecodedImage image; image.width = image.height = 2;
    image.rgba = {std::byte{255},std::byte{0},std::byte{0},std::byte{255},
        std::byte{0},std::byte{255},std::byte{0},std::byte{255},
        std::byte{0},std::byte{0},std::byte{255},std::byte{255},
        std::byte{255},std::byte{255},std::byte{0},std::byte{255}};
    if (reverse) std::swap_ranges(image.rgba.begin(), image.rgba.begin() + 4, image.rgba.begin() + 8);
    return F::Take(Image::EncodeBlpPaletted(image));
}
QJsonObject Call(Mcp::McpServer& server, const QJsonObject& args) {
    auto response = server.HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1},
        {"method", "tools/call"}, {"params", QJsonObject{{"name", "query_minimap_preview"}, {"arguments", args}}}}).toJson());
    if (!response || response->error) return {};
    const auto content = response->result.toObject()["content"].toArray();
    if (content.size() != 1) return {};
    return QJsonDocument::fromJson(content[0].toObject()["text"].toString().toUtf8()).object();
}
void Install(QtProject::EditorProject& project, const std::filesystem::path& path,
    const std::vector<MapArchiveMember>& members) {
    project.sourceArchivePath_ = path; project.projectRoot_ = path.parent_path();
    project.archiveSession_ = std::make_shared<MapArchiveSession>(F::Take(MapArchiveSession::Open(path)));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const auto terrain = std::find_if(members.begin(), members.end(), [](const auto& member) {
        return member.name == "war3map.w3e";
    });
    F::Require(terrain != members.end() && project.mapEdits_->AttachTerrain(terrain->data).Succeeded(), "preview attach terrain");
    project.mapDataUndoStack_->clear(); project.ResetEditTracking();
}
}
class MinimapPreviewTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("Audastra-MinimapPreview-Tests");
        QCoreApplication::setApplicationName("MinimapPreviewTests");
    }
    void SharedPixelsAndCliExport();
    void MissingCorruptBudgetAndStale();
    void ViewFollowsUndoSaveSourceAndSwitch();
    void ViewKeepsFailureDetailsInDiagnostics();
    void CurrentDefaultsAndStaleRequest();
    void CurrentKeepsPixelsUntilReplacementAndClearsFailures();
};

void MinimapPreviewTests::SharedPixelsAndCliExport() try {
    F::Fixture fixture; auto members = fixture.members;
    members.push_back({"war3mapMap.blp", ImageBytes()});
    const auto path = F::Take(WriteVerifiedArchive(fixture.root / "preview.w3x", members)).path;
    const auto sha = F::Take(Sha256File(path));
    const auto result = F::Take(Services::OpenMinimapPreview(path, sha));
    QVERIFY(result.present); QCOMPARE(result.image.width, 2U); QCOMPARE(result.image.height, 2U);
    QCOMPARE(std::to_integer<int>(result.image.rgba[0]), 255); // 红在左上，蓝在左下，不上下翻转。
    QCOMPARE(std::to_integer<int>(result.image.rgba[10]), 255);
    Mcp::McpServer server; Mcp::RegisterMinimapPreviewTool(server);
    QJsonObject args{{"source_path", QString::fromStdWString(path.wstring())},
        {"expected_source_sha256", QString::fromStdString(sha)}, {"include_rgba", true}};
    const auto reply = Call(server, args); QVERIFY(reply["ok"].toBool());
    auto invalid=args;invalid["source"]=true;QVERIFY(!Call(server,invalid)["ok"].toBool());
    invalid=args;invalid["layers"]=4;QVERIFY(!Call(server,invalid)["ok"].toBool());
    QCOMPARE(reply["rgba_sha256"].toString().toStdString(), result.rgbaSha256);
    const auto rgba = QByteArray::fromBase64(reply["rgba_base64"].toString().toLatin1());
    QCOMPARE(rgba, QByteArray(reinterpret_cast<const char*>(result.image.rgba.data()), result.image.rgba.size()));
    QProcess cli; const auto ppm = fixture.root / "actual.ppm";
    cli.start(QString::fromUtf8(AUDASTRA_PREVIEW_CLI), {"minimap-preview", QString::fromStdWString(path.wstring()),
        QString::fromStdString(sha), QString::fromStdWString(ppm.wstring())});
    QVERIFY(cli.waitForFinished(30000)); QCOMPARE(cli.exitCode(), 0);
    const auto output = QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
    QCOMPARE(output["rgba_sha256"].toString(), reply["rgba_sha256"].toString());
    const auto bytes = F::Take(IO::ReadFileBytes(ppm));
    const auto header = F::Text("P6\n2 2\n255\n");
    QVERIFY(std::equal(header.begin(), header.end(), bytes.begin())); QCOMPARE(bytes.size(), header.size() + 12);
    QVERIFY(!Services::ExportMinimapPreviewPpm(result, ppm).Succeeded());
    QCOMPARE(F::Take(Sha256File(path)), sha);
} catch (const std::exception& error) { QFAIL(error.what()); }
void MinimapPreviewTests::CurrentDefaultsAndStaleRequest() try {
    F::Fixture fixture;auto members=fixture.members;members.push_back({"war3mapMap.blp",ImageBytes()});
    const auto path=F::Take(WriteVerifiedArchive(fixture.root/"stale.w3x",members)).path;
    QtProject::EditorProject project;Install(project,path,members);
    Modules::WorldMinimapPreview view(project);view.show();
    auto* source=view.findChild<QTabBar*>("worldMinimapSource");QVERIFY(source);QCOMPARE(source->currentIndex(),0);
    QtProject::ProjectMinimapPreview controller(project);
    QtProject::MinimapPreviewOptions options;options.current=false;controller.Request(options);
    QTRY_VERIFY_WITH_TIMEOUT(controller.ResultValue()&&controller.ResultValue()->Succeeded(),15000);
    const auto oldSha=controller.ResultValue()->value->rgbaSha256;
    controller.Request(options,true); // 尚未完成的请求不能在换来源后发布。
    auto other=members;other.back().data=ImageBytes(true);
    const auto next=F::Take(WriteVerifiedArchive(fixture.root/"next.w3x",other)).path;
    Install(project,next,other);project.MapDocumentChanged();
    QVERIFY(!controller.ResultValue());
    QTRY_VERIFY_WITH_TIMEOUT(controller.ResultValue()&&controller.ResultValue()->Succeeded(),15000);
    QVERIFY(controller.ResultValue()->value->rgbaSha256!=oldSha);
    QCOMPARE(controller.ResultValue()->value->sourceSha256,project.archiveSession_->SourceSha256());
    QTRY_COMPARE_WITH_TIMEOUT(view.findChild<QLabel*>("worldMinimapState")->text(),QString("当前不可用"),15000);
    QVERIFY(view.property("rgbaSha256").toString().isEmpty());
} catch(const std::exception& error){QFAIL(error.what());}

void MinimapPreviewTests::MissingCorruptBudgetAndStale() try {
    F::Fixture fixture;
    const auto denied = Services::OpenMinimapPreview(fixture.root / "never-open.AI", std::string(64, 'a'));
    QVERIFY(!denied.Succeeded());
    QCOMPARE(denied.diagnostics.front().code, std::string("MINIMAP_PREVIEW_SOURCE_TYPE"));
    const auto sha = fixture.source->SourceSha256();
    QVERIFY(!F::Take(Services::CaptureMinimapPreview(*fixture.source, sha)).present);
    QVERIFY(!Services::CaptureMinimapPreview(*fixture.source, std::string(64, 'a')).Succeeded());
    QVERIFY(!Services::CaptureMinimapPreview(*fixture.source, "bad").Succeeded());
    for (int type = 0; type != 4; ++type) {
        auto members = fixture.members;
        std::vector<std::byte> blob;
        if (type == 1) blob = F::Text("not a BLP");
        if (type == 2) blob.resize(4U * 1024U * 1024U + 1);
        if (type == 3) {
            Image::DecodedImage oversized; oversized.width = 1024; oversized.height = 1;
            oversized.rgba.resize(1024 * 4, std::byte{255});
            blob = F::Take(Image::EncodeBlpPaletted(oversized));
        }
        members.push_back({"war3mapMap.blp", std::move(blob)});
        const auto output = F::Take(WriteVerifiedArchive(fixture.root / ("bad" + std::to_string(type) + ".w3x"), members));
        QVERIFY(!Services::OpenMinimapPreview(output.path, output.sha256).Succeeded());
    }
} catch (const std::exception& error) { QFAIL(error.what()); }

void MinimapPreviewTests::CurrentKeepsPixelsUntilReplacementAndClearsFailures() try {
    F::Fixture fixture;
    auto members = Tests::MinimapPreviewFixtures::MapMembers();
    const auto path = F::Take(WriteVerifiedArchive(fixture.root / "retain-current.w3x", members)).path;
    QtProject::EditorProject project;
    Install(project, path, members);
    project.war3GameRoot_ = fixture.game;
    Modules::WorldMinimapPreview view(project);
    view.show();
    auto* state = view.findChild<QLabel*>("worldMinimapState");
    auto* image = view.findChild<QLabel*>("worldMinimapImage");
    QTRY_VERIFY_WITH_TIMEOUT(!view.property("rgbaSha256").toString().isEmpty(), 15000);
    const auto before = view.property("rgbaSha256");
    const auto beforePixmap = image->pixmap().cacheKey();
    auto pending = project.BeginPendingTerrainCommit();
    QCOMPARE(state->text(), QString("更新中…"));
    QCOMPARE(view.property("rgbaSha256"), before);
    QCOMPARE(image->pixmap().cacheKey(), beforePixmap);
    QTest::qWait(600);
    QCOMPARE(image->pixmap().cacheKey(), beforePixmap);
    MapData::TerrainBrush brush;
    brush.kind = MapData::TerrainBrushKind::Raise;
    brush.radius = 0;
    brush.strength = 64;
    const std::array<MapData::TerrainStrokePoint, 1> points{{{4 * 128, 4 * 128}}};
    auto plan = F::Take(project.mapEdits_->PrepareTerrainEdit(brush, points, false));
    std::unique_ptr<QtProject::MapDataEditCommand> undo;
    QVERIFY(project.mapEdits_->ApplyPreparedTerrainEdit(plan, [&](const auto& receipt) {
        undo = std::make_unique<QtProject::MapDataEditCommand>(project, receipt, "raise", "raise");
        return Status::Ok();
    }).Succeeded());
    project.mapDataUndoStack_->push(undo.release());
    QCOMPARE(view.property("rgbaSha256"), before);
    pending.reset();
    QTRY_VERIFY_WITH_TIMEOUT(!view.property("rgbaSha256").toString().isEmpty()
        && view.property("rgbaSha256") != before, 15000);
    QVERIFY(!view.property("updating").toBool());
    QAction* pathing = nullptr;
    for (auto* action : view.findChildren<QAction*>()) {
        if (action->text() == "通行") pathing = action;
    }
    QVERIFY(pathing);
    pathing->setChecked(true);
    QCOMPARE(state->text(), QString("更新中…"));
    QVERIFY(!image->pixmap().isNull());
    QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("当前不可用"), 15000);
    QVERIFY(image->pixmap().isNull());
    QVERIFY(view.property("rgbaSha256").toString().isEmpty());
} catch (const std::exception& error) { QFAIL(error.what()); }

void MinimapPreviewTests::ViewFollowsUndoSaveSourceAndSwitch() try {
    F::Fixture fixture; auto members = fixture.members; members.push_back({"war3mapMap.blp", ImageBytes()});
    const auto path = F::Take(WriteVerifiedArchive(fixture.root / "view.w3x", members)).path;
    QtProject::EditorProject project; Install(project, path, members);
    auto pending = project.BeginPendingTerrainCommit();
    Modules::WorldMinimapPreview view(project); view.show();view.findChild<QTabBar*>("worldMinimapSource")->setCurrentIndex(1);
    auto* state = view.findChild<QLabel*>("worldMinimapState"); QVERIFY(state);
    QTest::qWait(30);
    QVERIFY(view.property("rgbaSha256").toString().isEmpty());
    pending.reset();
    QTRY_VERIFY_WITH_TIMEOUT(!view.property("rgbaSha256").toString().isEmpty(), 15000);
    QCOMPARE(state->text(), QString()); QVERIFY(state->isHidden());
    const auto original = view.property("rgbaSha256");
    QCOMPARE(project.mapDataUndoStack_->count(), 0); QVERIFY(!project.mapEdits_->IsDirty());
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::Raise; brush.radius = 0;
    const auto& header = project.mapEdits_->Terrain()->header;
    const std::array<MapData::TerrainStrokePoint, 1> points{{{header.offset[0] + 4 * 128, header.offset[1] + 4 * 128}}};
    auto plan = F::Take(project.mapEdits_->PrepareTerrainEdit(brush, points, false));
    std::unique_ptr<QtProject::MapDataEditCommand> undo;
    auto applied = project.mapEdits_->ApplyPreparedTerrainEdit(plan, [&](const auto& receipt) {
        undo = std::make_unique<QtProject::MapDataEditCommand>(project, receipt, "test raise", "test raise"); return Status::Ok();
    });
    QVERIFY(applied.Succeeded()); project.mapDataUndoStack_->push(undo.release());
    QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("未保存"),15000); QCOMPARE(view.property("rgbaSha256"), original);
    project.mapDataUndoStack_->undo(); QVERIFY(!project.mapEdits_->IsDirty()); QTRY_VERIFY_WITH_TIMEOUT(state->text().isEmpty(),15000);
    project.mapDataUndoStack_->redo(); QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("未保存"),15000);
    // 原创视图夹具安装新基线；真实正常保存/回读另由语料及专用窗口验收覆盖。
    members.back().data = ImageBytes(true);
    const auto next = F::Take(WriteVerifiedArchive(fixture.root / "saved-result.w3x", members)).path;
    Install(project, next, members); project.MapDocumentChanged();
    QTRY_VERIFY_WITH_TIMEOUT(!view.property("rgbaSha256").toString().isEmpty()
        && view.property("rgbaSha256") != original, 15000);
    QCOMPARE(view.property("sourceSha256").toString().toStdString(), project.archiveSession_->SourceSha256());
    project.sourceChangeSuspected_ = true; project.SourceStateChanged();
    QVERIFY(view.property("rgbaSha256").toString().isEmpty());
    QCOMPARE(state->text(), QString("地图文件已变化"));
    project.sourceChangeSuspected_ = false;
    Install(project, fixture.source->SourcePath(), fixture.members); project.MapDocumentChanged();
    QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("没有小地图"), 15000);
    QVERIFY(view.findChild<QLabel*>("worldMinimapImage")->pixmap().isNull());
} catch (const std::exception& error) { QFAIL(error.what()); }

void MinimapPreviewTests::ViewKeepsFailureDetailsInDiagnostics() try {
    F::Fixture fixture; auto members = fixture.members;
    members.push_back({"war3mapMap.blp", F::Text("invalid BLP")});
    const auto path = F::Take(WriteVerifiedArchive(fixture.root / "broken-view.w3x", members)).path;
    QtProject::EditorProject project; Install(project, path, members);
    Modules::WorldMinimapPreview view(project); view.show();view.findChild<QTabBar*>("worldMinimapSource")->setCurrentIndex(1);
    auto* state = view.findChild<QLabel*>("worldMinimapState"); QVERIFY(state);
    QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("当前不可用"), 15000);
    QVERIFY(!state->text().contains("MINIMAP_PREVIEW"));
    bool found = false;
    for (int row = 0; row < project.diagnosticsModel_->rowCount(); ++row)
        if (project.diagnosticsModel_->item(row, 2)->text() == "MINIMAP_PREVIEW_INVALID") {
            QVERIFY(!project.diagnosticsModel_->item(row, 3)->text().isEmpty()); found = true;
        }
    QVERIFY(found);
} catch (const std::exception& error) { QFAIL(error.what()); }
QTEST_MAIN(MinimapPreviewTests)
#include "MinimapPreviewTests.moc"
