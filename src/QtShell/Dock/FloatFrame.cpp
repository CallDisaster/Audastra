#include "Dock/FloatFrame.h"

#include "Icons/SvgIcons.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include <windows.h>

namespace War3Map::Shell {
namespace {

QToolButton* StripButton(const QString& icon, const QString& tip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("floatStripButton"));
    button->setIcon(UiKit::SvgIcon(icon));
    button->setIconSize(QSize(14, 14));
    button->setToolTip(tip);
    button->setAutoRaise(true);
    return button;
}

} // namespace

FloatFrame::FloatFrame(const ModuleInfo& info, QWidget* owner)
    : QWidget(owner, Qt::Window),
      id_(info.id) {
    setObjectName(QStringLiteral("floatFrame"));
    setWindowTitle(info.label);
    setWindowIcon(UiKit::SvgIcon(info.icon));
    setAttribute(Qt::WA_DeleteOnClose, false);

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);

    // 细条：放回主区 / 停靠到右侧。系统标题栏负责移动、缩放和最大化。
    auto* strip = new QWidget(this);
    strip->setObjectName(QStringLiteral("floatStrip"));
    auto* stripLayout = new QHBoxLayout(strip);
    stripLayout->setContentsMargins(8, 2, 4, 2);
    stripLayout->setSpacing(2);
    stripLayout->addStretch(1);
    auto* toSide = StripButton(QStringLiteral("lucide/panel-right"), tr("停靠到右侧"), strip);
    auto* toMain = StripButton(QStringLiteral("lucide/app-window"), tr("放回主编辑区"), strip);
    stripLayout->addWidget(toSide);
    stripLayout->addWidget(toMain);
    layout_->addWidget(strip);

    connect(toSide, &QToolButton::clicked, this, [this]() { emit DockRequested(id_, Area::Side); });
    connect(toMain, &QToolButton::clicked, this, [this]() { emit DockRequested(id_, Area::Main); });
}

void FloatFrame::SetContent(QWidget* content) {
    if (content_ == content) {
        return;
    }
    content_ = content;
    layout_->addWidget(content, 1);
    content->show();
}

QWidget* FloatFrame::TakeContent() {
    QWidget* content = content_;
    if (content != nullptr) {
        layout_->removeWidget(content);
        content->setParent(nullptr);
    }
    content_ = nullptr;
    return content;
}

void FloatFrame::ApplyGeometry(const FloatWindow& model) {
    applying_ = true;
    if (model.maximized) {
        if (!isMaximized()) {
            setGeometry(model.x, model.y, model.w, model.h);
            showMaximized();
        }
    } else {
        if (isMaximized()) {
            showNormal();
        }
        if (geometry() != QRect(model.x, model.y, model.w, model.h)) {
            setGeometry(model.x, model.y, model.w, model.h);
        }
        if (!isVisible()) {
            show();
        }
    }
    applying_ = false;
}

void FloatFrame::ReportGeometry() {
    if (applying_ || !isVisible()) {
        return;
    }
    FloatWindow model;
    model.id = id_;
    // 最大化时保留还原尺寸，取消最大化后回到原位置。
    const QRect normal = isMaximized() ? normalGeometry() : geometry();
    model.x = normal.x();
    model.y = normal.y();
    model.w = normal.width();
    model.h = normal.height();
    model.maximized = isMaximized();
    emit GeometryEdited(model);
}

bool FloatFrame::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG") {
        const auto* msg = static_cast<const MSG*>(message);
        switch (msg->message) {
        case WM_ENTERSIZEMOVE:
            inSizeMove_ = true;
            sizeAtMoveStart_ = size();
            break;
        case WM_MOVING:
            if (inSizeMove_) {
                emit MoveTracking(id_, QCursor::pos());
            }
            break;
        case WM_EXITSIZEMOVE:
            if (inSizeMove_) {
                inSizeMove_ = false;
                // 只有“移动”才可能停靠；调整大小结束不算。
                if (size() == sizeAtMoveStart_) {
                    emit MoveFinished(id_, QCursor::pos());
                }
            }
            break;
        default:
            break;
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

bool FloatFrame::event(QEvent* event) {
    if (event->type() == QEvent::WindowActivate) {
        emit Activated(id_);
    }
    return QWidget::event(event);
}

void FloatFrame::closeEvent(QCloseEvent* event) {
    // 模块不关闭：关闭浮动窗口等于放回主编辑区。
    event->ignore();
    emit DockRequested(id_, Area::Main);
}

void FloatFrame::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    ReportGeometry();
}

void FloatFrame::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    ReportGeometry();
}

void FloatFrame::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        ReportGeometry();
    }
}

} // namespace War3Map::Shell
