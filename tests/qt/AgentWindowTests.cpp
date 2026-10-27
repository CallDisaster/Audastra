#include "AgentWindow.h"
#include "AgentController.h"
#include "AgentSessionStore.h"
#include "AgentConversationView.h"
#include "AgentLayoutDiagnostics.h"
#include "AnimatedCounter.h"
#include "ComposerWidgets.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "Widgets/ElidedLabel.h"
#include "Widgets/Motion.h"
#include "Widgets/PopoverSurface.h"

#include <QComboBox>
#include <QAction>
#include <QInputMethodEvent>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QFontInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextTable>
#include <QAbstractTextDocumentLayout>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest>

#include <limits>

using namespace War3Map::Agent;

namespace {

AgentSettings TestSettings() {
    AgentSettings settings;
    settings.aiEnabled = true;
    settings.reducedMotion = true;
    ProviderSettings provider;
    provider.id = QStringLiteral("local-test");
    provider.name = QStringLiteral("测试提供商");
    ModelSettings enabled;
    enabled.id = QStringLiteral("enabled-model");
    enabled.name = QStringLiteral("启用模型");
    ModelSettings disabled;
    disabled.id = QStringLiteral("disabled-model");
    disabled.enabled = false;
    provider.models = {enabled, disabled};
    settings.providers = {provider};
    return settings;
}

QTreeWidgetItem* FindDetail(QTreeWidgetItem* item, const QString& title) {
    if (item == nullptr) return nullptr;
    if (item->text(0) == title) return item;
    for (int i = 0; i < item->childCount(); ++i) {
        if (auto* result = FindDetail(item->child(i), title)) return result;
    }
    return nullptr;
}

QTreeWidgetItem* FindDetail(QTreeWidget* tree, const QString& title) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (auto* item = FindDetail(tree->topLevelItem(i), title)) return item;
    }
    return nullptr;
}

} // namespace

class AgentWindowTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void countersRedirectAndFinishExactly();
    void continuousTargetsDoNotStarveAnimation();
    void reducedMotionStopsActiveAnimation();
    void counterWidthAndIntegerBoundaries();
    void onlyEnabledModelsCanBeSubmitted();
    void chineseCompositionRemainsEditable();
    void enterSendsShiftEnterBreaksCtrlEnterQueues();
    void actualChangesAreSeparateFromOutput();
    void toolLifecycleDetailsAndNavigation();
    void followChangesJumpsToLastLocatableItem();
    void streamedTextIsBatchedAndDoesNotStealScrolling();
    void stoppingIsExplicitAndRunEndIsNotInventedSuccess();
    void presentationSizesShrinkAndKeepComposer();
    void presentationRequestsPreserveConversationAndParent();
    void composerGrowsForWrappedInput();
    void newConversationIsExplicitAndDisabledWhileBusy();
    void restoredHistoryIsVisibleAndNotNewChanges();
    void finishedReplyLeavesNoGapAndDarkBlocks();
    void layoutDiagnosticsThresholdDedupBudgetsAndFailures();
    void conversationLayoutDiagnosticsStayPrivateAndIgnoreHiddenRows();
    void conversationLayoutDiagnosticsCaptureStableTrailingChange();
    void restoredHistoryIsBoundedToRecentMessages();
    void longToolDetailsDoNotCollapseTheToolNameColumn();
    void toolUpdatesPreserveExpandedSelection();
    void mcpTextResultsShowAsStructuredFields();
    void assistantMarkdownKeepsSourceText();
    void unavailableUsageClearsPreviousActualUsage();
    void headerControlsStayVisibleAtNarrowWidths();
    void hiddenHistoryNeverBecomesAssistantText();
    void sessionStorePreservesSeparateConversationsAndMaps();
    void legacyHistoryMigrationIsReadOnlyAndDoesNotResurrectArchive();
    void controllerRestoresSelectedConversationsWithoutNetwork();
    void contextPopoverUsesActualUsageAndKeyboard();
};

void AgentWindowTests::initTestCase() {
    const QString directory = War3Map::UiKit::DefaultFontDirectory();
    // 离屏平台也使用真实部署字体，避免缺字或替代字体改变省略与布局结果。
    qputenv("QT_QPA_FONTDIR", QFile::encodeName(directory));
    const auto fonts = War3Map::UiKit::LoadFonts(directory);
    QVERIFY(fonts.bundledUi);
    QVERIFY(fonts.bundledCode);
    QVERIFY(fonts.failedFiles.isEmpty());
    War3Map::UiKit::ApplyApplicationFont(fonts);
}

void AgentWindowTests::countersRedirectAndFinishExactly() {
    AnimatedCounter counter;
    QSignalSpy values(&counter, &AnimatedCounter::ValueChanged);
    counter.SetTarget(900);
    QTest::qWait(32);
    const qint64 valueAtRedirect = counter.Value();
    counter.SetTarget(12000);
    QCOMPARE(counter.Value(), War3Map::UiKit::AnimationsEnabled() ? valueAtRedirect : qint64(12000));
    QCOMPARE(counter.TargetValue(), qint64(12000));
    QTRY_COMPARE_WITH_TIMEOUT(counter.Value(), qint64(12000), 1000);
    QVERIFY(!counter.IsAnimating());
    QVERIFY(values.count() > 0);
    counter.SetTarget(7);
    counter.Finish();
    QCOMPARE(counter.Value(), qint64(7));
    QVERIFY(!counter.IsAnimating());
}

void AgentWindowTests::reducedMotionStopsActiveAnimation() {
    AnimatedCounter counter;
    counter.SetTarget(80000);
    counter.SetReducedMotion(true);
    QCOMPARE(counter.Value(), qint64(80000));
    QVERIFY(!counter.IsAnimating());
    counter.SetTarget(123456);
    QCOMPARE(counter.Value(), qint64(123456));
    QVERIFY(!counter.IsAnimating());
    QTest::qWait(280);
    QCOMPARE(counter.Value(), qint64(123456));
}

void AgentWindowTests::continuousTargetsDoNotStarveAnimation() {
    AnimatedCounter counter;
    QTimer stream;
    stream.setInterval(5);
    QObject::connect(&stream, &QTimer::timeout, &counter, [&counter] {
        counter.SetTarget(counter.TargetValue() + 100);
    });
    stream.start();
    QTest::qWait(160);
    QVERIFY(counter.Value() > 0);
    stream.stop();
    const qint64 finalTarget = counter.TargetValue();
    QTRY_COMPARE_WITH_TIMEOUT(counter.Value(), finalTarget, 1000);
    QVERIFY(!counter.IsAnimating());
}

void AgentWindowTests::counterWidthAndIntegerBoundaries() {
    AnimatedCounter counter;
    counter.SetReducedMotion(true);
    counter.SetTarget(9);
    const int width = counter.width();
    counter.SetTarget(99999);
    QCOMPARE(counter.width(), width);
    counter.SetTarget(std::numeric_limits<qint64>::max());
    QCOMPARE(counter.Value(), std::numeric_limits<qint64>::max());
    const int expanded = counter.width();
    counter.SetTarget(0);
    QCOMPARE(counter.width(), expanded);
    counter.SetReducedMotion(false);
    counter.SetTarget(std::numeric_limits<qint64>::min());
    QTRY_COMPARE_WITH_TIMEOUT(counter.Value(), std::numeric_limits<qint64>::min(), 1000);
}

void AgentWindowTests::onlyEnabledModelsCanBeSubmitted() {
    AgentWindow window;
    AgentSettings settings = TestSettings();
    ProviderSettings disabledProvider = settings.providers.front();
    disabledProvider.id = QStringLiteral("hidden-provider");
    disabledProvider.enabled = false;
    settings.providers.append(disabledProvider);
    window.SetSettings(settings);
    auto* models = window.findChild<QComboBox*>(QStringLiteral("agentModel"));
    auto* prompt = window.findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"));
    auto* send = window.findChild<QPushButton*>(QStringLiteral("agentSend"));
    auto* context = window.findChild<QLabel*>(QStringLiteral("agentContext"));
    QVERIFY(models && prompt && send && context);
    QCOMPARE(models->count(), 1);
    QVERIFY(context->text().contains(QStringLiteral("256K")));
    QVERIFY(context->text().contains(QStringLiteral("估算")));
    settings.providers.front().models.front().contextOverride = 65536;
    window.SetSettings(settings);
    QVERIFY(context->text().contains(QStringLiteral("64K")));
    QVERIFY(!context->text().contains(QStringLiteral("估算")));
    QSignalSpy submitted(&window, &AgentWindow::PromptSubmitted);
    prompt->setPlainText(QStringLiteral("检查地图的事件登记"));
    QVERIFY(send->isEnabled());
    send->click();
    QCOMPARE(submitted.count(), 1);
    QCOMPARE(submitted.front().at(0).toString(), QStringLiteral("检查地图的事件登记"));
    QCOMPARE(submitted.front().at(1).toString(), QStringLiteral("local-test"));
    QCOMPARE(submitted.front().at(2).toString(), QStringLiteral("enabled-model"));
    QVERIFY(!models->isEnabled());
    window.SetBusy(false);
    settings.aiEnabled = false;
    window.SetSettings(settings);
    prompt->setPlainText(QStringLiteral("不可提交"));
    QVERIFY(!send->isEnabled());
    send->click();
    QCOMPARE(submitted.count(), 1);
}

void AgentWindowTests::chineseCompositionRemainsEditable() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    auto* prompt = window.findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"));
    QVERIFY(prompt);
    QSignalSpy submitted(&window, &AgentWindow::PromptSubmitted);
    QInputMethodEvent composing(QStringLiteral("shijian"), {});
    QCoreApplication::sendEvent(prompt, &composing);
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(submitted.count(), 0);
    QInputMethodEvent committed;
    committed.setCommitString(QStringLiteral("事件分发"));
    QCoreApplication::sendEvent(prompt, &committed);
    QCOMPARE(prompt->toPlainText(), QStringLiteral("事件分发"));
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(submitted.count(), 1);
    QCOMPARE(submitted.front().at(0).toString(), QStringLiteral("事件分发"));
}

void AgentWindowTests::enterSendsShiftEnterBreaksCtrlEnterQueues() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    auto* prompt = window.findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"));
    auto* queue = window.findChild<QLabel*>(QStringLiteral("agentQueueNotice"));
    QVERIFY(prompt && queue);
    QSignalSpy submitted(&window, &AgentWindow::PromptSubmitted);
    prompt->insertPlainText(QStringLiteral("第一行"));
    QTest::keyClick(prompt, Qt::Key_Return, Qt::ShiftModifier);
    prompt->insertPlainText(QStringLiteral("第二行  "));
    QCOMPARE(submitted.count(), 0);
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(submitted.count(), 1);
    QCOMPARE(submitted.at(0).at(0).toString(), QStringLiteral("第一行\n第二行"));
    QVERIFY(prompt->toPlainText().isEmpty());
    // 运行中：Enter 不发送也不清空；Ctrl+Enter 排队并清空输入。
    QVERIFY(prompt->isEnabled());
    prompt->insertPlainText(QStringLiteral("排队一"));
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(submitted.count(), 1);
    QCOMPARE(prompt->toPlainText(), QStringLiteral("排队一"));
    QTest::keyClick(prompt, Qt::Key_Return, Qt::ControlModifier);
    prompt->insertPlainText(QStringLiteral("排队二"));
    QTest::keyClick(prompt, Qt::Key_Enter, Qt::ControlModifier | Qt::KeypadModifier);
    QCOMPARE(submitted.count(), 1);
    QVERIFY(prompt->toPlainText().isEmpty());
    QVERIFY(queue->isVisible() || !window.isVisible());
    QVERIFY(queue->text().contains(QStringLiteral("2 条")));
    // 本轮正常结束：依次发送第一条。
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_end")}, {QStringLiteral("success"), true}});
    QTRY_COMPARE(submitted.count(), 2);
    QCOMPARE(submitted.at(1).at(0).toString(), QStringLiteral("排队一"));
    QVERIFY(queue->text().contains(QStringLiteral("1 条")));
    // 本轮失败：暂停，不自动发送；空输入 Ctrl+Enter 继续下一条。
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_end")}, {QStringLiteral("success"), false},
        {QStringLiteral("error"), QStringLiteral("模型请求失败")}});
    QTest::qWait(20);
    QCOMPARE(submitted.count(), 2);
    QVERIFY(queue->text().contains(QStringLiteral("暂停")));
    QTest::keyClick(prompt, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(submitted.count(), 3);
    QCOMPARE(submitted.at(2).at(0).toString(), QStringLiteral("排队二"));
}

void AgentWindowTests::actualChangesAreSeparateFromOutput() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    auto* added = window.findChild<AnimatedCounter*>(QStringLiteral("agentAddedLines"));
    auto* removed = window.findChild<AnimatedCounter*>(QStringLiteral("agentRemovedLines"));
    auto* nodes = window.findChild<AnimatedCounter*>(QStringLiteral("agentTriggerNodes"));
    auto* files = window.findChild<AnimatedCounter*>(QStringLiteral("agentFiles"));
    auto* output = window.findChild<AnimatedCounter*>(QStringLiteral("agentOutputCharacters"));
    QVERIFY(added && removed && nodes && files && output);
    const QString text = QStringLiteral("正在检查自然语言触发器");
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")}, {QStringLiteral("text"), text}});
    QCOMPARE(output->Value(), qint64(text.size()));
    QCOMPARE(added->Value(), qint64(0));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_end")},
        {QStringLiteral("id"), QStringLiteral("query")}, {QStringLiteral("success"), true},
        {QStringLiteral("details"), QJsonObject{{QStringLiteral("added_lines"), 999}}}});
    QCOMPARE(added->Value(), qint64(0));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("changes")},
        {QStringLiteral("added_lines"), 7}, {QStringLiteral("removed_lines"), 2},
        {QStringLiteral("trigger_nodes"), 3}, {QStringLiteral("files"), 1}});
    QCOMPARE(added->Value(), qint64(7));
    QCOMPARE(removed->Value(), qint64(2));
    QCOMPARE(nodes->Value(), qint64(3));
    QCOMPARE(files->Value(), qint64(1));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("changes")},
        {QStringLiteral("trigger_nodes"), 4}});
    QCOMPARE(nodes->Value(), qint64(7));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("changes")},
        {QStringLiteral("mode"), QStringLiteral("total")}, {QStringLiteral("trigger_nodes"), 5}});
    QCOMPARE(nodes->Value(), qint64(5));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("changes")},
        {QStringLiteral("added_lines"), -100}, {QStringLiteral("files"), 1.5}});
    QCOMPARE(added->Value(), qint64(7));
    QCOMPARE(files->Value(), qint64(1));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("changes")},
        {QStringLiteral("mode"), QStringLiteral("total")}, {QStringLiteral("trigger_nodes"), -1}});
    QCOMPARE(nodes->Value(), qint64(5));
}

void AgentWindowTests::toolLifecycleDetailsAndNavigation() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    auto* conversation = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    QVERIFY(conversation);
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_start")},
        {QStringLiteral("id"), QStringLiteral("edit-1")},
        {QStringLiteral("title"), QStringLiteral("修改动作参数")},
        {QStringLiteral("kind"), QStringLiteral("trigger")}, {QStringLiteral("object_id"), QStringLiteral("123")},
        {QStringLiteral("details"), QJsonObject{{QStringLiteral("arguments"),
            QJsonObject{{QStringLiteral("trigger_id"), QStringLiteral("123")}}}}}});
    QCOMPARE(conversation->ToolCount(), 1);
    auto* row = window.findChild<QWidget*>(QStringLiteral("agentToolRow"));
    QVERIFY(row);
    auto* state = row->findChild<QLabel*>(QStringLiteral("agentToolState"));
    auto* duration = row->findChild<QLabel*>(QStringLiteral("agentToolDuration"));
    auto* toggle = row->findChild<QToolButton*>(QStringLiteral("agentToolToggle"));
    auto* tree = row->findChild<QTreeWidget*>(QStringLiteral("agentToolDetails"));
    auto* details = row->findChild<QPlainTextEdit*>(QStringLiteral("agentInlineDetailPreview"));
    auto* navigate = row->findChild<QPushButton*>(QStringLiteral("agentInlineNavigate"));
    QVERIFY(state && duration && toggle && tree && details && navigate);
    QCOMPARE(state->text(), QStringLiteral("运行中"));
    QVERIFY(!toggle->isChecked());
    QVERIFY(tree->parentWidget()->isHidden());
    toggle->click();
    QVERIFY(toggle->isChecked());
    tree->expandAll();
    auto* argument = FindDetail(tree->topLevelItem(0), QStringLiteral("trigger_id"));
    QVERIFY(argument);
    tree->setCurrentItem(argument);
    QCOMPARE(details->toPlainText(), QStringLiteral("123"));
    QSignalSpy navigation(&window, &AgentWindow::NavigateRequested);
    QVERIFY(navigate->isEnabled());
    navigate->click();
    QCOMPARE(navigation.count(), 1);
    QCOMPARE(navigation.front().at(0).toString(), QStringLiteral("trigger"));
    QCOMPARE(navigation.front().at(1).toString(), QStringLiteral("123"));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_end")},
        {QStringLiteral("id"), QStringLiteral("edit-1")}, {QStringLiteral("success"), false},
        {QStringLiteral("duration_ms"), 17}, {QStringLiteral("text"), QStringLiteral("修订已变化")}});
    QCOMPARE(conversation->ToolCount(), 1);
    QCOMPARE(state->text(), QStringLiteral("失败"));
    QCOMPARE(duration->text(), QStringLiteral("17 ms"));
    QVERIFY(toggle->isChecked());
    QCOMPARE(details->toPlainText(), QStringLiteral("123"));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("changes")},
        {QStringLiteral("trigger_nodes"), 1}, {QStringLiteral("items"), QJsonArray{
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("trigger")},
                {QStringLiteral("object_id"), QStringLiteral("123")},
                {QStringLiteral("title"), QStringLiteral("修改动作参数")},
                {QStringLiteral("before"), QStringLiteral("10")}, {QStringLiteral("after"), QStringLiteral("20")}}}}});
    QCOMPARE(conversation->ChangeCount(), 1);
    auto* changes = window.findChild<QTreeWidget*>(QStringLiteral("agentChangeDetails"));
    QVERIFY(changes);
    QVERIFY(FindDetail(changes, QStringLiteral("修改前")));
    QVERIFY(FindDetail(changes, QStringLiteral("修改后")));
}

void AgentWindowTests::followChangesJumpsToLastLocatableItem() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    auto* follow = window.findChild<QToolButton*>(QStringLiteral("agentFollowChanges"));
    QVERIFY(follow && follow->isCheckable() && !follow->isChecked());
    QSignalSpy toggled(&window, &AgentWindow::FollowChangesChangeRequested);
    QSignalSpy followed(&window, &AgentWindow::FollowRequested);
    QSignalSpy navigated(&window, &AgentWindow::NavigateRequested);
    const auto item = [](const char* kind, const char* id) {
        return QJsonObject{{QStringLiteral("kind"), QString::fromLatin1(kind)}, {QStringLiteral("object_id"), QString::fromLatin1(id)},
            {QStringLiteral("title"), QStringLiteral("改动")}};
    };
    const QJsonObject batch{{QStringLiteral("type"), QStringLiteral("changes")}, {QStringLiteral("trigger_nodes"), 2},
        {QStringLiteral("items"), QJsonArray{item("trigger", "11"), item("variable", "22"), item("file", "")}}};
    // 关着时只列出改动，不跳转。
    window.AppendEvent(batch);
    QCOMPARE(followed.count(), 0);
    // 点开关只发请求，由宿主保存后经 SetSettings 回写。
    follow->click();
    QCOMPARE(toggled.count(), 1);
    QVERIFY(toggled.front().at(0).toBool());
    auto settings = TestSettings();
    settings.followChanges = true;
    window.SetSettings(settings);
    QVERIFY(follow->isChecked());
    // 开着时跳到这批里最后一个能定位的对象（文件没有对象编号，跳过）；不发点击导航。
    window.AppendEvent(batch);
    QCOMPARE(followed.count(), 1);
    QCOMPARE(followed.front().at(0).toString(), QStringLiteral("variable"));
    QCOMPARE(followed.front().at(1).toString(), QStringLiteral("22"));
    QCOMPARE(navigated.count(), 0);
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("changes")}, {QStringLiteral("files"), 1},
        {QStringLiteral("items"), QJsonArray{item("import", "")}}});
    QCOMPARE(followed.count(), 1);
}

void AgentWindowTests::streamedTextIsBatchedAndDoesNotStealScrolling() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    window.resize(1040, 780);
    window.show();
    auto* conversation = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    QVERIFY(conversation);
    QSignalSpy documentChanges(conversation, &AgentConversationView::ContentsChanged);
    const QString fragment = QStringLiteral("检查事件登记\n");
    for (int i = 0; i < 80; ++i) {
        window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")},
            {QStringLiteral("text"), fragment}});
    }
    QTRY_VERIFY_WITH_TIMEOUT(conversation->toPlainText().contains(fragment.repeated(80)), 1000);
    QVERIFY(documentChanges.count() < 12);
    auto* scroll = conversation->verticalScrollBar();
    QTRY_VERIFY_WITH_TIMEOUT(scroll->maximum() > 0, 1000);
    scroll->setValue(0);
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")},
        {QStringLiteral("text"), QStringLiteral("最后一项")}});
    QTRY_VERIFY_WITH_TIMEOUT(conversation->toPlainText().endsWith(QStringLiteral("最后一项")), 1000);
    QCOMPARE(scroll->value(), 0);
    const QString finalText = fragment.repeated(80) + QStringLiteral("最后一项");
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_message")},
        {QStringLiteral("text"), finalText}});
    QCOMPARE(conversation->toPlainText().count(QStringLiteral("最后一项")), 1);
    QCOMPARE(scroll->value(), 0);

    // 用户上翻之后，隐藏/切换呈现位置不能把后续输出当作跟随到底部。
    scroll->setValue(72);
    QCOMPARE(scroll->value(), 72);
    window.hide();
    window.SetPresentationMode(AgentWindow::PresentationMode::Sidebar);
    window.resize(420, 840);
    window.show();
    QCoreApplication::processEvents();
    QCOMPARE(scroll->value(), 72);
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")},
        {QStringLiteral("text"), QStringLiteral("侧栏中的新输出")}});
    QTRY_VERIFY_WITH_TIMEOUT(conversation->toPlainText().endsWith(QStringLiteral("侧栏中的新输出")), 1000);
    QCOMPARE(scroll->value(), 72);

    // 所有内容短暂落入可见区域，range=0 不应抹掉此前的上滚意图。
    scroll->setValue(0);
    window.hide();
    window.SetPresentationMode(AgentWindow::PresentationMode::Page);
    window.resize(1920, 3200);
    window.show();
    QTRY_COMPARE_WITH_TIMEOUT(scroll->maximum(), 0, 1000);
    window.hide();
    window.SetPresentationMode(AgentWindow::PresentationMode::Sidebar);
    window.resize(420, 840);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(scroll->maximum() > 0, 1000);
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")},
        {QStringLiteral("text"), QStringLiteral("再次收窄后的新输出")}});
    QTRY_VERIFY_WITH_TIMEOUT(conversation->toPlainText().endsWith(QStringLiteral("再次收窄后的新输出")), 1000);
    QCOMPARE(scroll->value(), 0);
}

void AgentWindowTests::stoppingIsExplicitAndRunEndIsNotInventedSuccess() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    auto* stop = window.findChild<QPushButton*>(QStringLiteral("agentStop"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("agentStatus"));
    auto* elapsed = window.findChild<QLabel*>(QStringLiteral("agentElapsed"));
    QVERIFY(stop && status && elapsed);
    QSignalSpy stopped(&window, &AgentWindow::StopRequested);
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_start")}});
    stop->click();
    stop->click();
    QCOMPARE(stopped.count(), 1);
    QCOMPARE(status->text(), QStringLiteral("正在停止"));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_end")}, {QStringLiteral("cancelled"), true}});
    QCOMPARE(status->text(), QStringLiteral("已停止"));
    const QString stoppedElapsed = elapsed->text();
    QTest::qWait(140);
    QCOMPARE(elapsed->text(), stoppedElapsed);
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_start")}});
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_end")}});
    QCOMPARE(status->text(), QStringLiteral("本轮结束"));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_start")}});
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_end")}, {QStringLiteral("success"), true}});
    QCOMPARE(status->text(), QStringLiteral("本轮完成"));
}

void AgentWindowTests::presentationSizesShrinkAndKeepComposer() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    window.SetPresentationMode(AgentWindow::PresentationMode::Page);
    window.resize(1920, 1080);
    window.show();
    auto* conversation = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    auto* prompt = window.findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"));
    auto* column = window.findChild<QWidget*>(QStringLiteral("agentReadingColumn"));
    auto* composer = window.findChild<QWidget*>(QStringLiteral("agentComposerAxis"));
    auto* models = window.findChild<QComboBox*>(QStringLiteral("agentModel"));
    auto* send = window.findChild<QPushButton*>(QStringLiteral("agentSend"));
    QVERIFY(conversation && prompt && column && composer && models && send);
    prompt->setPlainText(QStringLiteral("保留切换中的任务草稿"));
    const QList<QPair<AgentWindow::PresentationMode, QSize>> sizes{
        {AgentWindow::PresentationMode::Page, QSize(1920, 1080)},
        {AgentWindow::PresentationMode::Sidebar, QSize(420, 840)},
        {AgentWindow::PresentationMode::Detached, QSize(740, 950)},
        {AgentWindow::PresentationMode::Sidebar, QSize(360, 760)},
        {AgentWindow::PresentationMode::Page, QSize(1280, 900)}};
    for (const auto& [mode, size] : sizes) {
        window.SetPresentationMode(mode);
        window.resize(size);
        QTRY_COMPARE_WITH_TIMEOUT(window.size(), size, 1000);
        QCoreApplication::processEvents();
        QVERIFY(conversation->height() > 120);
        QVERIFY(column->width() > 200);
        QVERIFY(column->width() <= conversation->viewport()->width() - 32);
        QVERIFY(composer->width() <= window.width() - 24);
        QVERIFY(composer->width() >= column->width());
        QVERIFY(models->isVisible());
        QVERIFY(send->isVisible());
        QVERIFY(prompt->isVisible());
        QVERIFY(prompt->isEnabled());
        QCOMPARE(prompt->toPlainText(), QStringLiteral("保留切换中的任务草稿"));
        QCOMPARE(window.minimumWidth(), 360);
        if (mode == AgentWindow::PresentationMode::Page) {
            QVERIFY(column->width() >= 680);
            QVERIFY(column->width() <= 760);
        }
    }
    QCOMPARE(conversation->horizontalScrollBar()->maximum(), 0);
}

void AgentWindowTests::presentationRequestsPreserveConversationAndParent() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    window.show();
    auto* prompt = window.findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"));
    auto* conversation = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    auto* back = window.findChild<QToolButton*>(QStringLiteral("agentBack"));
    auto* placement = window.findChild<QToolButton*>(QStringLiteral("agentPresentation"));
    QVERIFY(prompt && conversation && back && placement && placement->menu());
    prompt->setPlainText(QStringLiteral("模式切换保留草稿"));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_message")},
        {QStringLiteral("text"), QStringLiteral("保留同一个会话与工具结果")}});
    const auto flags = window.windowFlags();
    QWidget* parent = window.parentWidget();
    QSignalSpy requests(&window, &AgentWindow::PresentationModeRequested);
    QSignalSpy backs(&window, &AgentWindow::BackRequested);
    QSignalSpy stops(&window, &AgentWindow::StopRequested);
    for (auto* action : placement->menu()->actions()) action->trigger();
    QCOMPARE(requests.count(), 3);
    QCOMPARE(requests.at(0).at(0).value<AgentWindow::PresentationMode>(), AgentWindow::PresentationMode::Page);
    QCOMPARE(requests.at(1).at(0).value<AgentWindow::PresentationMode>(), AgentWindow::PresentationMode::Sidebar);
    QCOMPARE(requests.at(2).at(0).value<AgentWindow::PresentationMode>(), AgentWindow::PresentationMode::Detached);
    QCOMPARE(window.Mode(), AgentWindow::PresentationMode::Detached);
    window.SetBusy(true);
    for (auto mode : {AgentWindow::PresentationMode::Page, AgentWindow::PresentationMode::Sidebar}) {
        window.SetPresentationMode(mode);
        QVERIFY(!back->isHidden());
        back->click();
        QCOMPARE(window.parentWidget(), parent);
        QCOMPARE(window.windowFlags(), flags);
        QCOMPARE(prompt->toPlainText(), QStringLiteral("模式切换保留草稿"));
        QVERIFY(conversation->toPlainText().contains(QStringLiteral("保留同一个会话与工具结果")));
    }
    QCOMPARE(backs.count(), 2);
    window.SetPresentationMode(AgentWindow::PresentationMode::Detached);
    QVERIFY(back->isHidden());
    window.close();
    QVERIFY(window.isHidden());
    QVERIFY(!window.testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(stops.count(), 0);
    QVERIFY(prompt->isEnabled()); // 运行中输入框保持可用，Ctrl+Enter 排队；Enter 不发送
    QCOMPARE(window.findChild<AgentConversationView*>(QStringLiteral("agentConversation")), conversation);
    window.SetBusy(false);
}

void AgentWindowTests::composerGrowsForWrappedInput() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    window.SetPresentationMode(AgentWindow::PresentationMode::Sidebar);
    window.resize(420, 840);
    window.show();
    QCoreApplication::processEvents();
    auto* prompt = window.findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"));
    auto* conversation = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    QVERIFY(prompt && conversation);
    const int singleHeight = prompt->height();
    prompt->setPlainText(QStringLiteral("保留类型登记与实例生命周期，检查事件上下文，不转换为代码。 ").repeated(8));
    QTRY_VERIFY_WITH_TIMEOUT(prompt->height() > singleHeight, 1000);
    QVERIFY(prompt->height() <= 336);
    QVERIFY(conversation->height() > 120);
    prompt->setPlainText(QStringLiteral("单行任务"));
    QTRY_COMPARE_WITH_TIMEOUT(prompt->height(), singleHeight, 1000);
}

void AgentWindowTests::newConversationIsExplicitAndDisabledWhileBusy() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    window.SetActiveMap(QStringLiteral("测试地图"), QStringLiteral("fixture-map-identity"));
    auto* button = window.findChild<QToolButton*>(QStringLiteral("agentNewConversation"));
    auto* conversation = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    auto* prompt = window.findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"));
    auto* map = window.findChild<QLabel*>(QStringLiteral("agentActiveMap"));
    auto* count = window.findChild<AnimatedCounter*>(QStringLiteral("agentTriggerNodes"));
    QVERIFY(button && conversation && prompt && map && count);
    QSignalSpy requested(&window, &AgentWindow::NewConversationRequested);
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_message")},
        {QStringLiteral("text"), QStringLiteral("旧会话内容")}});
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("changes")},
        {QStringLiteral("trigger_nodes"), 3}});
    window.SetBusy(true);
    QVERIFY(!button->isEnabled());
    button->click();
    QCOMPARE(requested.count(), 0);
    window.SetBusy(false);
    QVERIFY(button->isEnabled());
    button->click();
    QCOMPARE(requested.count(), 1);
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("旧会话内容")));
    prompt->setPlainText(QStringLiteral("保留尚未发送的任务草稿"));
    window.ClearConversation();
    QVERIFY(conversation->toPlainText().isEmpty());
    QCOMPARE(count->Value(), qint64(0));
    QCOMPARE(prompt->toPlainText(), QStringLiteral("保留尚未发送的任务草稿"));
    QCOMPARE(map->property("mapIdentity").toString(), QStringLiteral("fixture-map-identity"));
    QCOMPARE(requested.count(), 1);
}

void AgentWindowTests::restoredHistoryIsVisibleAndNotNewChanges() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")},
        {QStringLiteral("text"), QStringLiteral("此前流式输出")}});
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("changes")},
        {QStringLiteral("trigger_nodes"), 9}, {QStringLiteral("files"), 2}});
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("usage")},
        {QStringLiteral("input_tokens"), 900}, {QStringLiteral("output_tokens"), 100}});
    const QJsonArray messages{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), QStringLiteral("<b>保留原事件上下文</b>")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
            {QStringLiteral("content"), QJsonArray{
                QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), QStringLiteral("先查询类型登记与实例订阅。")}},
                QJsonObject{{QStringLiteral("type"), QStringLiteral("toolCall")},
                    {QStringLiteral("id"), QStringLiteral("history-query-1")},
                    {QStringLiteral("name"), QStringLiteral("query_trigger_graph")},
                    {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("trigger_id"), QStringLiteral("17")}}}}}},
            {QStringLiteral("usage"), QJsonObject{{QStringLiteral("input"), 7000}, {QStringLiteral("output"), 3000}}}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("toolResult")},
            {QStringLiteral("toolCallId"), QStringLiteral("history-query-1")},
            {QStringLiteral("toolName"), QStringLiteral("query_trigger_graph")}, {QStringLiteral("isError"), false},
            {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), QStringLiteral("历史工具结果：类型登记 3 项。")}}}}}
    };
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("history_restored")},
        {QStringLiteral("messages"), messages}});
    auto* conversation = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    auto* notice = window.findChild<QLabel*>(QStringLiteral("agentHistoryNotice"));
    auto* usage = window.findChild<QLabel*>(QStringLiteral("agentUsage"));
    auto* nodes = window.findChild<AnimatedCounter*>(QStringLiteral("agentTriggerNodes"));
    auto* output = window.findChild<AnimatedCounter*>(QStringLiteral("agentOutputCharacters"));
    QVERIFY(conversation && notice && usage && nodes && output);
    QVERIFY(notice->text().contains(QStringLiteral("已恢复本地会话")));
    QVERIFY(!notice->isHidden());
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("<b>保留原事件上下文</b>")));
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("历史工具结果：类型登记 3 项。")));
    QVERIFY(!conversation->toPlainText().contains(QStringLiteral("此前流式输出")));
    auto* messageBody = conversation->findChild<QTextBrowser*>(QStringLiteral("agentMessageBody"));
    QVERIFY(messageBody);
    QVERIFY(messageBody->document()->toHtml().contains(QStringLiteral("&lt;b&gt;")));
    QCOMPARE(nodes->Value(), qint64(0));
    QCOMPARE(output->Value(), qint64(0));
    QCOMPARE(conversation->ChangeCount(), 0);
    QCOMPARE(conversation->ToolCount(), 1);
    auto* historyTool = conversation->findChild<QWidget*>(QStringLiteral("agentToolRow"));
    QVERIFY(historyTool);
    QCOMPARE(historyTool->findChild<QLabel*>(QStringLiteral("agentToolState"))->text(), QStringLiteral("已结束（历史）"));
    QCOMPARE(historyTool->findChild<QLabel*>(QStringLiteral("agentToolDuration"))->text(), QStringLiteral("历史"));
    QVERIFY(historyTool->findChild<QPushButton*>(QStringLiteral("agentInlineNavigate"))->isHidden());
    QCOMPARE(usage->text(), QStringLiteral("上下文用量未提供"));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")},
        {QStringLiteral("text"), QStringLiteral("本轮新输出")}});
    QTRY_VERIFY_WITH_TIMEOUT(conversation->toPlainText().endsWith(QStringLiteral("本轮新输出")), 1000);
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("历史工具结果：类型登记 3 项。")));
    QCOMPARE(output->Value(), qint64(QStringLiteral("本轮新输出").size()));
}

void AgentWindowTests::restoredHistoryIsBoundedToRecentMessages() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    QJsonArray messages;
    for (int i = 0; i < 25; ++i) {
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), QStringLiteral("历史序号%1").arg(i, 2, 10, QLatin1Char('0'))}});
    }
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("history_restored")},
        {QStringLiteral("messages"), messages}});
    auto* conversation = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    auto* notice = window.findChild<QLabel*>(QStringLiteral("agentHistoryNotice"));
    QVERIFY(conversation && notice);
    QCOMPARE(conversation->toPlainText().count(QStringLiteral("历史序号")), 20);
    QVERIFY(!conversation->toPlainText().contains(QStringLiteral("历史序号04")));
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("历史序号05")));
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("历史序号24")));
    QVERIFY(notice->text().contains(QStringLiteral("较早记录保留在本地")));
    window.ClearConversation();
    QVERIFY(notice->isHidden());
}

void AgentWindowTests::longToolDetailsDoNotCollapseTheToolNameColumn() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    window.resize(1024, 840);
    window.show();
    const QString title = QStringLiteral("查询初始化与单位类型生命周期事件分发登记");
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_start")},
        {QStringLiteral("id"), QStringLiteral("long-tool-name")}, {QStringLiteral("title"), title},
        {QStringLiteral("details"), QJsonObject{{QStringLiteral("arguments"), QJsonObject{
            {QStringLiteral("long_parameter"), QStringLiteral("长中文参数文字不能撑大状态列").repeated(20)}}}}}});
    auto* row = window.findChild<QWidget*>(QStringLiteral("agentToolRow"));
    QVERIFY(row);
    auto* label = row->findChild<War3Map::UiKit::ElidedLabel*>(QStringLiteral("agentToolTitle"));
    auto* toggle = row->findChild<QToolButton*>(QStringLiteral("agentToolToggle"));
    auto* details = row->findChild<QTreeWidget*>(QStringLiteral("agentToolDetails"));
    auto* column = window.findChild<QWidget*>(QStringLiteral("agentReadingColumn"));
    QVERIFY(label && toggle && details && column);
    QCoreApplication::processEvents();
    QVERIFY(!toggle->isChecked());
    QCOMPARE(label->text(), title);
    QVERIFY(label->width() >= 140);
    QVERIFY(row->width() <= column->width());
    toggle->click();
    details->expandAll();
    window.SetPresentationMode(AgentWindow::PresentationMode::Sidebar);
    window.resize(420, 950);
    QCoreApplication::processEvents();
    QCOMPARE(window.width(), 420);
    QVERIFY(toggle->isChecked());
    QVERIFY(label->width() >= 120);
    const auto geometryDiagnostic = [label, row, column](const QString& stage) {
        const QFontInfo resolved(label->font());
        return QStringLiteral("%1 text_advance=%2 contents_width=%3 label=%4x%5 row=%6x%7 column=%8x%9 "
            "requested_font=%10 resolved_family=%11 pixel_size=%12 point_size=%13 weight=%14")
            .arg(stage).arg(label->fontMetrics().horizontalAdvance(label->text())).arg(label->contentsRect().width())
            .arg(label->width()).arg(label->height()).arg(row->width()).arg(row->height())
            .arg(column->width()).arg(column->height()).arg(label->font().toString()).arg(resolved.family())
            .arg(resolved.pixelSize()).arg(resolved.pointSizeF()).arg(static_cast<int>(resolved.weight()));
    };
    const QString originalDiagnostic = geometryDiagnostic(QStringLiteral("normal-title-sidebar"));
    qInfo().noquote() << originalDiagnostic;
    // 正常名称能放下时应完整显示，不把“没有省略”误判成产品失败。
    if (label->fontMetrics().horizontalAdvance(title) <= label->contentsRect().width()) {
        QVERIFY2(!label->IsElided(), qPrintable(originalDiagnostic));
    } else {
        QVERIFY2(label->IsElided(), qPrintable(originalDiagnostic));
    }
    QCOMPARE(label->text(), title);
    QVERIFY(label->toolTip().contains(title));

    // 用真实运行事件更新为明确超宽标题，再单独验证省略路径及完整 tooltip。
    const QString overflowingTitle = title + QStringLiteral(" / 单位类型登记、生命周期与回调订阅").repeated(12);
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_update")},
        {QStringLiteral("id"), QStringLiteral("long-tool-name")}, {QStringLiteral("title"), overflowingTitle}});
    QCoreApplication::processEvents();
    const QString overflowingDiagnostic = geometryDiagnostic(QStringLiteral("overflowing-title-sidebar"));
    qInfo().noquote() << overflowingDiagnostic;
    QVERIFY2(label->fontMetrics().horizontalAdvance(overflowingTitle) > label->contentsRect().width(),
        qPrintable(overflowingDiagnostic));
    QVERIFY2(label->IsElided(), qPrintable(overflowingDiagnostic));
    QCOMPARE(label->text(), overflowingTitle);
    QVERIFY(label->toolTip().contains(overflowingTitle));
    QVERIFY(toggle->isChecked());
    QVERIFY(label->width() >= 120);
    QVERIFY(row->width() <= column->width());
    // 字段名列限定宽度，值列占剩余宽度：长参数在值列内省略，不挤压字段名，也不把值列压成一个“{”。
    QVERIFY(details->header()->stretchLastSection());
    QCOMPARE(details->header()->sectionResizeMode(0), QHeaderView::Interactive);
    QVERIFY(details->columnWidth(0) >= 110 && details->columnWidth(0) <= 200);
    QVERIFY(FindDetail(details, QStringLiteral("long_parameter")));
}

void AgentWindowTests::toolUpdatesPreserveExpandedSelection() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_start")},
        {QStringLiteral("id"), QStringLiteral("reading-tool")},
        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("nested"),
            QJsonObject{{QStringLiteral("trigger_id"), QStringLiteral("42")}}}}}});
    auto* row = window.findChild<QWidget*>(QStringLiteral("agentToolRow"));
    QVERIFY(row);
    auto* toggle = row->findChild<QToolButton*>(QStringLiteral("agentToolToggle"));
    auto* tree = row->findChild<QTreeWidget*>(QStringLiteral("agentToolDetails"));
    auto* preview = row->findChild<QPlainTextEdit*>(QStringLiteral("agentInlineDetailPreview"));
    QVERIFY(toggle && tree && preview);
    toggle->click();
    tree->expandAll();
    auto* selection = FindDetail(tree, QStringLiteral("trigger_id"));
    QVERIFY(selection);
    tree->setCurrentItem(selection);
    QCOMPARE(preview->toPlainText(), QStringLiteral("42"));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_update")},
        {QStringLiteral("id"), QStringLiteral("reading-tool")},
        {QStringLiteral("text"), QStringLiteral("正在读取返回值")}});
    QVERIFY(toggle->isChecked());
    QVERIFY(tree->currentItem());
    QCOMPARE(tree->currentItem()->text(0), QStringLiteral("trigger_id"));
    QVERIFY(FindDetail(tree, QStringLiteral("nested"))->isExpanded());
    QCOMPARE(preview->toPlainText(), QStringLiteral("42"));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_end")},
        {QStringLiteral("id"), QStringLiteral("reading-tool")}, {QStringLiteral("success"), true},
        {QStringLiteral("result"), QJsonObject{{QStringLiteral("revision"), QStringLiteral("r2")}}}});
    QVERIFY(toggle->isChecked());
    QCOMPARE(tree->currentItem()->text(0), QStringLiteral("trigger_id"));
    QVERIFY(FindDetail(tree, QStringLiteral("revision")));
}

void AgentWindowTests::mcpTextResultsShowAsStructuredFields() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    // MCP 结果的 JSON 被序列化在 content[].text 中，展示时应展开为字段，而不是一行“{”。
    const QString payload = QStringLiteral("{\"triggers\":[{\"id\":\"T1\",\"name\":\"初始化\"}],\"count\":1}");
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_start")},
        {QStringLiteral("id"), QStringLiteral("mcp-1")}, {QStringLiteral("title"), QStringLiteral("查询触发器")},
        {QStringLiteral("arguments"), QStringLiteral("{\"table\":\"triggers\"}")}});
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_end")},
        {QStringLiteral("id"), QStringLiteral("mcp-1")}, {QStringLiteral("success"), true},
        {QStringLiteral("result"), QJsonObject{{QStringLiteral("content"), QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), payload}}}}}}});
    auto* row = window.findChild<QWidget*>(QStringLiteral("agentToolRow"));
    QVERIFY(row);
    auto* tree = row->findChild<QTreeWidget*>(QStringLiteral("agentToolDetails"));
    auto* preview = row->findChild<QPlainTextEdit*>(QStringLiteral("agentInlineDetailPreview"));
    QVERIFY(tree && preview);
    QVERIFY(FindDetail(tree, QStringLiteral("table")));
    auto* count = FindDetail(tree, QStringLiteral("count"));
    QVERIFY(count);
    QCOMPARE(count->text(1), QStringLiteral("1"));
    QVERIFY(FindDetail(tree, QStringLiteral("triggers")));
    QVERIFY(!FindDetail(tree, QStringLiteral("content")));
    // 未手动选择时默认预览结果，内容为格式化 JSON 而非转义字符串。
    QVERIFY(tree->currentItem());
    QCOMPARE(tree->currentItem()->text(0), QStringLiteral("结果"));
    QVERIFY(tree->currentItem()->isExpanded());
    QVERIFY(preview->toPlainText().contains(QStringLiteral("\"name\": \"初始化\"")));
    QVERIFY(!preview->toPlainText().contains(QStringLiteral("\\\"")));
}

void AgentWindowTests::assistantMarkdownKeepsSourceText() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    auto* conversation = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    QVERIFY(conversation);
    const QString text = QStringLiteral("| 名称 | 数量 |\n|---|---|\n| 步兵 | 3 |\n\n公式 $a^2$");
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_message")}, {QStringLiteral("text"), text}});
    QVERIFY(conversation->toPlainText().endsWith(text));
    const auto bodies = conversation->findChildren<QTextBrowser*>(QStringLiteral("agentMessageBody"));
    QVERIFY(!bodies.isEmpty());
    const QString rendered = bodies.last()->toPlainText();
    QVERIFY(rendered.contains(QStringLiteral("步兵")));
    QVERIFY(!rendered.contains(QStringLiteral("|---|")));
    QVERIFY(bodies.last()->document()->toHtml().contains(QStringLiteral("forge-math:")));
}

void AgentWindowTests::unavailableUsageClearsPreviousActualUsage() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    auto* usage = window.findChild<QLabel*>(QStringLiteral("agentUsage"));
    QVERIFY(usage);
    // 旧事件没有 available 仍按实际数字显示。
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("usage")},
        {QStringLiteral("input_tokens"), 321}, {QStringLiteral("output_tokens"), 45}});
    QVERIFY(usage->text().contains(QStringLiteral("321")));
    QVERIFY(usage->text().contains(QStringLiteral("45")));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("usage")}, {QStringLiteral("available"), false},
        {QStringLiteral("input_tokens"), QJsonValue::Null}, {QStringLiteral("output_tokens"), QJsonValue::Null}});
    QCOMPARE(usage->text(), QStringLiteral("上下文用量暂不可用"));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("usage")}, {QStringLiteral("available"), true},
        {QStringLiteral("input_tokens"), 77}, {QStringLiteral("output_tokens"), 12}});
    QVERIFY(usage->text().contains(QStringLiteral("77")));
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("usage")}, {QStringLiteral("available"), false},
        {QStringLiteral("input_tokens"), 0}, {QStringLiteral("output_tokens"), 0}});
    QCOMPARE(usage->text(), QStringLiteral("上下文用量暂不可用"));
}

void AgentWindowTests::headerControlsStayVisibleAtNarrowWidths() {
    AgentWindow window;
    window.SetSettings(TestSettings());
    window.SetActiveMap(QStringLiteral("很长的地图名称用于确认侧栏里的按钮不被挤出标题栏_中文地图.w3x"), QStringLiteral("map-width-fixture"));
    window.SetPresentationMode(AgentWindow::PresentationMode::Sidebar);
    window.show();
    auto* header = window.findChild<QWidget*>(QStringLiteral("agentConversationHeader"));
    QVERIFY(header);
    for (const int width : {360, 420, 740}) {
        window.resize(width, 820);
        QCoreApplication::processEvents();
        QCOMPARE(window.width(), width);
        for (const auto* name : {"agentConversations", "agentFollowChanges", "agentNewConversation", "agentPresentation", "agentSettings"}) {
            auto* button = window.findChild<QToolButton*>(QString::fromLatin1(name));
            QVERIFY(button);
            QVERIFY2(button->isVisible(), name);
            QVERIFY2(header->rect().contains(button->geometry()), name);
            QVERIFY(button->width() >= 28);
        }
    }
}

void AgentWindowTests::hiddenHistoryNeverBecomesAssistantText() {
    AgentWindow window;
    const QJsonArray messages{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), QStringLiteral("当前地图：原图\nSkill 内容：参考边界 secret-skill-fixture")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("developer")}, {QStringLiteral("content"), QStringLiteral("开发者注入 fixture-developer")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("forge_hidden"), true}, {QStringLiteral("content"), QStringLiteral("压缩摘要 fixture-hidden")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("forge_hidden"), true}, {QStringLiteral("content"), QStringLiteral("上下文注入 fixture-assistant-hidden")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("请检查初始化；当前地图：是我自己的正常消息")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("已读取初始化")}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("toolCall")}, {QStringLiteral("id"), QStringLiteral("history-tool")},
                {QStringLiteral("name"), QStringLiteral("editor_state")}, {QStringLiteral("arguments"), QJsonObject{}}}}}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("toolResult")}, {QStringLiteral("toolCallId"), QStringLiteral("history-tool")},
            {QStringLiteral("toolName"), QStringLiteral("editor_state")}, {QStringLiteral("content"), QJsonArray{
                QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("{\"revision\":7}")}}}}}};
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("history_restored")}, {QStringLiteral("messages"), messages}});
    auto* view = window.findChild<AgentConversationView*>();
    QVERIFY(view);
    const auto text = view->toPlainText();
    QVERIFY(text.contains(QStringLiteral("已读取初始化")));
    QVERIFY(text.contains(QStringLiteral("是我自己的正常消息")));
    QVERIFY(!text.contains(QStringLiteral("secret-skill-fixture")));
    QVERIFY(!text.contains(QStringLiteral("fixture-developer")));
    QVERIFY(!text.contains(QStringLiteral("fixture-hidden")));
    QVERIFY(!text.contains(QStringLiteral("fixture-assistant-hidden")));
    QCOMPARE(view->ToolCount(), 1);
}

void AgentWindowTests::sessionStorePreservesSeparateConversationsAndMaps() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentSessionStore store(directory.path());
    QString error;
    auto first = store.Create(QStringLiteral("map-a"), &error);
    auto second = store.Create(QStringLiteral("map-a"), &error);
    auto other = store.Create(QStringLiteral("map-b"), &error);
    QVERIFY(first && second && other);
    QVERIFY(error.isEmpty());
    QVERIFY(first->id != second->id);
    first->messages = QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("第一段会话")},
        {QStringLiteral("forge_model_id"), QStringLiteral("model-a")}}, QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("第一段回答")}}}},
        {QStringLiteral("forge_model_id"), QStringLiteral("model-b")}}};
    second->messages = QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("第二段会话")}}};
    QVERIFY(store.Save(*first, &error));
    QVERIFY(store.Save(*second, &error));
    QCOMPARE(store.List(QStringLiteral("map-a"), &error).size(), 2);
    QCOMPARE(store.List(QStringLiteral("map-b"), &error).size(), 1);
    const auto restored = store.Read(QStringLiteral("map-a"), first->id, &error);
    QVERIFY(restored);
    QCOMPARE(restored->messages, first->messages);
    QVERIFY(!store.Read(QStringLiteral("map-b"), first->id, &error));
    QVERIFY(!error.isEmpty()); error.clear();
    QVERIFY(store.Rename(QStringLiteral("map-a"), first->id, QStringLiteral("自定标题"), &error));
    QCOMPARE(store.Read(QStringLiteral("map-a"), first->id)->title, QStringLiteral("自定标题"));
    QFile file(store.SessionPath(QStringLiteral("map-a"), first->id));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto validBytes = file.readAll(); file.close();
    first->messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), QJsonArray{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("toolCall")}, {QStringLiteral("id"), QStringLiteral("unpaired")}}}}});
    QVERIFY(!store.Save(*first, &error));
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), validBytes); file.close();
    error.clear();
    QVERIFY(store.Archive(QStringLiteral("map-a"), second->id, &error));
    QCOMPARE(store.List(QStringLiteral("map-a"), &error).size(), 1);
    const QDir archived(QDir(directory.path()).filePath(QStringLiteral("archives/") + AgentSessionStore::MapHash(QStringLiteral("map-a"))));
    QCOMPARE(archived.entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
    QVERIFY(!QFile::exists(store.SessionPath(QStringLiteral("map-a"), second->id)));
}

void AgentWindowTests::legacyHistoryMigrationIsReadOnlyAndDoesNotResurrectArchive() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto sessionsRoot = QDir(directory.path()).filePath(QStringLiteral("sessions"));
    QVERIFY(QDir().mkpath(sessionsRoot));
    const auto binding = AgentSessionStore::LegacyBinding(QStringLiteral("legacy-map"), QStringLiteral("provider"), QStringLiteral("model"));
    const auto path = QDir(sessionsRoot).filePath(binding + QStringLiteral(".json"));
    const QJsonObject legacy{{QStringLiteral("version"), 1}, {QStringLiteral("map_identity"), QStringLiteral("legacy-map")},
        {QStringLiteral("provider_id"), QStringLiteral("provider")}, {QStringLiteral("model_id"), QStringLiteral("model")},
        {QStringLiteral("complete"), true}, {QStringLiteral("messages"), QJsonArray{
            QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("timestamp"), 1},
                {QStringLiteral("content"), QStringLiteral("当前地图：legacy-map\n参考边界 Skill fixture")}},
            QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("旧会话的真实请求")}}}}};
    const auto bytes = QJsonDocument(legacy).toJson(QJsonDocument::Compact);
    QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write(bytes), qint64(bytes.size())); source.close();
    AgentSessionStore store(directory.path());
    QString error;
    QVERIFY(store.MigrateLegacy(QStringLiteral("legacy-map"), &error));
    auto migrated = store.List(QStringLiteral("legacy-map"), &error);
    QCOMPARE(migrated.size(), 1);
    QVERIFY(migrated.front().messages.at(0).toObject().value(QStringLiteral("forge_hidden")).toBool());
    QCOMPARE(migrated.front().messages.at(1).toObject().value(QStringLiteral("forge_model_id")).toString(), QStringLiteral("model"));
    QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), bytes); source.close();
    QVERIFY(store.MigrateLegacy(QStringLiteral("legacy-map"), &error));
    QCOMPARE(store.List(QStringLiteral("legacy-map"), &error).size(), 1);
    QVERIFY(store.Archive(QStringLiteral("legacy-map"), migrated.front().id, &error));
    QVERIFY(store.MigrateLegacy(QStringLiteral("legacy-map"), &error));
    QVERIFY(store.List(QStringLiteral("legacy-map"), &error).isEmpty());
    QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), bytes);
}

void AgentWindowTests::controllerRestoresSelectedConversationsWithoutNetwork() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentSessionStore store(directory.path());
    auto first = store.Create(QStringLiteral("map-a"));
    auto second = store.Create(QStringLiteral("map-a"));
    QVERIFY(first && second);
    first->messages = QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("会话甲")}}};
    second->messages = QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("会话乙")}}};
    QVERIFY(store.Save(*first)); QVERIFY(store.Save(*second));
    AgentController controller;
    controller.SetHistoryDirectory(directory.path());
    AgentWindow window;
    QObject::connect(&controller, &AgentController::Event, &window, &AgentWindow::AppendEvent);
    controller.SetActiveMap(QStringLiteral("地图甲"), QStringLiteral("map-a|revision-1"), QStringLiteral("map-a"));
    auto* view = window.findChild<AgentConversationView*>();
    QVERIFY(view);
    controller.SwitchConversation(first->id);
    QVERIFY(view->toPlainText().contains(QStringLiteral("会话甲")));
    QVERIFY(!view->toPlainText().contains(QStringLiteral("会话乙")));
    controller.SwitchConversation(second->id);
    QVERIFY(view->toPlainText().contains(QStringLiteral("会话乙")));
    QVERIFY(!view->toPlainText().contains(QStringLiteral("会话甲")));
    controller.SetActiveMap(QStringLiteral("地图甲"), QStringLiteral("map-a|revision-2"), QStringLiteral("map-a"));
    QVERIFY(view->toPlainText().contains(QStringLiteral("会话乙")));
    controller.NewConversation();
    QVERIFY(view->toPlainText().isEmpty());
    QCOMPARE(store.List(QStringLiteral("map-a")).size(), 3);
    controller.SwitchConversation(first->id);
    QVERIFY(view->toPlainText().contains(QStringLiteral("会话甲")));
    controller.SetActiveMap(QStringLiteral("地图乙"), QStringLiteral("map-b"));
    QVERIFY(view->toPlainText().isEmpty());
    auto* list = window.findChild<QListWidget*>(QStringLiteral("agentSessionList"));
    QVERIFY(list);
    QCOMPARE(list->count(), 1);
    QVERIFY(list->item(0)->data(Qt::UserRole).toString().isEmpty());
    QVERIFY(!controller.IsBusy());
    QCOMPARE(controller.RuntimeProcessId(), qint64(0));
}

void AgentWindowTests::layoutDiagnosticsThresholdDedupBudgetsAndFailures() {
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    AgentLayoutSnapshot snapshot;
    snapshot.viewportSize = {500, 600}; snapshot.columnSize = {460, 800}; snapshot.devicePixelRatio = 2;
    AgentLayoutRowSnapshot first;
    first.index = 7; first.kind = AgentLayoutRowKind::Assistant; first.geometry = {0, 0, 460, 100};
    first.hasDocument = true; first.bodyGeometry = {0, 10, 460, 78}; first.documentHeight = 76;
    AgentLayoutRowSnapshot second;
    second.index = 8; second.kind = AgentLayoutRowKind::User; second.geometry = {0, 148, 460, 60};
    snapshot.rows = {first, second};
    AgentLayoutDiagnostics diagnostics(temporary.filePath(QStringLiteral("threshold.jsonl")));
    using Observation = AgentLayoutDiagnostics::Observation;
    QCOMPARE(diagnostics.Observe(snapshot, 0), Observation::Quiet); // 净间隙48，非QRect inclusive bottom差。
    QVERIFY(!QFile::exists(diagnostics.FilePath()));
    snapshot.rows[1].geometry.moveTop(149);
    QCOMPARE(diagnostics.Observe(snapshot, 0), Observation::Recorded);
    QFile log(diagnostics.FilePath()); QVERIFY(log.open(QIODevice::ReadOnly));
    const auto bytes = log.readAll(); log.close();
    const auto record = QJsonDocument::fromJson(bytes).object();
    QCOMPARE(record.value("schema_version").toInt(), 1);
    QCOMPARE(record.value("device_pixel_ratio").toDouble(), 2.0);
    const auto pair = record.value("gaps").toArray().first().toObject();
    QCOMPARE(pair.value("gap_logical_px").toInt(), 49);
    QCOMPARE(pair.value("previous").toObject().value("row_index").toInt(), 7);
    QCOMPARE(pair.value("previous").toObject().value("document_height").toDouble(), 76.0);
    QCOMPARE(diagnostics.Observe(snapshot, 1000), Observation::Suppressed);
    snapshot.rows[1].geometry.moveTop(150);
    QCOMPARE(diagnostics.Observe(snapshot, 500), Observation::RateLimited); // 不同几何必须提示尾沿重采。
    QCOMPARE(diagnostics.RetryAfterMs(500), 500);
    QCOMPARE(diagnostics.Observe(snapshot, 1000), Observation::Recorded);
    for (int i = 2; i < 64; ++i) {
        snapshot.rows[1].geometry.moveTop(149 + i);
        QCOMPARE(diagnostics.Observe(snapshot, i * 1000), Observation::Recorded);
    }
    snapshot.rows[1].geometry.moveTop(220);
    QCOMPARE(diagnostics.Observe(snapshot, 64000), Observation::BudgetExhausted);
    QCOMPARE(diagnostics.LastError(), QStringLiteral("AGENT_LAYOUT_LOG_BUDGET"));
    QCOMPARE(diagnostics.Observe(snapshot, 65000), Observation::Suppressed);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll().count('\n'), 64); log.close();

    const auto boundedPath = temporary.filePath(QStringLiteral("bounded.jsonl"));
    QFile bounded(boundedPath); QVERIFY(bounded.open(QIODevice::WriteOnly));
    QVERIFY(bounded.resize(1024 * 1024)); bounded.close();
    AgentLayoutDiagnostics full(boundedPath);
    QCOMPARE(full.Observe(snapshot, 0), Observation::BudgetExhausted);
    QCOMPARE(QFileInfo(boundedPath).size(), qint64(1024 * 1024));
    AgentLayoutDiagnostics failed(temporary.path()); // 文件位置是目录，真实磁盘拒绝。
    QCOMPARE(failed.Observe(snapshot, 0), Observation::WriteFailed);
    QCOMPARE(failed.LastError(), QStringLiteral("AGENT_LAYOUT_LOG_WRITE_FAILED"));
    QCOMPARE(failed.Observe(snapshot, 1000), Observation::Suppressed);
}

void AgentWindowTests::conversationLayoutDiagnosticsStayPrivateAndIgnoreHiddenRows() {
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("conversation.jsonl"));
    AgentConversationView conversation;
    conversation.SetLayoutDiagnosticPath(path);
    conversation.resize(600, 800); conversation.show();
    conversation.BeginMessage(QStringLiteral("助手"));
    conversation.AppendText(QStringLiteral("PRIVATE_ASSISTANT_TEXT_93847")); conversation.EndMessage();
    conversation.BeginMessage(QStringLiteral("作者"), true);
    conversation.AppendText(QStringLiteral("PRIVATE_USER_TEXT_93847")); conversation.EndMessage();
    conversation.UpdateTool({{"id", "private-tool-id"}, {"title", "PRIVATE_TOOL_TEXT_93847"},
        {"input", QJsonObject{{"api_key", "PRIVATE_TOOL_SECRET_93847"}}}}, QStringLiteral("完成"), {});
    auto* column = conversation.findChild<QWidget*>(QStringLiteral("agentReadingColumn"));
    QVERIFY(column); QVERIFY(column->layout());
    auto* flow = column->layout();
    flow->setSpacing(48);
    QTest::qWait(400);
    QVERIFY(!QFile::exists(path)); // 真实Qt布局的精确48px不触发。
    flow->setSpacing(49);
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path), 1500);
    QFile log(path); QVERIFY(log.open(QIODevice::ReadOnly));
    const auto bytes = log.readAll(); log.close();
    QVERIFY(!bytes.contains("PRIVATE_")); QVERIFY(!bytes.contains("private-tool-id")); QVERIFY(!bytes.contains("api_key"));
    const auto record = QJsonDocument::fromJson(bytes).object();
    QCOMPARE(record.value("gaps").toArray().size(), 2);
    for (const auto& gap : record.value("gaps").toArray()) QCOMPARE(gap.toObject().value("gap_logical_px").toInt(), 49);
    QVERIFY(record.value("gaps").toArray().first().toObject().value("previous").toObject().contains("document_height"));
    // 相同布局再发事件不能追加；隐藏消息、工具行后没有相邻对，Clear不会采样已删除指针。
    QEvent layoutRequest(QEvent::LayoutRequest); QApplication::sendEvent(column, &layoutRequest);
    QTest::qWait(300); QCOMPARE(QFileInfo(path).size(), qint64(bytes.size()));
    for (int i = 2; i < flow->count(); ++i) flow->itemAt(i)->widget()->hide();
    QTest::qWait(300); QCOMPARE(QFileInfo(path).size(), qint64(bytes.size()));
    conversation.Clear(); QTest::qWait(300); QCOMPARE(QFileInfo(path).size(), qint64(bytes.size()));
    QVERIFY(conversation.LayoutDiagnosticError().isEmpty());

    // 写失败只产生诊断信号，正文继续显示；同一失败不循环重试。
    conversation.SetLayoutDiagnosticPath(temporary.path());
    QSignalSpy failed(&conversation, &AgentConversationView::LayoutDiagnosticFailed);
    conversation.BeginMessage(QStringLiteral("助手")); conversation.AppendText(QStringLiteral("仍可回复")); conversation.EndMessage();
    conversation.BeginMessage(QStringLiteral("作者"), true); conversation.AppendText(QStringLiteral("继续")); conversation.EndMessage();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 1500);
    QCOMPARE(conversation.LayoutDiagnosticError(), QStringLiteral("AGENT_LAYOUT_LOG_WRITE_FAILED"));
    QVERIFY(conversation.toPlainText().contains(QStringLiteral("仍可回复")));
    conversation.resize(620, 800); QTest::qWait(300); QCOMPARE(failed.count(), 1);
}

void AgentWindowTests::conversationLayoutDiagnosticsCaptureStableTrailingChange() {
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("trailing.jsonl"));
    AgentConversationView conversation;
    conversation.SetLayoutDiagnosticPath(path);
    conversation.resize(600, 800); conversation.show();
    conversation.BeginMessage(QStringLiteral("助手")); conversation.AppendText(QStringLiteral("第一行")); conversation.EndMessage();
    conversation.BeginMessage(QStringLiteral("作者"), true); conversation.AppendText(QStringLiteral("第二行")); conversation.EndMessage();
    auto* column = conversation.findChild<QWidget*>(QStringLiteral("agentReadingColumn"));
    QVERIFY(column); auto* flow = column->layout(); QVERIFY(flow);
    flow->setSpacing(49);
    const auto read = [&] {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    QTRY_COMPARE_WITH_TIMEOUT(read().count('\n'), 1, 1500);
    // 第一次记录后立即产生最后一次布局变化；250ms合并采样会落在1秒限流窗内。
    // 此后不再触发布局，尾沿必须自行记录最终73px现场。
    flow->setSpacing(73);
    QTest::qWait(400);
    QCOMPARE(read().count('\n'), 1);
    QTRY_COMPARE_WITH_TIMEOUT(read().count('\n'), 2, 1500);
    const auto lines = read().trimmed().split('\n');
    QCOMPARE(QJsonDocument::fromJson(lines.last()).object().value("gaps").toArray().first().toObject().value("gap_logical_px").toInt(), 73);
    const auto finalBytes = read();
    QEvent layoutRequest(QEvent::LayoutRequest); QApplication::sendEvent(column, &layoutRequest);
    QTest::qWait(1200); QCOMPARE(read(), finalBytes); // 相同几何不记录，亦不触发限流尾沿链。
    QVERIFY(conversation.LayoutDiagnosticError().isEmpty());
}

void AgentWindowTests::finishedReplyLeavesNoGapAndDarkBlocks() {
    QTemporaryDir diagnosticDirectory; QVERIFY(diagnosticDirectory.isValid());
    AgentWindow window;
    window.SetSettings(TestSettings());
    auto* diagnosticView = window.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
    QVERIFY(diagnosticView);
    const auto diagnosticPath = diagnosticDirectory.filePath(QStringLiteral("normal-stream.jsonl"));
    diagnosticView->SetLayoutDiagnosticPath(diagnosticPath);
    // 独立浮动窗口的调色板可能不是主题色：只把背景设成石墨、AlternateBase 留成白色，代码块和表头仍须取主题令牌。
    QPalette palette = War3Map::UiKit::ThemeManager::PaletteFor(War3Map::UiKit::ThemeManager::Theme(QStringLiteral("graphite")));
    palette.setColor(QPalette::AlternateBase, Qt::white);
    palette.setColor(QPalette::Midlight, Qt::white);
    window.setPalette(palette);
    window.resize(1280, 2400); window.show();
    QString reply = QStringLiteral("## 结构\n\n| 层 | 作用 |\n|---|---|\n| 说明 | 写约定 |\n| 注册 | 挂表 |\n\n```text\nSetVariable\ncall DispatchClear()\n```\n\n");
    reply += QStringLiteral("- 第一项 `code`\n- 第二项\n  1. 嵌套\n\n最后一段正文。");
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_start")}});
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")}, {QStringLiteral("text"), QStringLiteral("先查询。")}});
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_start")}, {QStringLiteral("id"), QStringLiteral("t1")}, {QStringLiteral("title"), QStringLiteral("查询")}});
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_end")}, {QStringLiteral("id"), QStringLiteral("t1")}, {QStringLiteral("success"), true}});
    for (int i = 0; i < reply.size(); i += 7) {
        window.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")}, {QStringLiteral("text"), reply.mid(i, 7)}});
        QTest::qWait(2);
    }
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_end")}, {QStringLiteral("success"), true}});
    QTest::qWait(150);
    window.findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"))->setPlainText(QStringLiteral("下一个问题"));
    window.findChild<QPushButton*>(QStringLiteral("agentSend"))->click();
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_start")}});
    QTest::qWait(150);

    // 画布比内容高：多余空间只能留在最后一条之后，行与行之间只有流式间距。
    // 先变窄再恢复（切换修改/助手面板时的宽度变化），正文高度须跟着回落。
    window.resize(520, 2400); QTest::qWait(80);
    window.resize(1280, 2400); QTest::qWait(80);
    auto* column = window.findChild<QWidget*>(QStringLiteral("agentReadingColumn"));
    QWidget* previous = nullptr;
    QTextDocument* finished = nullptr;
    int rows = 0;
    for (QWidget* child : column->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!child->isVisible()) continue;
        ++rows;
        if (previous != nullptr) {
            QVERIFY2(child->y() - previous->geometry().bottom() <= 8,
                qPrintable(QStringLiteral("%1 → %2 间隔 %3").arg(previous->objectName(), child->objectName()).arg(child->y() - previous->geometry().bottom())));
        }
        if (auto* body = child->findChild<QTextBrowser*>(QStringLiteral("agentMessageBody"))) {
            QVERIFY(body->height() - std::ceil(body->document()->size().height()) <= 4);
            if (child->objectName() == QStringLiteral("agentAssistantTurn")) finished = body->document();
        }
        previous = child;
    }
    QCOMPARE(rows, 4);
    QVERIFY(finished != nullptr);
    int tables = 0;
    for (QTextFrame* frame : finished->rootFrame()->childFrames()) {
        const auto* table = qobject_cast<QTextTable*>(frame);
        if (table == nullptr) continue;
        for (int r = 0; r < table->rows(); ++r) for (int c = 0; c < table->columns(); ++c) {
            const QBrush background = table->cellAt(r, c).format().background();
            if (background.style() != Qt::NoBrush) { ++tables; QVERIFY2(background.color().lightness() < 80, qPrintable(background.color().name())); }
        }
        const QBrush frameBackground = table->format().background();
        if (frameBackground.style() != Qt::NoBrush) QVERIFY2(frameBackground.color().lightness() < 80, qPrintable(frameBackground.color().name()));
    }
    for (QTextBlock block = finished->begin(); block.isValid(); block = block.next()) {
        const QBrush background = block.blockFormat().background();
        if (background.style() != Qt::NoBrush) QVERIFY2(background.color().lightness() < 80, qPrintable(background.color().name()));
    }
    QVERIFY(tables > 0);
    QTest::qWait(300);
    QVERIFY(!QFile::exists(diagnosticPath)); // 正常流式完成、宽度变化、下一条提问均不写诊断。
}

void AgentWindowTests::contextPopoverUsesActualUsageAndKeyboard() {
    AgentWindow window;
    auto settings = TestSettings();
    settings.providers[0].models[0].contextOverride = 258000;
    window.SetSettings(settings);
    window.resize(420, 840); window.show();
    auto* ring = window.findChild<ContextRing*>();
    auto* popup = window.findChild<War3Map::UiKit::PopoverSurface*>(QStringLiteral("agentContextPopover"));
    auto* fraction = window.findChild<QLabel*>(QStringLiteral("agentContextFraction"));
    auto* tokens = window.findChild<QLabel*>(QStringLiteral("agentContextTokens"));
    auto* breakdown = window.findChild<QLabel*>(QStringLiteral("agentContextBreakdown"));
    QVERIFY(ring && popup && fraction && tokens && breakdown);
    QTest::mouseClick(ring, Qt::LeftButton);
    QTRY_VERIFY(popup->isVisible());
    QCOMPARE(fraction->text(), QStringLiteral("暂无用量数据"));
    QVERIFY(!tokens->text().contains(QStringLiteral("0 /")));
    QTest::keyClick(popup, Qt::Key_Escape);
    QTRY_VERIFY(!popup->isVisible());
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("usage")}, {QStringLiteral("available"), true},
        {QStringLiteral("context_tokens"), 192000}, {QStringLiteral("input_tokens"), 180000},
        {QStringLiteral("output_tokens"), 12000}, {QStringLiteral("cache_read_tokens"), 120000}});
    ring->setFocus(); QTest::keyClick(ring, Qt::Key_Space);
    QTRY_VERIFY(popup->isVisible());
    QCOMPARE(fraction->text(), QStringLiteral("已用 74%（剩余 26%）"));
    QCOMPARE(tokens->text(), QStringLiteral("已用 192K，共 258K tokens"));
    QVERIFY(!breakdown->isVisible());
    QVERIFY(popup->ContentWidget()->toolTip().contains(QStringLiteral("缓存命中 120K")));
    QVERIFY(breakdown->text().contains(QStringLiteral("缓存命中 120K")));
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("agentContextSource"))->text().contains(QStringLiteral("用户指定")));
    QTest::mouseClick(ring, Qt::LeftButton);
    QTRY_VERIFY(!popup->isVisible());
    window.AppendEvent({{QStringLiteral("type"), QStringLiteral("usage")}, {QStringLiteral("available"), false}});
    QTest::mouseClick(ring, Qt::LeftButton);
    QTRY_VERIFY(popup->isVisible());
    QCOMPARE(fraction->text(), QStringLiteral("暂无用量数据"));
    QVERIFY(!breakdown->isVisible());
    QTest::keyClick(popup, Qt::Key_Escape);
}

QTEST_MAIN(AgentWindowTests)
#include "AgentWindowTests.moc"
