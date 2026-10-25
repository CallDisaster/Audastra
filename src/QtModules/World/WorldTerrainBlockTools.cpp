#include "WorldModule.h"
#include "WorldViewport.h"
#include "EditorProject.h"
#include "Widgets/ElidedLabel.h"
#include "War3MapCore/Render/EditorOverlay.h"

#include <QLabel>
#include <QPushButton>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace War3Map::Modules {
namespace {
bool SameBlockHeader(const MapData::TerrainHeader& first, const MapData::TerrainHeader& second) {
    return first.version == second.version && first.mainTileset == second.mainTileset
        && first.usesCustomTileset == second.usesCustomTileset && first.width == second.width
        && first.height == second.height && first.offset == second.offset
        && first.groundTilesets == second.groundTilesets && first.cliffTilesets == second.cliffTilesets;
}

std::optional<std::array<std::uint32_t, 2>> BlockCellAt(const MapData::TerrainGridSnapshot& terrain,
    double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || terrain.header.width < 2 || terrain.header.height < 2) return {};
    const double gx = std::floor((x - terrain.header.offset[0]) / MapData::TerrainGridSnapshot::tileSize);
    const double gy = std::floor((y - terrain.header.offset[1]) / MapData::TerrainGridSnapshot::tileSize);
    if (gx < 0 || gy < 0 || gx >= terrain.header.width - 1 || gy >= terrain.header.height - 1) return {};
    return std::array<std::uint32_t, 2>{static_cast<std::uint32_t>(gx), static_cast<std::uint32_t>(gy)};
}
} // namespace

bool WorldModule::TerrainBlockClipboardCurrent() const {
    if (!terrainBlockClipboard_ || !project_.mapEdits_ || !project_.mapEdits_->Terrain()) return false;
    const auto& copied = *terrainBlockClipboard_;
    return copied.mapKey == MapKey() && copied.session == project_.mapEdits_->SessionIdentity()
        && copied.sourceRevision == project_.mapEdits_->TerrainSourceRevision()
        && SameBlockHeader(copied.header, project_.mapEdits_->Terrain()->header);
}

void WorldModule::StopTerrainBlockTool(bool clearClipboard) {
    const bool wasSelecting = terrainBlockSelecting_, wasPasting = terrainBlockPasting_;
    terrainBlockSelecting_ = terrainBlockPasting_ = false;
    terrainBlockDestination_.reset();
    if (wasSelecting || wasPasting) brushCursor_.reset();
    if (viewport_ && (wasSelecting || wasPasting)) {
        viewport_->SetTerrainBlockSelection(false);
        if (wasPasting) viewport_->SetPlacing(false);
    }
    if (clearClipboard) { terrainBlockClipboard_.reset(); terrainBlockSelection_.reset(); }
    if (viewport_) UpdateOverlay();
}

void WorldModule::StartTerrainBlockSelection() {
    if (!viewport_ || !project_.mapEdits_ || !rawTerrain_ || rawTerrain_ != project_.mapEdits_->Terrain()
        || worker_ || strokeBase_ || waterCommit_ || project_.savingMap_) {
        if (terrainBlockStatus_) terrainBlockStatus_->setText(tr("请等待当前地形显示完成后再框选"));
        return;
    }
    StopPlacing(); CancelTerrainStroke();
    resumeBrushAfterRender_ = false;
    if (terrainToggle_->isChecked()) terrainToggle_->setChecked(false);
    terrainBlockPasting_ = false; terrainBlockSelecting_ = true;
    terrainBlockDestination_.reset();
    viewport_->SetTerrainBlockSelection(true);
    ClearSelection(); UpdateToolState(); UpdateOverlay(); viewport_->setFocus();
    terrainBlockStatus_->setText(tr("拖动选择地格；包括东、北侧顶点。Ctrl+C复制，Ctrl+V粘贴。"));
}

void WorldModule::SelectTerrainBlock(double x0, double y0, double x1, double y1) {
    if (!terrainBlockSelecting_ || !project_.mapEdits_ || project_.mapEdits_->Terrain() != rawTerrain_) return;
    const auto first = BlockCellAt(*rawTerrain_, x0, y0), last = BlockCellAt(*rawTerrain_, x1, y1);
    if (!first || !last) { terrainBlockStatus_->setText(tr("框选端点超出可复制地格")); return; }
    terrainBlockSelection_ = MapData::TerrainCellRect{std::min((*first)[0], (*last)[0]), std::min((*first)[1], (*last)[1]),
        std::max((*first)[0], (*last)[0]) + 1, std::max((*first)[1], (*last)[1]) + 1};
    const auto& cells = *terrainBlockSelection_;
    terrainBlockStatus_->setText(tr("已选 %1 × %2 地格 · Ctrl+C复制；绝对高度，源块变化须重新复制")
        .arg(cells.x1 - cells.x0).arg(cells.y1 - cells.y0));
    UpdateOverlay();
}

void WorldModule::CopyTerrainBlock() {
    if (!terrainBlockSelection_ || !project_.mapEdits_ || !rawTerrain_ || project_.mapEdits_->Terrain() != rawTerrain_
        || waterCommit_ || project_.savingMap_) {
        terrainBlockStatus_->setText(tr("请先在已显示的地面上框选地形")); return;
    }
    const auto fingerprint = MapData::CaptureTerrainBlockFingerprint(*rawTerrain_, *terrainBlockSelection_);
    if (!fingerprint.Succeeded() || !fingerprint.value) {
        terrainBlockStatus_->setText(tr("未复制：%1").arg(WorldDiagnosticText(fingerprint.diagnostics))); return;
    }
    terrainBlockClipboard_ = TerrainBlockClipboard{*terrainBlockSelection_, *fingerprint.value, MapKey(),
        project_.mapEdits_->SessionIdentity(), project_.mapEdits_->TerrainSourceRevision(), rawTerrain_->header};
    terrainBlockStatus_->setText(tr("已复制源区域 · Ctrl+V粘贴；绝对高度，源块变化须重新复制"));
}

void WorldModule::StartTerrainBlockPaste() {
    if (!TerrainBlockClipboardCurrent()) {
        terrainBlockClipboard_.reset(); terrainBlockDestination_.reset();
        terrainBlockStatus_->setText(tr("剪贴板来源、地图或贴图表已变化，请重新复制地形")); return;
    }
    if (!viewport_ || worker_ || waterCommit_ || project_.savingMap_ || project_.mapEdits_->Terrain() != rawTerrain_) {
        terrainBlockStatus_->setText(tr("请等待当前地形显示完成后再粘贴")); return;
    }
    StopPlacing(); CancelTerrainStroke(); resumeBrushAfterRender_ = false;
    if (terrainToggle_->isChecked()) terrainToggle_->setChecked(false);
    terrainBlockSelecting_ = false; terrainBlockPasting_ = true;
    viewport_->SetTerrainBlockSelection(false); viewport_->SetBrushMode(false); viewport_->SetPlacing(true);
    ClearSelection(); UpdateToolState(); QueueWaterBrushCapture(); viewport_->setFocus();
    terrainBlockStatus_->setText(tr("目标轮廓尚未提交 · 单击粘贴，Esc取消；绝对高度，源块变化须重新复制"));
    if (const auto& hover = viewport_->HoverCoordinate()) HoverTerrainBlock(hover->position[0], hover->position[1]);
}

void WorldModule::HoverTerrainBlock(double x, double y) {
    terrainBlockDestination_.reset();
    if (terrainBlockPasting_ && TerrainBlockClipboardCurrent() && rawTerrain_) {
        const auto destination = BlockCellAt(*rawTerrain_, x, y);
        const auto& cells = terrainBlockClipboard_->cells;
        if (destination && (*destination)[0] + cells.x1 - cells.x0 < rawTerrain_->header.width
            && (*destination)[1] + cells.y1 - cells.y0 < rawTerrain_->header.height)
            terrainBlockDestination_ = destination;
    }
    UpdateOverlay();
}

void WorldModule::PasteTerrainBlock(double x, double y) {
    if (!terrainBlockPasting_ || !TerrainBlockClipboardCurrent()) {
        terrainBlockClipboard_.reset(); StopTerrainBlockTool(); UpdateToolState();
        terrainBlockStatus_->setText(tr("地图或贴图表已变化，请重新复制地形")); return;
    }
    HoverTerrainBlock(x, y);
    if (!terrainBlockDestination_ || !project_.mapEdits_ || project_.mapEdits_->Terrain() != rawTerrain_
        || waterCommit_ || worker_ || project_.savingMap_) {
        terrainBlockStatus_->setText(tr("目标超出地图或地形正忙，本次未粘贴")); return;
    }
    if (!WaterBrushCaptureCurrent()) {
        QueueWaterBrushCapture(); terrainBlockStatus_->setText(tr("正在读取实际地形规则，请就绪后重新单击")); return;
    }
    auto frozen = project_.mapEdits_->FreezeForSave(MapData::TerrainWaterBrushLimits{}.maximumCandidateBytes);
    if (!frozen.Succeeded() || !frozen.value) {
        terrainBlockStatus_->setText(tr("未粘贴：%1").arg(WorldDiagnosticText(frozen.diagnostics))); return;
    }
    const auto capture = waterCapture_;
    const auto& copied = *terrainBlockClipboard_;
    const MapData::TerrainBlockPasteIntent intent{1, copied.cells, *terrainBlockDestination_, copied.fingerprint};
    // 先退出交互，再发布后台事务；SetChecked/SetPlacing的取消信号不能取消新任务。
    StopTerrainBlockTool();
    // 保持地形面板；视口待最终地形呈现后才恢复框选，准备期间不接受旧地面手势。
    terrainBlockSelecting_ = true;
    if (terrainToggle_) terrainToggle_->setEnabled(false);
    waterCommit_ = std::make_shared<WaterBrushCommit>(WaterBrushCommit{capture, intent, {}, false, std::move(*frozen.value)});
    waterCommitToken_ = project_.BeginPendingTerrainCommit();
    waterCommitPending_ = true; ++terrainRenderSerial_; pendingTerrainRender_.reset();
    terrainBlockStatus_->setText(tr("正在准备地形块；源区域已变化时会拒绝，可重新复制"));
    selectionStatus_->setText(tr("正在核验并准备地形块…"));
    StartTerrainRender(); UpdateToolState();
}

void WorldModule::AppendTerrainBlockOverlay(std::vector<War3RenderOverlayLineV2>& lines) const {
    if (!terrain_ || (!terrainBlockSelecting_ && !terrainBlockPasting_)) return;
    const auto append = [&](const MapData::TerrainCellRect& cells, const Render::OverlayColor& color) {
        const auto& origin = terrain_->header.offset;
        constexpr auto size = MapData::TerrainGridSnapshot::tileSize;
        Render::AppendGroundRectangle(lines, terrain_.get(), origin[0] + cells.x0 * size, origin[1] + cells.y0 * size,
            origin[0] + cells.x1 * size, origin[1] + cells.y1 * size, color, 6.0f);
    };
    if (terrainBlockSelection_) append(*terrainBlockSelection_, {0.36f, 0.92f, 0.68f});
    if (terrainBlockPasting_ && TerrainBlockClipboardCurrent() && terrainBlockDestination_) {
        const auto& source = terrainBlockClipboard_->cells;
        const auto& destination = *terrainBlockDestination_;
        append({destination[0], destination[1], destination[0] + source.x1 - source.x0,
            destination[1] + source.y1 - source.y0}, {0.95f, 0.78f, 0.32f});
    }
}

} // namespace War3Map::Modules
