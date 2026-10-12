#include "Widgets/ResponsiveToolBar.h"

#include "Icons/SvgIcons.h"

#include <QAbstractButton>
#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <functional>

namespace War3Map::UiKit {
namespace {

class SearchSlot final : public QWidget {
public:
    SearchSlot(QLineEdit* edit, QWidget* parent, std::function<void()> refresh)
        : QWidget(parent), edit_(edit), refresh_(std::move(refresh)) {
        setObjectName(QStringLiteral("responsiveSearch"));
        preferred_ = std::clamp(edit->maximumWidth(), 130, 230);
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0);
        edit_->setParent(this); edit_->setMinimumWidth(0); edit_->setMaximumWidth(QWIDGETSIZE_MAX);
        row->addWidget(edit_);
        button_ = new QToolButton(this);
        button_->setObjectName(QStringLiteral("responsiveSearchButton"));
        SetToolButtonSvgIcon(*button_, QStringLiteral("lucide/search"), tr("搜索工程"));
        button_->setToolTip(edit_->placeholderText()); button_->setFocusPolicy(Qt::StrongFocus);
        row->addWidget(button_); button_->hide(); edit_->installEventFilter(this);
        connect(button_, &QToolButton::clicked, this, [this] {
            expanded_ = true; refresh_(); edit_->setFocus(Qt::ShortcutFocusReason); edit_->selectAll();
        });
        SetMode(0);
    }
    void SetMode(int mode) {
        if (expanded_) mode = 0;
        setProperty("searchCollapsed", mode == 2);
        const bool icon = mode == 2;
        edit_->setVisible(!icon); button_->setVisible(icon);
        setFixedWidth(icon ? button_->sizeHint().width() : mode == 1 ? 130 : preferred_);
    }
    bool Expanded() const { return expanded_; }
    void ConstrainWidth(int maximum) { setFixedWidth(std::max(1, std::min(width(), maximum))); }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == edit_ && event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && expanded_) {
            expanded_ = false; refresh_(); button_->setFocus(Qt::ShortcutFocusReason); return true;
        }
        return QWidget::eventFilter(object, event);
    }
private:
    QLineEdit* edit_;
    QToolButton* button_;
    std::function<void()> refresh_;
    int preferred_ = 230;
    bool expanded_ = false;
};

struct ButtonState {
    QPointer<QAbstractButton> button;
    QString text;
    Qt::ToolButtonStyle fullStyle = Qt::ToolButtonTextBesideIcon;
    Qt::ToolButtonStyle appliedStyle = Qt::ToolButtonTextBesideIcon;
    bool textHidden = false;
    QPointer<QAction> proxy;
};

void SetCompact(ButtonState& state, bool compact) {
    auto* button = state.button.data();
    if (!button) return;
    compact = compact && !button->icon().isNull();
    button->setAccessibleName(state.text);
    if (button->toolTip().isEmpty()) button->setToolTip(state.text);
    if (auto* tool = qobject_cast<QToolButton*>(button)) {
        state.appliedStyle = compact ? Qt::ToolButtonIconOnly : state.fullStyle;
        tool->setToolButtonStyle(state.appliedStyle);
    } else {
        state.textHidden = compact;
        const QString text = compact ? QString{} : state.text;
        if (button->text() != text) button->setText(text);
    }
}

} // namespace

struct ResponsiveToolBar::Entry {
    QPointer<QWidget> widget;
    std::vector<ButtonState> buttons;
    SearchSlot* search = nullptr;
    int fullWidth = 0;
    int priority = 0;
    bool wanted = true;
    bool overflow = false;
    bool separator = false;
};

ResponsiveToolBar::ResponsiveToolBar(QWidget* parent) : QFrame(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(0, 0, 0, 0); row_->setSpacing(4);
    row_->setSizeConstraint(QLayout::SetNoConstraint);
    menu_ = new QMenu(this); menu_->setToolTipsVisible(true);
    more_ = new QToolButton(this);
    more_->setObjectName(QStringLiteral("responsiveToolbarMore"));
    SetToolButtonSvgIcon(*more_, QStringLiteral("lucide/chevron-down"), tr("更多操作"));
    more_->setAccessibleName(tr("更多操作")); more_->setToolTip(tr("更多操作"));
    more_->setPopupMode(QToolButton::InstantPopup); more_->setMenu(menu_);
    more_->setFocusPolicy(Qt::StrongFocus); more_->hide();
    row_->addWidget(more_);
    connect(menu_, &QMenu::aboutToShow, this, &ResponsiveToolBar::RebuildMenu);
}

ResponsiveToolBar::~ResponsiveToolBar() = default;

QToolButton* ResponsiveToolBar::AddAction(QAction* action, int priority) {
    auto* button = new QToolButton(this);
    button->setDefaultAction(action); button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setFocusPolicy(Qt::StrongFocus);
    if (action->menu()) button->setPopupMode(QToolButton::InstantPopup);
    // QAction也挂在工具栏，折叠按钮不会使它的窗口快捷键失效。
    QWidget::addAction(action);
    connect(action, &QAction::changed, this, &ResponsiveToolBar::QueueRefresh);
    AddWidget(button, priority);
    return button;
}

void ResponsiveToolBar::AddWidget(QWidget* widget, int priority) {
    if (auto* search = qobject_cast<QLineEdit*>(widget))
        widget = new SearchSlot(search, this, [this] { layoutDirty_ = true; Refresh(); });
    widget->setProperty("toolbarPriority", priority);
    row_->insertWidget(std::max(0, row_->count() - 1), widget);
    QueueRefresh();
}

void ResponsiveToolBar::AddSeparator() {
    auto* line = new QFrame(this);
    line->setObjectName(QStringLiteral("commandDivider")); line->setFixedSize(1, 18);
    line->setProperty("toolbarSeparator", true);
    AddWidget(line);
}

void ResponsiveToolBar::Synchronize() {
    // ContentLayout允许已有页面保持原布局和按钮连接，统一收取其命令控件。
    for (int index = 0; index < row_->count(); ++index) {
        auto* widget = row_->itemAt(index)->widget();
        if (!widget || widget == more_) continue;
        if (std::any_of(entries_.begin(), entries_.end(), [widget](const auto& entry) { return entry->widget == widget; })) continue;
        auto entry = std::make_unique<Entry>(); entry->widget = widget;
        entry->search = dynamic_cast<SearchSlot*>(widget);
        entry->wanted = !widget->testAttribute(Qt::WA_WState_ExplicitShowHide) || !widget->isHidden();
        entry->separator = widget->property("toolbarSeparator").toBool()
            || (widget->maximumWidth() <= 1 && qobject_cast<QFrame*>(widget));
        entry->priority = widget->property("toolbarPriority").toInt();
        auto buttons = widget->findChildren<QAbstractButton*>();
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) buttons.prepend(button);
        if (!entry->search) for (auto* button : buttons) {
            ButtonState state; state.button = button; state.text = button->text();
            if (auto* tool = qobject_cast<QToolButton*>(button)) state.fullStyle = state.appliedStyle = tool->toolButtonStyle();
            button->installEventFilter(this);
            if (auto* tool = qobject_cast<QToolButton*>(button); tool && tool->defaultAction()) {
                state.proxy = tool->defaultAction();
            } else {
                state.proxy = new QAction(this);
                connect(state.proxy, &QAction::triggered, button, [button] { button->click(); });
                if (!button->shortcut().isEmpty()) {
                    state.proxy->setShortcut(button->shortcut()); button->setShortcut({});
                    QWidget::addAction(state.proxy);
                }
                if (auto* push = qobject_cast<QPushButton*>(button); push && push->menu()) state.proxy->setMenu(push->menu());
            }
            entry->buttons.push_back(std::move(state));
        }
        widget->installEventFilter(this);
        entries_.push_back(std::move(entry)); measure_ = true;
    }
    // 调用者可以继续向ContentLayout尾部添加；更多入口始终保持最后。
    if (row_->itemAt(row_->count() - 1)->widget() != more_) {
        row_->removeWidget(more_); row_->addWidget(more_);
    }
}

void ResponsiveToolBar::QueueRefresh() {
    if (!refreshing_) layoutDirty_ = true;
    if (refreshing_ || queued_) return;
    queued_ = true;
    QTimer::singleShot(0, this, [this] { queued_ = false; Refresh(); });
}

void ResponsiveToolBar::Refresh() {
    if (refreshing_ || !row_) return;
    refreshing_ = true; const auto guard = qScopeGuard([this] { refreshing_ = false; });
    const int previousPreferred = preferredWidth_;
    Synchronize();
    for (auto& entry : entries_) for (auto& state : entry->buttons) {
        if (!state.button) continue;
        const auto text = state.button->text();
        if ((!state.textHidden || !text.isEmpty()) && state.text != text) { state.text = text; measure_ = true; }
        if (auto* tool = qobject_cast<QToolButton*>(state.button.data()); tool && tool->toolButtonStyle() != state.appliedStyle) {
            state.fullStyle = state.appliedStyle = tool->toolButtonStyle(); measure_ = true;
        }
    }
    if (!measure_ && !layoutDirty_ && lastWidth_ == width()) { RebuildMenu(); return; }
    layoutDirty_ = false;
    lastWidth_ = width();
    const auto margins = row_->contentsMargins();
    const int available = std::max(0, width() - margins.left() - margins.right());
    if (measure_) {
        preferredWidth_ = margins.left() + margins.right();
        for (auto& entry : entries_) if (entry->widget) {
            for (auto& state : entry->buttons) SetCompact(state, false);
            if (entry->search) entry->search->SetMode(0);
            if (entry->widget->layout()) entry->widget->layout()->invalidate();
            entry->fullWidth = std::max({entry->widget->sizeHint().width(), entry->widget->minimumSizeHint().width(), entry->widget->minimumWidth()});
            if (entry->wanted) preferredWidth_ += entry->fullWidth + row_->spacing();
        }
        measure_ = false;
    }
    const bool compact = preferredWidth_ - margins.left() - margins.right() > available;
    for (auto& entry : entries_) if (entry->widget) {
        entry->overflow = false;
        if (!entry->search && !entry->separator) entry->widget->setMinimumWidth(0);
        for (auto& state : entry->buttons) SetCompact(state, compact);
        if (entry->search) entry->search->SetMode(0);
        entry->widget->setVisible(entry->wanted && !(compact && entry->separator));
        if (entry->widget->layout()) entry->widget->layout()->invalidate();
    }
    auto required = [this] {
        int result = 0, count = 0;
        for (const auto& entry : entries_) if (entry->widget && !entry->widget->isHidden()) {
            result += std::max(entry->widget->minimumWidth(), entry->widget->sizeHint().width()); ++count;
        }
        if (std::any_of(entries_.begin(), entries_.end(), [](const auto& entry) { return entry->overflow; })) {
            result += more_->sizeHint().width(); ++count;
        }
        return result + std::max(0, count - 1) * row_->spacing();
    };
    std::vector<Entry*> candidates;
    for (auto& entry : entries_) if (entry->wanted && !entry->buttons.empty()) candidates.push_back(entry.get());
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto* left, const auto* right) { return left->priority > right->priority; });
    auto collapse = [&](bool lowOnly) {
        for (auto it = candidates.rbegin(); it != candidates.rend() && required() > available; ++it) {
            auto* entry = *it;
            const bool keep = entry->widget->property("headerKeepIcon").toBool();
            if (entry->overflow || (lowOnly && (entry->priority >= 100 || keep))) continue;
            entry->overflow = true; entry->widget->hide();
        }
    };
    collapse(true);
    for (const int mode : {1, 2}) if (required() > available)
        for (auto& entry : entries_) if (entry->search && !entry->search->Expanded()) entry->search->SetMode(mode);
    collapse(false);
    // 点击窄栏里的搜索图标仍展开同一个输入框，宽度取剩余空间；不挤出父控件。
    for (auto& entry : entries_) if (entry->search && entry->search->Expanded() && required() > available)
        entry->search->ConstrainWidth(entry->search->width() - (required() - available));
    more_->setVisible(std::any_of(entries_.begin(), entries_.end(), [](const auto& entry) { return entry->overflow; }));
    for (auto& entry : entries_) if (entry->widget && !entry->widget->isHidden()) {
        // 保证可见命令拿到完整sizeHint，布局不能再通过压文字来挤出空间。
        if (!entry->search && !entry->separator)
            entry->widget->setMinimumWidth(std::max(0, entry->widget->sizeHint().width()));
    }
    RebuildMenu();
    // 显隐和宽度已变更，同步更新子项位置；避免本次刷新仍沿用折叠前的布局。
    row_->activate();
    if (previousPreferred != preferredWidth_) updateGeometry();
}

void ResponsiveToolBar::RebuildMenu() {
    const bool open = menu_->isVisible();
    if (!open) menu_->clear();
    for (auto& entry : entries_) if (entry->overflow) for (auto& state : entry->buttons) {
        if (!state.button || !state.proxy) continue;
        auto* tool = qobject_cast<QToolButton*>(state.button.data());
        if (!tool || state.proxy != tool->defaultAction()) {
            state.proxy->setText(state.text); state.proxy->setIcon(state.button->icon());
            state.proxy->setEnabled(state.button->isEnabled()); state.proxy->setToolTip(state.button->toolTip());
            state.proxy->setCheckable(state.button->isCheckable()); state.proxy->setChecked(state.button->isChecked());
        }
        if (!open) menu_->addAction(state.proxy);
    }
}

QSize ResponsiveToolBar::sizeHint() const {
    const auto height = row_ ? row_->sizeHint().height() : 32;
    return {preferredWidth_, height};
}

QSize ResponsiveToolBar::minimumSizeHint() const {
    return {more_ ? more_->sizeHint().width() : 32, row_ ? row_->sizeHint().height() : 32};
}

bool ResponsiveToolBar::event(QEvent* event) {
    const bool result = QFrame::event(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) measure_ = true;
    bool commandsChanged = false;
    if (event->type() == QEvent::LayoutRequest && !refreshing_ && row_) {
        int count = 0;
        for (int i = 0; i < row_->count(); ++i) if (row_->itemAt(i)->widget() && row_->itemAt(i)->widget() != more_) ++count;
        commandsChanged = count != static_cast<int>(entries_.size());
        for (const auto& entry : entries_) for (const auto& state : entry->buttons) if (state.button) {
            if ((!state.textHidden || !state.button->text().isEmpty()) && state.text != state.button->text()) commandsChanged = true;
            if (auto* tool = qobject_cast<QToolButton*>(state.button.data()); tool && tool->toolButtonStyle() != state.appliedStyle) commandsChanged = true;
        }
    }
    if (event->type() == QEvent::Resize || event->type() == QEvent::Show || commandsChanged
        || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) QueueRefresh();
    return result;
}

bool ResponsiveToolBar::eventFilter(QObject* watched, QEvent* event) {
    if (refreshing_) return false;
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) measure_ = true;
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide)
        for (auto& entry : entries_) if (entry->widget == watched) entry->wanted = !entry->widget->isHidden();
    if (event->type() == QEvent::EnabledChange || event->type() == QEvent::Show || event->type() == QEvent::Hide
        || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange || event->type() == QEvent::DynamicPropertyChange)
        QueueRefresh();
    return false;
}

} // namespace War3Map::UiKit
