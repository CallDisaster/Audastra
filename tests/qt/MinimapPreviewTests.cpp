#include "WorldMinimapPreview.h"
#include "EditorProject.h"
#include "Commands/MapDataCommands.h"
#include "McpToolInternal.h"
#include "War3MapCore/Services/MinimapPreview.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "../TerrainCornerRampFixtures.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTest>
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
    F::Require(project.mapEdits_->AttachTerrain(members[0].data).Succeeded(), "preview attach terrain");
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

void MinimapPreviewTests::MissingCorruptBudgetAndStale() try {
    F::Fixture fixture;
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

void MinimapPreviewTests::ViewFollowsUndoSaveSourceAndSwitch() try {
    F::Fixture fixture; auto members = fixture.members; members.push_back({"war3mapMap.blp", ImageBytes()});
    const auto path = F::Take(WriteVerifiedArchive(fixture.root / "view.w3x", members)).path;
    QtProject::EditorProject project; Install(project, path, members);
    Modules::WorldMinimapPreview view(project); view.show();
    auto* state = view.findChild<QLabel*>("worldMinimapState"); QVERIFY(state);
    QTRY_VERIFY_WITH_TIMEOUT(!view.property("rgbaSha256").toString().isEmpty(), 15000);
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
    QVERIFY(state->text().contains("未保存")); QCOMPARE(view.property("rgbaSha256"), original);
    project.mapDataUndoStack_->undo(); QVERIFY(!project.mapEdits_->IsDirty()); QVERIFY(!state->text().contains("未保存"));
    project.mapDataUndoStack_->redo(); QVERIFY(state->text().contains("未保存"));
    // 原创视图夹具安装新基线；真实正常保存/回读另由语料及专用窗口验收覆盖。
    members.back().data = ImageBytes(true);
    const auto next = F::Take(WriteVerifiedArchive(fixture.root / "saved-result.w3x", members)).path;
    Install(project, next, members); project.MapDocumentChanged();
    QTRY_VERIFY_WITH_TIMEOUT(!view.property("rgbaSha256").toString().isEmpty()
        && view.property("rgbaSha256") != original, 15000);
    QCOMPARE(view.property("sourceSha256").toString().toStdString(), project.archiveSession_->SourceSha256());
    project.sourceChangeSuspected_ = true; project.SourceStateChanged();
    QVERIFY(view.property("rgbaSha256").toString().isEmpty());
    project.sourceChangeSuspected_ = false;
    Install(project, fixture.source->SourcePath(), fixture.members); project.MapDocumentChanged();
    QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("没有已保存"), 15000);
    QVERIFY(view.findChild<QLabel*>("worldMinimapImage")->pixmap().isNull());
} catch (const std::exception& error) { QFAIL(error.what()); }
QTEST_MAIN(MinimapPreviewTests)
#include "MinimapPreviewTests.moc"
