#include "Dock/ModuleSwitcher.h"

#include "Dock/ModuleDrag.h"
#include "Icons/SvgIcons.h"

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionToolButton>

#include <algorithm>

namespace War3Map::Shell {
namespace {

const char* PlacementName(Area area) {
    switch (area) {
    case Area::Side: return "side";
    case Area::Float: return "float";
    case Area::Main: break;
    }
    return "main";
}

QString PlacementHint(Area area) {
    switch (area) {
    case Area::Side: return QObject::tr("停靠在右侧");
    case Area::Float: return QObject::tr("浮动窗口");
    case Area::Main: break;
    }
    return QObject::tr("主编辑区");
}

} // namespace

ModuleChip::ModuleChip(const ModuleInfo& info, QWidget* parent)
    : QToolButton(parent),
      id_(info.id) {
    setObjectName(QStringLiteral("moduleChip"));
    setText(info.label);
    setIcon(UiKit::SvgIcon(info.icon));
    setIconSize(QSize(16, 16));
    setCheckable(true);
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setEnabled(info.available);
    setProperty("placement", "main");
    const QString shortcut = info.shortcut.isEmpty()
        ? QString()
        : QStringLiteral("  %1").arg(info.shortcut.toString(QKeySequence::NativeText));
    setToolTip(info.available
        ? info.label + shortcut + QStringLiteral("\n") + tr("拖出成为浮动窗口，拖到右边缘停靠")
        : info.label + QStringLiteral("\n") + info.unavailableReason);
}

int ModuleChip::TextModeWidth() const {
    // 图标 + 间距 + 文字 + 左右内边距（样式表 8px）+ 边框；中文标签按实际字形宽度计算。
    return iconSize().width() + 6 + fontMetrics().horizontalAdvance(text()) + 2 * 9 + 6;
}

int ModuleChip::IconModeWidth() const {
    QStyleOptionToolButton option;
    initStyleOption(&option);
    option.toolButtonStyle = Qt::ToolButtonIconOnly;
    option.text.clear();
    return style()->sizeFromContents(QStyle::CT_ToolButton, &option, iconSize(), this).width();
}

QSize ModuleChip::sizeHint() const {
    const QSize base = QToolButton::sizeHint();
    if (toolButtonStyle() == Qt::ToolButtonIconOnly) {
        return base;
    }
    // 文字模式宽度只由 TextModeWidth 决定，保证布局分配与“是否切换为图标模式”的判断一致。
    return QSize(TextModeWidth(), base.height());
}

void ModuleChip::SetPlacement(Area area) {
    const QByteArray name = PlacementName(area);
    if (property("placement").toByteArray() == name) {
        return;
    }
    setProperty("placement", name);
    setStatusTip(PlacementHint(area));
    // 动态属性变化后重新套用样式表。
    style()->unpolish(this);
    style()->polish(this);
}

void ModuleChip::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressPos_ = event->position().toPoint();
        pressed_ = true;
    }
    QToolButton::mousePressEvent(event);
}

void ModuleChip::mouseMoveEvent(QMouseEvent* event) {
    if (pressed_ && (event->buttons() & Qt::LeftButton) &&
        PassedDragThreshold(pressPos_, event->position().toPoint())) {
        pressed_ = false;
        setDown(false);
        const ModuleId id = id_;
        // 没有停靠目标接收时，在释放位置浮动。
        if (DragModule(this, id) == DragOutcome::Released) {
            emit DetachRequested(id, QCursor::pos());
        }
        return;
    }
    QToolButton::mouseMoveEvent(event);
}

void ModuleChip::contextMenuEvent(QContextMenuEvent* event) {
    emit MenuRequested(id_, event->globalPos());
}

ModuleSwitcher::ModuleSwitcher(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("moduleSwitcher"));
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(2);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ModuleSwitcher::SetModules(const std::vector<ModuleInfo>& modules) {
    for (auto* chip : chips_) {
        delete chip;
    }
    chips_.clear();
    for (const auto& info : modules) {
        auto* chip = new ModuleChip(info, this);
        connect(chip, &QToolButton::clicked, this, [this, id = info.id]() { emit ActivateRequested(id); });
        connect(chip, &ModuleChip::DetachRequested, this, &ModuleSwitcher::DetachRequested);
        connect(chip, &ModuleChip::MenuRequested, this, &ModuleSwitcher::MenuRequested);
        layout_->addWidget(chip);
        chips_.push_back(chip);
    }
    UpdateCompactMode();
    updateGeometry();
}

void ModuleSwitcher::SetState(const DockLayout& layout, const std::vector<ModuleId>& all) {
    const auto shown = MainShown(layout, all);
    for (auto* chip : chips_) {
        const Area area = WhereIs(layout, chip->Id());
        chip->SetPlacement(area);
        chip->setChecked(area == Area::Main && shown && *shown == chip->Id());
    }
}

ModuleChip* ModuleSwitcher::Chip(const ModuleId& id) const {
    for (auto* chip : chips_) {
        if (chip->Id() == id) {
            return chip;
        }
    }
    return nullptr;
}

int ModuleSwitcher::FullWidth() const {
    int width = 0;
    for (auto* chip : chips_) {
        width += chip->TextModeWidth();
    }
    return width + layout_->spacing() * static_cast<int>(chips_.size());
}

QSize ModuleSwitcher::sizeHint() const {
    return QSize(FullWidth(), QWidget::sizeHint().height());
}

QSize ModuleSwitcher::minimumSizeHint() const {
    int compact = std::max(0, static_cast<int>(chips_.size()) - 1) * layout_->spacing();
    for (const auto* chip : chips_) compact += chip->IconModeWidth();
    return QSize(compact, QWidget::minimumSizeHint().height());
}

void ModuleSwitcher::SetCompact(bool compact) {
    forceCompact_ = compact;
    UpdateCompactMode();
}

void ModuleSwitcher::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    UpdateCompactMode();
}

void ModuleSwitcher::UpdateCompactMode() {
    const bool compact = forceCompact_ || width() < FullWidth();
    if (compact == compact_ && !chips_.empty() &&
        chips_.front()->toolButtonStyle() == (compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon)) {
        return;
    }
    compact_ = compact;
    for (auto* chip : chips_) {
        chip->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    }
}

} // namespace War3Map::Shell
