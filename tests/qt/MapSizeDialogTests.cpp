#include "MapSizeDialog.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QResource>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QTableWidget>
#include <QTest>
#include <QTemporaryDir>

#include <cstdio>

static void InitializeMapSizeProductStyle() {
    Q_INIT_RESOURCE(war3map_uikit_style);
}

using namespace War3Map;
using QtApp::MapSizeDialog;
namespace {
QPointer<UiKit::ThemeManager> captureTheme;
QPointer<QStyle> captureFusion;
struct Fixture {
    Services::MapSizeContext context;
    Services::MapSizeAnalysisOptions options;
    Fixture() {
        context.contextRevision = "owned-map-size/revision-1"; context.membersComplete = true;
        context.members = {{"Textures/a&b.blp", 100}, {"Models/mesh.mdx", 200}, {"war3map.j", 5}, {"other.txt", {}}};
        context.assetEdits = {{"Models/mesh.mdx", "Models/renamed.mdx"}};
        context.pending = {{"Sound/external.wav", 70, Services::MapSizeByteTrust::ReportedUnverified},
            {"New/copy.mdx", 30, Services::MapSizeByteTrust::OwnedMetadata}};
    }
    Result<Services::MapSizeReport> Query(std::string_view revision) const {
        return Services::AnalyzeMapSize(context, revision, options);
    }
};
QTableWidget* Top(MapSizeDialog* dialog) { return dialog->findChild<QTableWidget*>("mapSizeTopMembers"); }
QTableWidget* Totals(MapSizeDialog* dialog) { return dialog->findChild<QTableWidget*>("mapSizeCategoryTotals"); }
QLabel* MapSizeStatusLabel(MapSizeDialog* dialog) { return dialog->findChild<QLabel*>("mapSizeStatus"); }
QLabel* Scope(MapSizeDialog* dialog) { return dialog->findChild<QLabel*>("mapSizeScope"); }
void Refresh(MapSizeDialog* dialog) {
    // 直接信号可测试删除sender的负例，不在返回后给已销毁按钮发送MouseRelease。
    dialog->findChild<QPushButton*>("mapSizeRefresh")->clicked(false);
}
void ActivateRow(MapSizeDialog* dialog, int row) {
    auto* table = Top(dialog); table->setCurrentCell(row, 0);
    table->itemActivated(table->item(row, 0));
}
QString CaptureMapSize(MapSizeDialog& dialog, const Services::MapSizeReport& report,
    const std::string& currentRevision, const QString& name) {
    // 仅 root 明确授权的已有 offscreen 输出目录允许落图，不改变默认测试行为。
    const auto output = qEnvironmentVariable("AUD_MAP_SIZE_CAPTURE_DIR");
    if (!QDir::isAbsolutePath(output) || !QFileInfo(output).isDir())
        return QStringLiteral("AUD_MAP_SIZE_CAPTURE_DIR must name an existing absolute directory");
    if (QApplication::platformName() != QStringLiteral("offscreen"))
        return QStringLiteral("Map-size capture requires the root offscreen test lease");
    dialog.ensurePolished();
    if (dialog.layout()) dialog.layout()->activate();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
    const auto pixels = dialog.grab();
    if (pixels.isNull()) return QStringLiteral("Map-size grab returned no pixels");
    const QDir directory(output);
    const auto imagePath = directory.filePath(name + QStringLiteral(".png"));
    const auto metadataPath = directory.filePath(name + QStringLiteral(".json"));
    if (QFileInfo::exists(imagePath) || QFileInfo::exists(metadataPath))
        return QStringLiteral("Map-size capture outputs already exist; use a fresh owned directory");
    const auto logicalBytes = [](const std::optional<std::uint64_t>& value) -> QJsonValue {
        return value ? QJsonValue(QString::number(static_cast<qulonglong>(*value))) : QJsonValue(QJsonValue::Null);
    };
    const auto sheet = qApp->styleSheet();
    const QJsonObject metadata{{"schema", "audastra.offscreen-map-size-capture.v1"},
        {"fixture", "authored metadata through actual AnalyzeMapSize"}, {"platform", QApplication::platformName()},
        {"actualTheme", captureTheme ? captureTheme->CurrentThemeId() : QString{}},
        {"fusion", captureFusion && captureFusion->objectName().compare(QStringLiteral("fusion"), Qt::CaseInsensitive) == 0},
        {"offscreen", QApplication::platformName() == QStringLiteral("offscreen")},
        {"actualFont", QJsonObject{{"family", QFontInfo(dialog.font()).family()},
            {"pointSize", QFontInfo(dialog.font()).pointSizeF()}, {"pixelSize", QFontInfo(dialog.font()).pixelSize()},
            {"styleName", QFontInfo(dialog.font()).styleName()}}},
        {"palette", QJsonObject{{"window", dialog.palette().color(QPalette::Window).name(QColor::HexArgb)},
            {"windowText", dialog.palette().color(QPalette::WindowText).name(QColor::HexArgb)},
            {"base", dialog.palette().color(QPalette::Base).name(QColor::HexArgb)},
            {"highlight", dialog.palette().color(QPalette::Highlight).name(QColor::HexArgb)}}},
        {"qss", QJsonObject{{"characters", static_cast<int>(sheet.size())},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(sheet.toUtf8(), QCryptographicHash::Sha256).toHex())},
            {"styleClass", QString::fromLatin1(QApplication::style()->metaObject()->className())}}},
        {"image", QJsonObject{{"pixelWidth", pixels.width()}, {"pixelHeight", pixels.height()},
            {"logicalWidth", dialog.width()}, {"logicalHeight", dialog.height()}, {"dpr", pixels.devicePixelRatio()}}},
        {"currentRevision", QString::fromStdString(currentRevision)},
        {"reportRevision", QString::fromStdString(report.contextRevision)}, {"query", "metadata-only"},
        {"reportScope", QJsonObject{{"state", static_cast<int>(report.state)}, {"sourceKind", static_cast<int>(report.sourceKind)},
            {"topWindowComplete", report.topWindowComplete}, {"inputTruncated", report.inputTruncated},
            {"outputTruncated", report.outputTruncated}, {"effectiveUnknown", report.effectiveUnknown},
            {"generatedScriptSizeUnknown", report.generatedScriptSizeUnknown},
            {"examinedMetadataRows", static_cast<int>(report.examinedMetadataRows)}, {"text", Scope(&dialog)->text()}}},
        {"baselineKnownLogicalBytes", logicalBytes(report.baseline.knownLogicalSubtotal)},
        {"effectiveKnownLogicalBytes", logicalBytes(report.effective.knownLogicalSubtotal)},
        {"effectiveReportedUnverifiedBytes", logicalBytes(report.effective.reportedUnverifiedSubtotal)},
        {"reportRows", static_cast<int>(report.top20.size())}, {"visibleRows", Top(&dialog)->rowCount()},
        {"categoryRows", Totals(&dialog)->rowCount()}, {"status", MapSizeStatusLabel(&dialog)->text()}};
    QFile image(imagePath);
    if (!image.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !pixels.save(&image, "PNG") || !image.flush())
        return QStringLiteral("Could not create map-size PNG");
    image.close();
    QFile receipt(metadataPath); const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    if (!receipt.open(QIODevice::WriteOnly | QIODevice::NewOnly) || receipt.write(bytes) != bytes.size() || !receipt.flush())
        return QStringLiteral("Could not create map-size capture metadata");
    return {};
}
} // namespace

class MapSizeDialogTests final : public QObject {
    Q_OBJECT
private slots:
    void ActualServiceTotalsAndOwnedActivation();
    void UnknownDirectoryAndKnownZeroAreDistinct();
    void BudgetedTop20RefreshAndStaleActivation();
    void CallbackContextChanged_data();
    void CallbackContextChanged();
    void CallbackDeletesOwner_data();
    void CallbackDeletesOwner();
    void FailureTextStaysPlainAndRefreshRecovers();
};

void MapSizeDialogTests::ActualServiceTotalsAndOwnedActivation() {
    Fixture fixture; int queries = 0, activations = 0;
    const bool captureRequested = qEnvironmentVariableIsSet("AUD_MAP_SIZE_CAPTURE_DIR");
    std::optional<Services::MapSizeReport> displayedReport;
    Services::MapSizeRow activated; std::string activatedRevision;
    MapSizeDialog dialog([&] { return fixture.context.contextRevision; }, [&](std::string_view revision) {
        ++queries; auto result = fixture.Query(revision);
        if (captureRequested && result.Succeeded() && result.value) displayedReport = *result.value;
        return result;
    }, [&](const auto& row, std::string_view revision) { ++activations; activated = row; activatedRevision = revision; });
    QSignalSpy accepted(&dialog, &QDialog::accepted), finished(&dialog, &QDialog::finished);
    dialog.Focus(); QCOMPARE(queries, 1); QCOMPARE(activations, 0);
    auto* totals = Totals(&dialog); auto* top = Top(&dialog); QVERIFY(totals && top && Scope(&dialog) && MapSizeStatusLabel(&dialog));
    QCOMPARE(totals->rowCount(), 6); QCOMPARE(totals->item(5, 1)->text(), QString("305 B"));
    QCOMPARE(totals->item(5, 2)->text(), QString("335 B")); QCOMPARE(totals->item(5, 3)->text(), QString("70 B"));
    QCOMPARE(totals->item(5, 4)->text(), QString("1"));
    QCOMPARE(totals->item(0, 1)->text(), QString("200 B")); QCOMPARE(totals->item(0, 2)->text(), QString("230 B"));
    QCOMPARE(top->rowCount(), 5); QCOMPARE(top->item(0, 0)->text(), QString("Models/renamed.mdx"));
    QCOMPARE(top->item(1, 0)->text(), QString("Textures/a&b.blp"));
    QVERIFY(top->item(1, 0)->toolTip().contains(QStringLiteral("a&amp;b.blp")));
    QCOMPARE(top->item(2, 3)->text(), QStringLiteral("外盘上报，未核验"));
    QCOMPARE(Scope(&dialog)->textFormat(), Qt::PlainText); QCOMPARE(MapSizeStatusLabel(&dialog)->textFormat(), Qt::PlainText);
    QVERIFY(Scope(&dialog)->text().contains(QStringLiteral("不是 w3x/w3m 磁盘大小")));
    QVERIFY(MapSizeStatusLabel(&dialog)->text().contains(QStringLiteral("不能称为全图最大成员")));
    if (captureRequested) {
        QVERIFY(displayedReport); QCOMPARE(displayedReport->contextRevision, fixture.context.contextRevision);
        const auto error = CaptureMapSize(dialog, *displayedReport, fixture.context.contextRevision, QStringLiteral("map-size-known"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(queries, 1); QCOMPARE(top->rowCount(), 5);
    }
    ActivateRow(&dialog, 0); QCOMPARE(activations, 1);
    QCOMPARE(activated.originalPath, std::string("Models/mesh.mdx")); QCOMPARE(activated.currentPath, std::string("Models/renamed.mdx"));
    QCOMPARE(activated.identity, std::string("member/MODELS\\MESH.MDX")); QCOMPARE(activatedRevision, fixture.context.contextRevision);
    QVERIFY(activated.navigationAllowed && !activated.managedMember);
    ActivateRow(&dialog, 4); QCOMPARE(activations, 2); QVERIFY(activated.managedMember && !activated.navigationAllowed);
    QVERIFY(dialog.isVisible()); QCOMPARE(accepted.count(), 0); QCOMPARE(finished.count(), 0);
    QCOMPARE(fixture.context.members[1].path, std::string("Models/mesh.mdx"));
    QCOMPARE(fixture.context.pending[0].trust, Services::MapSizeByteTrust::ReportedUnverified);
}

void MapSizeDialogTests::UnknownDirectoryAndKnownZeroAreDistinct() {
    Fixture fixture; fixture.context.sourceKind = Services::MapSizeSourceKind::DirectorySnapshot;
    fixture.context.membersComplete = false; fixture.context.overlayComplete = false; fixture.context.generatedScriptSizeUnknown = true;
    const bool captureRequested = qEnvironmentVariableIsSet("AUD_MAP_SIZE_CAPTURE_DIR");
    std::optional<Services::MapSizeReport> displayedReport;
    MapSizeDialog dialog([&] { return fixture.context.contextRevision; }, [&](std::string_view revision) {
        auto result = fixture.Query(revision);
        if (captureRequested && result.Succeeded() && result.value) displayedReport = *result.value;
        return result;
    }, {});
    dialog.Focus();
    QCOMPARE(Totals(&dialog)->item(5, 1)->text(), QString("305 B"));
    QCOMPARE(Totals(&dialog)->item(5, 2)->text(), QStringLiteral("未知"));
    QCOMPARE(Totals(&dialog)->item(5, 3)->text(), QStringLiteral("未知"));
    QCOMPARE(Totals(&dialog)->item(5, 4)->text(), QStringLiteral("未知")); QCOMPARE(Top(&dialog)->rowCount(), 0);
    QVERIFY(Scope(&dialog)->text().contains(QStringLiteral("目录快照")) && Scope(&dialog)->text().contains(QStringLiteral("部分覆盖")));
    QVERIFY(MapSizeStatusLabel(&dialog)->text().contains(QStringLiteral("不用旧大小代替")));
    if (captureRequested) {
        QVERIFY(displayedReport); QCOMPARE(displayedReport->contextRevision, fixture.context.contextRevision);
        QVERIFY(displayedReport->effectiveUnknown);
        const auto error = CaptureMapSize(dialog, *displayedReport, fixture.context.contextRevision, QStringLiteral("map-size-unknown"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(Top(&dialog)->rowCount(), 0); QCOMPARE(Totals(&dialog)->item(5, 2)->text(), QStringLiteral("未知"));
    }
    fixture.context = {}; fixture.context.contextRevision = "owned-map-size/revision-2";
    fixture.context.membersComplete = true; fixture.context.members = {{"empty.bin", 0}};
    dialog.ContextChanged(); Refresh(&dialog);
    QCOMPARE(Totals(&dialog)->item(5, 1)->text(), QString("0 B")); QCOMPARE(Totals(&dialog)->item(5, 2)->text(), QString("0 B"));
    QCOMPARE(Totals(&dialog)->item(5, 4)->text(), QString("0")); QCOMPARE(Top(&dialog)->rowCount(), 1);
    QCOMPARE(Top(&dialog)->item(0, 2)->text(), QString("0 B"));
    fixture.context.opaqueMemberCount = 2; dialog.ContextChanged(); Refresh(&dialog);
    QVERIFY(MapSizeStatusLabel(&dialog)->text().contains(QStringLiteral("2 个匿名成员")));
    QVERIFY(MapSizeStatusLabel(&dialog)->text().contains(QStringLiteral("不能称为全图最大成员")));
}

void MapSizeDialogTests::BudgetedTop20RefreshAndStaleActivation() {
    Fixture fixture; fixture.context.members.clear(); fixture.context.assetEdits.clear(); fixture.context.pending.clear();
    for (std::uint64_t index = 0; index < 50; ++index)
        fixture.context.members.push_back({"Models/model" + std::to_string(index) + ".mdx", 1000 + index});
    fixture.options.maxMetadataRows = 3; int queries = 0, activations = 0;
    MapSizeDialog dialog([&] { return fixture.context.contextRevision; }, [&](std::string_view revision) {
        ++queries; return fixture.Query(revision);
    }, [&](const auto&, std::string_view) { ++activations; });
    dialog.Focus(); QCOMPARE(queries, 1); QCOMPARE(Top(&dialog)->rowCount(), 3);
    QCOMPARE(Totals(&dialog)->item(5, 1)->text(), QString("3003 B"));
    QCOMPARE(Top(&dialog)->item(0, 2)->text(), QString("1002 B"));
    QVERIFY(MapSizeStatusLabel(&dialog)->text().contains(QStringLiteral("预算已截断")));
    fixture.options = {}; Refresh(&dialog); QCOMPARE(queries, 2); QCOMPARE(Top(&dialog)->rowCount(), 20);
    QCOMPARE(Top(&dialog)->item(0, 2)->text(), QString("1049 B"));
    fixture.context.contextRevision = "owned-map-size/revision-2";
    ActivateRow(&dialog, 0); QCOMPARE(activations, 0); QCOMPARE(Top(&dialog)->rowCount(), 0); QCOMPARE(Totals(&dialog)->rowCount(), 0);
    QVERIFY(MapSizeStatusLabel(&dialog)->text().contains(QStringLiteral("工程内容已变化")));
    Refresh(&dialog); QCOMPARE(queries, 3); QCOMPARE(Top(&dialog)->rowCount(), 20);
    ActivateRow(&dialog, 0); QCOMPARE(activations, 1);
}

void MapSizeDialogTests::CallbackContextChanged_data() {
    QTest::addColumn<QString>("phase");
    QTest::newRow("query") << QStringLiteral("query");
    QTest::newRow("revision-refresh") << QStringLiteral("revision-refresh");
    QTest::newRow("revision-after-query") << QStringLiteral("revision-after-query");
    QTest::newRow("revision-activation") << QStringLiteral("revision-activation");
}
void MapSizeDialogTests::CallbackContextChanged() {
    QFETCH(QString, phase);
    Fixture fixture; int revisions = 0, queries = 0, activations = 0;
    bool armed = phase != QStringLiteral("revision-activation"); QPointer<MapSizeDialog> guarded;
    MapSizeDialog dialog([&] {
        ++revisions;
        if (armed && (phase == QStringLiteral("revision-refresh") || phase == QStringLiteral("revision-activation")
            || (phase == QStringLiteral("revision-after-query") && revisions == 2))) {
            armed = false; guarded->ContextChanged();
        }
        return fixture.context.contextRevision;
    }, [&](std::string_view revision) {
        ++queries; auto report = fixture.Query(revision);
        if (armed && phase == QStringLiteral("query")) { armed = false; guarded->ContextChanged(); }
        return report;
    }, [&](const auto&, std::string_view) { ++activations; });
    guarded = &dialog; dialog.Focus();
    if (phase == QStringLiteral("revision-activation")) { QCOMPARE(Top(&dialog)->rowCount(), 5); armed = true; ActivateRow(&dialog, 0); }
    QCOMPARE(activations, 0); QCOMPARE(Top(&dialog)->rowCount(), 0); QCOMPARE(Totals(&dialog)->rowCount(), 0);
    QVERIFY(MapSizeStatusLabel(&dialog)->text().contains(QStringLiteral("工程内容已变化")));
    QCOMPARE(queries, phase == QStringLiteral("revision-refresh") ? 0 : 1);
    Refresh(&dialog); QCOMPARE(Top(&dialog)->rowCount(), 5); QCOMPARE(Totals(&dialog)->rowCount(), 6);
    ActivateRow(&dialog, 0); QCOMPARE(activations, 1);
}

void MapSizeDialogTests::CallbackDeletesOwner_data() {
    QTest::addColumn<QString>("phase");
    QTest::newRow("revision-refresh") << QStringLiteral("revision-refresh");
    QTest::newRow("query") << QStringLiteral("query");
    QTest::newRow("revision-after-query") << QStringLiteral("revision-after-query");
    QTest::newRow("revision-activation") << QStringLiteral("revision-activation");
    QTest::newRow("activation") << QStringLiteral("activation");
}
void MapSizeDialogTests::CallbackDeletesOwner() {
    QFETCH(QString, phase);
    Fixture fixture; int revisions = 0, queries = 0, activations = 0;
    const bool activationPhase = phase == QStringLiteral("revision-activation") || phase == QStringLiteral("activation");
    bool armed = !activationPhase; QPointer<QWidget> owner = new QWidget;
    const auto cleanup = qScopeGuard([&] { if (owner) delete owner.data(); });
    QPointer<MapSizeDialog> dialog = new MapSizeDialog([&] {
        ++revisions; const auto revision = fixture.context.contextRevision;
        if (armed && (phase == QStringLiteral("revision-refresh") || phase == QStringLiteral("revision-activation")
            || (phase == QStringLiteral("revision-after-query") && revisions == 2))) delete owner.data();
        return revision;
    }, [&](std::string_view revision) {
        ++queries; auto report = fixture.Query(revision);
        if (armed && phase == QStringLiteral("query")) delete owner.data();
        return report;
    }, [&](const auto&, std::string_view) {
        ++activations; if (armed && phase == QStringLiteral("activation")) delete owner.data();
    }, owner.data());
    QSignalSpy destroyed(dialog.data(), &QObject::destroyed);
    dialog->Focus();
    if (activationPhase) { QVERIFY(dialog && owner); QCOMPARE(Top(dialog.data())->rowCount(), 5); armed = true; ActivateRow(dialog.data(), 0); }
    QVERIFY(!dialog && !owner); QCOMPARE(destroyed.count(), 1);
    QCOMPARE(queries, phase == QStringLiteral("revision-refresh") ? 0 : 1);
    QCOMPARE(activations, phase == QStringLiteral("activation") ? 1 : 0); QVERIFY(!QApplication::activeModalWidget());
}

void MapSizeDialogTests::FailureTextStaysPlainAndRefreshRecovers() {
    Fixture fixture; bool fail = true; int queries = 0;
    MapSizeDialog dialog([&] { return fixture.context.contextRevision; }, [&](std::string_view revision) {
        ++queries;
        if (fail) return Result<Services::MapSizeReport>::Failure("owned-test", "OWNED_FAILURE", "<b>未知来源</b>");
        return fixture.Query(revision);
    }, {});
    dialog.Focus(); QCOMPARE(queries, 1); QCOMPARE(Top(&dialog)->rowCount(), 0);
    QCOMPARE(MapSizeStatusLabel(&dialog)->textFormat(), Qt::PlainText); QCOMPARE(MapSizeStatusLabel(&dialog)->text(), QStringLiteral("<b>未知来源</b>"));
    fail = false; Refresh(&dialog); QCOMPARE(queries, 2); QCOMPARE(Top(&dialog)->rowCount(), 5);
    QVERIFY(dialog.isVisible() && !MapSizeStatusLabel(&dialog)->text().contains(QStringLiteral("OWNED_FAILURE")));
}

int main(int argc, char** argv) {
    QTemporaryDir settings(QDir::tempPath() + QStringLiteral("/AudastraMapSizeTests-XXXXXX"));
    if (!settings.isValid()) { std::fputs("Cannot create isolated map-size-test settings directory\n", stderr); return 2; }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AudastraTests"));
    QCoreApplication::setApplicationName(QStringLiteral("MapSizeDialogTests"));
    QApplication application(argc, argv);
    InitializeMapSizeProductStyle();
    auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"));
    if (!fusion) { std::fputs("Fusion product base style unavailable\n", stderr); return 2; }
    application.setStyle(fusion); captureFusion = fusion;
    UiKit::ThemeManager theme; captureTheme = &theme;
    theme.ApplyTheme(QStringLiteral("graphite"));
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    if (theme.CurrentThemeId() != QStringLiteral("graphite") || !captureFusion || application.styleSheet().isEmpty()) {
        std::fputs("Graphite product theme/QSS was not applied\n", stderr); return 2;
    }
    MapSizeDialogTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MapSizeDialogTests.moc"
