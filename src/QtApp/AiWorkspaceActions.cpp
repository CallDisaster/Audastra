#include "MainWindow.h"
#include "TriggerModule.h"
#include "TriggerLuaScripts.h"
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
#include "WorldViewportToolService.h"
#include "GameUiModule.h"
#include "GameUiToolService.h"
#include "RecoveryPreferences.h"
#include "../QtCommon/ApplicationPaths.h"
#include "FeedbackClient.h"
#include "FeedbackDialog.h"
#include "UpdateClient.h"
#include "UpdateDialog.h"
#include "AgentWindow.h"
#include "CredentialStore.h"
#include "ExternalMcpManager.h"
#include "McpHttpTransport.h"
#include "ProjectMemoryService.h"
#include "TokenMeter.h"
#include "TriggerModule.h"
#include "Dock/DockHost.h"
#include "Fonts/FontLoader.h"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QRandomGenerator>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QSet>
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
        if (externalMcp) externalMcp->StopAll();
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
    std::unique_ptr<Agent::ProjectMemoryService> memory;
    std::unique_ptr<Agent::ExternalMcpManager> externalMcp;
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

// 助手改动列表里的对象：变量去变量表，其余（触发器、类别、注释）去触发器编辑器。
War3Map::Shell::NavigationTarget AgentChangeTarget(const QString& kind, const QString& id) {
    const auto module = kind == QStringLiteral("variable") ? QStringLiteral("variables") : QStringLiteral("triggers");
    return {module.toStdString(), kind, id, {{"stableIdentity", true}}};
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

QString BaseSystemPrompt() {
    return QStringLiteral(
        "你是 Audastra 地图编辑助手。使用实际提供的工具检查当前地图，然后通过同一事务修改。"
        "新增、优化逻辑默认写成作者可读可编辑的触发器（T，传统事件/条件/动作）；未经作者特别要求不转换为 JASS/Lua/WCT。"
        "每次写操作先取得当前地图身份、修订和图绑定；来源变化后重新查询，不猜 ID。"
        "若待创建的触发器数量较多或单个触发器节点复杂，请分批小步执行（例如先创建触发器骨架，再分批追加事件、条件与动作），避免单次输出过大被截断。"
        "当作者告知长期作图习惯、命名规范或项目约定时，可在允许修改的前提下使用 memory_write 记录到当前地图专属记忆文件。"
        "修改后回读实际结果，向作者说明修改的触发器、前后值、保存结果与未验证项。"
        "失败保留诊断，不放宽类型或保存门禁。工具未提供的能力准确说明不可用。"
        "当前 editor_* 工具操作 GUI 活动地图；旧独立 stdio 工具属于另一个工作区。\n");
}

struct LoadedSkills {
    QString prompt;
    QList<War3Map::Agent::ContextNamedItem> items;
};

LoadedSkills LoadSkillsPayload() {
    LoadedSkills out;
    const auto root = SkillsDirectory();
    for (const auto* skill : {"forge-editor-mcp", "forge-t-event-optimization"}) {
        const QString skillName = QString::fromLatin1(skill);
        const QString filePath = QString::fromStdWString((root / skill / "SKILL.md").wstring());
        QFile file(filePath);
        if (file.open(QIODevice::ReadOnly) && file.size() <= 65536) {
            const QString body = QString::fromUtf8(file.readAll());
            out.prompt += QStringLiteral("\n") + body;
            out.items.push_back({skillName, filePath, War3Map::Agent::TokenMeter::EstimateText(body)});
        }
    }
    return out;
}

void OpenProjectMemoryEditor(QWidget* parent, War3Map::Agent::ProjectMemoryService& memory, const QString& initialScope) {
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("项目与全局持久记忆"));
    dlg.resize(640, 480);
    auto* root = new QVBoxLayout(&dlg);
    root->setSpacing(8);

    auto* topRow = new QHBoxLayout;
    auto* scopeCombo = new QComboBox(&dlg);
    const QString projPath = memory.ProjectMemoryFilePath();
    if (!projPath.isEmpty()) {
        scopeCombo->addItem(QStringLiteral("当前地图记忆（%1）").arg(QFileInfo(projPath).fileName()), QStringLiteral("project"));
    }
    if (!memory.GlobalMemoryFilePath().isEmpty()) {
        scopeCombo->addItem(QStringLiteral("全局通用记忆（MEMORY.md）"), QStringLiteral("global"));
    }
    if (scopeCombo->count() == 0) {
        scopeCombo->addItem(QStringLiteral("当前未打开地图（请先打开地图后再编辑项目记忆）"), QStringLiteral("none"));
        scopeCombo->setEnabled(false);
    } else {
        const int idx = scopeCombo->findData(initialScope);
        if (idx >= 0) scopeCombo->setCurrentIndex(idx);
    }
    topRow->addWidget(scopeCombo, 1);
    auto* tokenBadge = new QLabel(&dlg);
    topRow->addWidget(tokenBadge);
    root->addLayout(topRow);

    auto* pathLabel = new QLabel(&dlg);
    pathLabel->setWordWrap(true);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(pathLabel);

    auto* editor = new QPlainTextEdit(&dlg);
    editor->setPlaceholderText(QStringLiteral("# 地图作图习惯与架构记忆\n- 单位/物品/变量命名约定：\n- 核心触发器分类与职责：\n- 特殊注意事项："));
    root->addWidget(editor, 1);

    auto* statusLabel = new QLabel(&dlg);
    statusLabel->setWordWrap(true);
    root->addWidget(statusLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存记忆"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("关闭"));
    root->addWidget(buttons);

    const auto updateTokenCount = [&] {
        const qint64 t = War3Map::Agent::TokenMeter::EstimateText(editor->toPlainText());
        tokenBadge->setText(QStringLiteral("约 %1 tokens").arg(t));
    };
    const auto loadCurrentScope = [&] {
        const QString scope = scopeCombo->currentData().toString();
        auto* save = buttons->button(QDialogButtonBox::Save);
        if (scope == QStringLiteral("none")) {
            editor->setEnabled(false);
            save->setEnabled(false);
            pathLabel->clear();
            return;
        }
        const auto snap = memory.ReadScope(scope);
        pathLabel->setText(QStringLiteral("文件路径：%1%2").arg(snap.filePath, snap.exists ? QString{} : QStringLiteral("（保存后创建）")));
        editor->setEnabled(true);
        save->setEnabled(true);
        editor->setPlainText(snap.content);
        // 未确认的地图记忆（如随地图附带）不提供给助手；作者在此保存即视为确认。
        save->setText(snap.trusted ? QStringLiteral("保存记忆") : QStringLiteral("确认并保存"));
        statusLabel->setText(!snap.error.isEmpty() ? snap.error
            : snap.trusted ? QString{} : QStringLiteral("未确认"));
        updateTokenCount();
    };
    QObject::connect(scopeCombo, &QComboBox::currentIndexChanged, &dlg, loadCurrentScope);
    QObject::connect(editor, &QPlainTextEdit::textChanged, &dlg, updateTokenCount);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        const QString scope = scopeCombo->currentData().toString();
        QString err;
        if (!memory.WriteScope(scope, editor->toPlainText(), QStringLiteral("replace"), &err)) {
            statusLabel->setText(err);
            return;
        }
        dlg.accept();
    });
    loadCurrentScope();
    dlg.exec();
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

// 把 MCP 服务与内置工具注册表的实际状态推给助手窗口；窗口不按设置推测运行状态。
void PublishConnectorStatus(War3Map::QtApp::AiWorkspace& state) {
    if (!state.window || !state.tools || !state.http) return;
    War3Map::Agent::AgentConnectorStatus status;
    status.mcpListening = state.http->IsListening();
    if (status.mcpListening) status.mcpEndpoint = state.http->EndpointUrl().toString();
    status.mcpError = state.serviceError;
    const QStringList registered = state.tools->RegisteredToolNames();
    status.registeredTools = static_cast<int>(registered.size());
    // 只数实际注册过的工具；设置里残留的旧工具名不计入禁用数。
    QSet<QString> disabled;
    for (const auto& name : state.settings.disabledTools) {
        if (registered.contains(name)) disabled.insert(name);
    }
    status.disabledTools = static_cast<int>(disabled.size());
    if (state.externalMcp) status.externalServers = state.externalMcp->Statuses();
    state.window->SetConnectorStatus(status);
}

void RestartMcpService(War3Map::QtApp::AiWorkspace& state) {
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

void ApplySettings(War3Map::QtApp::AiWorkspace& state) {
    state.tools->SetPolicy(Policy(state.settings));
    if (state.externalMcp) state.externalMcp->ApplySettings(state.settings);
    state.controller->SetSettings(state.settings);
    if (state.window) state.window->SetSettings(state.settings);
    RestartMcpService(state);
    PublishConnectorStatus(state);
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
    PublishConnectorStatus(state);
    tools = state.tools.get();
    return state.http->Port();
}

void MainWindow::SetupAgentWorkspace(bool readUserConfiguration) {
    if (aiWorkspace_ || (readUserConfiguration && startupMode_ != StartupMode::Normal)) return;
    auto* state = new War3Map::QtApp::AiWorkspace(this);
    aiWorkspace_ = state;
    QString loadError;
    QString loadWarning;
    // 离屏宿主测试显式用空配置；不接触作者设置、凭据、端口或站点。
    if (readUserConfiguration) state->settings = state->store.Load(&loadError, &loadWarning);
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
    state->memory = std::make_unique<War3Map::Agent::ProjectMemoryService>();
    if (readUserConfiguration) state->memory->UseDefaultGlobalMemoryPath();
    state->externalMcp = std::make_unique<War3Map::Agent::ExternalMcpManager>();
    state->externalMcp->SetCredentialStore(&state->credentials);
    state->controller = std::make_unique<War3Map::Agent::AgentController>();
    state->controller->SetSystemPrompt(BaseSystemPrompt());
    const auto skills = LoadSkillsPayload();
    state->controller->SetSkillsPrompt(skills.prompt, skills.items);
    state->controller->SetMemoryService(state->memory.get());
    state->controller->SetToolsProvider([state]() { return state->tools->Tools(); });
    state->controller->SetExternalToolsProvider([state]() {
        return state->externalMcp ? state->externalMcp->EnabledToolDefinitions() : QJsonArray{};
    });
    state->controller->SetMcpInstructionsProvider([state]() {
        return state->externalMcp ? state->externalMcp->CombinedServerInstructions() : QString{};
    });
    state->controller->SetToolDispatcher([state](const QString& name, const QJsonObject& args) {
        if (state->externalMcp && state->externalMcp->OwnsTool(name)) {
            return state->externalMcp->InvokeTool(name, args);
        }
        const QScopedValueRollback<bool> dispatchScope(state->controllerDispatchActive, true);
        return state->tools->Invoke(name, args);
    });
    connect(state->externalMcp.get(), &War3Map::Agent::ExternalMcpManager::StatusChanged, state, [state]() {
        PublishConnectorStatus(*state);
        if (state->controller) state->controller->RefreshContextComposition();
    });
    state->tools->SetBeforeWrite([this]() {
        if (project_->savingMap_) return false;
        return triggersModule_->CommitPendingEdits();
    });
    state->tools->SetSaveHandler([this](const std::filesystem::path& path, bool inPlace, bool keepOriginalMinimap, bool rebuildShadow) {
        return RunMapSave(path, inPlace, true, false, false, War3Map::Services::TerrainMinimapSaveChoice::Automatic, rebuildShadow,
            War3Map::Services::MapRuntimeSavePurpose::Normal, keepOriginalMinimap ? MinimapRiskSave::Accept : MinimapRiskSave::Decline);
    });
    state->tools->SetValidationSaveHandler([this](const std::filesystem::path& path) {
        return RunMapSave(path, false, true, true, false, War3Map::Services::TerrainMinimapSaveChoice::Automatic,
            false, War3Map::Services::MapRuntimeSavePurpose::InternalValidation, MinimapRiskSave::Decline);
    });
    state->tools->SetLuaTestHandler([this](bool submit) { return LuaTestReload(submit); });
    state->tools->SetMapTestHandler([this](const QString& name, const QJsonObject& arguments) { return MapTestTool(name, arguments); });
    const QPointer<War3Map::Modules::WorldModule> world(worldModule_);
    auto viewportTools = std::make_shared<War3Map::Modules::WorldViewportToolService>([world]() {
        return world ? world->ViewportForTools() : nullptr;
    }, [world]() { return world ? world->ScatterForTools() : nullptr; });
    state->tools->SetWorldViewportHandler([viewportTools](const QString& name, const QJsonObject& arguments) {
        return viewportTools->Invoke(name, arguments);
    });
    const QPointer<War3Map::Modules::GameUiModule> gameUi(gameUiModule_);
    auto gameUiTools = std::make_shared<War3Map::Modules::GameUiToolService>([gameUi]() { return gameUi.data(); });
    state->tools->SetGameUiHandler([gameUiTools](const QString& name, const QJsonObject& arguments) {
        return gameUiTools->Invoke(name, arguments);
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
    if (!loadWarning.isEmpty()) AddDiagnostic(tr("警告"), tr("助手"), QStringLiteral("AGENT_EXTERNAL_MCP_SKIPPED"), loadWarning);
    if (!state->serviceError.isEmpty()) AddDiagnostic(tr("错误"), tr("MCP"), QStringLiteral("MCP_LISTEN_FAILED"), state->serviceError);
    RefreshAgentMapContext();
}

void MainWindow::RefreshAgentMapContext() {
    if (!aiWorkspace_) return;
    const auto path = project_->sourceArchivePath_.empty() ? project_->projectRoot_ : project_->sourceArchivePath_;
    const auto mapPathString = path.empty() ? QString() : QString::fromStdWString(path.wstring());
    if (aiWorkspace_->memory) aiWorkspace_->memory->SetMapSourcePath(mapPathString);
    const auto title = path.empty() ? tr("未打开地图") : QString::fromStdWString(path.filename().wstring());
    const auto identity = path.empty() ? QString() : mapPathString + QLatin1Char('|')
        + QString::number(project_->editRevision_.Generation());
    const auto conversationIdentity = path.empty() ? QStringLiteral("forge:no-map")
        : QDir::cleanPath(QFileInfo(mapPathString).absoluteFilePath()).toCaseFolded();
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
        PublishConnectorStatus(*state);
        connect(state->window, &War3Map::Agent::AgentWindow::PromptSubmitted, state,
            [state](const QString& text, const QString& provider, const QString& model) {
                state->controller->StartPrompt(text, provider, model);
                // Invalid settings/credentials can reject before the controller becomes busy.
                state->window->SetBusy(state->controller->IsBusy());
            });
        connect(state->window, &War3Map::Agent::AgentWindow::CompactRequested, state,
            [state](const QString& provider, const QString& model) {
                state->controller->CompactConversation(provider, model);
                state->window->SetBusy(state->controller->IsBusy());
            });
        connect(state->window, &War3Map::Agent::AgentWindow::EditMemoryRequested, this,
            [this, state](const QString& scope) {
                if (state->memory) OpenProjectMemoryEditor(this, *state->memory, scope);
            });
        connect(state->window, &War3Map::Agent::AgentWindow::ExternalMcpReconnectRequested, state,
            [state](const QString& serverId) {
                if (state->externalMcp) state->externalMcp->Reconnect(serverId);
            });
        connect(state->window, &War3Map::Agent::AgentWindow::ModelThinkingConfigChanged, this,
            [this, state](const QString& providerId, const QString& modelId,
                const QString& selectedLevel, bool levelsCustomized, const QStringList& customLevels) {
                auto next = state->settings;
                bool found = false;
                for (auto& provider : next.providers) {
                    if (provider.id != providerId) continue;
                    for (auto& model : provider.models) {
                        if (model.id != modelId) continue;
                        model.selectedThinkingLevel = selectedLevel;
                        model.thinkingLevelsCustomized = levelsCustomized;
                        model.customThinkingLevels = customLevels;
                        found = true;
                        break;
                    }
                    if (found) break;
                }
                if (!found) return;
                QString error;
                if (!state->store.Save(next, &error)) {
                    AddDiagnostic(tr("错误"), tr("助手"), QStringLiteral("AGENT_SETTINGS_SAVE_FAILED"), error);
                    state->window->SetSettings(state->settings);
                    return;
                }
                state->settings = next;
                state->controller->SetSettings(state->settings);
                state->window->SetSettings(state->settings);
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
                Navigate(AgentChangeTarget(kind, id));
                activateWindow();
            });
        connect(state->window, &War3Map::Agent::AgentWindow::FollowRequested, this,
            [this, state](const QString& kind, const QString& id) {
                // 助手占整页时编辑区不可见，不跟随；跳转不激活窗口，不打断作者正在输入的地方。
                if (state->stack && state->stack->currentWidget() == state->window) return;
                ShellWindow::Navigate(AgentChangeTarget(kind, id));
            });
        connect(state->window, &War3Map::Agent::AgentWindow::FollowChangesChangeRequested, this,
            [this, state](bool follow) {
                auto next = state->settings;
                next.followChanges = follow;
                QString error;
                if (!state->store.Save(next, &error)) {
                    AddDiagnostic(tr("错误"), tr("助手"), QStringLiteral("AGENT_SETTINGS_SAVE_FAILED"), error);
                    state->window->SetSettings(state->settings);
                    return;
                }
                state->settings = next;
                state->window->SetSettings(state->settings);
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
    dialog.AddApplicationPage(tr("地图测试与备份"), mapTestPage, QStringLiteral("lucide/play"));
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
    QStringList allAvailableTools = state->tools->RegisteredToolNames();
    allAvailableTools << QStringLiteral("memory_read") << QStringLiteral("memory_write");
    if (state->externalMcp) allAvailableTools.append(state->externalMcp->AllDiscoveredToolNames());
    dialog.SetAvailableTools(allAvailableTools);
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
        services.renderAdapter = worldEffectsPage->RenderAdapterKey();
        services.renderAdapterName = worldEffectsPage->RenderAdapterName();
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
    triggersModule_->LuaScripts()->RefreshHLuaToolbar();
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
