#include "Shell/ShellWindow.h"

#include "Dock/DockHost.h"
#include "Dock/FloatFrame.h"
#include "Dock/ModuleSwitcher.h"
#include "Icons/SvgIcons.h"
#include "Layout/LayoutStore.h"
#include "Shell/HeaderBar.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QScopeGuard>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QUndoGroup>
#include <QUndoStack>

#include <algorithm>

namespace War3Map::Shell {
namespace {

constexpr char kGeometryKey[] = "shell/geometry-v1";
constexpr char kViewStateKey[] = "shell/views-v1";

QString Text(const std::string& value) {
    return QString::fromStdString(value);
}

} // namespace

ShellWindow::ShellWindow(ModuleRegistry& registry, QSettings* settings, QWidget* parent)
    : QMainWindow(parent),
      registry_(registry),
      settings_(settings) {
    setObjectName(QStringLiteral("shellWindow"));
    resize(1480, 900);
    setMinimumSize(960, 600);

    undoGroup_ = new QUndoGroup(this);
    header_ = new HeaderBar(this);
    setMenuWidget(header_);
    dock_ = new DockHost(registry_, this);
    dock_->SetHeaderTarget(header_);
    setCentralWidget(dock_);
    statusBar()->setSizeGripEnabled(false);

    std::vector<ModuleInfo> infos;
    for (const auto& module : registry_.Modules()) {
        const ModuleInfo info = module->Info();
        if (info.showInSwitcher) infos.push_back(info);
        module->AttachHost(this);
    }
    header_->Switcher()->SetModules(infos);
    BuildDefaultActions();

    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(800);
    connect(saveTimer_, &QTimer::timeout, this, &ShellWindow::SaveNow);

    auto* switcher = header_->Switcher();
    connect(switcher, &ModuleSwitcher::ActivateRequested, this, [this](const ModuleId& id) {
        if (moduleCommandHandler_) moduleCommandHandler_(id); else Activate(id, true);
    });
    connect(switcher, &ModuleSwitcher::DetachRequested, this, [this](const ModuleId& id, QPoint pos) {
        PlaceModule(id, Placement::Float(pos.x() - 120, pos.y() - 16));
    });
    connect(switcher, &ModuleSwitcher::MenuRequested, this, &ShellWindow::ShowModuleMenu);
    connect(header_, &HeaderBar::ModuleDropped, this, [this](const ModuleId& id) { PlaceModule(id, Placement::Main()); });
    connect(dock_, &DockHost::PlaceRequested, this, &ShellWindow::PlaceModule);
    connect(dock_, &DockHost::ActivateRequested, this, &ShellWindow::Activate);
    connect(dock_, &DockHost::MenuRequested, this, &ShellWindow::ShowModuleMenu);
    // 拖动 / 缩放浮动窗口只更新布局记录，不回写几何，避免和系统拖动循环互相干扰。
    connect(dock_, &DockHost::FloatGeometryEdited, this, [this](const FloatWindow& window) {
        Commit(UpdateFloat(layout_, window), false);
    });
    connect(dock_, &DockHost::SideWidthEdited, this, [this](int width) {
        DockLayout next = layout_;
        next.sideWidth = width;
        Commit(next, false);
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) { OnFocusChanged(now); });

    const auto ids = registry_.AvailableIds();
    DockLayout initial = settings_ != nullptr ? LoadLayout(*settings_, ids) : Sanitize(DefaultLayout(), ids);
    if (settings_ != nullptr) {
        restoreGeometry(settings_->value(kGeometryKey).toByteArray());
    }
    Commit(initial, true);
    if (const auto shown = MainShown(layout_, ids)) {
        SetCurrent(*shown);
    }
    RestoreViewStates();
    qApp->installEventFilter(this);
}

ShellWindow::~ShellWindow() {
    qApp->removeEventFilter(this);
    CancelNavigationRestorations();
    // 模块控件归模块所有（注册表比外壳活得久）：先交还全部控件，再按 Qt 父子关系析构外壳。
    disconnect(qApp, nullptr, this, nullptr);
    disconnect(dock_, nullptr, this, nullptr);
    for (const auto& module : registry_.Modules()) {
        module->AttachHost(nullptr);
    }
    undoGroup_->setActiveStack(nullptr);
    for (QUndoStack* stack : undoGroup_->stacks()) {
        undoGroup_->removeStack(stack);
    }
    dock_->ReleaseModuleWidgets();
}

void ShellWindow::BuildDefaultActions() {
    undoAction_ = undoGroup_->createUndoAction(this, tr("撤销"));
    undoAction_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/undo-2")));
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setShortcutContext(Qt::ApplicationShortcut);
    redoAction_ = undoGroup_->createRedoAction(this, tr("重做"));
    redoAction_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/redo-2")));
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setShortcutContext(Qt::ApplicationShortcut);
    addAction(undoAction_);
    addAction(redoAction_);

    backAction_ = new QAction(UiKit::SvgIcon(QStringLiteral("lucide/arrow-left")), tr("返回"), this);
    backAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    backAction_->setShortcutContext(Qt::ApplicationShortcut);
    backAction_->setEnabled(false);
    connect(backAction_, &QAction::triggered, this, &ShellWindow::GoBack);
    addAction(backAction_);

    forwardAction_ = new QAction(UiKit::SvgIcon(QStringLiteral("lucide/arrow-right")), tr("前进"), this);
    forwardAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    forwardAction_->setShortcutContext(Qt::ApplicationShortcut);
    forwardAction_->setEnabled(false);
    connect(forwardAction_, &QAction::triggered, this, &ShellWindow::GoForward);
    addAction(forwardAction_);

    auto* layoutMenu = new QMenu(this);
    for (const auto& preset : LayoutPresets()) {
        auto* action = layoutMenu->addAction(Text(preset.label));
        action->setData(Text(preset.id));
        action->setToolTip(Text(preset.detail));
        connect(action, &QAction::triggered, this, [this, id = preset.id]() { ApplyPreset(id); });
    }
    layoutAction_ = new QAction(UiKit::SvgIcon(QStringLiteral("lucide/layout-dashboard")), tr("布局"), this);
    layoutAction_->setToolTip(tr("布局预设"));
    layoutAction_->setMenu(layoutMenu);

    // 模块快捷键在浮动窗口里同样有效。
    for (const auto& module : registry_.Modules()) {
        const ModuleInfo info = module->Info();
        if (info.shortcut.isEmpty() || !info.available) {
            continue;
        }
        auto* action = new QAction(info.label, this);
        action->setShortcut(info.shortcut);
        action->setShortcutContext(Qt::ApplicationShortcut);
        connect(action, &QAction::triggered, this, [this, id = info.id]() {
            if (moduleCommandHandler_) moduleCommandHandler_(id); else Activate(id, true);
        });
        addAction(action);
    }
}

void ShellWindow::Commit(DockLayout next, bool apply) {
    const QPointer<ShellWindow> window(this);
    const auto epoch = navigationEpoch_;
    const auto ids = registry_.AvailableIds();
    layout_ = Sanitize(next, ids);
    if (apply) {
        dock_->Apply(layout_);
        if (!window || epoch != navigationEpoch_) return;
    }
    header_->Switcher()->SetState(layout_, ids);
    SelectVisibleModule(false);
    if (!window || epoch != navigationEpoch_) return;
    ScheduleSave();
    emit LayoutChanged();
}

void ShellWindow::SetLayout(const DockLayout& layout) {
    Commit(layout, true);
    SelectVisibleModule(true);
}

bool ShellWindow::ApplyPreset(const std::string& presetId) {
    const auto preset = MakePreset(presetId, layout_);
    if (!preset) {
        return false;
    }
    Commit(*preset, true);
    SelectVisibleModule(true);
    return true;
}

void ShellWindow::PlaceModule(const ModuleId& id, const Placement& target) {
    EditorModule* module = registry_.Find(id);
    if (module == nullptr || !module->Info().available) {
        return;
    }
    emit ModuleRevealRequested(id);
    Commit(Place(layout_, id, target), true);
    if (target.area == Area::Float) {
        if (auto* frame = dock_->Frame(id)) {
            frame->raise();
            frame->activateWindow();
        }
    }
    SetCurrent(id);
}

void ShellWindow::Activate(const ModuleId& id, bool reveal) {
    const QPointer<ShellWindow> window(this);
    const auto epoch = navigationEpoch_;
    EditorModule* module = registry_.Find(id);
    if (module == nullptr || !module->Info().available) {
        return;
    }
    if (reveal) {
        emit ModuleRevealRequested(id);
        if (!window || epoch != navigationEpoch_) return;
    }
    const DockLayout next = ::War3Map::Shell::Activate(layout_, id, reveal);
    if (next != layout_) {
        Commit(next, true);
        if (!window || epoch != navigationEpoch_) return;
    }
    if (reveal && WhereIs(layout_, id) == Area::Float) {
        if (auto* frame = dock_->Frame(id)) {
            frame->raise();
            frame->activateWindow();
        }
    }
    SetCurrent(id);
}

void ShellWindow::ActivateModule(const ModuleId& id) {
    Activate(id, true);
}

void ShellWindow::Navigate(const NavigationTarget& target) {
    (void)TryNavigate(target);
}

bool ShellWindow::GoBack() {
    return ReplayNavigation(false);
}

bool ShellWindow::GoForward() { return ReplayNavigation(true); }

std::optional<NavigationBookmark> ShellWindow::CaptureCurrentNavigation() const {
    if (!current_) return std::nullopt;
    const QPointer<const ShellWindow> window(this);
    const auto epoch = navigationEpoch_;
    const auto id = *current_;
    const auto* module = registry_.Find(*current_);
    if (!module || !module->Info().available) return std::nullopt;
    const auto state = module->CaptureNavigationBookmark();
    if (!window || epoch != navigationEpoch_) return std::nullopt;
    if (!state || QJsonDocument(*state).toJson(QJsonDocument::Compact).size() > 512 * 1024) return std::nullopt;
    return NavigationBookmark{id, *state, epoch};
}
void ShellWindow::RefreshNavigationActions() {
    backAction_->setEnabled(history_.CanGoBack());
    forwardAction_->setEnabled(history_.CanGoForward());
}
void ShellWindow::CancelNavigationRestorations() {
    const QPointer<ShellWindow> window(this);
    const auto ids = registry_.Ids();
    for (const auto& id : ids) {
        if (auto* module = registry_.Find(id)) module->CancelNavigationRestoration();
        if (!window) return;
    }
}
void ShellWindow::ResetNavigationHistory() {
    const QPointer<ShellWindow> window(this);
    ++navigationEpoch_;
    CancelNavigationRestorations();
    if (!window) return;
    history_.Clear();
    RefreshNavigationActions();
}
bool ShellWindow::TryNavigate(const NavigationTarget& target) {
    if (navigating_) return false;
    auto* module = registry_.Find(target.module);
    if (!module || !module->Info().available) return false;
    const QPointer<ShellWindow> window(this);
    navigating_ = true;
    const auto release = qScopeGuard([window]() { if (window) window->navigating_ = false; });
    const auto epoch = navigationEpoch_;
    auto* current = current_ ? registry_.Find(*current_) : nullptr;
    if (current && !current->PrepareNavigationLeave()) {
        if (window) ShowStatusMessage(tr("当前草稿尚未处理，导航位置保持原处。"));
        return false;
    }
    if (!window || epoch != navigationEpoch_) return false;
    const auto from = CaptureCurrentNavigation();
    if (!window || epoch != navigationEpoch_) return false;
    CancelNavigationRestorations();
    if (!window || epoch != navigationEpoch_) return false;
    if (!module->Reveal(target)) {
        if (window) ShowStatusMessage(tr("无法定位该目标；导航历史保持原处。"));
        return false;
    }
    if (!window || epoch != navigationEpoch_) return false;
    Activate(target.module, true);
    if (!window || epoch != navigationEpoch_) return false;
    const auto arrived = CaptureCurrentNavigation();
    if (!window || epoch != navigationEpoch_) return false;
    if (from && (!arrived || *from != *arrived)) history_.Push(*from);
    else history_.ClearForward();
    RefreshNavigationActions();
    return true;
}
bool ShellWindow::ReplayNavigation(bool forward) {
    if (navigating_ || !(forward ? history_.CanGoForward() : history_.CanGoBack())) return false;
    const QPointer<ShellWindow> window(this);
    navigating_ = true;
    const auto release = qScopeGuard([window]() { if (window) window->navigating_ = false; });
    const auto epoch = navigationEpoch_;
    auto* source = current_ ? registry_.Find(*current_) : nullptr;
    if (source && !source->PrepareNavigationLeave()) {
        if (window) ShowStatusMessage(tr("当前草稿尚未处理，导航历史保持原处。"));
        return false;
    }
    if (!window || epoch != navigationEpoch_) return false;
    const auto current = CaptureCurrentNavigation();
    if (!window || epoch != navigationEpoch_) return false;
    if (!current) {
        ShowStatusMessage(tr("当前视图没有可信导航位置，历史保持原处。"));
        return false;
    }
    CancelNavigationRestorations();
    if (!window || epoch != navigationEpoch_) return false;
    std::size_t skipped = 0;
    while (const auto position = history_.Peek(forward)) {
        auto* module = registry_.Find(position->module);
        const auto restored = position->epoch == navigationEpoch_ && module && module->Info().available
            ? module->RestoreNavigationBookmark(position->state) : NavigationRestoreResult::Invalid;
        if (!window || epoch != navigationEpoch_) return false;
        if (restored == NavigationRestoreResult::Blocked) {
            ShowStatusMessage(tr("目标视图尚有草稿或正在操作，导航历史保持原处。"));
            RefreshNavigationActions();
            return false;
        }
        if (restored == NavigationRestoreResult::Invalid) {
            history_.SkipInvalid(forward);
            ++skipped;
            continue;
        }
        Activate(position->module, true);
        if (!window || epoch != navigationEpoch_) return false;
        history_.Commit(forward, *current);
        RefreshNavigationActions();
        if (skipped) ShowStatusMessage(tr("已跳过 %1 个失效导航位置。 ").arg(skipped));
        return true;
    }
    RefreshNavigationActions();
    if (skipped) ShowStatusMessage(tr("已跳过 %1 个失效导航位置，没有可恢复的位置。 ").arg(skipped));
    return false;
}

bool ShellWindow::eventFilter(QObject* watched, QEvent* event) {
    if ((event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease)
        || QApplication::activeModalWidget() || QApplication::activePopupWidget())
        return QMainWindow::eventFilter(watched, event);
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::BackButton && mouse->button() != Qt::ForwardButton)
        return QMainWindow::eventFilter(watched, event);
    const auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget) return false;
    bool owned = widget->window() == this;
    if (!owned) {
        for (const auto& module : registry_.Modules()) {
            const auto* frame = dock_->Frame(module->Info().id);
            if (frame && widget->window() == frame) { owned = true; break; }
        }
    }
    if (!owned) return false;
    // 只在 release 执行一次；拦截 press 避免传播到父控件后重复。
    if (event->type() == QEvent::MouseButtonRelease) {
        if (mouse->button() == Qt::BackButton) GoBack(); else GoForward();
    }
    event->accept();
    return true;
}

void ShellWindow::SetCurrent(const ModuleId& id) {
    EditorModule* module = registry_.Find(id);
    if (module == nullptr || !module->Info().available) {
        return;
    }
    QUndoStack* stack = module->UndoStack();
    if (stack != nullptr && !undoGroup_->stacks().contains(stack)) {
        undoGroup_->addStack(stack);
    }
    undoGroup_->setActiveStack(stack);
    if (current_ == id) {
        return;
    }
    current_ = id;
    emit CurrentModuleChanged(id);
}

void ShellWindow::SelectVisibleModule(bool preferMain) {
    const auto ids = registry_.AvailableIds();
    const auto visible = VisibleModules(layout_, ids);
    if (!preferMain && current_ &&
        std::find(visible.begin(), visible.end(), *current_) != visible.end()) {
        SetCurrent(*current_);
        return;
    }
    // 显式切换布局以主区为编辑目标；没有主区时使用右侧活动页或浮窗。
    if (!visible.empty()) {
        SetCurrent(visible.front());
        return;
    }
    undoGroup_->setActiveStack(nullptr);
    if (current_) {
        current_.reset();
        emit CurrentModuleChanged(ModuleId{});
    }
}

void ShellWindow::UndoStackChanged(const ModuleId& id) {
    if (current_ == id) {
        SetCurrent(id);
    }
}

void ShellWindow::OnFocusChanged(QWidget* now) {
    if (now == nullptr) {
        return;
    }
    for (const auto& module : registry_.Modules()) {
        const ModuleId id = module->Info().id;
        if (!dock_->IsCreated(id)) {
            continue;
        }
        QWidget* widget = module->Widget();
        if (widget == now || widget->isAncestorOf(now)) {
            SetCurrent(id);
            return;
        }
    }
}

void ShellWindow::ShowModuleMenu(const ModuleId& id, QPoint globalPos) {
    EditorModule* module = registry_.Find(id);
    if (module == nullptr || !module->Info().available) {
        return;
    }
    QMenu menu(this);
    auto* group = new QActionGroup(&menu);
    const Area where = WhereIs(layout_, id);
    const auto add = [&](const QString& text, const QString& icon, Area area, const Placement& target) {
        auto* action = menu.addAction(UiKit::SvgIcon(icon), text);
        action->setCheckable(true);
        action->setChecked(where == area);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, id, target]() { PlaceModule(id, target); });
    };
    add(tr("在主编辑区显示"), QStringLiteral("lucide/app-window"), Area::Main, Placement::Main());
    add(tr("停靠到右侧"), QStringLiteral("lucide/panel-right"), Area::Side, Placement::Side());
    const QRect frame = geometry();
    add(tr("浮动窗口"), QStringLiteral("lucide/maximize-2"), Area::Float,
        Placement::Float(frame.x() + frame.width() / 4, frame.y() + frame.height() / 5));
    menu.exec(globalPos);
}

void ShellWindow::ShowStatusMessage(const QString& text, int timeoutMs) {
    statusBar()->showMessage(text, timeoutMs);
}

void ShellWindow::AddStatusWidget(QWidget* widget) {
    statusBar()->addPermanentWidget(widget);
}

void ShellWindow::ScheduleSave() {
    if (settings_ != nullptr) {
        saveTimer_->start();
    }
}

void ShellWindow::SaveNow() {
    if (settings_ == nullptr) {
        return;
    }
    SaveLayout(*settings_, layout_);
    settings_->setValue(kGeometryKey, saveGeometry());
    // 各模块的视图状态（选中项、页签、滚动位置）；模块自己决定保存什么，只存本机。
    QJsonObject views;
    for (const auto& module : registry_.Modules()) {
        const QJsonObject state = module->SaveViewState();
        if (!state.isEmpty()) {
            views.insert(QString::fromStdString(module->Info().id), state);
        }
    }
    settings_->setValue(kViewStateKey, QJsonDocument(views).toJson(QJsonDocument::Compact));
}

void ShellWindow::RestoreViewStates() {
    if (settings_ == nullptr) {
        return;
    }
    const QJsonObject views = QJsonDocument::fromJson(settings_->value(kViewStateKey).toByteArray()).object();
    for (const auto& module : registry_.Modules()) {
        const auto info = module->Info();
        const QJsonValue state = views.value(QString::fromStdString(info.id));
        if (info.available && state.isObject()) {
            module->RestoreViewState(state.toObject());
        }
    }
}

void ShellWindow::closeEvent(QCloseEvent* event) {
    if (closeGuard_ && !closeGuard_()) {
        event->ignore();
        return;
    }
    saveTimer_->stop();
    SaveNow();
    QMainWindow::closeEvent(event);
}

} // namespace War3Map::Shell
