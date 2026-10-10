#include "MapSettingsModule.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>

#include <algorithm>
#include <array>

namespace War3Map::Modules {

bool MapSettingsModule::eventFilter(QObject* watched, QEvent* event) {
    if (!rebuilding_ && event->type() == QEvent::Resize &&
        (watched == page_ || watched->objectName() == QStringLiteral("mapSettingsFormViewport"))) {
        UpdateResponsiveLayout();
    }
    return QObject::eventFilter(watched, event);
}

void MapSettingsModule::UpdateResponsiveLayout() {
    if (rebuilding_ || !page_ || !sections_ || !compactSections_) return;

    // 以模块实际宽度降级；只换导航与排版，不重建表单或提交草稿。
    const bool compact = page_->width() < 760;
    const bool navigationHasFocus = sections_->hasFocus() || compactSections_->hasFocus();
    sections_->setVisible(!compact);
    title_->setVisible(!compact);
    compactSections_->setVisible(compact);
    if (navigationHasFocus) {
        if (compact) compactSections_->setFocus(Qt::OtherFocusReason);
        else sections_->setFocus(Qt::OtherFocusReason);
    }
    const auto wrap = compact ? QFormLayout::WrapAllRows : QFormLayout::WrapLongRows;
    for (auto* form : forms_) {
        if (form->rowWrapPolicy() != wrap) form->setRowWrapPolicy(wrap);
    }

    if (!flagsLayout_ || pages_->count() == 0) return;
    const auto* scroll = qobject_cast<QScrollArea*>(pages_->widget(0));
    if (!scroll || !scroll->widget()) return;
    const auto margins = scroll->widget()->layout()->contentsMargins();
    const int available = scroll->viewport()->width() - margins.left() - margins.right();
    std::array<int, 2> columnWidths{};
    for (std::size_t i = 0; i < mapFlagFields_.size(); ++i) {
        columnWidths[i % 2] = std::max(columnWidths[i % 2], mapFlagFields_[i].widget->sizeHint().width());
    }
    const int columns = available >= columnWidths[0] + columnWidths[1] + flagsLayout_->horizontalSpacing()
        ? 2 : 1;
    if (columns == flagColumns_) return;
    flagColumns_ = columns;
    for (const auto& field : mapFlagFields_) flagsLayout_->removeWidget(field.widget);
    for (std::size_t i = 0; i < mapFlagFields_.size(); ++i) {
        const int index = static_cast<int>(i);
        flagsLayout_->addWidget(mapFlagFields_[i].widget, index / columns, index % columns);
    }
}

} // namespace War3Map::Modules
