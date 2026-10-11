#include "MainWindow.h"
#include "CampaignModule.h"
#include "LiveEditorTools.h"
#include "AgentController.h"
#include "AgentSettingsDialog.h"
#include "AgentSettingsStore.h"
#include "ApplicationSettingsTransfer.h"
#include "ApplicationSettingsCommit.h"
#include "MapTestSettingsPage.h"
#include "MapSavePreferences.h"
#include "LegacyMapBackupCheck.h"
#include "RecoverySettingsPage.h"
#include "WorldEffectsSettingsPage.h"
#include "WorldModule.h"
#include "RecoveryPreferences.h"
#include "../QtCommon/ApplicationPaths.h"
#include "FeedbackClient.h"
#include "FeedbackDialog.h"
#include "UpdateClient.h"
#include "UpdateDialog.h"
#include "AgentWindow.h"
#include "CredentialStore.h"
#include "McpHttpTransport.h"
#include "TriggerModule.h"
#include "Dock/DockHost.h"
#include "Fonts/FontLoader.h"

#include <QApplication>
#include <QAction>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QSettings>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QRandomGenerator>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QStyle>
#include <QTimer>
#include <QUndoGroup>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QVariant>
#include <QWindow>

#include <algorithm>

namespace War3Map::QtApp {

class AiWorkspace final : public QObject {
public:
    explicit AiWorkspace(QObject* parent) : QObject(parent) {}
    ~AiWorkspace() override {
        if (controller) controller->Stop();
        if (http) http->Stop();
        if (window) delete window;
    }

    Agent::AgentSettingsStore store;
    Agent::CredentialStore credentials;
    Agent::AgentSettings settings;
    QString token;
    QString mapIdentity;
    QString serviceError;
    std::unique_ptr<LiveEditorTools> tools;
    std::unique_ptr<Mcp::McpHttpTransport> http;
    std::unique_ptr<Agent::AgentController> controller;
    QPointer<Agent::AgentWindow> window;
    QPointer<QStackedWidget> stack;
    QPointer<QDockWidget> sidebar;
    QPointer<QWidget> sidebarContent;
    QVBoxLayout* sidebarLayout = nullptr;
    std::function<void()> updatePresentation;
    bool resizePending = false;
    bool presenting = false;
    bool statusHiddenForPage = false;
    bool statusWasVisible = true;
    bool controllerDispatchActive = false;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize && !resizePending && updatePresentation) {
            resizePending = true;
            // 等宿主完成本次布局，再切换呈现，避免 resize 的重入与闪烁。
            QTimer::singleShot(0, this, [this]() {
                resizePending = false;
                if (updatePresentation) updatePresentation();
            });
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace War3Map::QtApp

namespace {

constexpr int AgentSidebarTargetWidth = 420;

bool AgentSidebarFits(const QWidget& host, const QWidget& editor) {
    const int editorMinimum = std::max(editor.minimumWidth(), editor.minimumSizeHint().width());
    const int separator = host.style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, &host);
    // 用原编辑区的真实最低宽度判断；窄屏不压缩地图编辑区来凑侧栏。
    return host.width() >= 1280 && host.contentsRect().width() >= editorMinimum + AgentSidebarTargetWidth + separator;
}

// 记录 Qt 实际控件与原生视口状态；回执据此区分重复控件和旧像素残留。
void CapturePresentationFacts(QWidget& host, QWidget& editor) {
    const auto bars = host.findChildren<QStatusBar*>();
    qsizetype visibleBars = 0;
    for (auto* bar : bars) if (bar->isVisible()) ++visibleBars;
    host.setProperty("agentStatusBarObjectCount", bars.size());
    host.setProperty("agentVisibleStatusBarCount", visibleBars);
    bool viewportVisible = false, nativeVisible = false, withinEditor = true;
    for (auto* viewport : editor.findChildren<QWidget*>(QStringLiteral("worldViewport"))) {
        if (!viewport->isVisible()) continue;
        viewportVisible = true;
        if (auto* native = viewport->windowHandle()) nativeVisible |= native->isVisible();
        const QRect rect(viewport->mapTo(&editor, QPoint()), viewport->size());
        withinEditor &= editor.rect().contains(rect);
    }
    host.setProperty("agentWorldViewportVisible", viewportVisible);
    host.setProperty("agentNativeWorldViewportVisible", nativeVisible);
    host.setProperty("agentWorldViewportWithinEditor", withinEditor);
}

std::filesystem::path SkillsDirectory() {
    const auto portable = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("skills"));
    if (QFile::exists(portable + QStringLiteral("/forge-editor-mcp/SKILL.md"))) {
        return std::filesystem::path(portable.toStdWString());
    }
#ifdef WAR3MAP_AGENT_SKILL_SOURCE_DIR
    return std::filesystem::path(WAR3MAP_AGENT_SKILL_SOURCE_DIR);
#else
    return std::filesystem::path(portable.toStdWString());
#endif
}

QString SystemPrompt() {
    QString result = QStringLiteral(
        "你是 Audastra 地图编辑助手。使用实际提供的工具检查当前地图，然后通过同一事务修改。"
        "新增、优化逻辑默认写成作者可读可编辑的触发器（T，传统事件/条件/动作）；未经作者特别要求不转换为 JASS/Lua/WCT。"
        "每次写操作先取得当前地图身份、修订和图绑定；来源变化后重新查询，不猜 ID。"
        "修改后回读实际结果，向作者说明修改的触发器、前后值、保存结果与未验证项。"
        "失败保留诊断，不放宽类型或保存门禁。工具未提供的能力准确说明不可用。"
        "当前 editor_* 工具操作 GUI 活动地图；旧独立 stdio 工具属于另一个工作区。\n");
    const auto root = SkillsDirectory();
    for (const auto* skill : {"forge-editor-mcp", "forge-t-event-optimization"}) {
        QFile file(QString::fromStdWString((root / skill / "SKILL.md").wstring()));
        if (file.open(QIODevice::ReadOnly) && file.size() <= 65536) {
            result += QStringLiteral("\n") + QString::fromUtf8(file.readAll());
        }
    }
    return result;
}

War3Map::Mcp::ToolPolicy Policy(const War3Map::Agent::AgentSettings& settings) {
    War3Map::Mcp::ToolPolicy policy;
    policy.allowEdit = settings.allowEdits;
    policy.allowSave = settings.allowSave;
    policy.allowExternal = settings.allowCommands;
    for (const auto& name : settings.disabledTools) policy.disabledTools.insert(name);
    return policy;
}

QString RandomToken() {
    QByteArray bytes;
    bytes.reserve(32);
    for (int i = 0; i < 8; ++i) {
        const quint32 value = QRandomGenerator::system()->generate();
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    return QString::fromLatin1(bytes.toHex());
}

void ApplySettings(War3Map::QtApp::AiWorkspace& state) {
    state.tools->SetPolicy(Policy(state.settings));
    state.controller->SetSettings(state.settings);
    if (state.window) state.window->SetSettings(state.settings);
    state.http->Stop();
    state.serviceError.clear();
    if (!state.settings.mcpEnabled) return;
    if (state.token.isEmpty()) {
        QString error;
        state.token = state.credentials.Read(QStringLiteral("mcp-local-access"), &error);
        if (state.token.isEmpty()) {
            state.token = RandomToken();
            if (!state.credentials.Write(QStringLiteral("mcp-local-access"), state.token, &error)) {
                state.token.clear();
                state.serviceError = error;
                return;
            }
        }
    }
    War3Map::Mcp::McpHttpOptions options;
    options.port = state.settings.mcpPort;
    options.bearerToken = state.token.toLatin1();
    if (!state.http->Start(options, &state.serviceError)) return;
}

} // namespace

quint16 MainWindow::StartAgentHttpProbe(const QByteArray& token, War3Map::QtApp::LiveEditorTools*& tools, QString& error) {
    tools = nullptr;
    if (startupMode_ != StartupMode::HeadlessProbe || aiWorkspace_) {
        error = QStringLiteral("HTTP 探针只接受尚未初始化助手的隔离探针宿主。");
        return 0;
    }
    SetupAgentWorkspace(false);
    auto& state = *aiWorkspace_;
    state.settings.allowEdits = true;
    state.settings.allowSave = false;
    state.settings.allowCommands = false;
    state.tools->SetPolicy(Policy(state.settings));
    state.controller->SetSettings(state.settings);
    War3Map::Mcp::McpHttpOptions options;
    options.port = 0;
    options.bearerToken = token;
    if (!state.http->Start(options, &error)) return 0;
    tools = state.tools.get();
    return state.http->Port();
}

void MainWindow::SetupAgentWorkspace(bool readUserConfiguration) {
    if (aiWorkspace_ || (readUserConfiguration && startupMode_ != StartupMode::Normal)) return;
    auto* state = new War3Map::QtApp::AiWorkspace(this);
    aiWorkspace_ = state;
    QString loadError;
    // 离屏宿主测试显式用空配置；不接触作者设置、凭据、端口或站点。
    if (readUserConfiguration) state->settings = state->store.Load(&loadError);
    state->tools = std::make_unique<War3Map::QtApp::LiveEditorTools>(*project_);
    state->tools->Server().SetCampaign(campaignModule_->Session());
    state->tools->Server().SetCampaignBackupOptionsProvider([] {
        return War3Map::QtProject::ReadMapInPlaceSaveOptions();
    });
    state->tools->Server().SetCampaignCallbacks(
        [this] { return !project_->savingMap_ && !campaignModule_->HasModalOperation()
            && !campaignModule_->HasDraft() && !campaignModule_->HasUncommittedChapter(); },
        [this, state] {
            const auto session = state->tools->Server().Campaign();
            if (campaignModule_->Session() != session) campaignModule_->SetSession(session);
            else { campaignModule_->RefreshFromService(); UpdateExportAvailability(); UpdateWindowTitle(); }
        });
    connect(campaignModule_, &War3Map::Modules::CampaignModule::SessionChanged, state, [this, state] {
        state->tools->Server().SetCampaign(campaignModule_->Session());
    });
    const QPointer<MainWindow> searchWindow(this);
    state->tools->SetProjectSearchQuery(
        [searchWindow]() { return searchWindow ? searchWindow->CommandContextRevision() : std::string{}; },
        [searchWindow](std::string_view revision, std::string_view query,
            const War3Map::Services::ProjectSearchOptions& options, std::size_t pageLimit,
            const War3Map::Services::ProjectSearchCursor* cursor) {
            if (searchWindow) return searchWindow->QueryProjectSearchPage(revision, query, options, pageLimit, cursor);
            return War3Map::Result<War3Map::Services::ProjectSearchPage>::Failure(
                "Qt.Search", "PROJECT_SEARCH_WINDOW_CLOSED", "工程窗口已关闭。");
        });
    state->tools->SetSkillRoot(SkillsDirectory());
    state->http = std::make_unique<War3Map::Mcp::McpHttpTransport>(state->tools->Server());
    state->controller = std::make_unique<War3Map::Agent::AgentController>();
    state->controller->SetSystemPrompt(SystemPrompt());
    state->controller->SetToolsProvider([state]() { return state->tools->Tools(); });
    state->controller->SetToolDispatcher([state](const QString& name, const QJsonObject& args) {
        const QScopedValueRollback<bool> dispatchScope(state->controllerDispatchActive, true);
        return state->tools->Invoke(name, args);
    });
    state->tools->SetBeforeWrite([this]() {
        if (project_->savingMap_) return false;
        return triggersModule_->CommitPendingEdits();
    });
    state->tools->SetSaveHandler([this](const std::filesystem::path& path, bool inPlace) {
        return RunMapSave(path, inPlace, true);
    });
    connect(state->controller.get(), &War3Map::Agent::AgentController::Event, state, [state](const QJsonObject& event) {
        // Live broker already emits authoritative changes for both AI and HTTP calls.
        if (state->window && event.value(QStringLiteral("type")).toString() != QStringLiteral("changes")) {
            state->window->AppendEvent(event);
        }
    });
    connect(state->controller.get(), &War3Map::Agent::AgentController::BusyChanged, state, [state](bool busy) {
        if (state->window) state->window->SetBusy(busy);
    });
    connect(state->tools.get(), &War3Map::QtApp::LiveEditorTools::Activity, state, [state](const QJsonObject& event) {
        // Controller 自己提供模型调用身份；外部 HTTP 则直接展示 broker 的真实调用身份。
        if (state->window && (!state->controllerDispatchActive
            || event.value(QStringLiteral("type")).toString() == QStringLiteral("changes"))) {
            state->window->AppendEvent(event);
        }
    });
    connect(state->tools.get(), &War3Map::QtApp::LiveEditorTools::Changed, state, [this]() {
        triggersModule_->RefreshAfterUndo();
        UpdateEditDirtyFlags();
        UpdateExportAvailability();
        ScheduleRecovery();
        RefreshAgentMapContext();
        if (aiWorkspace_->window && aiWorkspace_->window->isVisible()) {
            auto* stack = project_->triggerUndoStack_;
            if (!UndoGroup()->stacks().contains(stack)) UndoGroup()->addStack(stack);
            UndoGroup()->setActiveStack(stack);
        }
    });
    connect(this, &ShellWindow::CurrentModuleChanged, state, [this](const auto&) {
        if (aiWorkspace_->stack && aiWorkspace_->window &&
            aiWorkspace_->stack->currentWidget() == aiWorkspace_->window) HideAgentWorkspace();
    });
    state->updatePresentation = [this]() {
        using Mode = War3Map::Agent::AgentWindow::PresentationMode;
        if (aiWorkspace_ && aiWorkspace_->window && aiWorkspace_->sidebar &&
            aiWorkspace_->sidebar->isVisible() && aiWorkspace_->window->Mode() == Mode::Sidebar && !AgentSidebarFits(*this, *Dock())) {
            PresentAgentWorkspace(static_cast<int>(Mode::Page));
        }
    };
    installEventFilter(state);
    auto* contextTimer = new QTimer(state);
    contextTimer->setInterval(500);
    connect(contextTimer, &QTimer::timeout, state, [this]() { RefreshAgentMapContext(); });
    contextTimer->start();
    ApplySettings(*state);
    if (!loadError.isEmpty()) AddDiagnostic(tr("错误"), tr("助手"), QStringLiteral("AGENT_SETTINGS_LOAD_FAILED"), loadError);
    if (!state->serviceError.isEmpty()) AddDiagnostic(tr("错误"), tr("MCP"), QStringLiteral("MCP_LISTEN_FAILED"), state->serviceError);
    RefreshAgentMapContext();
}

void MainWindow::RefreshAgentMapContext() {
    if (!aiWorkspace_) return;
    const auto path = project_->sourceArchivePath_.empty() ? project_->projectRoot_ : project_->sourceArchivePath_;
    const auto title = path.empty() ? tr("未打开地图") : QString::fromStdWString(path.filename().wstring());
    const auto identity = path.empty() ? QString() : QString::fromStdWString(path.wstring()) + QLatin1Char('|')
        + QString::number(project_->editRevision_.Generation());
    const auto conversationIdentity = path.empty() ? QStringLiteral("forge:no-map")
        : QDir::cleanPath(QFileInfo(QString::fromStdWString(path.wstring())).absoluteFilePath()).toCaseFolded();
    aiWorkspace_->controller->SetActiveMap(title, identity, conversationIdentity);
    if (aiWorkspace_->window) {
        aiWorkspace_->window->SetActiveMap(title, identity);
        aiWorkspace_->window->SetMapSummary(statusModCount_ ? statusModCount_->text() : QString());
    }
    aiWorkspace_->mapIdentity = identity;
}

void MainWindow::ShowAgentWindow() {
    PresentAgentWorkspace(static_cast<int>(War3Map::Agent::AgentWindow::PresentationMode::Page));
}

bool MainWindow::AgentWorkspaceVisible() const {
    if (!aiWorkspace_ || !aiWorkspace_->window || !aiWorkspace_->window->isVisible()) return false;
    using Mode = War3Map::Agent::AgentWindow::PresentationMode;
    if (aiWorkspace_->window->Mode() == Mode::Page)
        return aiWorkspace_->stack && aiWorkspace_->stack->currentWidget() == aiWorkspace_->window;
    if (aiWorkspace_->window->Mode() == Mode::Sidebar)
        return aiWorkspace_->sidebar && aiWorkspace_->sidebar->isVisible();
    return true;
}

void MainWindow::SyncAssistantAction() {
    if (auto* action = findChild<QAction*>(QStringLiteral("agentWorkspaceAction"))) {
        const QSignalBlocker blocker(action);
        action->setChecked(AgentWorkspaceVisible());
    }
    if (aiWorkspace_) CapturePresentationFacts(*this, *Dock());
}

void MainWindow::ToggleAgentWorkspace() {
    if (AgentWorkspaceVisible()) HideAgentWorkspace();
    else ShowAgentWindow();
}

void MainWindow::PresentAgentWorkspace(int mode) {
    SetupAgentWorkspace();
    if (!aiWorkspace_) { ShowStatusMessage(tr("启动探针不加载助手配置")); return; }
    auto* state = aiWorkspace_;
    const bool newWindow = !state->window;
    if (!state->window) {
        state->window = new War3Map::Agent::AgentWindow(this);
        state->window->SetSettings(state->settings);
        connect(state->window, &War3Map::Agent::AgentWindow::PromptSubmitted, state,
            [state](const QString& text, const QString& provider, const QString& model) {
                state->controller->StartPrompt(text, provider, model);
                // Invalid settings/credentials can reject before the controller becomes busy.
                state->window->SetBusy(state->controller->IsBusy());
            });
        connect(state->window, &War3Map::Agent::AgentWindow::StopRequested,
            state->controller.get(), &War3Map::Agent::AgentController::Stop);
        connect(state->window, &War3Map::Agent::AgentWindow::NewConversationRequested, state, [state]() {
            if (state->controller->IsBusy()) return;
            state->controller->NewConversation();
        });
        connect(state->window, &War3Map::Agent::AgentWindow::ConversationSwitchRequested,
            state->controller.get(), &War3Map::Agent::AgentController::SwitchConversation);
        connect(state->window, &War3Map::Agent::AgentWindow::ConversationRenameRequested,
            state->controller.get(), &War3Map::Agent::AgentController::RenameConversation);
        connect(state->window, &War3Map::Agent::AgentWindow::ConversationArchiveRequested,
            state->controller.get(), &War3Map::Agent::AgentController::ArchiveConversation);
        connect(state->window, &War3Map::Agent::AgentWindow::VisibilityChanged, this, [this, state](bool visible) {
            if (state->presenting) return;
            if (!visible) HideAgentWorkspace();
            else SyncAssistantAction();
        });
        connect(state->window, &War3Map::Agent::AgentWindow::SettingsRequested, this, &MainWindow::ShowApplicationSettings);
        connect(state->window, &War3Map::Agent::AgentWindow::PresentationModeRequested, this,
            [this](War3Map::Agent::AgentWindow::PresentationMode requested) {
                PresentAgentWorkspace(static_cast<int>(requested));
            });
        connect(state->window, &War3Map::Agent::AgentWindow::BackRequested, this, &MainWindow::HideAgentWorkspace);
        connect(state->window, &War3Map::Agent::AgentWindow::PermissionsChangeRequested, this,
            [this, state](bool edits, bool save, bool commands) {
                // 与设置窗口同一份配置：先保存，再统一应用到工具策略和窗口。
                auto next = state->settings;
                next.allowEdits = edits;
                next.allowSave = save;
                next.allowCommands = commands;
                QString error;
                if (!state->store.Save(next, &error)) {
                    AddDiagnostic(tr("错误"), tr("助手"), QStringLiteral("AGENT_SETTINGS_SAVE_FAILED"), error);
                    state->window->SetSettings(state->settings);
                    return;
                }
                state->settings = next;
                ApplySettings(*state);
            });
        connect(state->window, &War3Map::Agent::AgentWindow::NavigateRequested, this,
            [this](const QString& kind, const QString& id) {
                const auto module = kind == QStringLiteral("variable") ? QStringLiteral("variables") : QStringLiteral("triggers");
                Navigate({module.toStdString(), kind, id, {{"stableIdentity", true}}});
                activateWindow();
            });
    }
    using Mode = War3Map::Agent::AgentWindow::PresentationMode;
    Mode requested = Mode::Page;
    if (mode == static_cast<int>(Mode::Sidebar)) requested = AgentSidebarFits(*this, *Dock()) ? Mode::Sidebar : Mode::Page;
    else if (mode == static_cast<int>(Mode::Detached)) requested = Mode::Detached;
    if (!newWindow && state->window->Mode() == requested && AgentWorkspaceVisible()) {
        RefreshAgentMapContext();
        state->window->SetBusy(state->controller->IsBusy());
        SyncAssistantAction();
        return;
    }

    if (!state->stack && workspacePages_) state->stack = workspacePages_;
    if (!state->stack) {
        // 兼容没有启动页的原有无界面探针，保持Dock的原宿主合同。
        QWidget* editor = takeCentralWidget();
        state->stack = new QStackedWidget(this);
        state->stack->setObjectName(QStringLiteral("agentWorkspaceStack"));
        state->stack->addWidget(editor);
        setCentralWidget(state->stack);
    }
    state->presenting = true;
    state->window->hide();
    state->stack->removeWidget(state->window);
    if (state->sidebarLayout) state->sidebarLayout->removeWidget(state->window);
    if (state->sidebar) state->sidebar->hide();
    if (workspacePages_) ShowBaseWorkspace();
    else state->stack->setCurrentWidget(Dock());
    if (const auto current = CurrentModule()) UndoStackChanged(*current);
    state->window->SetPresentationMode(requested);

    if (requested == Mode::Page) {
        if (!state->statusHiddenForPage) {
            state->statusWasVisible = !statusBar()->isHidden();
            state->statusHiddenForPage = true;
        }
        statusBar()->hide();
        // Stack 会隐藏编辑宿主，显式隐藏确保原生 D3D 子窗口同步收到 hideEvent。
        Dock()->hide();
        state->window->setParent(state->stack, Qt::Widget);
        state->stack->addWidget(state->window);
        state->stack->setCurrentWidget(state->window);
        auto* undo = project_->triggerUndoStack_;
        if (!UndoGroup()->stacks().contains(undo)) UndoGroup()->addStack(undo);
        UndoGroup()->setActiveStack(undo);
    } else if (requested == Mode::Sidebar) {
        if (state->statusHiddenForPage) { statusBar()->setVisible(state->statusWasVisible); state->statusHiddenForPage = false; }
        if (workspacePages_) ShowBaseWorkspace(); else Dock()->show();
        if (!state->sidebar) {
            state->sidebar = new QDockWidget(tr("AI 助手"), this);
            state->sidebar->setObjectName(QStringLiteral("agentWorkspaceDock"));
            state->sidebar->setAllowedAreas(Qt::RightDockWidgetArea);
            state->sidebar->setFeatures(QDockWidget::NoDockWidgetFeatures);
            auto* title = new QWidget(state->sidebar);
            auto* titleLayout = new QVBoxLayout(title);
            titleLayout->setContentsMargins(0, 0, 0, 0);
            titleLayout->setSpacing(0);
            title->setFixedHeight(0);
            state->sidebar->setTitleBarWidget(title);
            state->sidebarContent = new QWidget(state->sidebar);
            state->sidebarContent->setObjectName(QStringLiteral("agentWorkspaceDockContent"));
            state->sidebarContent->setMinimumSize(360, 0);
            state->sidebarContent->setMaximumWidth(560);
            state->sidebarLayout = new QVBoxLayout(state->sidebarContent);
            state->sidebarLayout->setContentsMargins(0, 0, 0, 0);
            state->sidebar->setWidget(state->sidebarContent);
            addDockWidget(Qt::RightDockWidgetArea, state->sidebar);
        }
        state->window->setParent(state->sidebarContent, Qt::Widget);
        state->sidebarLayout->addWidget(state->window);
        // 确定可见内容的尺寸后再分配 Dock；隐藏控件的 sizeHint 不代表最终侧栏。
        state->window->show();
        state->sidebarLayout->activate();
        state->sidebar->show();
        resizeDocks({state->sidebar}, {AgentSidebarTargetWidth}, Qt::Horizontal);
    } else {
        if (state->statusHiddenForPage) { statusBar()->setVisible(state->statusWasVisible); state->statusHiddenForPage = false; }
        if (workspacePages_) ShowBaseWorkspace(); else Dock()->show();
        state->window->setParent(this, Qt::Window);
        state->window->resize(1060, 820);
    }
    RefreshAgentMapContext();
    state->window->SetBusy(state->controller->IsBusy());
    state->window->show();
    if (requested == Mode::Detached) {
        state->window->raise();
        state->window->activateWindow();
    }
    state->presenting = false;
    if (newWindow) state->controller->RefreshConversations();
    SyncAssistantAction();
    QTimer::singleShot(0, this, [this] { SyncAssistantAction(); if (centralWidget()) centralWidget()->update(); });
}

void MainWindow::HideAgentWorkspace() {
    if (!aiWorkspace_) return;
    if (aiWorkspace_->presenting) return;
    aiWorkspace_->presenting = true;
    if (aiWorkspace_->window) aiWorkspace_->window->hide();
    if (aiWorkspace_->sidebar) aiWorkspace_->sidebar->hide();
    if (workspacePages_) ShowBaseWorkspace();
    else {
        if (aiWorkspace_->stack) aiWorkspace_->stack->setCurrentWidget(Dock());
        Dock()->show();
    }
    if (aiWorkspace_->statusHiddenForPage) {
        statusBar()->setVisible(aiWorkspace_->statusWasVisible);
        aiWorkspace_->statusHiddenForPage = false;
    }
    if (const auto current = CurrentModule()) UndoStackChanged(*current);
    aiWorkspace_->presenting = false;
    SyncAssistantAction();
    QTimer::singleShot(0, this, [this] { SyncAssistantAction(); Dock()->update(); });
}

void MainWindow::ActivateModule(const War3Map::Shell::ModuleId& id) {
    if (aiWorkspace_ && aiWorkspace_->stack && aiWorkspace_->window &&
        aiWorkspace_->stack->currentWidget() == aiWorkspace_->window) HideAgentWorkspace();
    ShellWindow::ActivateModule(id);
    SyncAssistantAction();
}

void MainWindow::Navigate(const War3Map::Shell::NavigationTarget& target) {
    if (aiWorkspace_ && aiWorkspace_->stack && aiWorkspace_->window &&
        aiWorkspace_->stack->currentWidget() == aiWorkspace_->window) HideAgentWorkspace();
    ShellWindow::Navigate(target);
    SyncAssistantAction();
}

void MainWindow::ShowApplicationSettings() {
    SetupAgentWorkspace();
    if (!aiWorkspace_) return;
    auto* state = aiWorkspace_;
    SetupProductServices();
    War3Map::Agent::AgentSettingsDialog dialog(state->settings, this);
    auto* mapTestPage = new War3Map::QtApp::MapTestSettingsPage(War3Map::QtCommon::InstallationRoot());
    if (legacyMapBackupCheck_) {
        const QPointer<War3Map::QtApp::MapTestSettingsPage> page(mapTestPage);
        legacyMapBackupCheck_->SetObserver([page](QString text, bool busy) {
            if (!page) return;
            page->SetLegacyBackupInspectionText(text);
            page->SetLegacyBackupInspectionBusy(busy);
        });
        mapTestPage->SetLegacyBackupDirectoryRequestedHandler([this](QString directory) {
            legacyMapBackupCheck_->Start(directory);
        });
    }
    dialog.AddApplicationPage(tr("地图测试与备份"), mapTestPage);
    auto* recoveryPage = new War3Map::QtApp::RecoverySettingsPage;
    dialog.AddApplicationPage(tr("编辑恢复"), recoveryPage);
    auto* worldEffectsPage = new War3Map::QtApp::WorldEffectsSettingsPage;
    dialog.AddApplicationPage(tr("世界特效"), worldEffectsPage);
    auto* feedbackSection = feedbackClient_ ? new War3Map::QtFeedback::FeedbackSettingsSection(*feedbackClient_) : nullptr;
    if (feedbackSection) {
        dialog.AddGeneralSection(feedbackSection);
        dialog.AddApplicationPage(tr("反馈队列"), new War3Map::QtFeedback::FeedbackQueuePage(*feedbackClient_));
    }
    auto* updatePage = updateClient_ ? new War3Map::QtUpdate::UpdateSettingsWidget(*updateClient_) : nullptr;
    if (updatePage) {
        dialog.AddApplicationPage(tr("检查更新"), updatePage);
        connect(updatePage, &War3Map::QtUpdate::UpdateSettingsWidget::InstallationRequested, this, &MainWindow::BeginProductUpdate);
    }
    dialog.SetCredentialStore(&state->credentials);
    dialog.SetAvailableTools(state->tools->RegisteredToolNames());
    dialog.SetMcpAccessToken(state->token);
    const QSettings stored;
    const War3Map::Agent::ApplicationPreferences before{
        stored.value(QStringLiteral("war3/gameRoot")).toString(),
        stored.value(QStringLiteral("ui/dataRoot")).toString(),
        stored.value(QString::fromLatin1(War3Map::UiKit::kUiFontFamilySettingsKey)).toString(),
        themeManager_ ? themeManager_->CurrentThemeId() : QStringLiteral("graphite")};
    dialog.SetApplicationPreferences(before);
    dialog.SetCommitHandler([state, &dialog, feedbackSection, updatePage, mapTestPage, recoveryPage, worldEffectsPage](const War3Map::Agent::AgentSettings& settings, QString* error) {
        if (!mapTestPage->Validate(error) || !recoveryPage->Validate(error)) return false;
        auto services = mapTestPage->Preferences();
        const auto recovery = recoveryPage->Preferences();
        services.recoveryEnabled = recovery.recoveryEnabled;
        services.recoveryIntervalMinutes = recovery.recoveryIntervalMinutes;
        services.worldEffectQuality = worldEffectsPage->QualityName();
        if (feedbackSection) {
            if (!feedbackSection->Validate(error)) return false;
            const auto feedback = feedbackSection->Preferences();
            services.feedbackConsent = feedback.consent;
            services.feedbackEndpoint = feedback.endpointOverride;
        }
        if (updatePage) {
            if (!updatePage->Validate(error)) return false;
            const auto update = updatePage->Preferences();
            services.updateAutomatic = update.automaticCheckEnabled;
            services.updateChannel = update.channel == War3Map::Update::UpdateChannel::Beta ? QStringLiteral("beta") : QStringLiteral("stable");
            services.updateSkippedVersion = update.skippedVersion;
        }
        // 每次尝试创建新QSettings，先前IO错误不能通过sticky status污染重试。
        QSettings preferences;
        return War3Map::Agent::ApplicationSettingsCommit::Commit(preferences, state->store,
            settings, dialog.ApplicationSettings(), error, {}, services);
    });
    if (dialog.exec() != QDialog::Accepted) return;
    ApplyRecoveryPreferences(War3Map::Agent::LoadRecoveryPreferences(QSettings{}));
    if (worldModule_) worldModule_->ApplyEffectQualityFromSettings();
    QString productError;
    if (feedbackSection && !feedbackClient_->ApplyCommittedPreferences(feedbackSection->Preferences(), &productError))
        AddDiagnostic(tr("错误"), tr("反馈"), QStringLiteral("FEEDBACK_PREFERENCES_APPLY"), productError);
    if (updatePage && !updateClient_->ApplyCommittedPreferences(updatePage->Preferences(), &productError))
        AddDiagnostic(tr("错误"), tr("更新"), QStringLiteral("UPDATE_PREFERENCES_APPLY"), productError);
    const auto application = dialog.ApplicationSettings();
    if (application.uiFontFamily != before.uiFontFamily)
        War3Map::UiKit::ApplyPreferredUiFamily(application.uiFontFamily);
    if (themeManager_ && application.themeId != before.themeId) themeManager_->ApplyTheme(application.themeId);
    state->settings = dialog.Settings();
    ApplySettings(*state);
    QString message;
    if (!state->serviceError.isEmpty()) {
        AddDiagnostic(tr("错误"), tr("MCP"), QStringLiteral("MCP_LISTEN_FAILED"), state->serviceError);
        message = tr("设置已保存，MCP 未启动：%1").arg(state->serviceError);
    } else {
        message = state->http->IsListening() ? tr("设置已保存 · MCP %1").arg(state->http->EndpointUrl().toString())
                                            : tr("应用设置已保存");
    }
    if (application.gameRoot != before.gameRoot || application.uiDataRoot != before.uiDataRoot)
        message += tr(" · 资源目录将在下次启动时使用");
    ShowStatusMessage(message);
}

QString MainWindow::FeedbackConversationSnippet() const {
    return aiWorkspace_ && aiWorkspace_->controller
        ? aiWorkspace_->controller->CurrentFeedbackConversationSnippet() : QString{};
}

void MainWindow::StopAgentWorkspace() {
    if (!aiWorkspace_) return;
    HideAgentWorkspace();
    aiWorkspace_->presenting = true;
    const QPointer<QStackedWidget> stack = aiWorkspace_->stack;
    const QPointer<QDockWidget> sidebar = aiWorkspace_->sidebar;
    removeEventFilter(aiWorkspace_);
    delete aiWorkspace_;
    aiWorkspace_ = nullptr;
    if (sidebar) {
        removeDockWidget(sidebar);
        delete sidebar;
    }
    if (stack && centralWidget() == stack && !workspacePages_) {
        QWidget* presentation = takeCentralWidget();
        stack->removeWidget(Dock());
        Dock()->setParent(this);
        setCentralWidget(Dock());
        delete presentation;
    }
    SyncAssistantAction();
}
