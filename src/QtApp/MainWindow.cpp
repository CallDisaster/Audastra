#include "MainWindow.h"
#include "CampaignModule.h"
#include "LegacyMapBackupCheck.h"
#include "../QtCommon/ApplicationPaths.h"
#include "RecoveryPreferences.h"
#include "WorldFrameRecorder.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"
#include "AppHelpers.h"
#include "AssetsModule.h"
#include "ManagerRows.h"
#include "ObjectModule.h"
#include "TriggerBlueprint.h"
#include "TriggerModule.h"
#include "VariablesModule.h"
#include <QTemporaryDir>
#include "TriggerCommands.h"
#include "FeedbackClient.h"
#include "UpdateClient.h"

#include "War3MapCore/CompatibilityProfile.h"
#include "War3MapCore/EditorSession.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Workspace.h"
#include "Icons/IconProvider.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QSortFilterProxyModel>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QThread>
#include <QTextDocument>
#include <QIcon>
#include <QPixmap>
#include <QToolBar>
#include <QTreeView>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

using namespace AppDetail;

MainWindow::MainWindow(QWidget* parent)
    : MainWindow(StartupMode::Normal, parent) {
}

MainWindow::MainWindow(StartupMode startupMode, QWidget* parent)
    : MainWindow(startupMode, parent, QString{}) {
}

MainWindow::MainWindow(StartupMode startupMode, QWidget* parent, const QString& recentMapsDataDirectory)
    : EditorParts(startupMode == StartupMode::Normal),
      ShellWindow(modules_, shellSettings_.get(), parent),
      startupMode_(startupMode) {
    if (startupMode == StartupMode::Normal || !recentMapsDataDirectory.isEmpty())
        recentMapsStore_ = std::make_unique<War3Map::QtProject::RecentMapsStore>(recentMapsDataDirectory, 0);
    UpdateWindowTitle();
    // 宽度沿用共享外壳的下限，让响应式工具栏在1024宽度下正常折叠。
    setMinimumHeight(680);
    setDockOptions(AllowNestedDocks | AllowTabbedDocks | AnimatedDocks);
    // 实际模块的控件先全部创建：打开地图、切换游戏资源时可以直接刷新任何模块，
    // 不必等它第一次显示。控件仍由外壳按布局安置。
    (void)triggersModule_->Widget();
    (void)objectsModule_->Widget();
    (void)variablesModule_->Widget();
    (void)assetsModule_->Widget();
    ConnectModules();
    BuildHeader();
    SetupStartupPage();
    BuildStatusBar();
    BuildDiagnosticsView();
    ApplyTheme();

    SetupCommandPalette();
    SetupChangesPanel();
    SetupMapFileDrop();
    SetupCampaignActions();

    const QPointer<War3Map::QtProject::EditorProject> triggerObserverProject(project_.get());
    const QPointer<QUndoStack> triggerObserverStack(project_->triggerUndoStack_);
    connect(triggerObserverStack.data(), &QUndoStack::indexChanged, this, [this, triggerObserverProject, triggerObserverStack]() {
        const QPointer<MainWindow> liveWindow(this);
        const auto currentBinding = [&]() {
            return liveWindow && triggerObserverProject && triggerObserverStack
                && liveWindow->project_.get() == triggerObserverProject.data()
                && triggerObserverProject->triggerUndoStack_ == triggerObserverStack.data();
        };
        if (!currentBinding() || sender() != triggerObserverStack.data()) return;
        // 触发器模块自己订阅撤销栈并刷新目录与逻辑区。
        triggerObserverProject->triggerDirty_ = triggerObserverProject->restoredTriggerOverlay_ || !triggerObserverStack->isClean();
        ObserveTriggerEdit();
        if (!currentBinding()) return;
        UpdateExportAvailability();
        if (!currentBinding()) return;
        statusBar()->showMessage(
            triggerObserverProject->triggerDirty_
                ? tr("触发器存在未导出的修改 · Ctrl+Z 撤销 · 源目录仍只读")
                : tr("触发器修改已回到打开时状态 · 源目录只读"));
    });
    project_->recoveryTimer_ = new QTimer(this);
    project_->recoveryTimer_->setSingleShot(true);
    ApplyRecoveryPreferences(startupMode_ == StartupMode::Normal
        ? War3Map::Agent::LoadRecoveryPreferences(QSettings{}) : War3Map::Agent::RecoveryPreferences{});
    connect(project_->recoveryTimer_, &QTimer::timeout, this, &MainWindow::WriteRecovery);
    connect(triggerObserverStack.data(), &QUndoStack::indexChanged, this, [this, triggerObserverProject, triggerObserverStack]() {
        if (!triggerObserverProject || !triggerObserverStack || project_.get() != triggerObserverProject.data()
            || triggerObserverProject->triggerUndoStack_ != triggerObserverStack.data()
            || sender() != triggerObserverStack.data()) return;
        ScheduleRecovery();
    });
    connect(project_->mapDataUndoStack_, &QUndoStack::indexChanged, this, [this]() {
        (void)project_->ObserveMapDataEdit("修改公共地图数据");
        ScheduleRecovery();
        UpdateEditDirtyFlags();
    });
    for (int i = 0; i < 7; ++i)
        connect(project_->objectUndoStacks_[i], &QUndoStack::indexChanged, this, [this, i]() {
            ObserveObjectEdit(i);
            ScheduleRecovery();
        });

    statusBar()->showMessage(tr("未打开地图"));

    // 启动探针只读取命令行指定工作区，不接触用户持久化设置或 Warcraft 安装目录。
    if (startupMode_ == StartupMode::Normal) {
        legacyMapBackupCheck_ = new War3Map::QtProject::LegacyMapBackupCheck(
            War3Map::QtCommon::InstallationRoot(), this);
        // 目录与大备份 SHA 的只读核验均在后台；启动探针不碰本机历史位置。
        legacyMapBackupCheck_->Start();
        LoadUiDataRootFromSettings();
        LoadGameRootFromSettings();
        SetupAgentWorkspace();
        SetupProductServices();
    }
    UpdateBreadcrumb();
    SetupProjectNotices();
    ShowRecoveryNotice();
    // 保存地图期间（工作线程正在写文件）不允许关闭。
    SetCloseGuard([this]() {
        const QPointer<MainWindow> window(this);
        if (project_->savingMap_ || changesChecking_) return false;
        if (!WaitForRecoveryExit() || !window) return false;
        if (!window->ConfirmDiscardPendingEdits(window->productUpdateExit_ ? tr("更新") : tr("关闭"), !window->productUpdateExit_)) return false;
        if (!window || !window->campaignModule_->ConfirmClose(window->productUpdateExit_ ? tr("更新") : tr("关闭"), !window->productUpdateExit_)) return false;
        return window && (!window->productUpdateExit_ || window->CommitProductUpdateExit());
    });
}

MainWindow::~MainWindow() {
    if (productWorker_) {
        productWorker_->wait();
        delete productWorker_;
        productWorker_ = nullptr;
    }
    // 先停止产品工作线程，避免关闭过程中访问已经释放的诊断模型或当前会话。
    delete feedbackClient_;
    feedbackClient_ = nullptr;
    delete updateClient_;
    updateClient_ = nullptr;
    StopAgentWorkspace();
    // 后台测试的游戏看不见，编辑器关闭时一并结束；用户桌面上的测试游戏照常留着。
    if (mapTestLease_ && mapTestLease_->HiddenDesktop() && mapTestLease_->IsActive()) (void)mapTestLease_->Stop();
    if (worldFrameRecorder_) worldFrameRecorder_->Stop(tr("窗口关闭"));
    // UndoStack 析构会 clear 并发射 indexChanged；先停通知再释放其引用的视图/文档。
    for (auto* stack : project_->objectUndoStacks_) {
        stack->blockSignals(true);
        stack->clear();
    }
    if (project_->triggerUndoStack_) {
        project_->triggerUndoStack_->blockSignals(true);
        project_->triggerUndoStack_->clear();
    }
    // 外壳析构时交还模块控件，模块随后释放；工程文档最后释放。
}

bool MainWindow::HasLoadedTriggerAndObjectModels() const noexcept {
    const auto* unitDocument = project_->objectDocuments_[0].get();
    return triggersModule_ != nullptr && triggersModule_->HasLoadedContent() &&
        unitDocument != nullptr && !unitDocument->Objects().empty() &&
        objectsModule_ != nullptr && objectsModule_->HasLoadedContent();
}

QStringList MainWindow::LoadedStateSummary() const {
    QStringList lines = project_->DescribeLoadedState();
    // 物体管理各页的行构建耗时（验收记录用）。
    using War3Map::Modules::Manager::Page;
    QElapsedTimer timer;
    timer.start();
    const auto overview = War3Map::Modules::Manager::BuildOverview(*project_);
    lines << QStringLiteral("manager overview lines=%1 ms=%2").arg(overview.size()).arg(timer.restart());
    for (int page = static_cast<int>(Page::Unit); page < static_cast<int>(Page::Count); ++page) {
        const auto rows = War3Map::Modules::Manager::BuildRows(*project_, static_cast<Page>(page), false);
        lines << QStringLiteral("manager %1 rows=%2 ms=%3")
            .arg(War3Map::Modules::Manager::PageLabel(static_cast<Page>(page))).arg(rows.size()).arg(timer.restart());
    }
    // 蓝图：语句最多的触发器整张布局一次的耗时（验收记录用）。
    if (const auto* document = project_->triggerDocument_.get(); document != nullptr && project_->uiCatalog_ != nullptr) {
        std::size_t largest = 0;
        War3Map::StableId largestId = 0;
        for (const auto& trigger : document->triggers) {
            std::size_t count = 0;
            std::vector<std::uint32_t> pending(trigger.ecas.begin(), trigger.ecas.end());
            while (!pending.empty() && count < document->ecaNodes.size()) {
                const std::uint32_t node = pending.back();
                pending.pop_back();
                if (node >= document->ecaNodes.size()) continue;
                ++count;
                pending.insert(pending.end(), document->ecaNodes[node].children.begin(), document->ecaNodes[node].children.end());
            }
            if (count > largest) {
                largest = count;
                largestId = trigger.id;
            }
        }
        TriggerBlueprint blueprint;
        blueprint.resize(1400, 900);
        blueprint.SetSource(document, project_->uiCatalog_.get(), {});
        timer.restart();
        blueprint.ShowTrigger(largestId);
        lines << QStringLiteral("blueprint largest trigger ecas=%1 nodes=%2 wires=%3 ms=%4")
            .arg(largest).arg(blueprint.NodeCount()).arg(blueprint.WireCount()).arg(timer.restart());
    }
    return lines;
}

void MainWindow::AddDiagnostic(const QString& level, const QString& subsystem,
    const QString& code, const QString& message) {
    project_->AddDiagnostic(level, subsystem, code, message);
}

void MainWindow::ApplyTheme() {
    if (themeManager_ == nullptr) {
        themeManager_ = new War3Map::UiKit::ThemeManager(this);
        connect(themeManager_, &War3Map::UiKit::ThemeManager::ThemeChanged,
            this, &MainWindow::UpdateStatusBar);
    }
    // 用户选择的主题保存在本机设置；启动探针使用默认主题。
    const QString saved = startupMode_ == StartupMode::Normal
        ? QSettings().value(QStringLiteral("ui/theme")).toString()
        : QString();
    themeManager_->ApplyTheme(saved);
}

void MainWindow::UpdateBreadcrumb() {
    if (breadcrumb_ == nullptr) {
        return;
    }
    const auto current = CurrentModule().value_or(War3Map::Shell::ModuleId{});
    QString path;
    if (current == "objects") {
        path = objectsModule_->BreadcrumbText();
    } else if (current == "variables") {
        path = tr("全局变量");
    } else if (auto* module = modules_.Find(current)) {
        path = module->Info().label;
    } else {
        path = tr("概览");
    }
    breadcrumb_->setText(path);
}

void MainWindow::UpdateStatusBar() {
    UpdateWindowTitle();
    if (statusModCount_ != nullptr) {
        int modCount = 0;
        for (bool dirty : project_->objectDirty_) {
            if (dirty) ++modCount;
        }
        if (project_->triggerDirty_) ++modCount;
        if (project_->mapEdits_ && project_->mapEdits_->IsDirty()) ++modCount;
        if (project_->editTrackingReady_ && !project_->editTrackingFailed_) {
            statusModCount_->setText(tr("工程: %1 · 地图: %2 · 修订 %3")
                .arg(project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project) ? tr("未保存") : tr("已同步"))
                .arg(project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map) ? tr("未保存") : tr("已同步"))
                .arg(project_->editRevision_.Revision()));
        } else statusModCount_->setText(tr("修改: %1").arg(modCount));
        // 修改计数 > 0 时使用当前主题的警告色；切换主题时同步刷新。
        const auto amber = themeManager_ != nullptr
            ? themeManager_->Current().colors.amber
            : War3Map::UiKit::ThemeManager::Theme(QString{}).colors.amber;
        statusModCount_->setStyleSheet(modCount > 0
            ? QStringLiteral("color: %1; font-weight: 700;").arg(War3Map::UiKit::ToCss(amber))
            : QString());
    }
    if (statusConfig_ != nullptr) {
        statusConfig_->setText(project_->projectRoot_.empty()
            ? tr("未加载配置")
            : tr("项目: %1").arg(QString::fromStdWString(
                (project_->projectFilePath_.empty() ? project_->projectRoot_ : project_->projectFilePath_).filename().wstring())));
    }
}

void MainWindow::ResetProjectState() {
    const QPointer<MainWindow> liveWindow(this);
    const QPointer<War3Map::QtProject::EditorProject> liveProject(project_.get());
    ResetNavigationHistory();
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return;
    ResetProjectWatch();
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return;
    (void)project_->ClearLuaMapTarget();
    project_->editTrackingReady_ = false;
    project_->editTrackingFailed_ = false;
    (void)project_->editRevision_.Reset();
    project_->projectFilePath_.clear();
    project_->projectFileSha256_.clear();
    project_->restoredProjectJournalJson_.clear();
    project_->restoredTriggerOverlay_ = false;
    if (project_->recoveryTimer_) project_->recoveryTimer_->stop();
    project_->archiveSession_.reset();
    project_->readOnlyArchive_.reset();
    project_->mapDocument_.reset();
    mapDataAction_->setEnabled(false);
    project_->directorySnapshot_.reset();
    project_->recoveryPath_.clear();
    project_->recoveryFileSha256_.clear();
    project_->lastProjectFingerprint_.clear();
    objectsModule_->ResetView();
    for (auto* stack : project_->objectUndoStacks_) stack->clear();
    if (project_->triggerUndoStack_ != nullptr) {
        project_->triggerUndoStack_->clear();
        project_->triggerUndoStack_->setClean();
    }
    project_->triggerDirty_ = false;
    // “仅对此地图启用”的函数库只属于上一张地图。
    project_->uiLibraryMapOnly_.clear();
    project_->uiLibraryUsage_.reset();
    project_->session_.reset();
    project_->projectRoot_.clear();
    project_->sourceArchivePath_.clear();
    project_->pendingImports_.clear();
    project_->assetEdits_.clear();
    project_->iconProvider_->SetMapArchive({});
    project_->iconProvider_->SetMapResourceDirectory({});
    // 只清理项目会话的活动根；用户持久化的外部 UI 根保留。
    project_->activeUiDataRoot_.clear();
    project_->compilerDataRoot_.clear();
    project_->triggerStatus_.clear();
    project_->ReplaceTriggerWorkspace(nullptr, nullptr);
    RefreshVariablesPage();
    for (auto& document : project_->objectDocuments_) {
        document.reset();
    }
    for (auto& document : project_->nativeObjectDocuments_) {
        document.reset();
    }
    project_->restoredFiles_.reset();
    project_->mapWorkspace_.reset();
    // 来源通知会同步刷新物编页面；等旧文档全部释放后再通知，避免页面重新引用待销毁文档。
    project_->NotifyImportsChanged();
    objectsModule_->UpdateObjectReferenceEntries();
    project_->objectDirty_.fill(false);
    exportAction_->setEnabled(false);
    UpdateWindowTitle();
}

void MainWindow::UpdateWindowTitle() {
    if (CurrentModule() == "campaign" && campaignModule_->Session()) {
        const auto& session = *campaignModule_->Session();
        setWindowTitle(tr("%1%2 — 战役 — Audastra").arg(campaignModule_->Dirty() ? "● " : "",
            QString::fromUtf8(session.Info().name).isEmpty() ? tr("未命名战役") : QString::fromUtf8(session.Info().name)));
        return;
    }
    const auto& path = project_->sourceArchivePath_.empty() ? project_->projectRoot_ : project_->sourceArchivePath_;
    if (path.empty()) {
        setWindowTitle(QStringLiteral("Audastra · Per Audacia Ad Astra"));
        return;
    }
    if (workspacePages_ && workspacePages_->currentWidget() == startupPage_) ShowBaseWorkspace();
    // 地图标题以地图保存基线为准；单独保存 .forge 工程不代表地图已经保存。
    const bool tracked = project_->editTrackingReady_ && !project_->editTrackingFailed_;
    const bool dirty = (tracked ? project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map)
        : project_->triggerDirty_ || project_->HasPendingAssetEdits()
            || std::any_of(project_->objectDirty_.begin(), project_->objectDirty_.end(), [](bool value) { return value; })
            || (project_->mapEdits_ && project_->mapEdits_->IsDirty()))
        || settingsModule_->HasPendingDraft() || soundsModule_->HasPendingDraft() || triggersModule_->HasPendingCodeDraft();
    setWindowTitle(QStringLiteral("%1%2 — Audastra")
        .arg(dirty ? QStringLiteral("● ") : QString(), QString::fromStdWString(path.filename().wstring())));
}

bool MainWindow::CheckPendingTerrainCommit(const QString& action) {
    if (!project_->HasPendingTerrainCommit()) return true;
    const auto message = tr("地形操作正在核对并提交，请完成或取消后再%1；本次尚未保存或切换。").arg(action);
    AddDiagnostic(tr("警告"), tr("地形"), tr("TERRAIN_COMMIT_PENDING"), message);
    statusBar()->showMessage(message, 10000);
    return false;
}

bool MainWindow::ConfirmDiscardPendingEdits(const QString& action, bool allowDiscard) {
    if (!CheckPendingTerrainCommit(action)) return false;
    // 提交失败的代码仍参加保存 / 放弃提示，用户可明确放弃，无须被无效草稿困住。
    (void)triggersModule_->CommitPendingEdits();
    const bool settingsDraft = settingsModule_->HasPendingDraft();
    const bool soundDraft = soundsModule_->HasPendingDraft();
    const bool codeDraft = triggersModule_->HasPendingCodeDraft();
    const bool tracked = project_->editTrackingReady_ && ObserveEditState("切换或关闭前核对");
    if (tracked && !settingsDraft && !soundDraft && !codeDraft) {
        // 已打开工程后，以该工程为关闭依据。撤销回原图也可能是尚未保存的工程修改。
        const auto target = project_->projectFilePath_.empty() ? War3Map::EditSaveTarget::Map : War3Map::EditSaveTarget::Project;
        if (!project_->editRevision_.IsDirty(target)) return true;
    }
    const bool objectDirty = std::any_of(
        project_->objectDirty_.begin(), project_->objectDirty_.end(), [](bool dirty) { return dirty; });
    const bool mapDataDirty = project_->mapEdits_ && project_->mapEdits_->IsDirty();
    if (!settingsDraft && !soundDraft && !codeDraft && !tracked && !project_->editTrackingFailed_ && !project_->triggerDirty_
        && !objectDirty && !mapDataDirty && !project_->HasPendingAssetEdits()) {
        return true;
    }
    if (!settingsDraft && !soundDraft && !codeDraft && !tracked && !project_->editTrackingFailed_ && !project_->lastProjectFingerprint_.empty()
        && project_->lastProjectFingerprint_ == ProjectFingerprint()) return true;

    QMessageBox prompt(
        QMessageBox::Warning,
        tr("存在未保存的修改"),
        tr("保存工程后可恢复当前编辑。"),
        QMessageBox::NoButton,
        this);
    auto* keepEditing = prompt.addButton(tr("继续编辑"), QMessageBox::RejectRole);
    auto* save = prompt.addButton(tr("保存工程并%1").arg(action), QMessageBox::AcceptRole);
    auto* discard = allowDiscard ? prompt.addButton(
        tr("放弃修改并%1").arg(action),
        QMessageBox::DestructiveRole) : nullptr;
    prompt.setDefaultButton(qobject_cast<QPushButton*>(keepEditing));
    prompt.exec();
    if (prompt.clickedButton() == save) {
        SaveEditorProject();
        if (triggersModule_->HasPendingCodeDraft() || settingsModule_->HasPendingDraft() || soundsModule_->HasPendingDraft()) return false;
        if (project_->editTrackingReady_) return !project_->editTrackingFailed_ && !project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project);
        return !project_->lastProjectFingerprint_.empty() && project_->lastProjectFingerprint_ == ProjectFingerprint();
    }
    if (discard && prompt.clickedButton() == discard) {
        triggersModule_->DiscardPendingCodeDraft();
        return true;
    }
    return false;
}

// 按 ui/config 中的层序合并 define.txt（分类→纹理、函数→分类）。
// 第三方层可以任意命名且可缺失，GUI 不包含任何库名或函数名白名单。

void MainWindow::UpdateExportAvailability() {
    const bool open = !project_->projectRoot_.empty();
    const bool campaign = CurrentModule() == "campaign" && campaignModule_->Session();
    saveProjectAction_->setEnabled(open && !campaign);
    if (auto* saveAs = findChild<QAction*>("saveEditorProjectAsAction")) saveAs->setEnabled(open && !campaign);
    saveProjectAction_->setToolTip(campaign ? tr("战役请保存为 .w3n；地图工程不包含战役会话。") : tr("保存地图工程（编辑状态存档）"));
    buildMapAction_->setEnabled(open && !campaign);
    if (auto* button = assetsModule_->Widget()->findChild<QPushButton*>("addImportButton")) button->setEnabled(open);
    saveMapAction_->setEnabled(campaign || project_->archiveSession_ != nullptr);
    if (auto* shadows = findChild<QAction*>("calculateTerrainShadowsAction"))
        shadows->setEnabled(!campaign && project_->archiveSession_ != nullptr && !project_->savingMap_);
    saveMapAction_->setText(campaign ? tr("另存战役为…") : tr("另存地图为…"));
    saveMapInPlaceAction_->setText(campaign ? tr("保存战役") : tr("保存地图"));
    saveMapInPlaceAction_->setToolTip(campaign
        ? tr("保存当前战役；备份目录和保留数量可在应用设置中调整")
        : tr("重建并检查游戏脚本，重新打包后写回打开的地图；备份目录和保留数量可在应用设置中调整"));
    testMapAction_->setEnabled(!campaign && project_->archiveSession_ != nullptr && !project_->savingMap_);
    if (luaReloadAction_) luaReloadAction_->setEnabled(!campaign && (luaReloadController_ || (luaFileReload_ && luaRuntimeMailbox_))
        && LuaTestSessionCurrent() && !project_->savingMap_);
    saveMapInPlaceAction_->setEnabled(campaign || project_->archiveSession_ != nullptr || open);
    const bool objectDirty = std::any_of(
        project_->objectDirty_.begin(), project_->objectDirty_.end(), [](bool dirty) { return dirty; });
    exportAction_->setEnabled(open && !campaign && (objectDirty || project_->triggerDirty_));
}
