#include "WorldModule.h"
#include "WorldPanelWidgets.h"
#include "WorldViewport.h"
#include "EditorProject.h"
#include "War3MapCore/Render/EditorOverlay.h"

#include <QButtonGroup>
#include <QLabel>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

namespace War3Map::Modules {

QWidget* WorldModule::BuildPathingRestrictionControls(QWidget* parent) {
    using namespace WorldUi;
    terrainPathingSection_ = Surface(parent);
    auto* layout = new QVBoxLayout(terrainPathingSection_);
    layout->setContentsMargins(0, 8, 0, 0); layout->setSpacing(8);
    layout->addWidget(SectionLabel(tr("附加通行限制"), terrainPathingSection_));
    terrainPathingTypes_ = new QButtonGroup(terrainPathingSection_);
    layout->addWidget(Segment(terrainPathingSection_, terrainPathingTypes_,
        {{tr("不可走"), 2}, {tr("不可飞"), 4}, {tr("不可建"), 8}}, "worldTerrainPathingType"));
    terrainPathingTypes_->button(2)->setChecked(true);
    terrainPathingModes_ = new QButtonGroup(terrainPathingSection_);
    layout->addWidget(Segment(terrainPathingSection_, terrainPathingModes_,
        {{tr("追加"), 1}, {tr("撤回新增"), 0}}, "worldTerrainPathingMode"));
    terrainPathingModes_->button(1)->setChecked(true);
    terrainPathingStatus_ = new QLabel(terrainPathingSection_);
    terrainPathingStatus_->setObjectName(QStringLiteral("worldTerrainPathingStatus"));
    terrainPathingStatus_->setWordWrap(true); layout->addWidget(terrainPathingStatus_);
    return terrainPathingSection_;
}

void WorldModule::AppendPathingRestrictionOverlay(std::vector<War3RenderOverlayLineV2>& lines) {
    if (!terrainPathingStatus_ || !terrain_ || !project_.mapEdits_) return;
    const auto* edits = project_.mapEdits_.get();
    if (!edits->HasTerrainPathing()) {
        terrainPathingStatus_->setText(tr("通行图不可读"));
        terrainPathingStatus_->setProperty("pathingAddedCellCount", 0);
        terrainPathingStatus_->setProperty("pathingMarkerCount", 0);
        pathingOverlaySource_.reset(); pathingOverlay_.clear();
        return;
    }
    if (!viewport_ || !viewport_->HasOverlaySupport()) {
        terrainPathingStatus_->setText(tr("渲染器不支持通行格标记"));
        terrainPathingStatus_->setProperty("pathingAddedCellCount", 0);
        terrainPathingStatus_->setProperty("pathingMarkerCount", 0);
        pathingOverlaySource_.reset(); pathingOverlay_.clear();
        return;
    }
    const std::array identity{edits->SessionIdentity(), edits->Revision(), edits->TerrainSourceRevision()};
    if (identity != pathingOverlayIdentity_ || pathingOverlaySource_.lock() != terrain_) {
        pathingOverlayIdentity_ = identity; pathingOverlaySource_ = terrain_; pathingOverlay_.clear();
        const auto preview = edits->QueryPathingRestrictions(1024);
        if (!preview.Succeeded() || !preview.value) {
            terrainPathingStatus_->setText(tr("通行限制不可读"));
            terrainPathingStatus_->setProperty("pathingAddedCellCount", 0);
            terrainPathingStatus_->setProperty("pathingMarkerCount", 0);
            return;
        }
        constexpr std::array<float, 3> color{1.0f, 0.57f, 0.22f};
        for (const auto& cell : preview.value->cells) {
            const auto left = terrain_->header.offset[0] + cell.x * MapData::kPathingCellSize;
            const auto bottom = terrain_->header.offset[1] + cell.y * MapData::kPathingCellSize;
            Render::AppendGroundRectangle(pathingOverlay_, terrain_.get(), static_cast<float>(left), static_cast<float>(bottom),
                static_cast<float>(left + MapData::kPathingCellSize), static_cast<float>(bottom + MapData::kPathingCellSize), color, 8.0f);
        }
        const auto total = preview.value->totalCells, shown = preview.value->cells.size();
        terrainPathingStatus_->setText(total == shown ? tr("新增限制：%1 格").arg(total)
            : tr("新增限制：%1 格（标记 %2）").arg(total).arg(shown));
        terrainPathingStatus_->setProperty("pathingAddedCellCount", QVariant::fromValue(static_cast<qulonglong>(total)));
        terrainPathingStatus_->setProperty("pathingMarkerCount", QVariant::fromValue(static_cast<qulonglong>(shown)));
    }
    lines.insert(lines.end(), pathingOverlay_.begin(), pathingOverlay_.end());
}

} // namespace War3Map::Modules
