#include "Shell/HeaderBar.h"

#include "Dock/ModuleDrag.h"
#include "Dock/ModuleSwitcher.h"
#include "Theme/DesignTokens.generated.h"
#include "Widgets/ElidedLabel.h"
#include "Widgets/ResponsiveToolBar.h"

#include <QAction>
#include <QDragEnterEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QScopeGuard>
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
    actions_ = new UiKit::ResponsiveToolBar(this);
    actions_->setObjectName(QStringLiteral("headerActions"));
    actions_->ContentLayout()->setSpacing(2);
    layout->addWidget(actions_, 1);
}

void HeaderBar::SetProjectTitle(const QString& title, const QString& tooltip) {
    const QString elided = project_->fontMetrics().elidedText(title, Qt::ElideMiddle, project_->maximumWidth());
    project_->setText(elided);
    project_->setToolTip(tooltip.isEmpty() ? title : tooltip);
}

QToolButton* HeaderBar::AddAction(QAction* action) {
    auto* button = actions_->AddAction(action);
    button->setObjectName(QStringLiteral("headerAction"));
    button->setAutoRaise(true);
    button->setIconSize(QSize(17, 17));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    if (action->menu() != nullptr) {
        button->setPopupMode(QToolButton::InstantPopup);
    }
    return button;
}

void HeaderBar::AddWidget(QWidget* widget) {
    actions_->AddWidget(widget);
}

void HeaderBar::AddSeparator() {
    actions_->AddSeparator();
}

bool HeaderBar::event(QEvent* event) {
    const auto type = event->type();
    const bool handled = QFrame::event(event);
    if (type == QEvent::Resize || type == QEvent::LayoutRequest || type == QEvent::Show
        || type == QEvent::FontChange || type == QEvent::StyleChange) UpdateActionLayout();
    return handled;
}

void HeaderBar::UpdateActionLayout() {
    if (updatingLayout_ || !actions_ || !switcher_) return;
    updatingLayout_ = true;
    const auto guard = qScopeGuard([this] { updatingLayout_ = false; });
    actions_->Refresh();
    project_->setVisible(width() >= 1200);
    project_->setMaximumWidth(width() >= 1600 ? 160 : 80);
    const auto* brand = findChild<QLabel*>(QStringLiteral("brand"));
    const int identity = 22 + 40 + (brand ? brand->sizeHint().width() : 90)
        + (project_->isHidden() ? 0 : project_->maximumWidth());
    const bool compact = identity + switcher_->sizeHint().width() + actions_->sizeHint().width() > width();
    switcher_->SetCompact(compact);
    switcher_->setFixedWidth(compact ? switcher_->minimumSizeHint().width() : switcher_->sizeHint().width());
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
