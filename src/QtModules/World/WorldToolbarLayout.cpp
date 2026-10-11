#include "WorldModule.h"
#include "Widgets/ResponsiveToolBar.h"
#include <QWidget>

namespace War3Map::Modules {
void WorldModule::UpdateToolbarLayout() {
    if (!page_) return;
    if (auto* toolbar = page_->findChild<UiKit::ResponsiveToolBar*>(QStringLiteral("worldToolbar"))) toolbar->Refresh();
}
} // namespace War3Map::Modules
