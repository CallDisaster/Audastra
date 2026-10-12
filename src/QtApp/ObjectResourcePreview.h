#pragma once
#include "Widgets/ObjectResourcePicker.h"
#include "WorldTerrainAssets.h"

namespace War3Map::QtApp {
[[nodiscard]] Modules::ObjectResourcePreviewHandle MakeObjectResourcePreview(QWidget* parent,
    Result<Modules::WorldTerrainSources> sources, std::string revision,
    std::uint64_t generation, std::uint64_t editRevision, std::function<bool()> isCurrent);
}
