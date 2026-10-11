#include "MainWindow.h"
#include "AudastraVersion.generated.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"

#include "AssetsModule.h"
#include "Brand/ApplicationBrand.h"
#include "Icons/SvgIcons.h"
#include "ObjectModule.h"
#include "Shell/HeaderBar.h"
#include "TriggerModule.h"
#include "VariablesModule.h"
#include "Widgets/PopoverSurface.h"

#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDockWidget>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

// 外壳上的应用操作：顶栏（文件 / 保存 / 撤销重做 / 地图 / 布局 / 设置）、状态栏与诊断面板，
// 以及模块信号到工程生命周期的连接。没有侧栏与菜单栏，所有入口都在一行顶栏里。
namespace {

constexpr char kThemeKey[] = "ui/theme";

QAction* MenuAction(QObject* parent, const QString& icon, const QString& text, const QString& tip = {}) {
    auto* action = new QAction(War3Map::UiKit::SvgIcon(icon), text, parent);
    action->setToolTip(tip.isEmpty() ? text : tip);
    return action;
}

} // namespace

void MainWindow::ConnectModules() {
    connect(settingsModule_, &War3Map::Modules::MapSettingsModule::DraftChanged, this, &MainWindow::UpdateWindowTitle);
    connect(soundsModule_, &War3Map::Modules::SoundsModule::DraftChanged, this, &MainWindow::UpdateWindowTitle);
    connect(settingsModule_, &War3Map::Modules::MapSettingsModule::SettingsEdited, this, [this]() {
        UpdateEditDirtyFlags();
        ScheduleRecovery();
        UpdateExportAvailability();
    });
    connect(triggersModule_, &War3Map::Modules::TriggerModule::DraftEdited, this, &MainWindow::ScheduleRecovery);
    connect(triggersModule_, &War3Map::Modules::TriggerModule::UiRootRequested, this, &MainWindow::SelectUiDataRoot);
    connect(objectsModule_, &War3Map::Modules::ObjectModule::DocumentEdited, this, [this]() {
        UpdateEditDirtyFlags();
        ScheduleRecovery();
        UpdateExportAvailability();
    });
    connect(objectsModule_, &War3Map::Modules::ObjectModule::DocumentsChanged, this, [this]() {
        UpdateExportAvailability();
        UpdateStatusBar();
    });
    connect(objectsModule_, &War3Map::Modules::ObjectModule::GameRootRequested, this, &MainWindow::SelectGameRoot);
    connect(objectsModule_, &War3Map::Modules::ObjectModule::ContextChanged, this, &MainWindow::UpdateBreadcrumb);
    connect(project_.get(), &War3Map::QtProject::EditorProject::ImportsEdited, this, &MainWindow::ScheduleRecovery);
    connect(this, &ShellWindow::CurrentModuleChanged, this, [this](const War3Map::Shell::ModuleId& id) {
        UpdateBreadcrumb();
        if (id == "variables") RefreshVariablesPage();
    });
}

void MainWindow::BuildHeader() {
    auto* header = Header();

    globalSearch_ = new QLineEdit;
    globalSearch_->setObjectName("globalSearch");
    globalSearch_->setPlaceholderText(tr("搜索工程…"));
    globalSearch_->setClearButtonEnabled(true);
    globalSearch_->setMinimumWidth(130);
    globalSearch_->setMaximumWidth(230);
    globalSearch_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    connect(globalSearch_, &QLineEdit::textChanged, this, [this](const QString& text) {
        objectsModule_->SetSearchText(text);
        triggersModule_->SetSearchText(text);
    });
    header->AddWidget(globalSearch_);
    header->AddSeparator();

    // 文件：打开 / 保存 / 另存 / 构建。
    auto* fileMenu = new QMenu(this);
    fileMenu->setObjectName(QStringLiteral("applicationFileMenu"));
    auto* newMap = fileMenu->addAction(tr("新建地图…"), this, &MainWindow::CreateNewMap);
    newMap->setObjectName("createMapAction");
    newMap->setShortcut(QKeySequence::New);
    RegisterCommandAction("file.new.map", newMap, tr("工程"));
    addAction(newMap);
    auto* openMap = fileMenu->addAction(War3Map::UiKit::SvgIcon("lucide/folder-open"), tr("打开地图…"), this, &MainWindow::OpenMapArchive);
    openMap->setObjectName(QStringLiteral("openMapAction"));
    openMap->setShortcut(QKeySequence::Open);
    openMap->setToolTip(tr("直接打开 Warcraft III 地图归档（*.w3x / *.w3m）"));
    RegisterCommandAction("file.open.map", openMap, tr("工程"));
    SetupRecentMapsMenu(fileMenu);
    RegisterCommandAction("file.open.workspace", fileMenu->addAction(War3Map::UiKit::SvgIcon("lucide/folder"), tr("打开展开目录…"), this, &MainWindow::OpenWorkspace), tr("工程"));
    RegisterCommandAction("file.open.project", fileMenu->addAction(tr("打开工程…"), this, &MainWindow::OpenEditorProject), tr("工程"));
    RegisterCommandAction("file.restore", fileMenu->addAction(tr("恢复编辑…"), this, &MainWindow::RestoreRecovery), tr("工程"));
    fileMenu->addSeparator();
    // 与 WE 一致：Ctrl+S 保存地图（重建脚本、重新打包、写回原图并备份）；工程文件是次要的编辑状态存档。
    saveMapInPlaceAction_ = MenuAction(this, "lucide/save", tr("保存地图"), tr("保存地图 · Ctrl+S"));
    saveMapInPlaceAction_->setObjectName("saveMapAction");
    saveMapInPlaceAction_->setShortcut(QKeySequence::Save);
    saveMapInPlaceAction_->setShortcutContext(Qt::ApplicationShortcut);
    saveMapInPlaceAction_->setToolTip(tr("重建并检查游戏脚本，重新打包后写回打开的地图；备份目录和保留数量可在应用设置中调整"));
    saveMapInPlaceAction_->setEnabled(false);
    connect(saveMapInPlaceAction_, &QAction::triggered, this, &MainWindow::SaveMapInPlace);
    fileMenu->addAction(saveMapInPlaceAction_);
    saveMapAction_ = fileMenu->addAction(tr("另存地图为…"), this, &MainWindow::SaveAsMap);
    saveMapAction_->setObjectName("saveMapCopyAction");
    saveMapAction_->setShortcut(QKeySequence::SaveAs);
    saveMapAction_->setToolTip(tr("重建并检查游戏脚本后写出新的地图文件"));
    saveMapAction_->setEnabled(false);
    RegisterCommandAction("file.save.map", saveMapInPlaceAction_, tr("工程"));
    RegisterCommandAction("file.save.map.copy", saveMapAction_, tr("工程"));
    testMapAction_ = MenuAction(this, "lucide/play", tr("测试"), tr("测试地图 · F9"));
    testMapAction_->setObjectName(QStringLiteral("testMapAction"));
    testMapAction_->setShortcut(QKeySequence(Qt::Key_F9));
    testMapAction_->setShortcutContext(Qt::ApplicationShortcut);
    testMapAction_->setToolTip(tr("用当前内容生成独立测试副本并启动 Warcraft III；保留原图和未保存编辑"));
    testMapAction_->setEnabled(false);
    connect(testMapAction_, &QAction::triggered, this, &MainWindow::TestMap);
    fileMenu->addAction(testMapAction_);
    RegisterCommandAction("map.test", testMapAction_, tr("地图"));
    fileMenu->addSeparator();
    saveProjectAction_ = MenuAction(this, "lucide/save", tr("保存工程"), tr("保存工程（编辑状态存档）"));
    saveProjectAction_->setObjectName("saveEditorProjectAction");
    saveProjectAction_->setEnabled(false);
    connect(saveProjectAction_, &QAction::triggered, this, &MainWindow::SaveEditorProject);
    fileMenu->addAction(saveProjectAction_);
    RegisterCommandAction("file.save.project", saveProjectAction_, tr("工程"));
    auto* saveAs = fileMenu->addAction(tr("另存工程…"), this, &MainWindow::SaveEditorProjectAs);
    saveAs->setObjectName("saveEditorProjectAsAction");
    RegisterCommandAction("file.save.project.copy", saveAs, tr("工程"));
    fileMenu->addSeparator();
    buildMapAction_ = fileMenu->addAction(tr("构建地图"), this, &MainWindow::CheckGameBuild);
    buildMapAction_->setObjectName("buildGameMapAction");
    buildMapAction_->setEnabled(false);
    exportAction_ = fileMenu->addAction(tr("导出旁路候选"), this, &MainWindow::ExportCandidate);
    exportAction_->setObjectName("exportCandidateAction");
    exportAction_->setEnabled(false);
    RegisterCommandAction("project.build", buildMapAction_, tr("检查"));
    RegisterCommandAction("project.export.candidate", exportAction_, tr("工程"));
    fileMenu->setToolTipsVisible(true);
    auto* file = MenuAction(this, "lucide/file", tr("文件"));
    file->setMenu(fileMenu);
    header->AddAction(file)->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* saveButton = header->AddAction(saveMapInPlaceAction_);
    saveButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    saveButton->setProperty("headerKeepIcon", true);
    auto* testButton = header->AddAction(testMapAction_);
    testButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    testButton->setProperty("headerKeepIcon", true);
    addAction(openMap);
    addAction(saveAs);
    header->AddSeparator();
    BackAction()->setIcon(War3Map::UiKit::SvgIcon("lucide/arrow-left"));
    ForwardAction()->setIcon(War3Map::UiKit::SvgIcon("lucide/arrow-right"));
    BackAction()->setToolTip(tr("后退 · Alt+← · 鼠标后退键"));
    ForwardAction()->setToolTip(tr("前进 · Alt+→ · 鼠标前进键"));
    header->AddAction(BackAction());
    header->AddAction(ForwardAction());
    header->AddAction(UndoAction());
    header->AddAction(RedoAction());
    header->AddSeparator();

    // 地图：地图数据与检查（原版“高级”菜单的地图级操作后续在此补齐）。
    auto* mapMenu = new QMenu(this);
    mapDataAction_ = mapMenu->addAction(tr("地图数据"), this, &MainWindow::ShowMapData);
    mapDataAction_->setObjectName("mapDataAction");
    mapDataAction_->setEnabled(false);
    RegisterCommandAction("map.data", mapDataAction_, tr("地图"));
    auto* mapSize = mapMenu->addAction(tr("地图体积分析…"), this, &MainWindow::ShowMapSizeAnalysis);
    mapSize->setObjectName("mapSizeAnalysisAction");
    mapSize->setToolTip(tr("只统计会话中的命名成员与资源修改；显示逻辑字节大小和覆盖范围"));
    RegisterCommandAction("map.size-analysis", mapSize, tr("地图"));
    mapMenu->addAction(tr("地图设置"), this,
        [this]() { ExecuteCommand("module.map-settings"); });
    mapMenu->addAction(tr("地图实例、区域与镜头"), this,
        [this]() { ExecuteCommand("module.entities"); });
    RegisterCommandAction("map.lua-target", mapMenu->addAction(tr("选择 Lua 保存档案…"),
        this, &MainWindow::SelectLuaMapTarget), tr("地图"));
    RegisterCommandAction("map.lua-target.clear", mapMenu->addAction(tr("清除 Lua 保存档案"),
        this, &MainWindow::ClearLuaMapTarget), tr("地图"));
    RegisterCommandAction("project.validate", mapMenu->addAction(tr("验证工作区"), this, &MainWindow::ValidateWorkspace), tr("检查"));
    RegisterCommandAction("project.capabilities", mapMenu->addAction(tr("功能状态"), this, &MainWindow::ShowCapabilities), tr("检查"));
    auto* map = MenuAction(this, "lucide/map", tr("地图"));
    map->setMenu(mapMenu);
    header->AddAction(map)->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->AddAction(LayoutAction());

    // 设置：本机资源目录与主题。
    auto* settingsMenu = new QMenu(this);
    auto* applicationSettings = settingsMenu->addAction(tr("应用设置…"), this, &MainWindow::ShowApplicationSettings);
    applicationSettings->setObjectName("applicationSettingsAction");
    RegisterCommandAction("settings.application", applicationSettings, tr("设置"));
    auto* updates = settingsMenu->addAction(tr("检查更新…"), this, &MainWindow::ShowUpdates);
    updates->setObjectName(QStringLiteral("applicationUpdatesAction"));
    RegisterCommandAction("settings.updates", updates, tr("设置"));
    settingsMenu->addSeparator();
    RegisterCommandAction("settings.game", settingsMenu->addAction(tr("Warcraft III 安装目录…"), this, &MainWindow::SelectGameRoot), tr("设置"));
    RegisterCommandAction("settings.resources", settingsMenu->addAction(tr("扩展资源目录…"), this, &MainWindow::SelectExtraIconDir), tr("设置"));
    RegisterCommandAction("settings.triggers", settingsMenu->addAction(tr("触发器定义目录…"), this, &MainWindow::SelectUiDataRoot), tr("设置"));
    settingsMenu->addSeparator();
    auto* themeMenu = settingsMenu->addMenu(tr("主题"));
    auto* themes = new QActionGroup(themeMenu);
    for (const QString& id : War3Map::UiKit::ThemeManager::ThemeIds()) {
        auto* action = themeMenu->addAction(War3Map::UiKit::ThemeManager::ThemeLabel(id));
        action->setCheckable(true);
        action->setData(id);
        themes->addAction(action);
        connect(action, &QAction::triggered, this, [this, id]() {
            themeManager_->ApplyTheme(id);
            if (startupMode_ == StartupMode::Normal) QSettings().setValue(kThemeKey, id);
        });
        RegisterCommandAction("settings.theme." + id.toStdString(), action, tr("主题"));
    }
    connect(themeMenu, &QMenu::aboutToShow, this, [this, themes]() {
        for (QAction* action : themes->actions()) {
            action->setChecked(action->data().toString() == themeManager_->CurrentThemeId());
        }
    });
    settingsMenu->addSeparator();
    auto* about = settingsMenu->addAction(tr("关于 Audastra"), this, &MainWindow::ShowAbout);
    about->setObjectName(QStringLiteral("applicationAboutAction"));
    RegisterCommandAction("application.about", about, tr("帮助"));
    auto* settings = MenuAction(this, "lucide/settings-2", tr("设置"));
    settings->setMenu(settingsMenu);
    header->AddAction(settings);
    auto* helpMenu = new QMenu(this);
    auto* feedback = helpMenu->addAction(tr("反馈…"), this, &MainWindow::ShowFeedback);
    feedback->setObjectName(QStringLiteral("applicationFeedbackAction"));
    RegisterCommandAction("application.feedback", feedback, tr("帮助"));
    auto* help = MenuAction(this, "lucide/circle-help", tr("帮助"));
    help->setMenu(helpMenu);
    header->AddAction(help);
    changesAction_ = MenuAction(this, "lucide/git-branch", tr("修改"));
    changesAction_->setObjectName("projectChangesAction");
    changesAction_->setCheckable(true);
    connect(changesAction_, &QAction::triggered, this, &MainWindow::ShowChangesPanel);
    RegisterCommandAction("project.changes", changesAction_, tr("工程"));
    header->AddAction(changesAction_)->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* assistant = MenuAction(this, "lucide/sparkles", tr("助手"));
    assistant->setObjectName("agentWorkspaceAction");
    assistant->setCheckable(true);
    assistant->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    connect(assistant, &QAction::triggered, this, &MainWindow::ToggleAgentWorkspace);
    RegisterCommandAction("assistant.open", assistant, tr("助手"));
    addAction(assistant);
    header->AddAction(assistant)->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
}

void MainWindow::BuildStatusBar() {
    breadcrumb_ = new QLabel(tr("概览"));
    breadcrumb_->setObjectName("breadcrumb");
    statusModCount_ = new QLabel(tr("修改: 0"));
    statusConfig_ = new QLabel(tr("未加载配置"));
    AddStatusWidget(breadcrumb_);
    AddStatusWidget(statusModCount_);
    AddStatusWidget(statusConfig_);
}

void MainWindow::BuildDiagnosticsView() {
    auto* dock = new QDockWidget(tr("诊断"), this);
    dock->setObjectName("diagnosticsDock");
    auto* view = new QTreeView(dock);
    view->setModel(project_->diagnosticsModel_);
    view->header()->setStretchLastSection(true);
    dock->setWidget(view);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    resizeDocks({dock}, {170}, Qt::Vertical);
    dock->hide();
    // 问题入口在状态栏：点击展开 / 收起诊断面板。
    auto* toggle = new QToolButton;
    toggle->setObjectName("diagnosticsToggle");
    toggle->setDefaultAction(dock->toggleViewAction());
    toggle->setIcon(War3Map::UiKit::SvgIcon("lucide/triangle-alert"));
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setAutoRaise(true);
    AddStatusWidget(toggle);
}

void MainWindow::SetProjectTitle(const QString& title, const QString& tooltip) {
    Header()->SetProjectTitle(title, tooltip);
    UpdateWindowTitle();
}

void MainWindow::ShowAbout() {
    auto* surface = findChild<War3Map::UiKit::PopoverSurface*>(QStringLiteral("applicationAbout"));
    if (!surface) {
        surface = new War3Map::UiKit::PopoverSurface(this);
        surface->setObjectName(QStringLiteral("applicationAbout"));
        surface->setWindowTitle(tr("关于 Audastra"));
        auto* content = surface->ContentWidget();
        content->setFixedWidth(384);
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(20, 20, 20, 16);
        layout->setSpacing(12);
        auto* brandRow = new QHBoxLayout;
        brandRow->setContentsMargins(0, 0, 0, 0);
        brandRow->setSpacing(16);
        auto* logo = new QLabel(content);
        logo->setObjectName(QStringLiteral("aboutLogo"));
        logo->setAccessibleName(tr("Audastra 标志"));
        logo->setFixedSize(64, 64);
        logo->setPixmap(War3Map::UiKit::ApplicationBrandPixmap());
        logo->setScaledContents(true);
        brandRow->addWidget(logo);
        auto* brandText = new QVBoxLayout;
        brandText->setContentsMargins(0, 0, 0, 0);
        brandText->setSpacing(6);
        brandRow->addLayout(brandText, 1);
        layout->addLayout(brandRow);
        const auto label = [content, layout](const QString& text, const char* name, QVBoxLayout* target = nullptr) {
            auto* item = new QLabel(text, content);
            item->setObjectName(QString::fromLatin1(name));
            item->setTextFormat(Qt::PlainText);
            item->setWordWrap(true);
            item->setTextInteractionFlags(Qt::TextSelectableByMouse);
            (target ? target : layout)->addWidget(item);
            return item;
        };
        label(QStringLiteral("Audastra"), "aboutProductName", brandText);
        label(QStringLiteral("Per Audacia Ad Astra"), "aboutTagline", brandText);
        label(tr("Warcraft III 地图编辑器"), "aboutDescription");
        const QString version = QCoreApplication::applicationVersion().isEmpty()
            ? QString::fromUtf8(War3Map::BuildInfo::Version.data(), static_cast<qsizetype>(War3Map::BuildInfo::Version.size()))
            : QCoreApplication::applicationVersion();
        label(tr("版本 %1").arg(version), "aboutVersion");
        label(tr("提交 %1").arg(QString::fromUtf8(War3Map::BuildInfo::Commit)), "aboutCommit");
        extern const char* kWar3MapBuildStamp;
        label(tr("构建 %1").arg(QString::fromUtf8(kWar3MapBuildStamp)), "aboutBuildStamp");
        label(tr("© 2026 CallDisaster\nPolyForm Noncommercial 1.0.0\n用本软件制作的地图可自由发布和出售；出售或商业分发编辑器本身需版权人另行书面授权。第三方组件遵循各自许可证，详见随附许可文件。"), "aboutLicense");
        auto* close = new QPushButton(tr("关闭"), content);
        close->setObjectName(QStringLiteral("aboutClose"));
        connect(close, &QPushButton::clicked, surface, &War3Map::UiKit::PopoverSurface::Dismiss);
        layout->addWidget(close, 0, Qt::AlignRight);
    }
    surface->ShowAnchored(Header(), War3Map::UiKit::PopoverSurface::PreferredSide::Below, Qt::AlignHCenter);
}

void MainWindow::RefreshVariablesPage() {
    variablesModule_->Refresh();
}
