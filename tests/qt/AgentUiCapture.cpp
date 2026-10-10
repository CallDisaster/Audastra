#include "AgentSettingsDialog.h"
#include "AgentWindow.h"
#include "AgentConversationView.h"
#include "AnimatedCounter.h"
#include "ComposerWidgets.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "Widgets/Motion.h"
#include "Widgets/PopoverSurface.h"
#include "PopupCapture.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QStandardPaths>
#include <QFontDatabase>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <array>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

using namespace War3Map::Agent;

namespace {

AgentSettings VisualFixtureSettings() {
    AgentSettings settings;
    settings.aiEnabled = true;
    settings.allowEdits = true;
    settings.allowSave = false;
    settings.allowCommands = false;
    settings.mcpEnabled = false;
    ProviderSettings provider;
    provider.id = QStringLiteral("visual-fixture-provider");
    provider.name = QStringLiteral("视觉测试夹具 · 本地原创数据");
    provider.website = QStringLiteral("https://visual-fixture.invalid");
    provider.baseUrl = QStringLiteral("https://visual-fixture.invalid/v1");
    ModelSettings model;
    model.id = QStringLiteral("visual-fixture-t-agent");
    model.name = QStringLiteral("展示模型");
    model.contextOverride = 262144;
    model.imagesSupported = true;
    provider.models.append(model);
    ModelSettings unknown;
    unknown.id = QStringLiteral("visual-fixture-unknown-context");
    unknown.name = QStringLiteral("未声明上下文的长中文模型名称 · 默认 256K 估算");
    provider.models.append(unknown);
    ModelSettings disabled;
    disabled.id = QStringLiteral("visual-fixture-disabled");
    disabled.name = QStringLiteral("未启用模型");
    disabled.enabled = false;
    provider.models.append(disabled);
    settings.providers.append(provider);
    return settings;
}

QTreeWidgetItem* FindField(QTreeWidgetItem* root, const QString& title) {
    if (root == nullptr) return nullptr;
    if (root->text(0) == title) return root;
    for (int i = 0; i < root->childCount(); ++i) {
        if (auto* item = FindField(root->child(i), title)) return item;
    }
    return nullptr;
}

QTreeWidgetItem* FindField(QTreeWidget* tree, const QString& title) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (auto* item = FindField(tree->topLevelItem(i), title)) return item;
    }
    return nullptr;
}

QString ModeText(AgentWindow::PresentationMode mode) {
    switch (mode) {
    case AgentWindow::PresentationMode::Page: return QStringLiteral("page");
    case AgentWindow::PresentationMode::Sidebar: return QStringLiteral("sidebar");
    case AgentWindow::PresentationMode::Detached: return QStringLiteral("detached");
    }
    return QStringLiteral("unknown");
}

class CaptureRun final {
public:
    CaptureRun(QApplication& application, QString outputDirectory)
        : application_(application), outputDirectory_(std::move(outputDirectory)),
          settings_(VisualFixtureSettings()), settingsDialog_(settings_) {
        started_.start();
        window_.setWindowTitle(QStringLiteral("FORGE · AI 工作区【视觉测试夹具】"));
        window_.SetActiveMap(QStringLiteral("视觉测试夹具｜长中文地图名与事件分发工作区"),
            QStringLiteral("fixture-only:no-map-loaded:no-network:no-secret:no-disk-edit"));
        window_.SetSettings(settings_);
        SetWorkspace(AgentWindow::PresentationMode::Page, QSize(1280, 900));
        window_.show();
        settingsDialog_.setWindowTitle(QStringLiteral("设置 · AI 与 MCP【视觉测试夹具】"));
        if (auto* label = settingsDialog_.findChild<QLabel*>(QStringLiteral("agentSettingsMessage"))) {
            label->setText(QStringLiteral("视觉测试夹具 · 不联网、不保存设置、不读写密钥"));
        }
        // 只使用当前 GUI Agent 真正开放的工具名称，不示意尚未开放的业务工具。
        settingsDialog_.SetAvailableTools({QStringLiteral("editor_state"), QStringLiteral("editor_query_trigger_graph"),
            QStringLiteral("editor_get_ui_catalog"), QStringLiteral("editor_edit_trigger_graph"), QStringLiteral("editor_save_map")});
        stream_.setInterval(18);
        QObject::connect(&stream_, &QTimer::timeout, &application_, [this] { TickFixture(); });
    }

    void Start() {
        Later(60, [this] { Capture(window_, QStringLiteral("empty-page-1280.png"), QStringLiteral("empty-page")); });
        Later(90, [this] { SetWorkspace(AgentWindow::PresentationMode::Sidebar, QSize(420, 840)); });
        Later(150, [this] { Capture(window_, QStringLiteral("empty-sidebar-420.png"), QStringLiteral("empty-sidebar")); });
        Later(180, [this] { SetWorkspace(AgentWindow::PresentationMode::Detached, QSize(740, 950)); });
        Later(240, [this] { Capture(window_, QStringLiteral("empty-detached-740.png"), QStringLiteral("empty-detached")); });
        Later(270, [this] { SetWorkspace(AgentWindow::PresentationMode::Page, QSize(1920, 1080)); });
        Later(330, [this] { Capture(window_, QStringLiteral("empty-page-1920.png"), QStringLiteral("empty-wide-page")); });
        Later(370, [this] {
            auto* prompt = window_.findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"));
            auto* send = window_.findChild<QPushButton*>(QStringLiteral("agentSend"));
            if (prompt == nullptr || send == nullptr) { Fail(QStringLiteral("缺少真实输入与发送控件")); return; }
            prompt->setPlainText(QStringLiteral("检查任意单位事件，把业务接入类型登记和实例订阅。"
                "请保留触发器结构、事件上下文与初始化顺序。"));
            send->click(); // 没有 Controller；只是原创夹具的真实输入信号与消息布局。
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_start")}, {QStringLiteral("fixture"), true}});
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_message")},
                {QStringLiteral("text"), QStringLiteral("【视觉测试夹具】以下事件仅用于检查界面，不连接模型或编辑地图。\n\n"
                    "先查原始事件和类型登记，再核对回调如何接入。参数、结果和修改前后值可在工具行就地展开。\n\n"
                    "## 核对项\n\n| 项目 | 数量 | 状态 |\n|:--|--:|:--:|\n| **类型登记** | 3 | 已读取 |\n| 回调 `OnUnitDeath` | 2 | 待核对 |\n\n"
                    "- [x] 读取原始事件\n- [ ] 接入实例订阅\n\n> 只改触发器，不转换成代码。\n\n"
                    "```jass\nfunction Init takes nothing returns nothing\n    call TriggerRegisterAnyUnitEventBJ(t, EVENT_PLAYER_UNIT_DEATH) // 登记\nendfunction\n```\n\n"
                    "伤害公式 $d = a\\cdot(1-\\frac{0.06k}{1+0.06k})$，合计：\n\n$$\\sum_{i=1}^{n} w_i x_i \\le \\sqrt{x^2+y^2}$$")},
                {QStringLiteral("fixture"), true}});
            stream_.start();
        });
        Later(410, [this] {
            toolStarted_.start();
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_start")},
                {QStringLiteral("id"), QStringLiteral("visual-fixture-query-1")},
                {QStringLiteral("tool_name"), QStringLiteral("editor_query_trigger_graph")},
                {QStringLiteral("title"), QStringLiteral("视觉夹具：查询初始化与单位类型生命周期事件登记")},
                {QStringLiteral("target"), QStringLiteral("0-0-1 初始化 / 类型登记 / 生命周期清理")},
                {QStringLiteral("kind"), QStringLiteral("trigger")},
                {QStringLiteral("object_id"), QStringLiteral("fixture-trigger-101")},
                {QStringLiteral("fixture"), true},
                {QStringLiteral("details"), QJsonObject{{QStringLiteral("fixture_only"), true},
                    {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("trigger_id"), QStringLiteral("fixture-trigger-101")},
                        {QStringLiteral("revision"), QStringLiteral("fixture-revision-7")},
                        {QStringLiteral("include_children"), true},
                        {QStringLiteral("event_roles"), QJsonArray{QStringLiteral("事件单位"), QStringLiteral("攻击者")}}}}}}});
        });
        Later(450, [this] {
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_update")},
                {QStringLiteral("id"), QStringLiteral("visual-fixture-query-1")},
                {QStringLiteral("text"), QStringLiteral("夹具返回 3 项类型登记与 2 项回调，不涉及作者地图")},
                {QStringLiteral("fixture"), true}});
        });
        Later(490, [this] {
            Capture(window_, QStringLiteral("task-page-1920.png"), QStringLiteral("collapsed-tool-wide-page"));
            SetWorkspace(AgentWindow::PresentationMode::Page, QSize(1280, 900));
        });
        for (int i = 0; i < 9; ++i) {
            Later(540 + i * 60, [this, i] {
                Capture(window_, QStringLiteral("counter-%1.png").arg(i, 2, 10, QLatin1Char('0')),
                    QStringLiteral("actual-fixture-counter-animation"));
            });
        }
        Later(850, [this] {
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("tool_end")},
                {QStringLiteral("id"), QStringLiteral("visual-fixture-query-1")},
                {QStringLiteral("success"), true}, {QStringLiteral("fixture"), true},
                {QStringLiteral("duration_ms"), static_cast<double>(toolStarted_.elapsed())},
                {QStringLiteral("details"), QJsonObject{{QStringLiteral("fixture_only"), true},
                    {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("trigger_id"), QStringLiteral("fixture-trigger-101")}}},
                    {QStringLiteral("result"), QJsonObject{{QStringLiteral("content"), QJsonArray{QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("{\"type_templates\":3,\"callbacks\":2,\"native_game_validated\":false}")}}}}}}}}});
        });
        Later(1060, [this] {
            if (!WaitForFixtureTicks(6)) return;
            stream_.stop();
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_end")},
                {QStringLiteral("success"), true}, {QStringLiteral("fixture"), true}});
        });
        Later(1110, [this] {
            Capture(window_, QStringLiteral("task-page-1280.png"), QStringLiteral("completed-collapsed-tool"));
            SetDisclosure(QStringLiteral("agentToolRow"), true, QStringLiteral("trigger_id"));
        });
        Later(1200, [this] {
            Capture(window_, QStringLiteral("tool-expanded-page-1280.png"), QStringLiteral("inline-tool-arguments-result"));
            SetDisclosure(QStringLiteral("agentToolRow"), false);
            SetDisclosure(QStringLiteral("agentInlineChanges"), true, QStringLiteral("修改后"));
        });
        Later(1290, [this] {
            Capture(window_, QStringLiteral("changes-page-1280.png"), QStringLiteral("inline-change-before-after"));
            SetWorkspace(AgentWindow::PresentationMode::Sidebar, QSize(420, 840));
        });
        Later(1380, [this] {
            Capture(window_, QStringLiteral("changes-sidebar-420.png"), QStringLiteral("wide-to-narrow-sidebar"));
            SetWorkspace(AgentWindow::PresentationMode::Detached, QSize(740, 950));
        });
        Later(1470, [this] {
            Capture(window_, QStringLiteral("task-detached-740.png"), QStringLiteral("narrow-detached"));
            SetWorkspace(AgentWindow::PresentationMode::Sidebar, QSize(360, 760));
        });
        Later(1560, [this] {
            Capture(window_, QStringLiteral("task-sidebar-360.png"), QStringLiteral("minimum-sidebar"));
            SetDisclosure(QStringLiteral("agentInlineChanges"), false);
            SetWorkspace(AgentWindow::PresentationMode::Page, QSize(1280, 900));
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_start")}, {QStringLiteral("fixture"), true}});
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_message")},
                {QStringLiteral("text"), QStringLiteral("【滚动视觉夹具】\n")
                    + QStringLiteral("保留事件角色、业务启停、嵌套顺序与生命周期清理。这是原创滚动和布局数据。\n").repeated(70)},
                {QStringLiteral("fixture"), true}});
        });
        Later(1660, [this] {
            auto* conversation = window_.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
            if (conversation == nullptr || conversation->verticalScrollBar()->maximum() <= 0) {
                Fail(QStringLiteral("滚动夹具未形成真实可滚动会话")); return;
            }
            conversation->verticalScrollBar()->setValue(0);
            scrollBefore_ = conversation->verticalScrollBar()->value();
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")},
                {QStringLiteral("text"), QStringLiteral("\n新输出抵达；上滚的阅读位置应保持。")},
                {QStringLiteral("fixture"), true}});
        });
        Later(1790, [this] {
            Capture(window_, QStringLiteral("scroll-retained-page-1280.png"), QStringLiteral("scroll-retention"));
            auto* conversation = window_.findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
            if (conversation != nullptr) scrollAfter_ = conversation->verticalScrollBar()->value();
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("run_end")},
                {QStringLiteral("cancelled"), true}, {QStringLiteral("fixture"), true}});
            window_.hide();
            settingsDialog_.resize(1024, 840);
            settingsDialog_.show();
            SelectSettingsPage(0);
        });
        Later(1900, [this] {
            Capture(settingsDialog_, QStringLiteral("settings-general-1024.png"), QStringLiteral("settings-general"));
            SelectSettingsPage(1);
        });
        Later(2060, [this] {
            Capture(settingsDialog_, QStringLiteral("settings-providers-1024.png"), QStringLiteral("settings-provider"));
            SelectSettingsPage(2);
        });
        Later(2220, [this] {
            Capture(settingsDialog_, QStringLiteral("settings-permissions-1024.png"), QStringLiteral("settings-permissions"));
            SelectSettingsPage(3);
        });
        Later(2380, [this] {
            Capture(settingsDialog_, QStringLiteral("settings-mcp-1024.png"), QStringLiteral("settings-mcp-disabled"));
        });
        Later(2460, [this] {
            settingsDialog_.hide(); window_.ClearConversation();
            SetWorkspace(AgentWindow::PresentationMode::Page, QSize(1280, 900)); window_.show();
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_message")},
                {QStringLiteral("fixture"), true}, {QStringLiteral("text"), QStringLiteral(
                    "【原创视觉夹具】这段文字只核对阅读效果，不连接模型或地图。\n\n"
                    "## `初始化登记` 与事件分发\n\n"
                    "先读取 `editor_query_trigger_graph` 返回的实际身份，再检查每个订阅是否有业务回调。"
                    "名称中保留作者能辨认的功能，数字只是实际修改计数。\n\n"
                    "- **类型登记**：将需要的单位类型与业务入口关联。\n"
                    "- **实例订阅**：单位进入地图后登记，离开或销毁时清理。\n"
                    "  - 同一类型的不同事件分别记录。\n"
                    "  - 保留原事件上下文与执行顺序。\n\n"
                    "| 阶段 | 结果 | 后续 |\n|:--|:--|:--|\n"
                    "| 已读取 | 实际节点与参数 | 检查引用 |\n"
                    "| 待验收 | 保存后回读 | 作者体验 |\n\n"
                    "> 保存结果与进游戏结果分开记录，不能互相代替。\n\n"
                    "使用 `登记 → 订阅 → 分发 → 清理` 的顺序，既让作者能继续编辑，也保留明确的诊断。")}});
        });
        Later(2540, [this] { Capture(window_, QStringLiteral("readability-page-1280.png"), QStringLiteral("markdown-readability-1280")); });
        Later(2620, [this] { SetWorkspace(AgentWindow::PresentationMode::Sidebar, QSize(420, 840)); });
        Later(2700, [this] { Capture(window_, QStringLiteral("readability-sidebar-420.png"), QStringLiteral("markdown-readability-420")); });
        Later(2720, [this] {
            if (auto* conversation = window_.findChild<AgentConversationView*>(QStringLiteral("agentConversation")))
                conversation->verticalScrollBar()->setValue(conversation->verticalScrollBar()->minimum());
            else Fail(QStringLiteral("缺少实际会话滚动区域"));
        });
        Later(2760, [this] { Capture(window_, QStringLiteral("readability-sidebar-420-top.png"), QStringLiteral("markdown-readability-420-top")); });
        Later(2780, [this] { SetWorkspace(AgentWindow::PresentationMode::Page, QSize(1920, 1080)); });
        Later(2860, [this] { Capture(window_, QStringLiteral("readability-page-1920.png"), QStringLiteral("markdown-readability-1920")); });
        Later(2940, [this] { SetWorkspace(AgentWindow::PresentationMode::Sidebar, QSize(360, 820)); });
        Later(3020, [this] { Capture(window_, QStringLiteral("header-sidebar-360.png"), QStringLiteral("narrow-header-360")); });
        Later(3100, [this] {
            SetWorkspace(AgentWindow::PresentationMode::Sidebar, QSize(420, 840));
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("usage")}, {QStringLiteral("fixture"), true},
                {QStringLiteral("available"), true}, {QStringLiteral("context_tokens"), 192000},
                {QStringLiteral("input_tokens"), 180000}, {QStringLiteral("output_tokens"), 12000},
                {QStringLiteral("cache_read_tokens"), 120000}});
            auto* ring = window_.findChild<ContextRing*>();
            if (!ring) { Fail(QStringLiteral("缺少上下文圆环")); return; }
            emit ring->Activated();
        });
        Later(3180, [this] { CapturePopover(QStringLiteral("agentContextPopover"), QStringLiteral("context-usage-popover.png"), QStringLiteral("context-actual-fixture")); });
        Later(3260, [this] {
            if (auto* popup = window_.findChild<War3Map::UiKit::PopoverSurface*>(QStringLiteral("agentContextPopover"))) popup->Dismiss();
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("usage")}, {QStringLiteral("available"), false}, {QStringLiteral("fixture"), true}});
            if (auto* ring = window_.findChild<ContextRing*>()) emit ring->Activated();
        });
        Later(3340, [this] { CapturePopover(QStringLiteral("agentContextPopover"), QStringLiteral("context-unknown-popover.png"), QStringLiteral("context-unavailable")); });
        Later(3420, [this] {
            if (auto* popup = window_.findChild<War3Map::UiKit::PopoverSurface*>(QStringLiteral("agentContextPopover"))) popup->Dismiss();
            window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("conversations")}, {QStringLiteral("fixture"), true},
                {QStringLiteral("selected_id"), QStringLiteral("fixture-session-a")}, {QStringLiteral("sessions"), QJsonArray{
                    QJsonObject{{QStringLiteral("id"), QStringLiteral("fixture-session-a")}, {QStringLiteral("title"), QStringLiteral("类型登记与实例订阅的实际迁移")},
                        {QStringLiteral("updated_at"), QStringLiteral("2026-10-04T01:00:00.000Z")}, {QStringLiteral("model_id"), QStringLiteral("视觉夹具模型")}},
                    QJsonObject{{QStringLiteral("id"), QStringLiteral("fixture-session-b")}, {QStringLiteral("title"), QStringLiteral("伤害分发的业务回调与清理")},
                        {QStringLiteral("updated_at"), QStringLiteral("2026-10-04T00:50:00.000Z")}, {QStringLiteral("model_id"), QStringLiteral("另一个视觉夹具模型")}}}}});
            if (auto* button = window_.findChild<QToolButton*>(QStringLiteral("agentConversations"))) button->click();
        });
        Later(3500, [this] { CapturePopover(QStringLiteral("agentSessionPopover"), QStringLiteral("sessions-popover.png"), QStringLiteral("map-session-fixture")); });
        Later(3580, [this] {
            if (auto* popup = window_.findChild<War3Map::UiKit::PopoverSurface*>(QStringLiteral("agentSessionPopover"))) popup->Dismiss();
        });
        Later(3660, [this] {
            auto* button = window_.findChild<QToolButton*>(QStringLiteral("agentPermissions"));
            if (!button || !button->menu()) { Fail(QStringLiteral("缺少实际权限菜单")); return; }
            button->menu()->popup(button->mapToGlobal(QPoint(0, button->height())));
        });
        Later(3740, [this] {
            auto* button = window_.findChild<QToolButton*>(QStringLiteral("agentPermissions"));
            auto* popup = button ? button->menu() : nullptr;
            if (!popup || !popup->isVisible()) { Fail(QStringLiteral("权限菜单没有显示")); return; }
            const auto error = PopupCapture::Save(window_, *popup, button, outputDirectory_, QStringLiteral("permission-menu"));
            if (!error.isEmpty()) { Fail(error); return; }
            Capture(*popup, QStringLiteral("permission-menu.png"), QStringLiteral("real-permission-menu"));
            popup->hide();
        });
        Later(3820, [this] {
            auto* models = window_.findChild<QComboBox*>(QStringLiteral("agentModel"));
            if (!models) { Fail(QStringLiteral("缺少实际模型选择器")); return; }
            models->showPopup();
        });
        Later(3900, [this] {
            auto* models = window_.findChild<QComboBox*>(QStringLiteral("agentModel"));
            auto* popup = models && models->view() ? models->view()->window() : nullptr;
            if (!popup || !popup->isVisible()) { Fail(QStringLiteral("模型下拉没有显示")); return; }
            const auto error = PopupCapture::Save(window_, *popup, models, outputDirectory_, QStringLiteral("model-menu"));
            if (!error.isEmpty()) { Fail(error); return; }
            Capture(*popup, QStringLiteral("model-menu.png"), QStringLiteral("real-model-menu"));
            models->hidePopup();
        });
        // 计划登记顺序不是时间顺序：工具完成与计数截图交错，稳定排序后才执行。
        std::stable_sort(steps_.begin(), steps_.end(), [](const Step& left, const Step& right) {
            return left.milliseconds < right.milliseconds;
        });
        ScheduleNext();
    }

private:
    struct Step {
        int milliseconds = 0;
        std::function<void()> callback;
    };

    void Later(int milliseconds, std::function<void()> callback) {
        steps_.push_back({milliseconds, std::move(callback)});
    }

    void ScheduleNext() {
        if (failed_) return;
        if (nextStep_ >= steps_.size()) { Finish(); return; }
        Step step = steps_.at(nextStep_++);
        const int interval = std::max(0, step.milliseconds - plannedElapsed_);
        plannedElapsed_ = step.milliseconds;
        // 每步完成之后才开始等下一间隔；grab/save 较慢不能吞掉夹具 timer 的执行机会。
        QTimer::singleShot(interval, &application_, [this, callback = std::move(step.callback)] {
            if (failed_) return;
            callback();
            if (failed_) return;
            ++completedSteps_;
            ScheduleNext();
        });
    }

    bool WaitForFixtureTicks(int minimum) {
        if (fixtureTick_ >= minimum) return true;
        if (!stream_.isActive()) {
            Fail(QStringLiteral("夹具定时器已停止：actual ticks=%1 required=%2").arg(fixtureTick_).arg(minimum));
            return false;
        }
        QElapsedTimer elapsed;
        elapsed.start();
        QEventLoop wait;
        QTimer check;
        check.setInterval(18);
        QObject::connect(&check, &QTimer::timeout, &wait, [this, &wait, minimum] {
            if (fixtureTick_ >= minimum) wait.quit();
        });
        QTimer::singleShot(3000, &wait, &QEventLoop::quit);
        check.start();
        // 只等待真实 stream_ tick；未启动 Controller，不手工制造改动行或计数。
        wait.exec();
        tickWaitMilliseconds_ += elapsed.elapsed();
        if (fixtureTick_ < minimum) {
            Fail(QStringLiteral("等待真实夹具输出超时：actual ticks=%1 required=%2 wait_ms=%3")
                .arg(fixtureTick_).arg(minimum).arg(elapsed.elapsed()));
            return false;
        }
        return true;
    }

    void SetWorkspace(AgentWindow::PresentationMode mode, const QSize& size) {
        window_.SetPresentationMode(mode);
        window_.resize(size);
    }

    void SetDisclosure(const QString& name, bool expanded, const QString& field = {}) {
        auto* row = window_.findChild<QWidget*>(name);
        if (row == nullptr) { Fail(QStringLiteral("缺少实际行内工具/改动行：%1").arg(name)); return; }
        auto* toggle = row->findChild<QToolButton*>();
        if (toggle == nullptr) { Fail(QStringLiteral("缺少 disclosure 控件")); return; }
        toggle->setChecked(expanded);
        if (expanded && !field.isEmpty()) {
            auto* tree = row->findChild<QTreeWidget*>();
            if (tree == nullptr) { Fail(QStringLiteral("缺少实际行内详情")); return; }
            tree->expandAll();
            auto* item = FindField(tree, field);
            if (item == nullptr) { Fail(QStringLiteral("详情缺少夹具字段：%1").arg(field)); return; }
            tree->setCurrentItem(item);
        }
    }

    void SelectSettingsPage(int index) {
        auto* navigation = settingsDialog_.findChild<QListWidget*>(QStringLiteral("agentSettingsNavigation"));
        if (navigation == nullptr || index >= navigation->count()) {
            Fail(QStringLiteral("缺少实际设置页：%1").arg(index)); return;
        }
        navigation->setCurrentRow(index);
    }

    void CapturePopover(const QString& name, const QString& file, const QString& stage) {
        auto* popup = window_.findChild<War3Map::UiKit::PopoverSurface*>(name);
        if (!popup || !popup->isVisible()) { Fail(QStringLiteral("浮层没有实际显示：%1").arg(name)); return; }
        QWidget* anchor = name == QStringLiteral("agentSessionPopover")
            ? window_.findChild<QWidget*>(QStringLiteral("agentConversations")) : window_.findChild<ContextRing*>();
        const auto error = PopupCapture::Save(window_, *popup, anchor, outputDirectory_, QFileInfo(file).completeBaseName());
        if (!error.isEmpty()) { Fail(error); return; }
        Capture(*popup, file, stage);
    }

    void TickFixture() {
        ++fixtureTick_;
        const QString delta = fixtureTick_ == 1 ? QStringLiteral("\n夹具持续输出：")
            : fixtureTick_ % 4 == 0 ? QStringLiteral("核对类型与实例订阅。 ") : QStringLiteral("继续检查。 ");
        window_.AppendEvent({{QStringLiteral("type"), QStringLiteral("assistant_delta")},
            {QStringLiteral("text"), delta}, {QStringLiteral("fixture"), true}});
        if (fixtureTick_ % 3 != 0) return;
        QJsonObject change{{QStringLiteral("type"), QStringLiteral("changes")},
            {QStringLiteral("mode"), QStringLiteral("delta")}, {QStringLiteral("fixture"), true},
            {QStringLiteral("added_lines"), 3}, {QStringLiteral("removed_lines"), 1},
            {QStringLiteral("trigger_nodes"), 2}, {QStringLiteral("files"), 0}};
        if (fixtureTick_ == 6) {
            // 只呈现一个原创内存差异；不把计数夹具伪装成地图保存事务。
            change.insert(QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("trigger")},
                {QStringLiteral("object_id"), QStringLiteral("fixture-trigger-106")},
                {QStringLiteral("title"), QStringLiteral("视觉夹具：长中文动作参数与类型订阅关系")},
                {QStringLiteral("target"), QStringLiteral("0-0-1 / 初始化 / 原创订阅参数")},
                {QStringLiteral("before"), QStringLiteral("原创参数：类型订阅数量 3")},
                {QStringLiteral("after"), QStringLiteral("原创参数：类型订阅数量 6")}}});
        }
        window_.AppendEvent(change);
    }

    void Capture(QWidget& widget, const QString& fileName, const QString& stage) {
        if (failed_) return;
        const QString path = QDir(outputDirectory_).filePath(fileName);
        if (QFileInfo::exists(path)) { Fail(QStringLiteral("保留已有截图，拒绝覆盖：%1").arg(path)); return; }
        const qint64 begin = started_.elapsed();
        QElapsedTimer capture;
        capture.start();
        const QPixmap image = widget.grab();
        const qint64 grabMilliseconds = capture.elapsed();
        if (image.isNull() || !image.save(path, "PNG")) { Fail(QStringLiteral("截图写出失败：%1").arg(path)); return; }
        const qint64 captureMilliseconds = capture.elapsed();
        QJsonObject frame{{QStringLiteral("file"), fileName}, {QStringLiteral("stage"), stage},
            {QStringLiteral("elapsed_ms"), static_cast<double>(started_.elapsed())},
            {QStringLiteral("planned_elapsed_ms"), plannedElapsed_},
            {QStringLiteral("capture_started_ms"), static_cast<double>(begin)},
            {QStringLiteral("grab_ms"), static_cast<double>(grabMilliseconds)},
            {QStringLiteral("png_write_ms"), static_cast<double>(captureMilliseconds - grabMilliseconds)},
            {QStringLiteral("capture_ms"), static_cast<double>(captureMilliseconds)},
            {QStringLiteral("actual_fixture_timer_ticks"), fixtureTick_},
            {QStringLiteral("logical_width"), widget.width()}, {QStringLiteral("logical_height"), widget.height()},
            {QStringLiteral("pixel_width"), image.width()}, {QStringLiteral("pixel_height"), image.height()},
            {QStringLiteral("device_pixel_ratio"), image.devicePixelRatio()}, {QStringLiteral("fixture_only"), true}};
        QJsonObject counters;
        const std::array<QString, 5> names{QStringLiteral("agentAddedLines"), QStringLiteral("agentRemovedLines"),
            QStringLiteral("agentTriggerNodes"), QStringLiteral("agentFiles"), QStringLiteral("agentOutputCharacters")};
        for (const auto& name : names) {
            if (auto* counter = window_.findChild<AnimatedCounter*>(name)) {
                counters.insert(name, QJsonObject{{QStringLiteral("actual_display_value"), static_cast<double>(counter->Value())},
                    {QStringLiteral("actual_target_value"), static_cast<double>(counter->TargetValue())},
                    {QStringLiteral("is_animating"), counter->IsAnimating()}, {QStringLiteral("logical_width"), counter->width()}});
            }
        }
        frame.insert(QStringLiteral("actual_counters"), counters);
        frame.insert(QStringLiteral("presentation_mode"), ModeText(window_.Mode()));
        QJsonObject layout;
        for (const auto& name : {QStringLiteral("agentReadingColumn"), QStringLiteral("agentComposerAxis")}) {
            if (auto* region = window_.findChild<QWidget*>(name)) {
                layout.insert(name, QJsonObject{{QStringLiteral("width"), region->width()}, {QStringLiteral("height"), region->height()},
                    {QStringLiteral("minimum_width"), region->minimumWidth()}, {QStringLiteral("maximum_width"), region->maximumWidth()}});
            }
        }
        frame.insert(QStringLiteral("actual_layout"), layout);
        QJsonArray headerControls;
        for (const auto* name : {"agentConversations", "agentNewConversation", "agentPresentation", "agentSettings"}) {
            if (auto* button = window_.findChild<QToolButton*>(QString::fromLatin1(name)))
                headerControls.append(QJsonObject{{QStringLiteral("name"), QString::fromLatin1(name)}, {QStringLiteral("visible"), button->isVisible()},
                    {QStringLiteral("x"), button->x()}, {QStringLiteral("width"), button->width()},
                    {QStringLiteral("inside_header"), button->parentWidget()->rect().contains(button->geometry())}});
        }
        frame.insert(QStringLiteral("actual_header_controls"), headerControls);
        if (auto* conversation = window_.findChild<AgentConversationView*>(QStringLiteral("agentConversation"))) {
            frame.insert(QStringLiteral("actual_tools"), conversation->ToolCount());
            frame.insert(QStringLiteral("actual_changes"), conversation->ChangeCount());
            frame.insert(QStringLiteral("actual_scroll"), conversation->verticalScrollBar()->value());
            frame.insert(QStringLiteral("actual_scroll_maximum"), conversation->verticalScrollBar()->maximum());
        }
        QJsonArray disclosures;
        for (auto* toggle : window_.findChildren<QToolButton*>()) {
            if (toggle->objectName() != QStringLiteral("agentToolToggle") && toggle->objectName() != QStringLiteral("agentChangeToggle")) continue;
            disclosures.append(QJsonObject{{QStringLiteral("kind"), toggle->objectName()},
                {QStringLiteral("expanded"), toggle->isChecked()}});
        }
        frame.insert(QStringLiteral("actual_disclosures"), disclosures);
        frames_.append(frame);
    }

    void Fail(const QString& error) {
        if (failed_) return;
        failed_ = true;
        QTextStream(stderr) << error << Qt::endl;
        stream_.stop();
        QString writeError;
        if (!TryWriteEvidence(QStringLiteral("capture-partial-frames.jsonl"), FrameJsonl(), writeError)) {
            QTextStream(stderr) << writeError << Qt::endl;
        }
        if (!TryWriteEvidence(QStringLiteral("capture-partial-failure.json"),
            QJsonDocument(Manifest(false, error)).toJson(QJsonDocument::Indented), writeError)) {
            QTextStream(stderr) << writeError << Qt::endl;
        }
        application_.exit(1);
    }

    bool TryWriteEvidence(const QString& name, const QByteArray& bytes, QString& error) {
        const QString path = QDir(outputDirectory_).filePath(name);
        if (QFileInfo::exists(path)) { error = QStringLiteral("保留已有回执，拒绝覆盖：%1").arg(path); return false; }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            error = QStringLiteral("视觉回执写出失败：%1").arg(file.errorString()); return false;
        }
        return true;
    }

    bool WriteEvidence(const QString& name, const QByteArray& bytes) {
        QString error;
        if (TryWriteEvidence(name, bytes, error)) return true;
        Fail(error);
        return false;
    }

    QJsonObject Manifest(bool complete, const QString& error = {}) const {
        QJsonObject manifest{{QStringLiteral("fixture_only"), true},
            {QStringLiteral("evidence_scope"), QStringLiteral("offscreen native Qt conversation/disclosure layout and real fixture timers; no provider, map edit, toolchain, game or visible author acceptance")},
            {QStringLiteral("capture_complete"), complete}, {QStringLiteral("failure"), error},
            {QStringLiteral("scheduler"), QStringLiteral("sequential-planned-intervals")},
            {QStringLiteral("planned_elapsed_ms"), plannedElapsed_},
            {QStringLiteral("scheduled_steps"), static_cast<double>(steps_.size())},
            {QStringLiteral("completed_steps"), static_cast<double>(completedSteps_)},
            {QStringLiteral("actual_tick_wait_ms"), static_cast<double>(tickWaitMilliseconds_)},
            {QStringLiteral("platform"), QGuiApplication::platformName()},
            {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
            {QStringLiteral("system_animations_enabled"), War3Map::UiKit::AnimationsEnabled()},
            {QStringLiteral("duration_ms"), static_cast<double>(started_.elapsed())},
            {QStringLiteral("fixture_timer_ticks"), fixtureTick_}, {QStringLiteral("actual_scroll_before"), scrollBefore_},
            {QStringLiteral("actual_scroll_after"), scrollAfter_}, {QStringLiteral("scroll_retained"), scrollBefore_ == scrollAfter_},
            {QStringLiteral("frames"), frames_}};
        if (auto* screen = QGuiApplication::primaryScreen()) {
            manifest.insert(QStringLiteral("screen_logical_dpi"), screen->logicalDotsPerInch());
            manifest.insert(QStringLiteral("screen_physical_dpi"), screen->physicalDotsPerInch());
        }
        return manifest;
    }

    QByteArray FrameJsonl() const {
        QByteArray jsonl;
        for (const auto& frame : frames_) {
            jsonl.append(QJsonDocument(frame.toObject()).toJson(QJsonDocument::Compact));
            jsonl.append('\n');
        }
        return jsonl;
    }

    void Finish() {
        if (failed_) return;
        if (!WriteEvidence(QStringLiteral("capture-frames.jsonl"), FrameJsonl())) return;
        if (!WriteEvidence(QStringLiteral("capture-manifest.json"), QJsonDocument(Manifest(true)).toJson(QJsonDocument::Indented))) return;
        settingsDialog_.reject();
        QTextStream(stdout) << QStringLiteral("fixture_only=true screenshots=%1 manifest=%2")
            .arg(frames_.size()).arg(QDir(outputDirectory_).filePath(QStringLiteral("capture-manifest.json"))) << Qt::endl;
        application_.exit(scrollBefore_ >= 0 && scrollBefore_ == scrollAfter_ ? 0 : 3);
    }

    QApplication& application_;
    QString outputDirectory_;
    AgentSettings settings_;
    AgentWindow window_;
    AgentSettingsDialog settingsDialog_;
    QElapsedTimer started_;
    QElapsedTimer toolStarted_;
    QTimer stream_;
    QJsonArray frames_;
    std::vector<Step> steps_;
    std::size_t nextStep_ = 0;
    std::size_t completedSteps_ = 0;
    int plannedElapsed_ = 0;
    qint64 tickWaitMilliseconds_ = 0;
    int fixtureTick_ = 0;
    int scrollBefore_ = -1;
    int scrollAfter_ = -2;
    bool failed_ = false;
};

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

int main(int argc, char** argv) {
    // 本程序只产生离屏夹具；即使命令行试图覆盖平台，也在创建窗口前拒绝。
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("FORGE Agent native UI visual fixture"));
    application.setQuitOnLastWindowClosed(false);
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        QTextStream(stderr) << QStringLiteral("拒绝非 offscreen 平台，不能触碰作者前台。") << Qt::endl;
        return 2;
    }
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("原生 Qt AI 视觉夹具；不连接提供商、不读写地图或用户设置。"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("outDir"), QStringLiteral("PNG、JSONL 与实际计数回执输出目录"));
    QCommandLineOption outputOption({QStringLiteral("o"), QStringLiteral("output")}, QStringLiteral("输出目录"), QStringLiteral("directory"));
    parser.addOption(outputOption);
    parser.process(application);
    QString output = parser.value(outputOption);
    const QStringList positional = parser.positionalArguments();
    if (output.isEmpty() && positional.size() == 1) output = positional.front();
    if (output.isEmpty() || positional.size() > 1) parser.showHelp(2);
    output = QDir(output).absolutePath();
    if (!QDir().mkpath(output)) { QTextStream(stderr) << QStringLiteral("无法创建输出目录：") << output << Qt::endl; return 2; }
    RegisterSystemMathFonts();
    War3Map::UiKit::ThemeManager theme;
    theme.ApplyTheme(QStringLiteral("graphite"));
    const auto fonts = War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory());
    if (!fonts.bundledUi || !fonts.bundledCode || !fonts.failedFiles.isEmpty()) {
        QTextStream(stderr) << QStringLiteral("视觉夹具缺少已加载的随附字体，拒绝产出缺字截图。") << Qt::endl;
        return 2;
    }
    War3Map::UiKit::ApplyApplicationFont(fonts);
    QTextStream(stdout) << QStringLiteral("bundled_ui_font=true bundled_code_font=true loaded_fonts=%1")
        .arg(fonts.loadedFiles.join(QLatin1Char(','))) << Qt::endl;
    CaptureRun run(application, output);
    run.Start();
    return application.exec();
}
