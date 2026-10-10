#include "Dock/SideGroup.h"

#include "Dock/ModuleDrag.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace War3Map::Shell {

SideTabBar::SideTabBar(QWidget* parent)
    : QTabBar(parent) {
    setObjectName(QStringLiteral("sideTabs"));
    setDocumentMode(true);
    setDrawBase(false);
    setExpanding(false);
    setElideMode(Qt::ElideRight);
    setUsesScrollButtons(true);
}

void SideTabBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressPos_ = event->position().toPoint();
        pressIndex_ = tabAt(pressPos_);
    }
    QTabBar::mousePressEvent(event);
}

void SideTabBar::mouseMoveEvent(QMouseEvent* event) {
    if (pressIndex_ >= 0 && (event->buttons() & Qt::LeftButton) &&
        PassedDragThreshold(pressPos_, event->position().toPoint())) {
        const int index = pressIndex_;
        pressIndex_ = -1;
        emit DragStarted(index);
        return;
    }
    QTabBar::mouseMoveEvent(event);
}

void SideTabBar::contextMenuEvent(QContextMenuEvent* event) {
    const int index = tabAt(event->pos());
    if (index >= 0) {
        emit MenuRequested(index, event->globalPos());
    }
}

SideGroup::SideGroup(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("shellSide"));
    setAcceptDrops(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    tabs_ = new SideTabBar(this);
    stack_ = new QStackedWidget(this);
    layout->addWidget(tabs_);
    layout->addWidget(stack_, 1);

    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (updating_ || index < 0 || index >= static_cast<int>(ids_.size())) {
            return;
        }
        emit ActivateRequested(ids_[static_cast<std::size_t>(index)]);
    });
    connect(tabs_, &SideTabBar::DragStarted, this, [this](int index) {
        if (index < 0 || index >= static_cast<int>(ids_.size())) {
            return;
        }
        const ModuleId id = ids_[static_cast<std::size_t>(index)];
        if (DragModule(tabs_, id) == DragOutcome::Released) {
            emit DetachRequested(id, QCursor::pos());
        }
    });
    connect(tabs_, &SideTabBar::MenuRequested, this, [this](int index, QPoint pos) {
        if (index >= 0 && index < static_cast<int>(ids_.size())) {
            emit MenuRequested(ids_[static_cast<std::size_t>(index)], pos);
        }
    });
}

void SideGroup::SetModules(const std::vector<ModuleId>& ids, const std::vector<QWidget*>& widgets,
    const std::vector<QString>& labels, const std::vector<QIcon>& icons, const std::optional<ModuleId>& active) {
    updating_ = true;
    // 先移除不再属于右侧组的内容（控件由调用方重新安置，不在这里销毁）。
    for (int index = stack_->count() - 1; index >= 0; --index) {
        QWidget* widget = stack_->widget(index);
        if (std::find(widgets.begin(), widgets.end(), widget) == widgets.end()) {
            stack_->removeWidget(widget);
        }
    }
    while (tabs_->count() > 0) {
        tabs_->removeTab(0);
    }
    ids_ = ids;
    int current = -1;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (stack_->indexOf(widgets[index]) < 0) {
            stack_->addWidget(widgets[index]);
        }
        tabs_->addTab(icons[index], labels[index]);
        if (active && *active == ids[index]) {
            current = static_cast<int>(index);
        }
    }
    if (current < 0 && !ids.empty()) {
        current = 0;
    }
    if (current >= 0) {
        tabs_->setCurrentIndex(current);
        stack_->setCurrentWidget(widgets[static_cast<std::size_t>(current)]);
    }
    updating_ = false;
}

int SideGroup::DropIndex(QPoint globalPos) const {
    const QPoint local = tabs_->mapFromGlobal(globalPos);
    for (int index = 0; index < tabs_->count(); ++index) {
        const QRect rect = tabs_->tabRect(index);
        if (local.x() < rect.center().x()) {
            return index;
        }
    }
    return tabs_->count();
}

void SideGroup::SetDropHighlight(bool on) {
    if (property("dropTarget").toBool() == on) {
        return;
    }
    setProperty("dropTarget", on);
    style()->unpolish(this);
    style()->polish(this);
}

void SideGroup::dragEnterEvent(QDragEnterEvent* event) {
    if (ModuleFromMime(event->mimeData())) {
        SetDropHighlight(true);
        event->acceptProposedAction();
    }
}

void SideGroup::dragMoveEvent(QDragMoveEvent* event) {
    if (ModuleFromMime(event->mimeData())) {
        event->acceptProposedAction();
    }
}

void SideGroup::dragLeaveEvent(QDragLeaveEvent* event) {
    SetDropHighlight(false);
    QFrame::dragLeaveEvent(event);
}

void SideGroup::dropEvent(QDropEvent* event) {
    SetDropHighlight(false);
    const auto id = ModuleFromMime(event->mimeData());
    if (!id) {
        return;
    }
    event->acceptProposedAction();
    emit ModuleDropped(*id, DropIndex(mapToGlobal(event->position().toPoint())));
}

} // namespace War3Map::Shell
