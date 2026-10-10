#include "WorldModule.h"
#include <QPushButton>
#include <QWidget>

namespace War3Map::Modules {
void WorldModule::UpdateToolbarLayout() {
    if (!page_) return;
    auto* toolbar = page_->findChild<QWidget*>(QStringLiteral("worldToolbar"));
    if (!toolbar) return;
    const auto buttons = toolbar->findChildren<QPushButton*>();
    int required = 80;
    for (const auto* button : buttons) {
        const auto label = button->property("worldFullLabel").toString();
        required += button->fontMetrics().horizontalAdvance(label) + 52;
    }
    const bool compact = required > toolbar->width();
    for (auto* button : buttons) {
        const auto text = compact ? QString{} : button->property("worldFullLabel").toString();
        if (button->text() != text) button->setText(text);
        button->setAccessibleName(button->property("worldFullLabel").toString());
    }
}
} // namespace War3Map::Modules
