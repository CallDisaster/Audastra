#include "Fonts/FontLoader.h"
#include "Icons/SvgIcons.h"
#include "Markdown/MarkdownRenderer.h"
#include "Markdown/TexRenderer.h"
#include "Theme/ThemeManager.h"
#include "Widgets/CodeView.h"
#include "Widgets/CommandPalette.h"
#include "Widgets/ChangesPanel.h"
#include "Widgets/PopoverSurface.h"
#include "Widgets/RoundedPopups.h"

#include <QApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QWindow>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QDir>
#include <QImage>
#include <QMenu>
#include <QComboBox>
#include <QAbstractItemView>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QInputMethodEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextList>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextTable>
#include <QTimer>
#include <QtTest>

#include <stdexcept>

using namespace War3Map::UiKit;

namespace {

// 取不透明度最高的像素颜色：线条中心即图标着色结果。
QColor StrongestPixel(const QIcon& icon, QIcon::Mode mode, QIcon::State state) {
    const QImage image = icon.pixmap(QSize(48, 48), 1.0, mode, state).toImage()
        .convertToFormat(QImage::Format_ARGB32);
    QColor best(0, 0, 0, 0);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > best.alpha()) {
                best = pixel;
            }
        }
    }
    return best;
}

bool Near(const QColor& a, const QColor& b) {
    return qAbs(a.red() - b.red()) <= 2 && qAbs(a.green() - b.green()) <= 2 && qAbs(a.blue() - b.blue()) <= 2;
}

// offscreen 平台只认 Qt 自带字体目录；公式排版在正式窗口中使用系统数学字体，这里显式登记同一组系统字体，
// 让离屏截图与测试看到真实字形而不是缺字方框。找不到时不登记，相关检查按缺字体处理。
void RegisterSystemMathFonts() {
    const QDir fonts(QStandardPaths::writableLocation(QStandardPaths::FontsLocation));
    for (const char* name : {"cambria.ttc", "cambriai.ttf", "cambriab.ttf", "cambriaz.ttf", "times.ttf", "timesi.ttf"}) {
        const QString path = fonts.filePath(QString::fromLatin1(name));
        if (QFileInfo::exists(path)) QFontDatabase::addApplicationFont(path);
    }
}

} // namespace

class UiKitTests final : public QObject {
    Q_OBJECT

private slots:
    void styleSheetsResolveEveryToken();
    void unknownThemeFallsBackToDefault();
    void paletteFollowsTokens();
    void svgIconsRecolorByState();
    void unknownIconIsNull();
    void fontsFallBackWithoutBundledFiles();
    void bundledFontsTakePriority();
    void commandPaletteKeyboardAndDisabledRows();
    void commandPaletteProviderBoundsAndFailures();
    void changesPanelGroupsAndCallbacks();
    void changesPanelBoundsAndHostActions();
    void codeViewLoadsLargeScriptsAsync();
    void codeViewRealScriptStallIfProvided();
    void markdownRendersGfmBlocks();
    void markdownCjkStrongAndEscapedHtml();
    void markdownMathOnlyWhenSettled();
    void texRendersCommonFormulas();
    void texMarksUnknownCommands();
    void roundedPopoverHasTransparentCorners_data();
    void roundedPopoverHasTransparentCorners();
    void roundedMenuHasTransparentCorners_data();
    void roundedMenuHasTransparentCorners();
    void comboPopupUsesSameRoundedSurface();
    void comboPopupCreatedBeforePolishStaysTranslucent();
    void themedComboPopupListsRowsBelowWithoutScroller();
    void popoverAnchorsFlipAndClampToScreen();
    void popoverTogglesAndDismissesFromChildKeyboard();
    void markdownTypographyUsesQuietCodeAndEvenRhythm();
};

void UiKitTests::styleSheetsResolveEveryToken() {
    static const QRegularExpression unresolved(QStringLiteral("@[A-Za-z]"));
    QCOMPARE(ThemeManager::ThemeIds(), (QStringList{"graphite", "midnight", "paper"}));
    for (const QString& id : ThemeManager::ThemeIds()) {
        const auto& theme = ThemeManager::Theme(id);
        const QString sheet = ThemeManager::StyleSheetFor(theme);
        QVERIFY2(sheet.size() > 1000, qPrintable(id));
        const auto match = unresolved.match(sheet);
        QVERIFY2(!match.hasMatch(), qPrintable(id + QStringLiteral(": ") + sheet.mid(match.capturedStart(), 40)));
        QVERIFY(sheet.contains(ToCss(theme.colors.bg)));
        QVERIFY(sheet.contains(ToCss(theme.colors.accentLine)));
        // 字体族由应用字体决定，样式表不能再写死族名。
        QVERIFY(!sheet.contains(QStringLiteral("font-family")));
    }
}

void UiKitTests::unknownThemeFallsBackToDefault() {
    QCOMPARE(ThemeManager::Theme(QStringLiteral("no-such-theme")).id, Tokens::kDefaultTheme);
    QCOMPARE(ThemeManager::ThemeLabel(QStringLiteral("paper")), QStringLiteral("浅色"));
    QCOMPARE(ToCss({0x8a, 0xd6, 0xb0, 255}), QStringLiteral("#8ad6b0"));
    QCOMPARE(ToCss({0x8a, 0xd6, 0xb0, 0x12}), QStringLiteral("rgba(138, 214, 176, 18)"));
}

void UiKitTests::paletteFollowsTokens() {
    ThemeManager manager;
    QSignalSpy changed(&manager, &ThemeManager::ThemeChanged);
    manager.ApplyTheme(QStringLiteral("paper"));
    QCOMPARE(manager.CurrentThemeId(), QStringLiteral("paper"));
    QCOMPARE(changed.count(), 1);
    const auto& paper = ThemeManager::Theme(QStringLiteral("paper"));
    QCOMPARE(QApplication::palette().color(QPalette::Window), ToQColor(paper.colors.bg));
    QCOMPARE(QApplication::palette().color(QPalette::Disabled, QPalette::Text), ToQColor(paper.colors.disabled));
    QCOMPARE(CurrentIconPalette().on, ToQColor(paper.colors.accent));
    manager.ApplyTheme(QString());
    QCOMPARE(manager.CurrentThemeId(), QStringLiteral("graphite"));
}

void UiKitTests::svgIconsRecolorByState() {
    SetIconPalette({QColor(200, 10, 10), QColor(10, 200, 10), QColor(10, 10, 200), QColor(200, 200, 10), QColor(90, 90, 90)});
    QVERIFY(HasSvgIcon(QStringLiteral("lucide/square")));
    const QIcon icon = SvgIcon(QStringLiteral("lucide/square"));
    QVERIFY(!icon.isNull());
    QVERIFY(Near(StrongestPixel(icon, QIcon::Normal, QIcon::Off), QColor(200, 10, 10)));
    QVERIFY(Near(StrongestPixel(icon, QIcon::Active, QIcon::Off), QColor(10, 200, 10)));
    QVERIFY(Near(StrongestPixel(icon, QIcon::Selected, QIcon::Off), QColor(10, 10, 200)));
    QVERIFY(Near(StrongestPixel(icon, QIcon::Normal, QIcon::On), QColor(200, 200, 10)));
    QVERIFY(Near(StrongestPixel(icon, QIcon::Disabled, QIcon::Off), QColor(90, 90, 90)));

    // 换配色后旧缓存不能复用。
    SetIconPalette({QColor(1, 120, 250), QColor(), QColor(), QColor(), QColor(90, 90, 90)});
    QVERIFY(Near(StrongestPixel(icon, QIcon::Normal, QIcon::Off), QColor(1, 120, 250)));

    const QIcon fixed = SvgIcon(QStringLiteral("lucide/square"), QColor(240, 180, 20));
    QVERIFY(Near(StrongestPixel(fixed, QIcon::Normal, QIcon::Off), QColor(240, 180, 20)));
    QVERIFY(Near(StrongestPixel(fixed, QIcon::Disabled, QIcon::Off), QColor(90, 90, 90)));

    // 高分屏：按设备像素比渲染，逻辑尺寸不变。
    const QPixmap hiDpi = icon.pixmap(QSize(24, 24), 2.0);
    QCOMPARE(hiDpi.size(), QSize(48, 48));
    QCOMPARE(hiDpi.deviceIndependentSize().toSize(), QSize(24, 24));
}

void UiKitTests::unknownIconIsNull() {
    QVERIFY(!HasSvgIcon(QStringLiteral("lucide/no-such-icon")));
    QVERIFY(SvgIcon(QStringLiteral("lucide/no-such-icon")).isNull());
}

void UiKitTests::fontsFallBackWithoutBundledFiles() {
    const FontSetup setup = LoadFonts(QDir::temp().filePath(QStringLiteral("war3map-no-such-font-dir")));
    // 自带字体目录缺失时，只可能补登记系统里的微软雅黑（离屏平台看不到系统字体目录）。
    for (const QString& file : setup.loadedFiles) {
        QVERIFY2(file.startsWith(QLatin1String("msyh"), Qt::CaseInsensitive), qPrintable(file));
    }
    QVERIFY(setup.failedFiles.isEmpty());
    for (const QString& family : setup.uiFamilies + setup.codeFamilies) {
        QVERIFY(family != QLatin1String("sans-serif") && family != QLatin1String("monospace"));
    }
    const QFont code = CodeFont(setup, 12);
    QCOMPARE(code.pixelSize(), 12);
    QVERIFY(code.fixedPitch());
}

void UiKitTests::bundledFontsTakePriority() {
    const QDir fonts(QStringLiteral(WAR3MAP_TEST_FONT_DIR));
    if (!fonts.exists(QStringLiteral("SourceHanSansSC-VF.ttf"))) {
        QSKIP("bundled fonts not fetched (tools/fetch-fonts.ps1)");
    }
    const FontSetup setup = LoadFonts(fonts.absolutePath());
    QVERIFY(setup.failedFiles.isEmpty());
    QVERIFY(setup.loadedFiles.contains(QStringLiteral("SourceHanSansSC-VF.ttf")));
    QVERIFY2(setup.bundledUi, qPrintable(setup.uiFamilies.join('|')));
    QVERIFY2(setup.bundledCode, qPrintable(setup.codeFamilies.join('|')));
    // 界面默认微软雅黑；本机没有时才轮到自带的思源黑体。
    QVERIFY(setup.uiFamilies.contains(QStringLiteral("Source Han Sans SC VF")));
    const QString expectedFirst = setup.uiFamilies.contains(QStringLiteral("Microsoft YaHei UI"))
        ? QStringLiteral("Microsoft YaHei UI") : QStringLiteral("Source Han Sans SC VF");
    const QFont ui = UiFont(setup, 13, Tokens::Weight::kWeightMedium);
    QCOMPARE(ui.families().front(), expectedFirst);
    QCOMPARE(ui.weight(), QFont::Medium);
}

void UiKitTests::commandPaletteKeyboardAndDisabledRows() {
    QWidget parent;
    QLineEdit origin(&parent);
    parent.show(); origin.setFocus(); QApplication::processEvents();
    CommandPalette palette(&parent);
    QString received, activated;
    bool commandOnly = false;
    int calls = 0;
    palette.SetQueryProvider([&](const QString& query, bool only) {
        received = query; commandOnly = only;
        return std::vector<CommandPalette::Entry>{{"保存", "地图", [&, query] { activated = query; ++calls; }, "save", "Ctrl S", "命令"},
            {"禁用", "只读", [&] { calls += 100; }, "disabled", {}, "命令", false},
            {"触发器", "初始化", [&] { ++calls; }, "trigger:7", {}, "工程内容"}};
    });
    palette.Show(QStringLiteral(">保存"));
    auto* search = palette.findChild<QLineEdit*>(QStringLiteral("commandPaletteSearch"));
    auto* list = palette.findChild<QListView*>(QStringLiteral("commandPaletteResults"));
    QVERIFY(search && list);
    QCOMPARE(received, QStringLiteral("保存")); QVERIFY(commandOnly);
    QCOMPARE(list->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("save"));
    QVERIFY(!list->model()->index(0, 0).flags().testFlag(Qt::ItemIsSelectable));
    QTest::keyClick(search, Qt::Key_Down);
    QCOMPARE(list->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("trigger:7"));
    QTest::keyClick(search, Qt::Key_Up);
    QCOMPARE(list->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("save"));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualRect(list->model()->index(2, 0)).center());
    QCOMPARE(calls, 0); QVERIFY(palette.isVisible());
    QCOMPARE(list->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("save"));
    QTest::keyClick(search, Qt::Key_Return); QCOMPARE(calls, 1); QCOMPARE(activated, QStringLiteral("保存"));
    palette.Show();
    search->setText(QStringLiteral("新查询")); QTest::keyClick(search, Qt::Key_Return);
    QCOMPARE(calls, 2); QCOMPARE(activated, QStringLiteral("新查询")); QVERIFY(!palette.isVisible());
    palette.Show(); QVERIFY(!commandOnly); QVERIFY(received.isEmpty());
    QTest::keyClick(search, Qt::Key_Escape); QVERIFY(!palette.isVisible());
    QTRY_VERIFY(origin.hasFocus());
    // 原来的三字段 Entry/SetEntries/Show 仍可用。
    palette.SetEntries({{"兼容命令", {}, [&] { ++calls; }}});
    palette.Show(); QTest::keyClick(search, Qt::Key_Return); QCOMPARE(calls, 3);
    // Qt 合成 IME 事件只验证组合/提交路径；微软拼音候选窗口仍需真实 Windows 验收。
    palette.SetQueryProvider([&](const QString& query, bool) {
        received = query; return std::vector<CommandPalette::Entry>{{query, {}, [&] { ++calls; }}};
    });
    palette.Show(); QCOMPARE(palette.windowType(), Qt::Tool);
    QApplication::processEvents();
    QTRY_VERIFY(search->hasFocus() && QApplication::activeWindow() == &palette);
    QVERIFY(search->testAttribute(Qt::WA_InputMethodEnabled));
    QInputMethodEvent preedit(QStringLiteral("shengyin"), {}); QApplication::sendEvent(search, &preedit);
    const auto selected = list->currentIndex(); QTest::keyClick(search, Qt::Key_Down);
    QCOMPARE(list->currentIndex(), selected); QTest::keyClick(search, Qt::Key_Return); QCOMPARE(calls, 3);
    QEvent candidateActivation(QEvent::WindowDeactivate); QApplication::sendEvent(&palette, &candidateActivation);
    QApplication::processEvents();
    QVERIFY2(palette.isVisible(), qPrintable(QStringLiteral("active=%1 state=%2 searchFocus=%3")
        .arg(QApplication::activeWindow() ? QApplication::activeWindow()->objectName() : QStringLiteral("<null>"))
        .arg(static_cast<int>(QApplication::applicationState())).arg(search->hasFocus())));
    QInputMethodEvent commit; commit.setCommitString(QStringLiteral("声音")); QApplication::sendEvent(search, &commit);
    QCOMPARE(search->text(), QStringLiteral("声音")); QCOMPARE(received, QStringLiteral("声音"));
    search->clear(); commit.setCommitString(QStringLiteral("暗影")); QApplication::sendEvent(search, &commit);
    QCOMPARE(received, QStringLiteral("暗影"));
    parent.activateWindow(); origin.setFocus(); QTRY_VERIFY(!palette.isVisible()); QTRY_VERIFY(origin.hasFocus());
}

void UiKitTests::commandPaletteProviderBoundsAndFailures() {
    CommandPalette palette;
    auto* search = palette.findChild<QLineEdit*>(QStringLiteral("commandPaletteSearch"));
    auto* list = palette.findChild<QListView*>(QStringLiteral("commandPaletteResults"));
    palette.SetQueryProvider([](const QString&, bool) {
        std::vector<CommandPalette::Entry> rows(102);
        for (std::size_t i = 0; i < rows.size(); ++i) rows[i] = {"命令", {}, [] {}, QString::number(i)};
        return rows;
    });
    QCOMPARE(list->model()->rowCount(), 100);
    QCOMPARE(list->model()->index(99, 0).data(Qt::UserRole).toString(), QStringLiteral("99"));
    QString activated;
    palette.SetQueryProvider([&](const QString& query, bool) {
        if (query == QLatin1String("outer")) search->setText(QStringLiteral("inner"));
        return std::vector<CommandPalette::Entry>{{query, {}, [&, query] { activated = query; }, query}};
    });
    palette.Show(); search->setText(QStringLiteral("outer"));
    QCOMPARE(list->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("inner"));
    QTest::keyClick(search, Qt::Key_Return); QCOMPARE(activated, QStringLiteral("inner"));
    palette.SetQueryProvider([](const QString&, bool) -> std::vector<CommandPalette::Entry> { throw std::runtime_error("provider unavailable"); });
    palette.Show();
    QCOMPARE(list->model()->index(0, 0).data().toString(), QStringLiteral("查询失败"));
    QVERIFY(!list->model()->index(0, 0).flags().testFlag(Qt::ItemIsEnabled));
    QTest::keyClick(search, Qt::Key_Return); QVERIFY(palette.isVisible());
    QTest::keyClick(search, Qt::Key_Escape); QVERIFY(!palette.isVisible());
}

void UiKitTests::changesPanelGroupsAndCallbacks() {
    ChangesPanel panel;
    int revealed = 0, reversed = 0;
    const QString value(800, QChar('x'));
    panel.SetEntries({{"volume", "war3map.w3s", "声音 3", "音量", value, "20", {}, true,
            [&] { ++revealed; }, [&] { ++reversed; panel.SetEntries({}); }},
        {"added", "war3mapUnits.doo", "单位 7", "步兵", {}, "hfoo", "暂不支持单项还原。", false,
            {}, [&] { reversed += 100; }, ChangesPanel::Kind::Added},
        {"removed", "war3mapUnits.doo", "单位 8", "步兵", "hfoo", {}, "暂不支持单项还原。", false,
            {}, {}, ChangesPanel::Kind::Removed}}, {"物编", "触发器"});
    auto* tree = panel.findChild<QTreeView*>(QStringLiteral("changesTree"));
    auto* reveal = panel.findChild<QPushButton*>(QStringLiteral("changesReveal"));
    auto* revert = panel.findChild<QPushButton*>(QStringLiteral("changesRevert"));
    QVERIFY(tree && reveal && revert);
    QCOMPARE(tree->model()->rowCount(), 2);
    const auto member = tree->model()->index(0, 0), record = tree->model()->index(0, 0, member);
    QVERIFY(!member.flags().testFlag(Qt::ItemIsSelectable));
    tree->setCurrentIndex(tree->model()->index(0, 0, record));
    QCOMPARE(panel.SelectedId(), QStringLiteral("volume")); QVERIFY(reveal->isEnabled() && revert->isEnabled());
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("changesBefore"))->toolTip(), value);
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("changesAfter"))->text(), QStringLiteral("20"));
    panel.resize(420, 640); panel.show(); QTest::mouseClick(reveal, Qt::LeftButton); QCOMPARE(revealed, 1);
    const auto added = tree->model()->index(0, 0, tree->model()->index(0, 0, tree->model()->index(1, 0)));
    tree->setCurrentIndex(added); QVERIFY(added.data().toString().startsWith(QStringLiteral("新增")));
    QVERIFY(!revert->isEnabled()); QTest::mouseClick(revert, Qt::LeftButton); QCOMPARE(reversed, 0);
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("changesReason"))->text(), QStringLiteral("暂不支持单项还原。"));
    const auto removed = tree->model()->index(0, 0, tree->model()->index(1, 0, tree->model()->index(1, 0)));
    QVERIFY(removed.data().toString().startsWith(QStringLiteral("删除")));
    tree->setCurrentIndex(tree->model()->index(0, 0, record)); QTest::mouseClick(revert, Qt::LeftButton);
    QCOMPARE(reversed, 1); QVERIFY(panel.SelectedId().isEmpty()); QVERIFY(!revert->isEnabled());
}

void UiKitTests::changesPanelBoundsAndHostActions() {
    ChangesPanel panel;
    std::vector<ChangesPanel::Entry> entries(300);
    for (std::size_t i = 0; i < entries.size(); ++i) entries[i] = {QString::number(i), "war3map.w3r", "区域 1", "边界", "0", "1"};
    panel.SetEntries(std::move(entries), {"脚本未接入"});
    auto* tree = panel.findChild<QTreeView*>(QStringLiteral("changesTree"));
    const auto member = tree->model()->index(0, 0), record = tree->model()->index(0, 0, member);
    QCOMPARE(tree->model()->rowCount(record), 256);
    QVERIFY(!panel.findChild<QLabel*>(QStringLiteral("changesTruncated"))->isHidden());
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("changesUnsupported"))->text().contains(QStringLiteral("脚本未接入")));
    auto* save = panel.findChild<QPushButton*>(QStringLiteral("changesSave"));
    auto* check = panel.findChild<QPushButton*>(QStringLiteral("changesCheckSave"));
    QSignalSpy saved(&panel, &ChangesPanel::SaveRequested), checked(&panel, &ChangesPanel::CheckSaveRequested);
    panel.SetActionsAvailable(false, true, QStringLiteral("存在保存阻断。")); panel.show();
    QTest::mouseClick(save, Qt::LeftButton); QTest::mouseClick(check, Qt::LeftButton);
    QCOMPARE(saved.count(), 0); QCOMPARE(checked.count(), 1);
    panel.SetActionsAvailable(true, true); QTest::mouseClick(save, Qt::LeftButton); QCOMPARE(saved.count(), 1);
    panel.SetPreflightResult(QStringLiteral("检查未通过"), {"原始成员不完整"});
    QCOMPARE(panel.findChild<QTextEdit*>(QStringLiteral("changesPreflightDetails"))->toPlainText(), QStringLiteral("原始成员不完整"));
    panel.SetEntries({}, {"触发器"}, true);
    QVERIFY(panel.SelectedId().isEmpty() && !save->isEnabled() && !check->isEnabled());
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("changesUnsupported"))->text().contains(QStringLiteral("触发器")));
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("changesSummary"))->text().contains(QStringLiteral("未展开")));
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("changesPreflight"))->text(), QStringLiteral("尚未检查保存。"));
}

void UiKitTests::codeViewLoadsLargeScriptsAsync() {
    CodeView view;
    QSignalSpy loaded(&view, &CodeView::TextLoaded);
    // 先发起一次载入，再立即替换：只有最后一次结果生效。
    view.SetBytesAsync(QByteArrayLiteral("stale\n"));
    QByteArray script;
    for (int line = 0; line < 20000; ++line) {
        script += "call DoNothing() // line " + QByteArray::number(line + 1) + "\n";
    }
    // 无声明来源的通用控件默认严格 UTF-8；旧字节的推测显示由产品来源显式注入 decoder。
    script += QStringLiteral("// 中文\n").toUtf8();
    view.SetBytesAsync(script);
    QVERIFY(view.IsLoading());
    QVERIFY(loaded.wait(10000));
    QTest::qWait(50);
    QCOMPARE(loaded.count(), 1);
    QVERIFY(!view.IsLoading());
    QCOMPARE(view.blockCount(), 20002);
    QVERIFY(view.toPlainText().contains(QStringLiteral("中文")));
    QCOMPARE(view.toPlainText(), QString::fromUtf8(script));
    QVERIFY(!view.toPlainText().contains(QStringLiteral("stale")));
    QVERIFY(view.isReadOnly());
    QVERIFY(view.FindNext(QStringLiteral("line 19999")));
    QCOMPARE(view.textCursor().selectedText(), QStringLiteral("line 19999"));
    QVERIFY(!view.FindNext(QStringLiteral("missing-token")));
    view.SetText(QStringLiteral("a\nb"));
    QCOMPARE(view.blockCount(), 2);
    QVERIFY(view.GutterWidth() > 0);
}

void UiKitTests::codeViewRealScriptStallIfProvided() {
    // 可选：WAR3_TEST_SCRIPT 指向真实 war3map.j 时，测量载入期间界面线程的最长停顿。
    const QString path = qEnvironmentVariable("WAR3_TEST_SCRIPT");
    if (path.isEmpty()) {
        QSKIP("WAR3_TEST_SCRIPT not set");
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    CodeView view;
    view.resize(900, 600);
    view.show();
    QSignalSpy loaded(&view, &CodeView::TextLoaded);
    QElapsedTimer clock;
    qint64 last = 0;
    qint64 longestStall = 0;
    QTimer tick;
    tick.setInterval(5);
    connect(&tick, &QTimer::timeout, &view, [&]() {
        const qint64 now = clock.elapsed();
        longestStall = qMax(longestStall, now - last);
        last = now;
    });
    clock.start();
    tick.start();
    view.SetBytesAsync(bytes);
    QVERIFY(loaded.wait(60000));
    const qint64 total = clock.elapsed();
    // 滚到末尾触发末段布局，同样计入停顿。
    view.moveCursor(QTextCursor::End);
    QTest::qWait(100);
    tick.stop();
    qInfo().noquote() << QStringLiteral("real script: bytes=%1 lines=%2 loadMs=%3 longestUiStallMs=%4")
        .arg(bytes.size()).arg(view.blockCount()).arg(total).arg(longestStall);
    QVERIFY2(longestStall < 500, "UI thread stalled for more than 500 ms while loading the script");
}

void UiKitTests::markdownRendersGfmBlocks() {
    const War3Map::UiKit::MarkdownTheme theme;
    const QString source = QStringLiteral(
        "# 标题\n\n| 名称 | 数量 |\n|:--|--:|\n| 步兵 | 12 |\n| `hfoo` | 3 |\n\n"
        "- [x] 完成\n- [ ] 待办\n  1. 嵌套\n\n> 引用 ~~删除~~\n\n```json\n{\"a\": 1}\n```\n\n"
        "见 https://example.com/a_b 与脚注[^1]。\n\n[^1]: 说明");
    const QString html = War3Map::UiKit::MarkdownToHtml(source, theme, false);
    QVERIFY(html.contains(QStringLiteral("<h1")));
    QVERIFY(html.contains(QStringLiteral("<th")));
    QVERIFY(html.contains(QStringLiteral("align=\"right\"")));
    QVERIFY(html.contains(QStringLiteral("<td align=\"left\">步兵</td>")));
    QVERIFY(html.contains(QStringLiteral("forge-check:1")));
    QVERIFY(html.contains(QStringLiteral("forge-check:0")));
    QVERIFY(html.contains(QStringLiteral(">1.</td>")));
    QVERIFY(html.contains(QStringLiteral("<s>删除</s>")));
    QVERIFY(html.contains(QStringLiteral("<pre")));
    QVERIFY(html.contains(QStringLiteral("href=\"https://example.com/a_b\"")));
    QVERIFY(html.contains(QStringLiteral("<sup")));
    QVERIFY(html.contains(QStringLiteral("说明")));
    QTextDocument document;
    War3Map::UiKit::RenderMarkdown(document, source, theme, true);
    const QString plain = document.toPlainText();
    QVERIFY(plain.contains(QStringLiteral("步兵")));
    QVERIFY(plain.contains(QStringLiteral("{\"a\": 1}")));
    QVERIFY(!plain.contains(QStringLiteral("|:--|")));
    QVERIFY(!plain.contains(QStringLiteral("```")));
    // GFM 表格画细线网格；代码块用的布局表不画边框
    int gridTables = 0;
    for (QTextFrame* frame : document.rootFrame()->childFrames()) {
        if (auto* table = qobject_cast<QTextTable*>(frame)) {
            if (table->format().border() > 0) {
                ++gridTables;
                QCOMPARE(table->format().borderBrush().color(), theme.border);
                QVERIFY(table->format().borderCollapse());
                QCOMPARE(table->cellAt(0, 0).format().toTableCellFormat().topBorder(), 1.0);
            }
        }
    }
    QCOMPARE(gridTables, 1);
}

void UiKitTests::markdownCjkStrongAndEscapedHtml() {
    const War3Map::UiKit::MarkdownTheme theme;
    // DSH cjkFriendlyStrong：右侧是中文、左侧是标点时 ** 仍能闭合。
    QVERIFY(War3Map::UiKit::MarkdownToHtml(QStringLiteral("**“单位”**属性"), theme, false)
                .contains(QStringLiteral("<b>“单位”</b>属性")));
    QVERIFY(War3Map::UiKit::MarkdownToHtml(QStringLiteral("这是**粗体**和*斜体*"), theme, false)
                .contains(QStringLiteral("<b>粗体</b>和<i>斜体</i>")));
    QVERIFY(!War3Map::UiKit::MarkdownToHtml(QStringLiteral("snake_case_name"), theme, false).contains(QStringLiteral("<i>")));
    const QString html = War3Map::UiKit::MarkdownToHtml(QStringLiteral("<b>原样</b> <img src=x>"), theme, false);
    QVERIFY(html.contains(QStringLiteral("&lt;b&gt;原样&lt;/b&gt;")));
    QVERIFY(!html.contains(QStringLiteral("<img")));
    QVERIFY(!War3Map::UiKit::MarkdownToHtml(QStringLiteral("[本地](file:///C:/x)"), theme, false).contains(QStringLiteral("href")));
}

void UiKitTests::markdownMathOnlyWhenSettled() {
    const War3Map::UiKit::MarkdownTheme theme;
    QStringList seen;
    const War3Map::UiKit::MathImageFactory factory = [&seen](const QString& tex, bool display) {
        seen << (display ? QStringLiteral("D:") : QStringLiteral("I:")) + tex;
        return QStringLiteral("<img src=\"forge-math:x\"/>");
    };
    const QString source = QStringLiteral("价格 $5 和 $6，公式 $E=mc^2$ 与 \\(a+b\\)\n\n$$\n\\frac{1}{2}\n$$");
    (void)War3Map::UiKit::MarkdownToHtml(source, theme, false, factory);
    QVERIFY(seen.isEmpty());
    (void)War3Map::UiKit::MarkdownToHtml(source, theme, true, factory);
    QCOMPARE(seen, (QStringList{QStringLiteral("I:E=mc^2"), QStringLiteral("I:a+b"), QStringLiteral("D:\\frac{1}{2}")}));
    QTextDocument document;
    War3Map::UiKit::RenderMarkdown(document, QStringLiteral("$x^2$"), theme, true);
    QVERIFY(document.toHtml().contains(QStringLiteral("forge-math:")));
}

void UiKitTests::roundedPopoverHasTransparentCorners_data() {
    QTest::addColumn<qreal>("ratio");
    QTest::addColumn<QString>("themeId");
    for (const QString& theme : {QStringLiteral("graphite"), QStringLiteral("paper")}) {
        for (const qreal ratio : {1.0, 1.5, 2.0}) {
            const QByteArray name = (theme + QStringLiteral("-%1").arg(ratio)).toUtf8();
            QTest::newRow(name.constData()) << ratio << theme;
        }
    }
}

void UiKitTests::roundedPopoverHasTransparentCorners() {
    QFETCH(qreal, ratio);
    QFETCH(QString, themeId);
    PopoverSurface surface;
    surface.setObjectName(QStringLiteral("aNamedContextPopover"));
    surface.setPalette(ThemeManager::PaletteFor(ThemeManager::Theme(themeId)));
    surface.SetReducedMotion(true);
    surface.resize(240, 160);
    surface.ensurePolished();
    QImage image(QSize(240, 160) * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    surface.render(&painter);
    painter.end();
    for (const QPoint point : {QPoint(1, 1), QPoint(image.width() - 2, 1),
             QPoint(1, image.height() - 2), QPoint(image.width() - 2, image.height() - 2)}) {
        QCOMPARE(image.pixelColor(point).alpha(), 0);
    }
    QCOMPARE(image.pixelColor(image.width() / 2, image.height() / 2).alpha(), 255);
    QVERIFY(surface.testAttribute(Qt::WA_TranslucentBackground));
    QVERIFY(surface.windowFlags().testFlag(Qt::NoDropShadowWindowHint));
    QVERIFY(surface.windowFlags().testFlag(Qt::FramelessWindowHint));
}

void UiKitTests::roundedMenuHasTransparentCorners_data() {
    QTest::addColumn<qreal>("ratio");
    for (const qreal ratio : {1.0, 1.5, 2.0}) {
        const QByteArray name = QByteArray::number(ratio);
        QTest::newRow(name.constData()) << ratio;
    }
}

void UiKitTests::roundedMenuHasTransparentCorners() {
    QFETCH(qreal, ratio);
    InstallRoundedPopups(*qApp);
    InstallRoundedPopups(*qApp);
    QCOMPARE(qApp->findChildren<QObject*>(QStringLiteral("forgeRoundedPopupInstaller"),
        Qt::FindDirectChildrenOnly).size(), 1);
    QMenu menu;
    menu.addAction(QStringLiteral("修改触发器"));
    menu.addAction(QStringLiteral("可编辑"));
    menu.setPalette(ThemeManager::PaletteFor(ThemeManager::Theme(QStringLiteral("graphite"))));
    menu.ensurePolished();
    menu.adjustSize();
    QImage image(menu.size() * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    menu.render(&painter);
    painter.end();
    for (const QPoint point : {QPoint(1, 1), QPoint(image.width() - 2, 1),
             QPoint(1, image.height() - 2), QPoint(image.width() - 2, image.height() - 2)}) {
        QCOMPARE(image.pixelColor(point).alpha(), 0);
    }
    QCOMPARE(image.pixelColor(image.width() / 2, image.height() / 2).alpha(), 255);
    QVERIFY(menu.testAttribute(Qt::WA_TranslucentBackground));
    QVERIFY(menu.windowFlags().testFlag(Qt::NoDropShadowWindowHint));
    QVERIFY(menu.actionGeometry(menu.actions().front()).height() >= Tokens::Size::kMenuItemHeight);
}

void UiKitTests::comboPopupUsesSameRoundedSurface() {
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, true);
    InstallRoundedPopups(*qApp);
    // 展开动画会把透明圆角和柔影截成黑底位图，共用弹窗必须关闭。
    QVERIFY(!QApplication::isEffectEnabled(Qt::UI_AnimateCombo));
    QVERIFY(!QApplication::isEffectEnabled(Qt::UI_AnimateMenu) && !QApplication::isEffectEnabled(Qt::UI_FadeMenu));
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* combo = new QComboBox(&host);
    combo->addItems({QStringLiteral("可编辑"), QStringLiteral("编辑并保存"), QStringLiteral("完全访问")});
    layout->addWidget(combo);
    host.resize(360, 120);
    host.show();
    combo->showPopup();
    QWidget* popup = combo->view()->window();
    QVERIFY(popup != host.window());
    QVERIFY(popup->isVisible());
    QVERIFY(popup->testAttribute(Qt::WA_TranslucentBackground));
    QVERIFY(popup->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
    const QImage image = popup->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    if (qEnvironmentVariableIsSet("FORGE_POPUP_DEBUG")) {
        image.save(qEnvironmentVariable("FORGE_POPUP_DEBUG") + QStringLiteral("/combo.png"));
        qInfo() << "POPUP" << popup->size() << popup->palette().color(QPalette::Window);
        for (auto* child : popup->findChildren<QWidget*>())
            qInfo() << child->metaObject()->className() << child->geometry() << child->isVisible() << child->autoFillBackground();
    }
    for (const QPoint point : {QPoint(1, 1), QPoint(image.width() - 2, 1),
             QPoint(1, image.height() - 2), QPoint(image.width() - 2, image.height() - 2)}) {
        QCOMPARE(image.pixelColor(point).alpha(), 0);
    }
    QVERIFY(combo->view()->viewport()->height() > 0);
    combo->hidePopup();
}

void UiKitTests::comboPopupCreatedBeforePolishStaysTranslucent() {
    InstallRoundedPopups(*qApp);
    // 下拉容器可能在 Polish 前就有了原生窗口（QComboBox::showPopup 先 create() 再 show()）；
    // 那时的表面没有透明通道，圆角外显示为黑框。显示前必须带透明通道重建。
    QWidget host; auto* layout = new QVBoxLayout(&host);
    auto* combo = new QComboBox;
    combo->addItems({QStringLiteral("Attack"), QStringLiteral("Attack 2"), QStringLiteral("Attack Slam")});
    QWidget* container = combo->view()->window();
    // 已带无边框/无系统影子标志（Windows 11 样式会预先设置）时，之后再改标志不会触发 Qt 重建原生窗口。
    container->setWindowFlag(Qt::FramelessWindowHint);
    container->setWindowFlag(Qt::NoDropShadowWindowHint);
    (void)container->winId(); // 公开接口强制创建原生窗口
    QVERIFY(container->windowHandle() && !container->windowHandle()->requestedFormat().hasAlpha());
    layout->addWidget(combo); host.resize(360, 120); host.show();
    combo->showPopup();
    QVERIFY(container->isVisible() && container->testAttribute(Qt::WA_TranslucentBackground));
    QVERIFY(container->windowHandle()->handle() && container->windowHandle()->requestedFormat().hasAlpha());
    combo->hidePopup();
    combo->showPopup(); // 再次弹出不重复重建
    QVERIFY(container->windowHandle()->requestedFormat().hasAlpha());
    combo->hidePopup();
}

void UiKitTests::themedComboPopupListsRowsBelowWithoutScroller() {
    // 实际主题样式下：短列表全部可见、没有上下滚动箭头；长列表默认一次显示 20 行。
    ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    InstallRoundedPopups(*qApp);
    const auto visibleRows = [](QComboBox& combo) {
        int rows = 0; const QRect viewport = combo.view()->viewport()->rect();
        for (int row = 0; row < combo.count(); ++row)
            if (viewport.contains(combo.view()->visualRect(combo.model()->index(row, 0)))) ++rows;
        return rows;
    };
    QWidget host; auto* layout = new QVBoxLayout(&host);
    auto* shortCombo = new QComboBox(&host); shortCombo->addItems({QStringLiteral("Stand"), QStringLiteral("Walk"), QStringLiteral("Attack")});
    auto* longCombo = new QComboBox(&host);
    for (int index = 0; index < 40; ++index) longCombo->addItem(QStringLiteral("Sequence %1").arg(index));
    auto* explicitCombo = new QComboBox(&host); explicitCombo->setMaxVisibleItems(5);
    layout->addWidget(shortCombo); layout->addWidget(longCombo); layout->addWidget(explicitCombo);
    host.resize(360, 160); host.show();
    QCOMPARE(longCombo->maxVisibleItems(), 20); QCOMPARE(explicitCombo->maxVisibleItems(), 5);
    shortCombo->setCurrentIndex(1); shortCombo->showPopup();
    QTRY_VERIFY(shortCombo->view()->isVisible());
    QCOMPARE(visibleRows(*shortCombo), 3);
    for (auto* child : shortCombo->view()->window()->findChildren<QWidget*>())
        if (child->inherits("QComboBoxPrivateScroller")) QVERIFY2(!child->isVisible(), "短列表不应出现滚动箭头");
    shortCombo->hidePopup();
    longCombo->showPopup();
    QTRY_VERIFY(longCombo->view()->isVisible());
    QVERIFY2(visibleRows(*longCombo) >= 19, qPrintable(QString::number(visibleRows(*longCombo))));
    longCombo->hidePopup();
}

void UiKitTests::popoverAnchorsFlipAndClampToScreen() {
    const QRect screen(0, 0, 800, 600);
    const QRect bottomAnchor(740, 570, 40, 24);
    const QRect below = PopoverSurface::AnchoredGeometry(bottomAnchor, QSize(240, 180), screen);
    QVERIFY(screen.contains(below));
    QVERIFY(below.bottom() - PopupShadowExtent() < bottomAnchor.top());
    const QRect topAnchor(10, 0, 40, 24);
    const QRect above = PopoverSurface::AnchoredGeometry(topAnchor, QSize(240, 180), screen,
        PopoverSurface::PreferredSide::Above, Qt::AlignRight);
    QVERIFY(screen.contains(above));
    QVERIFY(above.top() + PopupShadowExtent() > topAnchor.bottom());
    QCOMPARE(PopoverSurface::AnchoredGeometry(topAnchor, QSize(1200, 900), screen).size(), screen.size());
}

void UiKitTests::popoverTogglesAndDismissesFromChildKeyboard() {
    QWidget host;
    auto* hostLayout = new QVBoxLayout(&host);
    auto* anchor = new QPushButton(QStringLiteral("上下文"), &host);
    auto* outside = new QPushButton(QStringLiteral("其它操作"), &host);
    hostLayout->addWidget(anchor);
    hostLayout->addWidget(outside);
    host.resize(480, 240);
    host.show();
    PopoverSurface surface(&host);
    surface.SetReducedMotion(true);
    auto* contentLayout = new QVBoxLayout(surface.ContentWidget());
    contentLayout->setContentsMargins(0, 0, 0, 0);
    auto* child = new QPushButton(QStringLiteral("关闭详情"), surface.ContentWidget());
    contentLayout->addWidget(child);
    QSignalSpy dismissed(&surface, &PopoverSurface::dismissed);
    surface.ShowAnchored(anchor);
    QVERIFY(surface.isVisible());
    QCOMPARE(surface.windowOpacity(), 1.0);
    QTest::keyClick(child, Qt::Key_Escape);
    QVERIFY(!surface.isVisible());
    QCOMPARE(dismissed.count(), 1);
    surface.ToggleAnchored(anchor);
    QVERIFY(surface.isVisible());
    surface.ToggleAnchored(anchor);
    QVERIFY(!surface.isVisible());
    QCOMPARE(dismissed.count(), 2);
    surface.ShowAnchored(anchor);
    QTest::mouseClick(outside, Qt::LeftButton);
    QVERIFY(!surface.isVisible());
    QCOMPARE(dismissed.count(), 3);
}

void UiKitTests::markdownTypographyUsesQuietCodeAndEvenRhythm() {
    MarkdownTheme theme;
    theme.textFamily = QStringLiteral("Segoe UI");
    theme.codeFamily = QStringLiteral("Consolas");
    const QString source = QStringLiteral(
        "# 标题 `headerName`\n\n正文 `symbol` 不应该带灰块。\n\n"
        "- 第一项\n  - 内层\n- 第二项\n\n| 字段 | 值 |\n|--|--|\n| 单位 | 12 |\n\n> 引用正文");
    const QString html = MarkdownToHtml(source, theme, false);
    QVERIFY(html.contains(theme.text.name()));
    QVERIFY(html.contains(theme.headingText.name()));
    const QRegularExpression inlineCodes(QStringLiteral("<code[^>]*>[^<]*</code>"));
    auto codes = inlineCodes.globalMatch(html);
    int codeCount = 0;
    while (codes.hasNext()) {
        const QString code = codes.next().captured();
        QVERIFY(!code.contains(QStringLiteral("background")));
        QVERIFY(!code.contains(QStringLiteral("&nbsp;")));
        QVERIFY(code.contains(theme.inlineCodeAccent.name()));
        ++codeCount;
    }
    QCOMPARE(codeCount, 2);
    QVERIFY(html.contains(QStringLiteral("font-size:22px; color:%1;\">headerName").arg(theme.inlineCodeAccent.name())));
    QTextDocument document;
    RenderMarkdown(document, source, theme, false);
    document.setTextWidth(640);
    QCOMPARE(document.indentWidth(), 20.0);
    bool nested = false, paragraph = false, table = false, quote = false;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (block.text().startsWith(QStringLiteral("正文"))) {
            paragraph = true;
            QCOMPARE(block.blockFormat().lineHeight(), 165.0);
            QCOMPARE(block.blockFormat().bottomMargin(), 11.25);
            for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
                const auto fragment = iterator.fragment();
                if (fragment.text() == QStringLiteral("symbol")) {
                    QCOMPARE(fragment.charFormat().background().style(), Qt::NoBrush);
                }
            }
        }
        if (block.text() == QStringLiteral("内层")) {
            // 列表是“标记列 + 内容列”两列表格；嵌套列表位于外层内容单元格里，标记与正文同一行距。
            nested = true;
            QTextCursor at(block);
            QTextTable* inner = at.currentTable();
            QVERIFY(inner);
            QVERIFY(qobject_cast<QTextTable*>(inner->parentFrame()));
            QCOMPARE(block.blockFormat().lineHeight(), 165.0);
            const QTextBlock marker = inner->cellAt(at).row() >= 0
                ? inner->cellAt(inner->cellAt(at).row(), 0).firstCursorPosition().block() : QTextBlock();
            QCOMPARE(marker.text(), QStringLiteral("◦"));
            QCOMPARE(marker.blockFormat().lineHeight(), 165.0);
        }
        QVERIFY(!block.textList());
        if (block.text() == QStringLiteral("12")) {
            table = true;
            QCOMPARE(block.blockFormat().lineHeight(), 165.0);
        }
        if (block.text() == QStringLiteral("引用正文")) {
            quote = true;
            QCOMPARE(block.blockFormat().lineHeight(), 165.0);
        }
    }
    QVERIFY(paragraph && nested && table && quote);
}

void UiKitTests::texRendersCommonFormulas() {
    War3Map::UiKit::TexStyle style;
    for (const QString& tex : {QStringLiteral("\\frac{a}{b}+\\sqrt{x^2+1}"), QStringLiteral("\\sum_{i=1}^{n} i^2"),
             QStringLiteral("\\begin{pmatrix}1&0\\\\0&1\\end{pmatrix}"), QStringLiteral("\\left(\\int_0^1 f(x)\\,dx\\right)"),
             QStringLiteral("f(x)=\\begin{cases}1&x>0\\\\0&\\text{否则}\\end{cases}"), QStringLiteral("\\alpha\\leq\\beta\\cdot\\hat{v}")}) {
        const auto image = War3Map::UiKit::RenderTex(tex, style);
        QVERIFY2(!image.image.isNull(), qPrintable(tex));
        QVERIFY2(image.complete, qPrintable(tex));
        QVERIFY(image.width > 4 && image.height > 4);
        QVERIFY(image.baseline > 0 && image.baseline <= image.height);
    }
    style.display = true;
    const auto inlineSum = War3Map::UiKit::RenderTex(QStringLiteral("\\sum_{i=1}^{n} i"), War3Map::UiKit::TexStyle{});
    const auto displaySum = War3Map::UiKit::RenderTex(QStringLiteral("\\sum_{i=1}^{n} i"), style);
    QVERIFY(displaySum.height > inlineSum.height);
    // 有系统数学字体时检查真实字形宽度：窄字母应窄于宽字母，下标比并排书写更紧凑。
    if (QFontDatabase::families().contains(QStringLiteral("Cambria"))) {
        const auto width = [](const char* tex) { return War3Map::UiKit::RenderTex(QString::fromLatin1(tex), War3Map::UiKit::TexStyle{}).width; };
        QVERIFY(width("i") < width("w"));
        QVERIFY(width("w_i") < width("wi"));
    }
}

void UiKitTests::texMarksUnknownCommands() {
    const auto image = War3Map::UiKit::RenderTex(QStringLiteral("a+\\unknowncommand{b}"), War3Map::UiKit::TexStyle{});
    QVERIFY(!image.image.isNull());
    QVERIFY(!image.complete);
    QVERIFY(!War3Map::UiKit::RenderTex(QStringLiteral("\\frac{a}{"), War3Map::UiKit::TexStyle{}).complete);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    RegisterSystemMathFonts();
    UiKitTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "UiKitTests.moc"
