#include "MainWindow.h"
#include "Dock/DockHost.h"
#include "Shell/StartupPage.h"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QScreen>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>

void MainWindow::SetupStartupPage() {
    if (startupMode_ == StartupMode::HeadlessProbe || workspacePages_) return;
    // 共用同一层中央页，保留原Dock及全部编辑模块；助手页不再包裹第二层Stack。
    QWidget* editor = takeCentralWidget();
    workspacePages_ = new QStackedWidget(this);
    workspacePages_->setObjectName(QStringLiteral("agentWorkspaceStack"));
    workspacePages_->addWidget(editor);
    startupPage_ = new War3Map::Shell::StartupPage(
        findChild<QAction*>(QStringLiteral("openMapAction")),
        findChild<QAction*>(QStringLiteral("createMapAction")), workspacePages_);
    if (auto* content = startupPage_->findChild<QWidget*>(QStringLiteral("startupContent"))) {
        if (auto* column = qobject_cast<QVBoxLayout*>(content->layout())) {
            column->addSpacing(24);
            auto* title = new QLabel(tr("最近地图"), content);
            title->setTextFormat(Qt::PlainText);
            column->addWidget(title);
            startupRecentMaps_ = new War3Map::Shell::RecentMapsList(content);
            startupRecentMaps_->setObjectName(QStringLiteral("startupRecentMaps"));
            column->addWidget(startupRecentMaps_);
            BindRecentMapsList(startupRecentMaps_);
            auto* manage = new QToolButton(content);
            manage->setObjectName(QStringLiteral("startupManageRecentMaps"));
            manage->setText(tr("管理最近地图…"));
            column->addWidget(manage);
            connect(manage, &QToolButton::clicked, this, &MainWindow::ShowRecentMapsManager);
        }
    }
    workspacePages_->addWidget(startupPage_);
    setCentralWidget(workspacePages_);
    connect(this, &ShellWindow::ModuleRevealRequested, this,
        [this](const War3Map::Shell::ModuleId&) { DismissStartupPage(); });
    ShowBaseWorkspace();
}

void MainWindow::SetupRecentMapsMenu(QMenu* fileMenu) {
    recentMapsMenu_ = fileMenu->addMenu(tr("最近地图"));
    recentMapsMenu_->setObjectName(QStringLiteral("recentMapsMenu"));
    auto* scroll = new QScrollArea(recentMapsMenu_);
    scroll->setObjectName(QStringLiteral("recentMapsMenuScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(360);
    auto* container = new QWidget(scroll);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(10, 8, 10, 8);
    menuRecentMaps_ = new War3Map::Shell::RecentMapsList(container);
    menuRecentMaps_->setObjectName(QStringLiteral("menuRecentMaps"));
    layout->addWidget(menuRecentMaps_);
    scroll->setWidget(container);
    auto* widgetAction = new QWidgetAction(recentMapsMenu_);
    widgetAction->setDefaultWidget(scroll);
    recentMapsMenu_->addAction(widgetAction);
    recentMapsMenu_->addSeparator();
    auto* manage = recentMapsMenu_->addAction(tr("管理最近地图…"), this, &MainWindow::ShowRecentMapsManager);
    manage->setObjectName(QStringLiteral("manageRecentMapsAction"));
    RegisterCommandAction("file.recent.manage", manage, tr("工程"));
    BindRecentMapsList(menuRecentMaps_);
    connect(recentMapsMenu_, &QMenu::aboutToShow, this, [this, scroll] {
        RefreshRecentMaps();
        // 只限制弹出菜单；启动页已有自己的滚动容器，不再嵌套滚动。
        const auto* display = recentMapsMenu_->screen();
        const int availableHeight = display ? display->availableGeometry().height() : 720;
        scroll->setMaximumHeight(std::max(80, std::min(560, availableHeight - 160)));
    });
    scroll->setMaximumHeight(560);
    RefreshRecentMaps();
}

void MainWindow::BindRecentMapsList(War3Map::Shell::RecentMapsList* list) {
    (void)list->SetEntries(recentMapRows_);
    list->SetStatus(recentMapsMessage_);
    connect(list, &War3Map::Shell::RecentMapsList::OpenRequested, this, [this](const QString& path) {
        if (recentMapsMenu_) recentMapsMenu_->close();
        const QPointer<MainWindow> liveWindow(this);
        const QPointer<War3Map::QtProject::EditorProject> liveProject(project_.get());
        const bool opened = OpenUserMapArchivePath(std::filesystem::path(path.toStdWString()));
        if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return;
        if (!opened) liveWindow->RefreshRecentMaps();
    });
    connect(list, &War3Map::Shell::RecentMapsList::PinRequested, this, [this](const QString& path, bool pinned) {
        if (recentMapsStore_) PublishRecentMaps(recentMapsStore_->SetPinned(path, pinned));
    });
    connect(list, &War3Map::Shell::RecentMapsList::RemoveRequested, this, [this](const QString& path) {
        if (recentMapsStore_) PublishRecentMaps(recentMapsStore_->Remove(path));
    });
}

void MainWindow::PublishRecentMaps(const War3Map::QtProject::RecentMapsResult& result) {
    if (result.succeeded) {
        recentMapRows_.clear();
        for (const auto& entry : result.entries)
            recentMapRows_.push_back({entry.path, entry.lastOpenedUtc, entry.pinned, entry.available});
    }
    recentMapsMessage_ = result.message;
    for (auto* list : {startupRecentMaps_.data(), menuRecentMaps_.data(), managedRecentMaps_.data()}) {
        if (!list) continue;
        // 失败时保留上一份可用列表，不用空结果掩盖损坏配置或锁争用。
        if (result.succeeded) (void)list->SetEntries(recentMapRows_);
        list->SetStatus(recentMapsMessage_);
    }
    if (!result.message.isEmpty()) statusBar()->showMessage(result.message, 10000);
}

void MainWindow::RefreshRecentMaps() {
    if (recentMapsStore_) PublishRecentMaps(recentMapsStore_->Read());
}

void MainWindow::RecordUserMap(const std::filesystem::path& path) {
    if (!recentMapsStore_ || startupMode_ != StartupMode::Normal) return;
    const QString source = QString::fromStdWString(path.wstring());
    const auto suffix = QFileInfo(source).suffix().toLower();
    if (suffix != QStringLiteral("w3x") && suffix != QStringLiteral("w3m")) return;
    PublishRecentMaps(recentMapsStore_->RecordOpened(source));
}

void MainWindow::ShowRecentMapsManager() {
    if (recentMapsMenu_) recentMapsMenu_->close();
    RefreshRecentMaps();
    if (recentMapsManager_) {
        recentMapsManager_->show();
        recentMapsManager_->raise();
        recentMapsManager_->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    recentMapsManager_ = dialog;
    dialog->setObjectName(QStringLiteral("recentMapsManager"));
    dialog->setWindowTitle(tr("管理最近地图"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(520, 460);
    auto* column = new QVBoxLayout(dialog);
    auto* scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    managedRecentMaps_ = new War3Map::Shell::RecentMapsList(scroll);
    managedRecentMaps_->setObjectName(QStringLiteral("managedRecentMaps"));
    scroll->setWidget(managedRecentMaps_);
    column->addWidget(scroll, 1);
    auto* buttons = new QDialogButtonBox(dialog);
    buttons->addButton(tr("关闭"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    column->addWidget(buttons);
    BindRecentMapsList(managedRecentMaps_);
    dialog->open();
}

void MainWindow::ShowBaseWorkspace() {
    if (!workspacePages_) return;
    const bool loaded = !project_->projectRoot_.empty() || !project_->sourceArchivePath_.empty();
    workspacePages_->setCurrentWidget(loaded || startupDismissed_ ? static_cast<QWidget*>(Dock()) : startupPage_);
}

void MainWindow::DismissStartupPage() {
    startupDismissed_ = true;
    ShowBaseWorkspace();
}
