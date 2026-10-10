#include "Widgets/CodeView.h"
#include "Widgets/CodeViewHighlight.h"
#include "Widgets/CodeProblemsView.h"
#include "Theme/ThemeManager.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QElapsedTimer>
#include <QDir>
#include <QLabel>
#include <QVBoxLayout>
#include <QFile>
#include <QFontMetricsF>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTimer>
#include <QTreeWidget>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>

using namespace War3Map::UiKit;
namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable ready;
    bool release = false;
    std::atomic<int> active = 0, peak = 0, entered = 0, exited = 0;
    std::atomic<bool> sawStop = false;
};
CodeCheckTask GatedCheck(std::shared_ptr<Gate> gate, QString text, std::uint64_t generation) {
    return [gate, text, generation](std::stop_token stop) {
        const int active = ++gate->active;
        int expected = gate->peak.load(); while (active > expected && !gate->peak.compare_exchange_weak(expected, active)) {}
        ++gate->entered;
        {
            std::unique_lock lock(gate->mutex);
            while (!gate->release) { gate->ready.wait_for(lock, std::chrono::milliseconds(5)); if (stop.stop_requested()) gate->sawStop = true; }
        }
        --gate->active; ++gate->exited;
        CodeCheckResult result; result.generation = generation; result.reason = text;
        result.completed = true; result.syntaxChecked = true; result.cancelled = stop.stop_requested();
        return result;
    };
}
void Release(const std::shared_ptr<Gate>& gate) { { std::lock_guard lock(gate->mutex); gate->release = true; } gate->ready.notify_all(); }
struct GateRelease { std::shared_ptr<Gate> gate; ~GateRelease() { Release(gate); } };
bool ContainsKind(const CodeLexLine& line, CodeTokenKind kind) {
    for (const auto& span : line.spans) if (span.kind == kind) return true;
    return false;
}
}

class CodeViewTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    }
    void ThreeLanguagesCommentsStringsAndBracketIsolation() {
        const auto jass = ScanCode(QStringLiteral("function Demo takes nothing returns nothing\n    call Print(\"endif )\") // endfunction (\n    if true then\n        call Work()\n    endif\nendfunction"), CodeLanguage::Jass);
        QCOMPARE(jass.lines[0].foldEnd, 5); QCOMPARE(jass.lines[2].foldEnd, 4);
        QVERIFY(ContainsKind(jass.lines[1], CodeTokenKind::String)); QVERIFY(ContainsKind(jass.lines[1], CodeTokenKind::Comment));
        QCOMPARE(jass.brackets.size(), 4); QVERIFY(!jass.references.contains(QStringLiteral("endif"))); QVERIFY(jass.references.contains(QStringLiteral("Work")));
        const auto vjass = ScanCode(QStringLiteral("library Demo\n/* struct Fake\nendstruct */\nstruct Real\nmethod Work takes nothing returns nothing\nendmethod\nendstruct\nendlibrary"), CodeLanguage::VJass);
        QCOMPARE(vjass.lines[0].foldEnd, 7); QCOMPARE(vjass.lines[3].foldEnd, 6); QCOMPARE(vjass.lines[4].foldEnd, 5);
        QCOMPARE(vjass.lines[1].foldEnd, -1); QVERIFY(ContainsKind(vjass.lines[2], CodeTokenKind::Comment));
        const auto lua = ScanCode(QStringLiteral("local function Work()\n--[=[ if fake then (\nend ]=]\nlocal s = [==[end )\nfunction fake()]==]\nif true then\nreturn {1, 2}\nend\nend"), CodeLanguage::Lua);
        QCOMPARE(lua.lines[0].foldEnd, 8); QCOMPARE(lua.lines[5].foldEnd, 7);
        QVERIFY(ContainsKind(lua.lines[1], CodeTokenKind::Comment)); QVERIFY(ContainsKind(lua.lines[3], CodeTokenKind::String));
        QCOMPARE(lua.brackets.size(), 4); QVERIFY(!lua.references.contains(QStringLiteral("fake")));
        std::stop_source cancelled; cancelled.request_stop(); QVERIFY(ScanCode(QStringLiteral("function X"), CodeLanguage::Jass, cancelled.get_token()).cancelled);
    }
    void FoldNavigationAndVisibleFormats() {
        CodeView view; view.resize(800, 360); view.show();
        view.SetText(QStringLiteral("function Demo takes nothing returns nothing\n    if true then\n        call Work()\n    endif\nendfunction\n// next"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 4000);
        QVERIFY(view.ToggleFold(2)); QVERIFY(!view.document()->findBlockByNumber(2).isVisible());
        QVERIFY(view.ToggleFold(1)); QVERIFY(!view.document()->findBlockByNumber(1).isVisible());
        QVERIFY(view.ToggleFold(1)); QVERIFY(view.document()->findBlockByNumber(1).isVisible()); QVERIFY(!view.document()->findBlockByNumber(2).isVisible());
        view.GoToLine(3, 9); QCOMPARE(view.textCursor().blockNumber(), 2); QVERIFY(view.document()->findBlockByNumber(2).isVisible());
        QVERIFY(!view.IsFolded(2)); QVERIFY(!view.document()->firstBlock().layout()->formats().isEmpty());
        QVERIFY(view.GutterWidth() > 30); QVERIFY(view.findChild<QWidget*>("codeGutter"));
        view.SetEditable(true); view.GoToLine(1); QTest::keyClicks(&view, "// "); QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 4000);
        QVERIFY(!view.ToggleFold(1)); QVERIFY(view.document()->findBlockByNumber(4).isVisible());
    }
    void EditableIndentCompletionAndLocalUndo() {
        CodeView view; view.SetEditable(true); view.resize(700, 300); view.show(); view.setFocus();
        view.SetText(QStringLiteral("if true then")); view.moveCursor(QTextCursor::End);
        QTest::keyClick(&view, Qt::Key_Return); QTest::keyClicks(&view, "call Demo()");
        QCOMPARE(view.toPlainText(), QStringLiteral("if true then\n    call Demo()"));
        auto cursor = view.textCursor(); cursor.select(QTextCursor::Document); view.setTextCursor(cursor);
        QTest::keyClick(&view, Qt::Key_Tab); QVERIFY(view.toPlainText().startsWith("    if"));
        QTest::keyClick(&view, Qt::Key_Z, Qt::ControlModifier); QVERIFY(view.toPlainText().startsWith("if"));
        view.SetText(QStringLiteral("De")); view.moveCursor(QTextCursor::End);
        view.SetCompletionItems({{"Demo", "function", "Demo takes nothing returns nothing", "main.j", "original declaration", 4, 1}});
        QTest::keyClick(&view, Qt::Key_Space, Qt::ControlModifier);
        auto* completer = view.findChild<QCompleter*>(); QVERIFY(completer); QTRY_VERIFY(completer->popup()->isVisible());
        QVERIFY(completer->popup()->currentIndex().isValid()); QCOMPARE(completer->popup()->currentIndex().data().toString(), QStringLiteral("Demo"));
        QTest::keyClick(completer->popup(), Qt::Key_Return); QTRY_COMPARE(view.toPlainText(), QStringLiteral("Demo"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 4000);
        QSignalSpy jump(&view, &CodeView::JumpRequested); QTest::keyClick(&view, Qt::Key_F12); QCOMPARE(jump.count(), 1); QCOMPARE(jump.at(0).at(1).toString(), QStringLiteral("main.j"));
        QSignalSpy references(&view, &CodeView::ReferencesRequested); QTest::keyClick(&view, Qt::Key_F12, Qt::ShiftModifier); QCOMPARE(references.count(), 1);
        auto* dialog = view.findChild<QWidget*>("codeReferencesDialog"); QVERIFY(dialog); dialog->close();
        view.SetCheckBindingValidator([](const QString&) { return false; });
        QTest::keyClick(&view, Qt::Key_F12); QCOMPARE(jump.count(), 2); QVERIFY(jump.last().at(1).toString().isEmpty());
        QCOMPARE(jump.last().at(2).toInt(), 0);
        view.SetEditable(false); const auto before = view.toPlainText(); QTest::keyClicks(&view, "ignored"); QCOMPARE(view.toPlainText(), before);
    }
    void SemanticColorsFollowVisualStudioCategories() {
        // 语义索引优先；未知标识符按调用与声明位置推断，控制流关键字单独着色。
        QPalette dark; dark.setColor(QPalette::Base, QColor("#151719"));
        CodeSymbolKinds symbols{{QStringLiteral("GetTriggerUnit"), CodeTokenKind::Native}, {QStringLiteral("unit"), CodeTokenKind::Type},
            {QStringLiteral("udg_Hero"), CodeTokenKind::Global}, {QStringLiteral("bj_MAX_PLAYERS"), CodeTokenKind::Constant}};
        const auto colorOf = [&](const QString& text, const QString& word, CodeLanguage language = CodeLanguage::Jass) {
            const auto lexed = ScanCode(text, language);
            const int start = static_cast<int>(text.indexOf(word));
            for (const auto& range : CodeFormats(lexed.lines.front(), text, language, symbols, dark))
                if (range.start == start && range.length == word.size()) return range.format.foreground().color();
            return QColor{};
        };
        const QString header = QStringLiteral("function Act takes unit whichUnit, integer count returns nothing");
        QCOMPARE(colorOf(header, QStringLiteral("function")), CodeTokenColor(CodeTokenKind::Keyword, false));
        QCOMPARE(colorOf(header, QStringLiteral("Act")), CodeTokenColor(CodeTokenKind::Function, false));
        QCOMPARE(colorOf(header, QStringLiteral("unit")), CodeTokenColor(CodeTokenKind::Type, false));
        QCOMPARE(colorOf(header, QStringLiteral("whichUnit")), CodeTokenColor(CodeTokenKind::Parameter, false));
        QCOMPARE(colorOf(header, QStringLiteral("integer")), CodeTokenColor(CodeTokenKind::Type, false));
        QCOMPARE(colorOf(header, QStringLiteral("count")), CodeTokenColor(CodeTokenKind::Parameter, false));
        QCOMPARE(colorOf(header, QStringLiteral("nothing")), CodeTokenColor(CodeTokenKind::Type, false));
        const QString local = QStringLiteral("    local unit array picked");
        QCOMPARE(colorOf(local, QStringLiteral("picked")), CodeTokenColor(CodeTokenKind::Local, false));
        const QString body = QStringLiteral("    if udg_Hero == GetTriggerUnit() and bj_MAX_PLAYERS > 0 then");
        QCOMPARE(colorOf(body, QStringLiteral("if")), CodeTokenColor(CodeTokenKind::ControlKeyword, false));
        QCOMPARE(colorOf(body, QStringLiteral("udg_Hero")), CodeTokenColor(CodeTokenKind::Global, false));
        QCOMPARE(colorOf(body, QStringLiteral("GetTriggerUnit")), CodeTokenColor(CodeTokenKind::Native, false));
        QCOMPARE(colorOf(body, QStringLiteral("bj_MAX_PLAYERS")), CodeTokenColor(CodeTokenKind::Constant, false));
        QCOMPARE(colorOf(body, QStringLiteral("==")), CodeTokenColor(CodeTokenKind::Operator, false));
        QCOMPARE(colorOf(QStringLiteral("    call MyHelper(1)"), QStringLiteral("MyHelper")), CodeTokenColor(CodeTokenKind::Function, false));
        QCOMPARE(colorOf(QStringLiteral("local x = f(1) -- note"), QStringLiteral("x"), CodeLanguage::Lua), CodeTokenColor(CodeTokenKind::Local, false));
        QCOMPARE(CodeSymbolKindFor(QStringLiteral("全局变量"), QStringLiteral("constant integer bj_MAX_PLAYERS")), CodeTokenKind::Constant);
        QCOMPARE(CodeSymbolKindFor(QStringLiteral("原生函数"), {}), CodeTokenKind::Native);
        // 浅色主题下全部分类与纸色底对比度不低于 4.5。
        const auto luminance = [](QColor color) {
            const auto linear = [](qreal component) { return component <= 0.04045 ? component / 12.92 : std::pow((component + 0.055) / 1.055, 2.4); };
            return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
        };
        const double paper = luminance(QColor(233, 237, 235));
        for (int kind = 0; kind <= static_cast<int>(CodeTokenKind::Operator); ++kind) {
            const auto color = CodeTokenColor(static_cast<CodeTokenKind>(kind), true);
            QVERIFY2((paper + 0.05) / (luminance(color) + 0.05) >= 4.5, qPrintable(color.name()));
        }
    }
    void SemanticColorsCaptureIfRequested() {
        // 人工目检：设置 FORGE_CODEVIEW_CAPTURE=<目录> 时按深/浅主题各截一张示例脚本。
        const auto directory = qEnvironmentVariable("FORGE_CODEVIEW_CAPTURE");
        if (directory.isEmpty()) QSKIP("FORGE_CODEVIEW_CAPTURE not set");
        ThemeManager theme;
        struct RestoreGraphite { ThemeManager& theme; ~RestoreGraphite() { theme.ApplyTheme(QStringLiteral("graphite")); } } restore{theme};
        for (const auto* themeId : {"graphite", "paper"}) {
            theme.ApplyTheme(QString::fromLatin1(themeId));
            CodeView view; view.resize(900, 420); view.show();
            view.SetCompletionItems({
                {QStringLiteral("GetTriggerUnit"), QStringLiteral("原生函数"), QStringLiteral("GetTriggerUnit takes nothing returns unit"), QStringLiteral("common.j"), {}, 1, 1},
                {QStringLiteral("KillUnit"), QStringLiteral("原生函数"), QStringLiteral("KillUnit takes unit whichUnit returns nothing"), QStringLiteral("common.j"), {}, 2, 1},
                {QStringLiteral("unit"), QStringLiteral("类型"), QStringLiteral("unit"), QStringLiteral("common.j"), {}, 3, 1},
                {QStringLiteral("player"), QStringLiteral("类型"), QStringLiteral("player"), QStringLiteral("common.j"), {}, 4, 1},
                {QStringLiteral("udg_Hero"), QStringLiteral("全局变量"), QStringLiteral("unit udg_Hero"), QStringLiteral("war3map.j"), {}, 5, 1},
                {QStringLiteral("bj_MAX_PLAYERS"), QStringLiteral("全局变量"), QStringLiteral("constant integer bj_MAX_PLAYERS"), QStringLiteral("Blizzard.j"), {}, 6, 1}});
            view.SetText(QStringLiteral(
                "// 示例：英雄死亡后复活\n"
                "function Trig_Revive_Actions takes unit whichUnit, integer delay returns nothing\n"
                "    local unit u = GetTriggerUnit()\n"
                "    local integer i = 0\n"
                "    local string text = \"复活倒计时\"\n"
                "    loop\n"
                "        exitwhen i >= bj_MAX_PLAYERS\n"
                "        if u == udg_Hero and delay > 0 then\n"
                "            call KillUnit(u)\n"
                "            call MyHelper(i, 3.50)\n"
                "        endif\n"
                "        set i = i + 1\n"
                "    endloop\n"
                "    set u = null\n"
                "endfunction\n"));
            QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 4000);
            QApplication::processEvents();
            QVERIFY(view.grab().save(directory + QStringLiteral("/codeview-%1.png").arg(QString::fromLatin1(themeId))));
        }
    }
    void HoverShowsSignatureLibraryNoteCommentsAndDefinition() {
        CodeView view; view.SetText(QStringLiteral("call CreateUnit(p)"));
        view.SetCompletionItems({{QStringLiteral("CreateUnit"), QStringLiteral("原生函数"), QStringLiteral("CreateUnit takes player id returns unit"),
            QStringLiteral("common.j"), QStringLiteral("Creates a unit."), 12, 1}});
        view.SetSymbolNotes({{QStringLiteral("CreateUnit"), QStringLiteral("创建单位\n<提示>")}, {QStringLiteral("OnlyUi"), QStringLiteral("仅函数库")}});
        const auto hover = view.HoverText(QStringLiteral("CreateUnit"));
        QVERIFY(hover.contains(QStringLiteral("原生函数"))); QVERIFY(hover.contains(QStringLiteral("CreateUnit takes player id returns unit")));
        QVERIFY(hover.contains(QStringLiteral("创建单位<br>&lt;提示&gt;"))); QVERIFY(hover.contains(QStringLiteral("Creates a unit.")));
        QVERIFY(hover.contains(QStringLiteral("common.j 第 12 行")));
        QVERIFY(hover.indexOf(QStringLiteral("创建单位")) < hover.indexOf(QStringLiteral("Creates a unit.")));
        QVERIFY(view.HoverText(QStringLiteral("OnlyUi")).contains(QStringLiteral("仅函数库")));
        QVERIFY(view.HoverText(QStringLiteral("Unknown")).isEmpty());
    }
    void ThemeSwitchRefreshesVisibleTokenFormatsWithoutRescanning() {
        ThemeManager theme;
        struct RestoreGraphite { ThemeManager& theme; ~RestoreGraphite() { theme.ApplyTheme(QStringLiteral("graphite")); } } restore{theme};
        theme.ApplyTheme(QStringLiteral("graphite"));
        CodeView view; view.resize(800, 240); view.show();
        view.SetText(QStringLiteral("call Print(\"text\", 42) // comment"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 4000);
        const auto foreground = [&](CodeTokenKind kind) {
            const auto& spans = view.LexSnapshot().lines.front().spans;
            for (const auto& span : spans) if (span.kind == kind) {
                for (const auto& format : view.document()->firstBlock().layout()->formats())
                    if (format.start == span.start && format.length == span.length) return format.format.foreground().color();
            }
            return QColor{};
        };
        const auto contrast = [](QColor foreground, QColor background) {
            const auto luminance = [](QColor color) {
                const auto linear = [](qreal component) { return component <= 0.04045 ? component / 12.92 : std::pow((component + 0.055) / 1.055, 2.4); };
                return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
            };
            const double a = luminance(foreground), b = luminance(background);
            return (qMax(a, b) + 0.05) / (qMin(a, b) + 0.05);
        };
        const auto graphiteKeyword = foreground(CodeTokenKind::Keyword);
        QCOMPARE(graphiteKeyword, QColor("#569cd6"));
        const auto generation = view.Generation(); const auto submitted = view.WorkerStats().submitted;
        theme.ApplyTheme(QStringLiteral("paper"));
        QTRY_COMPARE(foreground(CodeTokenKind::Keyword), QColor("#0000ff"));
        for (const auto kind : {CodeTokenKind::Keyword, CodeTokenKind::String, CodeTokenKind::Comment, CodeTokenKind::Number}) {
            const auto color = foreground(kind); QVERIFY(color.isValid());
            QVERIFY2(contrast(color, view.palette().color(QPalette::Base)) >= 4.5, qPrintable(color.name()));
        }
        QCOMPARE(view.Generation(), generation); QCOMPARE(view.WorkerStats().submitted, submitted);
        theme.ApplyTheme(QStringLiteral("graphite"));
        QTRY_COMPARE(foreground(CodeTokenKind::Keyword), graphiteKeyword);
        QCOMPARE(view.Generation(), generation); QCOMPARE(view.WorkerStats().submitted, submitted);
    }
    void BoundedLatestPendingCancellationAndDestruction() {
        auto gate = std::make_shared<Gate>();
        const GateRelease releaseFirst{gate};
        CodeView view; view.SetText(QStringLiteral("first"));
        view.SetPrepareCheck([gate](const QString& text, std::uint64_t generation) { return GatedCheck(gate, text, generation); });
        QTRY_COMPARE_WITH_TIMEOUT(gate->entered.load(), 1, 4000);
        view.SetText(QStringLiteral("middle")); view.RequestCheck(); view.SetText(QStringLiteral("latest")); view.RequestCheck();
        QCOMPARE(view.WorkerStats().running, 1); QCOMPARE(view.WorkerStats().pending, 1); QCOMPARE(gate->peak.load(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(gate->sawStop.load(), 1000); Release(gate);
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 4000);
        QCOMPARE(view.CheckResult().reason, QStringLiteral("latest")); QCOMPARE(view.CheckResult().generation, view.Generation());
        QCOMPARE(gate->peak.load(), 1); QCOMPARE(view.WorkerStats().peakRunning, 1); QCOMPARE(view.WorkerStats().peakPending, 1);
        QVERIFY(view.WorkerStats().discarded >= 1); QVERIFY(!view.CheckResult().semanticChecked);
        auto destruction = std::make_shared<Gate>(); const GateRelease releaseDestruction{destruction}; auto* transient = new CodeView;
        transient->SetPrepareCheck([destruction](const QString& text, std::uint64_t generation) { return GatedCheck(destruction, text, generation); });
        QTRY_COMPARE_WITH_TIMEOUT(destruction->entered.load(), 1, 4000);
        QElapsedTimer timer; timer.start(); delete transient; QVERIFY(timer.elapsed() < 100);
        QTRY_VERIFY_WITH_TIMEOUT(destruction->sawStop.load(), 1000); Release(destruction); QTRY_COMPARE_WITH_TIMEOUT(destruction->active.load(), 0, 4000);
    }
    void LoadingLocksEditableAndLastGenerationWins() {
        auto gate = std::make_shared<Gate>(); const GateRelease releaseOnExit{gate};
        CodeView view; view.SetEditable(true); view.show(); view.setFocus();
        view.SetDecoder([gate](const QByteArray& bytes, std::stop_token stop) {
            ++gate->entered;
            std::unique_lock lock(gate->mutex); while (!gate->release) { gate->ready.wait_for(lock, std::chrono::milliseconds(5)); if (stop.stop_requested()) gate->sawStop = true; }
            return CodeDecodedSource{QString::fromUtf8(bytes)};
        });
        view.SetBytesAsync("old text"); QTRY_COMPARE_WITH_TIMEOUT(gate->entered.load(), 1, 4000);
        QVERIFY(view.isReadOnly()); view.SetEditable(true); QVERIFY(view.isReadOnly()); QTest::keyClicks(&view, "ignored"); QVERIFY(view.toPlainText().isEmpty());
        view.SetBytesAsync("latest text"); view.SetLanguage(CodeLanguage::Lua); QCOMPARE(view.WorkerStats().pending, 1);
        Release(gate); QTRY_VERIFY_WITH_TIMEOUT(!view.IsLoading() && !view.IsChecking(), 4000);
        QCOMPARE(view.toPlainText(), QStringLiteral("latest text")); QVERIFY(!view.isReadOnly()); QCOMPARE(view.Language(), CodeLanguage::Lua);
        view.SetBytesAsync("cancel me"); view.SetText("replace synchronously"); QTRY_VERIFY_WITH_TIMEOUT(!view.IsLoading() && !view.IsChecking(), 4000);
        QCOMPARE(view.toPlainText(), QStringLiteral("replace synchronously"));
    }
    void FontChangedDuringLoadUsesCurrentFontAndTabMetrics() {
        auto gate = std::make_shared<Gate>(); const GateRelease releaseOnExit{gate};
        CodeView view; view.SetEditable(true); view.resize(800, 300); view.show();
        auto initialFont = view.font(); initialFont.setPixelSize(12); view.setFont(initialFont);
        view.SetDecoder([gate](const QByteArray& bytes, std::stop_token stop) {
            ++gate->entered;
            std::unique_lock lock(gate->mutex);
            while (!gate->release) {
                gate->ready.wait_for(lock, std::chrono::milliseconds(5));
                if (stop.stop_requested()) gate->sawStop = true;
            }
            return CodeDecodedSource{QString::fromUtf8(bytes)};
        });
        const QByteArray body = "local value = 1\n\treturn value\n";
        view.SetBytesAsync(body); QTRY_COMPARE_WITH_TIMEOUT(gate->entered.load(), 1, 4000);
        const auto generation = view.Generation(); const auto submitted = view.WorkerStats().submitted;
        auto currentFont = view.font(); currentFont.setPixelSize(21); view.setFont(currentFont);
        QVERIFY(view.IsLoading()); QVERIFY(view.isReadOnly());
        QCOMPARE(view.Generation(), generation); QCOMPARE(view.WorkerStats().submitted, submitted);
        Release(gate); QTRY_VERIFY_WITH_TIMEOUT(!view.IsLoading() && !view.IsChecking(), 4000);
        QCOMPARE(view.toPlainText().toUtf8(), body); QVERIFY(!view.isReadOnly());
        QCOMPARE(view.document()->defaultFont(), view.font());
        QCOMPARE(view.document()->defaultFont().pixelSize(), 21);
        const auto expectedTab = QFontMetricsF(view.font()).horizontalAdvance(QLatin1Char(' ')) * 4.0;
        QVERIFY(std::abs(view.tabStopDistance() - expectedTab) < 0.1);
        QCOMPARE(view.Generation(), generation); QCOMPARE(view.WorkerStats().submitted, submitted);
        QCOMPARE(view.WorkerStats().adopted, std::uint64_t{1});
    }
    void DiagnosticsRemainScopedAndExplicitlyIncomplete() {
        CodeView view; CodeProblemsView problems; problems.SetView(&view); view.SetSourceName("main.j"); view.SetText("first\nsecond");
        view.SetPrepareCheck([](const QString&, std::uint64_t generation) {
            return [generation](std::stop_token) {
                CodeCheckResult result; result.generation = generation; result.completed = true; result.syntaxChecked = true;
                result.reason = "No frozen declarations";
                result.diagnostics = {{CodeSeverity::Error, "main.j", "E1", "current error", 2, 2, 1}, {CodeSeverity::Error, "include.j", "E2", "external error", 1, 1, 1}};
                return result;
            };
        });
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 4000);
        QVERIFY(problems.StatusText().contains(QStringLiteral("仅语法"))); QCOMPARE(problems.IssueCount(), 2);
        QCOMPARE(view.extraSelections().size(), 2); // 当前行 + main 源诊断；include 不在当前正文标记。
        view.SetEditable(true); view.insertPlainText("changed"); QVERIFY(!view.CheckResult().completed); QVERIFY(problems.StatusText().contains(QStringLiteral("检查中")));
        QVERIFY(!view.CheckResult().semanticChecked);
    }
    void OldLoadExceptionCannotUnlockOrOverwriteNewLoad() {
        auto old = std::make_shared<Gate>(), latest = std::make_shared<Gate>();
        const GateRelease releaseOld{old}, releaseLatest{latest};
        CodeView view; view.SetEditable(true); view.show(); view.setFocus();
        view.SetDecoder([old, latest](const QByteArray& bytes, std::stop_token stop) {
            const auto gate = bytes == "old" ? old : latest; ++gate->entered;
            std::unique_lock lock(gate->mutex);
            while (!gate->release) { gate->ready.wait_for(lock, std::chrono::milliseconds(5)); if (stop.stop_requested()) gate->sawStop = true; }
            if (bytes == "old") throw std::runtime_error("old decoder failed after cancellation");
            return CodeDecodedSource{QString::fromUtf8(bytes)};
        });
        view.SetBytesAsync("old"); QTRY_COMPARE_WITH_TIMEOUT(old->entered.load(), 1, 4000);
        view.SetBytesAsync("latest"); Release(old); QTRY_COMPARE_WITH_TIMEOUT(latest->entered.load(), 1, 4000);
        QVERIFY(view.IsLoading()); QVERIFY(view.isReadOnly()); view.SetEditable(true); QVERIFY(view.isReadOnly());
        QTest::keyClicks(&view, "must not enter"); QVERIFY(view.toPlainText().isEmpty());
        Release(latest); QTRY_VERIFY_WITH_TIMEOUT(!view.IsLoading() && !view.IsChecking(), 4000);
        QCOMPARE(view.toPlainText(), QStringLiteral("latest")); QVERIFY(!view.isReadOnly()); QVERIFY(view.WorkerStats().discarded >= 1);
    }
    void BindingChangeAndWorkerExceptionNeverPublishSuccess() {
        CodeView view; view.SetSourceName("main.j"); view.SetText("original");
        view.SetCheckBindingValidator([](const QString& binding) { return binding == "current"; });
        view.SetPrepareCheck([](const QString&, std::uint64_t generation) {
            return [generation](std::stop_token) { CodeCheckResult result; result.generation = generation; result.completed = true; result.syntaxChecked = true; result.semanticChecked = true; result.contextComplete = true; result.contextBinding = "old"; return result; };
        });
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 4000); QVERIFY(!view.CheckResult().completed); QVERIFY(view.CheckResult().reason.contains(QStringLiteral("上下文")));
        view.SetPrepareCheck([](const QString&, std::uint64_t) -> CodeCheckTask {
            return [](std::stop_token) -> CodeCheckResult { throw std::runtime_error("original injected worker failure"); };
        });
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 4000); QVERIFY(!view.CheckResult().completed); QVERIFY(view.CheckResult().reason.contains("original injected worker failure"));
        QCOMPARE(view.WorkerStats().running, 0); QCOMPARE(view.toPlainText(), QStringLiteral("original"));
        QVERIFY(ScanCode(QString(32 * 1024 * 1024 + 1, 'x'), CodeLanguage::Jass).budgetExceeded);
    }
    void OriginalThreeLanguageVisualsAndFrozenSourceNavigation() {
        const auto directory = qEnvironmentVariable("WAR3_CODEVIEW_SCREENSHOT_DIR");
        if (directory.isEmpty()) QSKIP("Optional original Qt UI screenshots; not compiler/runtime proof.");
        QWidget page; auto* layout = new QVBoxLayout(&page); auto* label = new QLabel(&page); layout->addWidget(label);
        auto* view = new CodeView(&page); view->setFont(QFont("Consolas", 11)); view->SetEditable(true); layout->addWidget(view, 1);
        auto* problems = new CodeProblemsView(&page); problems->SetView(view); layout->addWidget(problems); page.resize(1120, 730); page.show();
        const QStringList texts{
            QStringLiteral("// 原创界面夹具：展示功能，不执行地图\nfunction Demo takes integer value returns integer\n    if value > 0 then\n        return value + 1\n    endif\n    return 0\nendfunction\n"),
            QStringLiteral("library Demo\n    struct Counter\n        integer value = 0\n        method step takes nothing returns nothing\n            set this.value = this.value + 1\n        endmethod\n    endstruct\nendlibrary\n"),
            QStringLiteral("-- 原创 Lua 5.3 代码；只读语法/词法展示\nlocal function greet(name)\n    local text = \"你好，\" .. name\n    return text\nend\n\nlocal result = greet(\"Audastra\")\n")};
        const QStringList names{"jass", "vjass", "lua"};
        for (int i = 0; i < texts.size(); ++i) {
            label->setText(tr("%1 · 原创 Qt 编辑器展示，未执行代码").arg(names[i]));
            view->SetLanguage(i == 2 ? CodeLanguage::Lua : i == 1 ? CodeLanguage::VJass : CodeLanguage::Jass);
            view->SetSourceName(names[i] + ".source"); view->SetText(texts[i]); QTRY_VERIFY_WITH_TIMEOUT(!view->IsChecking(), 4000);
            QCoreApplication::processEvents(); QVERIFY(page.grab().save(QDir(directory).filePath("c2-" + names[i] + "-original.png")));
        }
        view->SetPrepareCheck([](const QString&, std::uint64_t generation) {
            return [generation](std::stop_token) {
                CodeCheckResult result; result.generation = generation; result.completed = true; result.syntaxChecked = true;
                result.externalSources = {{"frozen.j", "function Frozen takes nothing returns nothing\nendfunction", {}, "function Frozen takes nothing returns nothing\nendfunction"}};
                result.declarations = {{"Frozen", "function", "Frozen takes nothing returns nothing", "frozen.j", "original frozen test source", 1, 1}};
                return result;
            };
        });
        QTRY_VERIFY_WITH_TIMEOUT(!view->IsChecking(), 4000);
        QVERIFY(view->ShowExternalSource("frozen.j", 1, 1)); auto* external = view->findChild<CodeView*>(); QVERIFY(external);
        QTRY_VERIFY_WITH_TIMEOUT(!external->IsLoading() && !external->IsChecking(), 4000); QVERIFY(external->isReadOnly()); QVERIFY(external->toPlainText().contains("Frozen"));
    }
    void LargeEightMiBReadonlyLoadKeepsEventLoopBelow500ms() {
        CodeView view; view.resize(900, 600); view.show();
        QByteArray bytes; bytes.reserve(8 * 1024 * 1024);
        const QByteArray line = "// an original large readonly fixture: strings, comments and function names are display data only.\n";
        while (bytes.size() < 8 * 1024 * 1024) bytes += line;
        bytes.resize(8 * 1024 * 1024);
        QElapsedTimer timer; timer.start(); qint64 last = 0, maxGap = 0;
        QTimer pulse; pulse.setInterval(10); connect(&pulse, &QTimer::timeout, this, [&] { const auto now = timer.elapsed(); maxGap = qMax(maxGap, now - last); last = now; }); pulse.start();
        view.SetBytesAsync(bytes); QTRY_VERIFY_WITH_TIMEOUT(!view.IsLoading() && !view.IsChecking(), 15000);
        QTest::qWait(30); pulse.stop();
        QCOMPARE(view.toPlainText().toUtf8(), bytes); QVERIFY(view.isReadOnly()); QVERIFY(view.blockCount() > 50000);
        QVERIFY2(maxGap < 500, qPrintable(QStringLiteral("actual maximum UI event-loop gap %1 ms").arg(maxGap)));
        const auto output = qEnvironmentVariable("WAR3_CODEVIEW_TIMING_RECEIPT");
        if (!output.isEmpty()) {
            QFile file(output); QVERIFY(file.open(QIODevice::WriteOnly));
            QJsonObject receipt{{"bytes", bytes.size()}, {"blocks", view.blockCount()}, {"elapsedMs", timer.elapsed()}, {"maxUiGapMs", maxGap}, {"maxRunning", view.WorkerStats().peakRunning}, {"maxPending", view.WorkerStats().peakPending}, {"evidence", "original 8MiB readonly Qt fixture; not Warcraft runtime"}};
            file.write(QJsonDocument(receipt).toJson());
        }
    }
    void DenseReadonlyScriptKeepsFullBodyWhenDisplayIndexReachesBudget() {
        CodeView view; view.resize(900, 600); view.show();
        QByteArray bytes;
        const QByteArray line("call Dense(1)\r\n");
        for (int row = 0; row < 150000; ++row) bytes += line;
        view.SetBytesAsync(bytes); QTRY_VERIFY_WITH_TIMEOUT(!view.IsLoading() && !view.IsChecking(), 15000);
        QByteArray expected = bytes; expected.replace("\r\n", "\n");
        QCOMPARE(view.toPlainText().toUtf8(), expected); QVERIFY(view.isReadOnly());
        QVERIFY(view.LexSnapshot().budgetExceeded); QVERIFY(!view.LexSnapshot().referenceIndexComplete);
        QVERIFY(view.CheckResult().reason.contains(QStringLiteral("正文已完整载入")));
        QCOMPARE(view.WorkerStats().peakRunning, 1); QVERIFY(view.WorkerStats().peakPending <= 1);
        view.GoToLine(1, 8); QSignalSpy jumps(&view, &CodeView::JumpRequested); QTest::keyClick(&view, Qt::Key_F12);
        QCOMPARE(jumps.size(), 1); QCOMPARE(jumps.front().at(1).toString(), QString{});
        view.SetPrepareCheck([](const QString&, std::uint64_t generation) {
            return [generation](std::stop_token) {
                CodeCheckResult result; result.generation = generation; result.completed = true; result.syntaxChecked = true;
                result.declarations = {{"Dense", "function", "Dense takes integer value", {}, {}, 3, 1}};
                return result;
            };
        });
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking(), 5000); QVERIFY(view.CheckResult().completed);
        QVERIFY(!view.CheckResult().semanticChecked); QVERIFY(view.LexSnapshot().budgetExceeded);
        jumps.clear(); QTest::keyClick(&view, Qt::Key_F12); QCOMPARE(jumps.size(), 1); QCOMPARE(jumps.front().at(1).toString(), QString{});
    }
    void AsyncDisplayOffsetsUseActualQtLineAndWhitespaceNormalization() {
        CodeView view;
        const QString text = QStringLiteral("// first\r\nfunction Alpha takes nothing returns nothing\r\nendfunction\r\n")
            + QChar(0x00a0) + QChar(0x2028) + QChar(0x2029);
        view.SetBytesAsync(text.toUtf8()); QTRY_VERIFY_WITH_TIMEOUT(!view.IsLoading() && !view.IsChecking(), 5000);
        QCOMPARE(view.toPlainText(), QStringLiteral("// first\nfunction Alpha takes nothing returns nothing\nendfunction\n \n\n"));
        const auto exact = ScanCode(view.toPlainText(), CodeLanguage::Jass);
        QCOMPARE(view.LexSnapshot().references.value("Alpha"), exact.references.value("Alpha"));
        QCOMPARE(view.LexSnapshot().brackets, exact.brackets);
    }
};
QTEST_MAIN(CodeViewTests)
#include "CodeViewTests.moc"
