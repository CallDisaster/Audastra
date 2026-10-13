#include "GlobalSearchDialog.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QResource>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <string>
#include <cstdio>
#include <utility>

// 静态 UiKit 的真实 qrc 名；显式引用保证本测试加载相同 Workbench.qss。
static void InitializeSearchProductStyle() {
    Q_INIT_RESOURCE(war3map_uikit_style);
}

using namespace War3Map;
using QtApp::GlobalSearchDialog;
namespace {
QPointer<UiKit::ThemeManager> captureTheme;
QPointer<QStyle> captureFusion;
// 原创内存 IR 经过正式搜索服务生成页，窗口测试不自己实现排序或游标。
struct SearchFixture {
    Trigger::TriggerDocument document;
    Services::ProjectSearchContext context;
    SearchFixture() {
        for (std::uint32_t index = 0; index < 30; ++index) {
            Trigger::TriggerRecord trigger; trigger.id = 100 + index; trigger.name.text = "Needle";
            document.triggers.push_back(std::move(trigger));
            Trigger::VariableRecord variable; variable.id = 1000 + index;
            variable.name.text = "Needle"; variable.typeName.text = "integer";
            document.variables.push_back(std::move(variable));
        }
        context.revision = "owned-dialog/revision-1";
        context.triggers = &document; context.triggerState = Services::ProjectSearchState::Ready;
    }
    Result<Services::ProjectSearchPage> Query(std::string_view revision, std::string_view query,
        const Services::ProjectSearchOptions& options, std::size_t pageLimit,
        const Services::ProjectSearchCursor* cursor) const {
        return Services::SearchProjectPage(context, revision, query, options, pageLimit, cursor);
    }
};
QLineEdit* Edit(GlobalSearchDialog* dialog) { return dialog->findChild<QLineEdit*>("globalSearchQuery"); }
QTreeWidget* Tree(GlobalSearchDialog* dialog) { return dialog->findChild<QTreeWidget*>("globalSearchResults"); }
QLabel* SearchStatusLabel(GlobalSearchDialog* dialog) { return dialog->findChild<QLabel*>("globalSearchStatus"); }
int ResultRows(const QTreeWidget* tree) {
    int count = 0;
    for (int index = 0; index < tree->topLevelItemCount(); ++index) count += tree->topLevelItem(index)->childCount();
    return count;
}
bool ReturnSignal(QLineEdit* edit) {
    // 生命周期反例会删除 edit。直接信号不再给已销毁对象发送第二个 KeyRelease。
    return QMetaObject::invokeMethod(edit, "returnPressed", Qt::DirectConnection);
}
QString CaptureGroupedFirstPage(GlobalSearchDialog& dialog, const Services::ProjectSearchPage& page,
    const std::string& currentRevision) {
    // 仅 root 显式给出的 offscreen 输出目录允许落图；正常回归无磁盘捕获副作用。
    const auto output = qEnvironmentVariable("AUD_SEARCH_CAPTURE_DIR");
    if (!QDir::isAbsolutePath(output) || !QFileInfo(output).isDir())
        return QStringLiteral("AUD_SEARCH_CAPTURE_DIR must name an existing absolute directory");
    if (QApplication::platformName() != QStringLiteral("offscreen"))
        return QStringLiteral("Search capture requires the root offscreen test lease");
    dialog.ensurePolished();
    if (dialog.layout()) dialog.layout()->activate();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
    const auto pixels = dialog.grab();
    if (pixels.isNull()) return QStringLiteral("Grouped search grab returned no pixels");
    const QDir directory(output);
    const auto imagePath = directory.filePath(QStringLiteral("global-search-grouped-page-1.png"));
    const auto metadataPath = directory.filePath(QStringLiteral("global-search-grouped-page-1.json"));
    if (QFileInfo::exists(imagePath) || QFileInfo::exists(metadataPath))
        return QStringLiteral("Search capture outputs already exist; use a fresh owned directory");
    QJsonArray coverage;
    for (const auto& item : page.coverage) {
        coverage.push_back(QJsonObject{{"category", QString::fromStdString(std::string(Services::ProjectSearchCategoryName(item.category)))},
            {"state", static_cast<int>(item.state)}, {"examined", static_cast<int>(item.examined)},
            {"matches", static_cast<int>(item.matches)}, {"scanTruncated", item.scanTruncated},
            {"resultsTruncated", item.resultsTruncated}});
    }
    const auto sheet = qApp->styleSheet();
    const QJsonObject metadata{{"schema", "audastra.offscreen-search-capture.v1"},
        {"fixture", "authored IR through actual SearchProjectPage"}, {"platform", QApplication::platformName()},
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
        {"reportRevision", QString::fromStdString(page.contextRevision)}, {"query", QString::fromStdString(page.queryUtf8)},
        {"reportScope", QJsonObject{{"complete", page.complete}, {"windowHasMore", page.windowHasMore},
            {"totalWindowRows", static_cast<int>(page.totalWindowRows)}, {"coverage", coverage}}},
        {"reportRows", static_cast<int>(page.rows.size())}, {"visibleRows", ResultRows(Tree(&dialog))},
        {"groups", Tree(&dialog)->topLevelItemCount()}, {"page", 1}, {"status", SearchStatusLabel(&dialog)->text()}};
    QFile image(imagePath);
    if (!image.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !pixels.save(&image, "PNG") || !image.flush())
        return QStringLiteral("Could not create grouped search PNG");
    image.close();
    QFile receipt(metadataPath); const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    if (!receipt.open(QIODevice::WriteOnly | QIODevice::NewOnly) || receipt.write(bytes) != bytes.size() || !receipt.flush())
        return QStringLiteral("Could not create grouped search capture metadata");
    return {};
}
} // namespace

class GlobalSearchDialogTests final : public QObject {
    Q_OBJECT
private slots:
    void GroupedPaginationAndEnterKeepsWindowOpen();
    void ContextChangedSameQueryEnterReruns();
    void QueryContextChangedDiscardsOldPage();
    void RevisionContextChangedPreventsQueryAndActivation();
    void OwnerDeletionDuringCallbacks_data();
    void OwnerDeletionDuringCallbacks();
};

void GlobalSearchDialogTests::GroupedPaginationAndEnterKeepsWindowOpen() {
    SearchFixture fixture; int queries = 0, activations = 0;
    const bool captureRequested = qEnvironmentVariableIsSet("AUD_SEARCH_CAPTURE_DIR");
    std::optional<Services::ProjectSearchPage> displayedPage;
    Services::ProjectSearchRow activated;
    GlobalSearchDialog dialog([&] { return fixture.context.revision; },
        [&](auto revision, auto query, const auto& options, auto pageLimit, const auto* cursor) {
            ++queries; auto result = fixture.Query(revision, query, options, pageLimit, cursor);
            if (captureRequested && result.Succeeded() && result.value) displayedPage = *result.value;
            return result;
        }, [&](const auto& row) { ++activations; activated = row; });
    QSignalSpy accepted(&dialog, &QDialog::accepted), finished(&dialog, &QDialog::finished);
    dialog.FocusQuery("Needle");
    auto* edit = Edit(&dialog); auto* tree = Tree(&dialog);
    auto* previous = dialog.findChild<QPushButton*>("globalSearchPrevious");
    auto* next = dialog.findChild<QPushButton*>("globalSearchNext");
    QVERIFY(edit && tree && previous && next && SearchStatusLabel(&dialog));
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(queries, 1); QCOMPARE(activations, 1); QCOMPARE(ResultRows(tree), 40);
    QCOMPARE(tree->topLevelItemCount(), 2);
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        const auto* group = tree->topLevelItem(index);
        QVERIFY(!(group->flags() & Qt::ItemIsSelectable));
        QVERIFY(!group->data(0, Qt::UserRole).isValid()); QVERIFY(group->isExpanded());
    }
    QVERIFY(activated.sourceId && activated.contextRevision == fixture.context.revision);
    QVERIFY(tree->currentItem() && tree->currentItem()->parent());
    QVERIFY(dialog.isVisible()); QCOMPARE(accepted.count(), 0); QCOMPARE(finished.count(), 0);
    QVERIFY(!previous->isEnabled() && next->isEnabled());
    if (captureRequested) {
        QVERIFY(displayedPage); QCOMPARE(displayedPage->contextRevision, fixture.context.revision);
        const auto error = CaptureGroupedFirstPage(dialog, *displayedPage, fixture.context.revision);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(queries, 1); QCOMPARE(ResultRows(tree), 40);
    }
    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(queries, 2); QCOMPARE(ResultRows(tree), 20); QCOMPARE(tree->topLevelItemCount(), 1);
    QVERIFY(previous->isEnabled() && !next->isEnabled());
    QVERIFY(SearchStatusLabel(&dialog)->text().contains(QStringLiteral("第 2 页")));
    QTest::mouseClick(previous, Qt::LeftButton);
    QCOMPARE(queries, 3); QCOMPARE(ResultRows(tree), 40); QVERIFY(!previous->isEnabled() && next->isEnabled());
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(queries, 3); QCOMPARE(activations, 2); QVERIFY(dialog.isVisible());
    QCOMPARE(accepted.count(), 0); QCOMPARE(finished.count(), 0);
}

void GlobalSearchDialogTests::ContextChangedSameQueryEnterReruns() {
    SearchFixture fixture; int queries = 0, activations = 0; std::string activatedRevision;
    GlobalSearchDialog dialog([&] { return fixture.context.revision; },
        [&](auto revision, auto query, const auto& options, auto pageLimit, const auto* cursor) {
            ++queries; return fixture.Query(revision, query, options, pageLimit, cursor);
        }, [&](const auto& row) { ++activations; activatedRevision = row.contextRevision; });
    dialog.FocusQuery("Needle"); QVERIFY(ReturnSignal(Edit(&dialog)));
    QCOMPARE(queries, 1); QCOMPARE(activations, 1); QCOMPARE(ResultRows(Tree(&dialog)), 40);
    fixture.context.revision = "owned-dialog/revision-2";
    dialog.ContextChanged(); QCOMPARE(ResultRows(Tree(&dialog)), 0);
    QVERIFY(SearchStatusLabel(&dialog)->text().contains(QStringLiteral("内容已变化")));
    dialog.FocusQuery("Needle"); // 相同文本不会发 textChanged，必须仍可通过 Enter 重查。
    QVERIFY(ReturnSignal(Edit(&dialog)));
    QCOMPARE(queries, 2); QCOMPARE(activations, 2); QCOMPARE(ResultRows(Tree(&dialog)), 40);
    QCOMPARE(activatedRevision, fixture.context.revision); QVERIFY(dialog.isVisible());
}

void GlobalSearchDialogTests::QueryContextChangedDiscardsOldPage() {
    SearchFixture fixture; int queries = 0, activations = 0; bool invalidate = true;
    QPointer<GlobalSearchDialog> guarded;
    GlobalSearchDialog dialog([&] { return fixture.context.revision; },
        [&](auto revision, auto query, const auto& options, auto pageLimit, const auto* cursor) {
            ++queries; auto page = fixture.Query(revision, query, options, pageLimit, cursor);
            if (invalidate) { invalidate = false; guarded->ContextChanged(); }
            return page;
        }, [&](const auto&) { ++activations; });
    guarded = &dialog; dialog.FocusQuery("Needle"); QVERIFY(ReturnSignal(Edit(&dialog)));
    QCOMPARE(queries, 1); QCOMPARE(activations, 0); QCOMPARE(ResultRows(Tree(&dialog)), 0);
    QVERIFY(SearchStatusLabel(&dialog)->text().contains(QStringLiteral("内容已变化")));
    QVERIFY(!dialog.findChild<QPushButton*>("globalSearchNext")->isEnabled());
    QVERIFY(ReturnSignal(Edit(&dialog)));
    QCOMPARE(queries, 2); QCOMPARE(activations, 1); QCOMPARE(ResultRows(Tree(&dialog)), 40);
}

void GlobalSearchDialogTests::RevisionContextChangedPreventsQueryAndActivation() {
    SearchFixture fixture; int queries = 0, activations = 0; bool invalidate = true;
    QPointer<GlobalSearchDialog> guarded;
    GlobalSearchDialog dialog([&] {
        if (invalidate) { invalidate = false; guarded->ContextChanged(); }
        return fixture.context.revision;
    }, [&](auto revision, auto query, const auto& options, auto pageLimit, const auto* cursor) {
        ++queries; return fixture.Query(revision, query, options, pageLimit, cursor);
    }, [&](const auto&) { ++activations; });
    guarded = &dialog; dialog.FocusQuery("Needle"); QVERIFY(ReturnSignal(Edit(&dialog)));
    QCOMPARE(queries, 0); QCOMPARE(activations, 0); QCOMPARE(ResultRows(Tree(&dialog)), 0);
    QVERIFY(ReturnSignal(Edit(&dialog)));
    QCOMPARE(queries, 1); QCOMPARE(activations, 1); QCOMPARE(ResultRows(Tree(&dialog)), 40);
    invalidate = true; // 页已加载且 clean，此次 revision 回调发生在 ActivateCurrent。
    QVERIFY(ReturnSignal(Edit(&dialog)));
    QCOMPARE(queries, 1); QCOMPARE(activations, 1); QCOMPARE(ResultRows(Tree(&dialog)), 0);
    QVERIFY(SearchStatusLabel(&dialog)->text().contains(QStringLiteral("内容已变化")));
    QVERIFY(ReturnSignal(Edit(&dialog)));
    QCOMPARE(queries, 2); QCOMPARE(activations, 2); QCOMPARE(ResultRows(Tree(&dialog)), 40);
}

void GlobalSearchDialogTests::OwnerDeletionDuringCallbacks_data() {
    QTest::addColumn<QString>("phase");
    QTest::newRow("revision-refresh") << QStringLiteral("revision-refresh");
    QTest::newRow("query") << QStringLiteral("query");
    QTest::newRow("revision-activation") << QStringLiteral("revision-activation");
    QTest::newRow("activation") << QStringLiteral("activation");
}
void GlobalSearchDialogTests::OwnerDeletionDuringCallbacks() {
    QFETCH(QString, phase);
    SearchFixture fixture; int queries = 0, activations = 0; bool armed = false;
    QPointer<QWidget> owner = new QWidget;
    const auto cleanup = qScopeGuard([&] { if (owner) delete owner.data(); });
    QPointer<GlobalSearchDialog> dialog = new GlobalSearchDialog([&] {
        const auto revision = fixture.context.revision;
        if (armed && phase.startsWith(QStringLiteral("revision"))) delete owner.data();
        return revision;
    }, [&](auto revision, auto query, const auto& options, auto pageLimit, const auto* cursor) {
        ++queries; auto page = fixture.Query(revision, query, options, pageLimit, cursor);
        if (armed && phase == QStringLiteral("query")) delete owner.data();
        return page;
    }, [&](const auto&) {
        ++activations; if (armed && phase == QStringLiteral("activation")) delete owner.data();
    }, owner.data());
    QSignalSpy destroyed(dialog.data(), &QObject::destroyed);
    dialog->FocusQuery("Needle"); QPointer<QLineEdit> edit = Edit(dialog.data()); QVERIFY(edit);
    const bool activationPhase = phase == QStringLiteral("revision-activation") || phase == QStringLiteral("activation");
    if (activationPhase) {
        QVERIFY(ReturnSignal(edit.data())); QCOMPARE(queries, 1); QCOMPARE(activations, 1);
        QVERIFY(dialog && owner && edit); QCOMPARE(ResultRows(Tree(dialog.data())), 40);
    }
    armed = true; QVERIFY(ReturnSignal(edit.data()));
    QVERIFY(!dialog && !owner && !edit); QCOMPARE(destroyed.count(), 1);
    QCOMPARE(queries, phase == QStringLiteral("revision-refresh") ? 0 : 1);
    const int expectedActivations = phase == QStringLiteral("activation") ? 2 : activationPhase ? 1 : 0;
    QCOMPARE(activations, expectedActivations); QVERIFY(!QApplication::activeModalWidget());
}

int main(int argc, char** argv) {
    // 只给本进程设置格式/路径，不实例化作者偏好；任何间接 QSettings 都落自有临时 INI 根。
    QTemporaryDir settings(QDir::tempPath() + QStringLiteral("/AudastraGlobalSearchTests-XXXXXX"));
    if (!settings.isValid()) { std::fputs("Cannot create isolated search-test settings directory\n", stderr); return 2; }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AudastraTests"));
    QCoreApplication::setApplicationName(QStringLiteral("GlobalSearchDialogTests"));
    QApplication application(argc, argv);
    InitializeSearchProductStyle();
    auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"));
    if (!fusion) { std::fputs("Fusion product base style unavailable\n", stderr); return 2; }
    application.setStyle(fusion); captureFusion = fusion;
    UiKit::ThemeManager theme; captureTheme = &theme;
    theme.ApplyTheme(QStringLiteral("graphite"));
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    if (theme.CurrentThemeId() != QStringLiteral("graphite") || !captureFusion || application.styleSheet().isEmpty()) {
        std::fputs("Graphite product theme/QSS was not applied\n", stderr); return 2;
    }
    GlobalSearchDialogTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "GlobalSearchDialogTests.moc"
