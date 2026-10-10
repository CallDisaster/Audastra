#include "Shell/HeaderBar.h"

#include "Dock/ModuleDrag.h"
#include "Dock/ModuleSwitcher.h"
#include "Theme/DesignTokens.generated.h"
#include "Widgets/ElidedLabel.h"

#include <QAction>
#include <QDragEnterEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QStyle>
#include <QToolButton>

namespace War3Map::Shell {

HeaderBar::HeaderBar(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("shellHeader"));
    setAcceptDrops(true);
    setFixedHeight(UiKit::Tokens::Size::kHeaderHeight);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 10, 0);
    layout->setSpacing(10);

    auto* brand = new QLabel(QStringLiteral("Audastra"), this);
    brand->setToolTip(QStringLiteral("Per Audacia Ad Astra"));
    brand->setObjectName(QStringLiteral("brand"));
    project_ = new UiKit::ElidedLabel(tr("未打开地图"), this);
    project_->setObjectName(QStringLiteral("projectIdentity"));
    project_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    project_->setMaximumWidth(160);
    project_->setTextFormat(Qt::PlainText);
    layout->addWidget(brand);
    layout->addWidget(project_);

    auto* divider = new QFrame(this);
    divider->setObjectName(QStringLiteral("headerDivider"));
    divider->setFixedSize(1, 18);
    layout->addWidget(divider);

    switcher_ = new ModuleSwitcher(this);
    layout->addWidget(switcher_, 0);
    layout->addStretch(1);

    actions_ = new QHBoxLayout;
    actions_->setContentsMargins(0, 0, 0, 0);
    actions_->setSpacing(2);
    layout->addLayout(actions_);
}

void HeaderBar::SetProjectTitle(const QString& title, const QString& tooltip) {
    const QString elided = project_->fontMetrics().elidedText(title, Qt::ElideMiddle, project_->maximumWidth());
    project_->setText(elided);
    project_->setToolTip(tooltip.isEmpty() ? title : tooltip);
}

QToolButton* HeaderBar::AddAction(QAction* action) {
    auto* button = new QToolButton(this);
    button->setObjectName(QStringLiteral("headerAction"));
    button->setDefaultAction(action);
    button->setAutoRaise(true);
    // 操作名称不能被布局压缩裁切；剩余宽度由搜索框和可紧凑的模块切换器分配。
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    button->setIconSize(QSize(17, 17));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    if (action->menu() != nullptr) {
        button->setPopupMode(QToolButton::InstantPopup);
    }
    actions_->addWidget(button);
    return button;
}

void HeaderBar::AddWidget(QWidget* widget) {
    widget->setParent(this);
    actions_->addWidget(widget);
}

void HeaderBar::AddSeparator() {
    auto* divider = new QFrame(this);
    divider->setObjectName(QStringLiteral("headerDivider"));
    divider->setFixedSize(1, 18);
    actions_->addSpacing(4);
    actions_->addWidget(divider);
    actions_->addSpacing(4);
}

bool HeaderBar::event(QEvent* event) {
    const auto type = event->type();
    const bool handled = QFrame::event(event);
    if (type == QEvent::Resize || type == QEvent::LayoutRequest || type == QEvent::Show
        || type == QEvent::FontChange || type == QEvent::StyleChange) UpdateActionLayout();
    return handled;
}

void HeaderBar::UpdateActionLayout() {
    // 窄窗口保留操作文字；先省略工程名，再去掉非固定操作的装饰图标。
    const auto buttons = findChildren<QToolButton*>("headerAction", Qt::FindDirectChildrenOnly);
    int fullWidth = QFrame::minimumSizeHint().width();
    for (auto* button : buttons) if (button->toolButtonStyle() == Qt::ToolButtonTextOnly && !button->icon().isNull())
        fullWidth += button->iconSize().width() + 6;
    const bool compact = fullWidth > width();
    for (auto* button : buttons) {
        if (!button->property("headerKeepIcon").toBool()
            && (button->toolButtonStyle() == Qt::ToolButtonTextBesideIcon || button->toolButtonStyle() == Qt::ToolButtonTextOnly))
            button->setToolButtonStyle(compact ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
        const int minimum = button->sizeHint().width();
        if (button->minimumWidth() != minimum) button->setMinimumWidth(minimum);
    }
}

void HeaderBar::SetDropHighlight(bool on) {
    if (property("dropTarget").toBool() == on) {
        return;
    }
    setProperty("dropTarget", on);
    style()->unpolish(this);
    style()->polish(this);
}

void HeaderBar::dragEnterEvent(QDragEnterEvent* event) {
    if (ModuleFromMime(event->mimeData())) {
        SetDropHighlight(true);
        event->acceptProposedAction();
    }
}

void HeaderBar::dragLeaveEvent(QDragLeaveEvent* event) {
    SetDropHighlight(false);
    QFrame::dragLeaveEvent(event);
}

void HeaderBar::dropEvent(QDropEvent* event) {
    SetDropHighlight(false);
    if (const auto id = ModuleFromMime(event->mimeData())) {
        event->acceptProposedAction();
        emit ModuleDropped(*id);
    }
}

} // namespace War3Map::Shell
