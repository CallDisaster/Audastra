// 世界视图的编辑交互：摆放/区域/镜头的选择与拖动、删除、摆放面板、编辑叠加线与状态文本。
// 数据修改全部经 MapData::PlacementEditing 提交到共用地图数据撤销栈，本文件只做交互与显示。
#include "WorldModule.h"
#include "WorldAnimationPlayer.h"
#include "WorldPlacementPreview.h"
#include "WorldPlacementModels.h"
#include "WorldInstanceTiming.h"
#include "WorldPathingHelperPresenter.h"
#include "WorldSelectionProperties.h"
#include "WorldViewport.h"
#include "EditorProject.h"
#include "ObjectNames.h"
#include "ObjectContextProvider.h"
#include "War3MapCore/Services/PlacementCreation.h"
#include "ObjectViewEntries.h"
#include "Commands/MapDataCommands.h"
#include "War3MapCore/Render/EditorOverlay.h"
#include "War3MapCore/Render/ModelPicking.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "Widgets/ElidedLabel.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QThread>
#include <QUndoStack>
#include <QSignalBlocker>
#include <QSpinBox>

#include <algorithm>
#include <type_traits>
#include <cmath>
#include <limits>

namespace War3Map::Modules {
namespace {

constexpr float kRegionGrid = 32.0f;     // 原版区域按 32 单位对齐
constexpr double kCameraPickRadius = 64; // 镜头目标点选半径
constexpr double kCornerRadius = 32;     // 区域角手柄半径
constexpr Render::OverlayColor kSelected{0.36f, 0.92f, 0.68f}; // R03 薄荷绿
constexpr Render::OverlayColor kCameraColor{0.95f, 0.78f, 0.32f};

float Snap(double value) { return static_cast<float>(std::round(value / kRegionGrid) * kRegionGrid); }

std::array<std::array<double, 2>, 4> Corners(float left, float bottom, float right, float top) {
    return {{{left, bottom}, {right, bottom}, {right, top}, {left, top}}};
}

struct RegionDragHandle {
    int corner = -1;
    int edge = -1;
};

RegionDragHandle PickRegionDragHandle(const MapData::RegionRecord& region, double x, double y) {
    RegionDragHandle handle;
    const auto corners = Corners(region.left, region.bottom, region.right, region.top);
    double best = kCornerRadius;
    for (int i = 0; i < 4; ++i) {
        const double distance = std::hypot(corners[i][0] - x, corners[i][1] - y);
        if (distance <= best) { best = distance; handle.corner = i; }
    }
    if (handle.corner >= 0) return handle;
    const std::array<double, 4> distances{
        std::abs(x - region.left), std::abs(y - region.bottom),
        std::abs(x - region.right), std::abs(y - region.top)};
    for (int i = 0; i < 4; ++i) {
        const bool within = i % 2 == 0 ? y >= region.bottom && y <= region.top
            : x >= region.left && x <= region.right;
        if (within && distances[i] <= best) { best = distances[i]; handle.edge = i; }
    }
    return handle;
}

std::optional<ObjectData::ObjectType> PlaceableType(const QString& kind) {
    for (const auto type : {ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item,
             ObjectData::ObjectType::Doodad, ObjectData::ObjectType::Destructable})
        if (kind == QString::fromUtf8(ObjectData::ToString(type))) return type;
    return std::nullopt;
}

std::vector<std::array<float, 12>> PreviewTransforms(const std::vector<Render::ModelInstanceRequest>& requests,
    const std::optional<std::array<float, 3>>& hover, bool pasting, const std::array<float, 3>& anchor,
    const std::optional<float>& oldGround) {
    if (!hover || (pasting && !oldGround)) return {};
    std::vector<std::array<float, 12>> transforms;
    transforms.reserve(requests.size());
    for (auto request : requests) {
        if (pasting) {
            request.position[0] += (*hover)[0] - anchor[0];
            request.position[1] += (*hover)[1] - anchor[1];
            request.position[2] += (*hover)[2] - *oldGround;
        } else request.position = WorldPlacementDisplayPosition(request, *hover);
        transforms.push_back(Render::ModelInstanceTransform(request));
    }
    return transforms;
}

} // namespace

// ── 选择 ──

std::optional<std::uint32_t> WorldModule::SelectedRequest() const {
    if (!selection_) return std::nullopt;
    const auto found = std::find(placementIds_.begin(), placementIds_.end(), *selection_);
    if (found == placementIds_.end()) return std::nullopt;
    return static_cast<std::uint32_t>(found - placementIds_.begin());
}

std::optional<std::uint32_t> WorldModule::InstanceOfRequest(std::uint32_t request) const {
    if (!scene_) return std::nullopt;
    const auto found = std::find(scene_->instanceRequests.begin(), scene_->instanceRequests.end(), request);
    if (found == scene_->instanceRequests.end()) return std::nullopt;
    return static_cast<std::uint32_t>(found - scene_->instanceRequests.begin());
}

const MapData::RegionRecord* WorldModule::SelectedRegion() const {
    if (!selectedRegion_ || selectedRegion_->index >= regions_.size()) return nullptr;
    const auto& region = regions_[selectedRegion_->index];
    return MapData::MakeRegionId(selectedRegion_->index, region) == *selectedRegion_ ? &region : nullptr;
}

const MapData::CameraRecord* WorldModule::SelectedCamera() const {
    if (!selectedCamera_ || selectedCamera_->index >= cameras_.size()) return nullptr;
    const auto& camera = cameras_[selectedCamera_->index];
    return MapData::MakeCameraId(selectedCamera_->index, camera) == *selectedCamera_ ? &camera : nullptr;
}

void WorldModule::ClearSelection() {
    if (RefreshSelectionContext() && worldSelection_.Version()) {
        const auto result = worldSelection_.Select(SelectionContext(), *worldSelection_.Version(),
            Services::SelectionMode::Replace, {});
        if (result.Succeeded()) ApplySelectionSnapshot(*result.value);
    } else { selectedPlacements_.clear(); selection_.reset(); selectedRegion_.reset(); selectedCamera_.reset(); }
    dragGroup_.clear();
    selectionName_.clear(); selectionIconPath_.clear(); selectionType_.reset(); selectionOwner_.reset();
    dragStart_.reset(); dragPosition_.reset(); dragRotation_.reset(); regionDraft_.reset(); cameraDraft_.reset();
    rotationOrigin_.reset(); dragSelectionVersion_.reset(); dragMapKey_.clear();
    if (viewport_) viewport_->SetSelectedInstance(std::nullopt);
}

void WorldModule::ValidateSelection() {
    (void)RefreshSelectionContext(); // typed身份仍存在即可；没有模型不等于地图对象被删除。
    if (viewport_) viewport_->SetSelectedInstance(SelectedRequest() ? InstanceOfRequest(*SelectedRequest()) : std::nullopt);
}

void WorldModule::PresentSelection() {
    selectionName_.clear(); selectionIconPath_.clear(); selectionType_.reset(); selectionOwner_.reset();
    if (selection_) {
        const auto request = SelectedRequest();
        viewport_->SetSelectedInstance(request ? InstanceOfRequest(*request) : std::nullopt);
        const bool unit = selection_->file == MapData::PlacementFile::Units;
        selectionName_ = QString::fromUtf8(selection_->rawcode);
        const auto first = unit ? ObjectData::ObjectType::Unit : ObjectData::ObjectType::Destructable;
        const auto second = unit ? ObjectData::ObjectType::Item : ObjectData::ObjectType::Doodad;
        if (const auto document = CurrentDocument()) {
            auto presentation = PlacementPresentation(project_, *document, selection_->rawcode, first, second);
            selectionName_ = std::move(presentation.name);
            selectionIconPath_ = std::move(presentation.iconPath);
            if (unit && document->Units() && selection_->index < document->Units()->units.size())
                selectionOwner_ = document->Units()->units[selection_->index].owner;
        }
        selectionType_ = PlacementType(project_, selection_->rawcode, first, second);
        if (unit && selection_->rawcode == "sloc") {
            selectionName_ = tr("起始位置");
            selectionIconPath_.clear(); selectionType_.reset();
        }
    } else {
        viewport_->SetSelectedInstance(std::nullopt);
        if (selectedRegion_) selectionName_ = QString::fromUtf8(selectedRegion_->name);
        if (selectedCamera_) selectionName_ = QString::fromUtf8(selectedCamera_->name);
    }
    UpdateOverlay(); UpdateSelectionText();
}

void WorldModule::SelectInstance(int instance, int modifiers) {
    std::vector<Services::WorldObjectId> ids;
    if (instance >= 0 && scene_ && static_cast<std::size_t>(instance) < scene_->instanceRequests.size()) {
        const auto request = scene_->instanceRequests[static_cast<std::size_t>(instance)];
        if (request < placementIds_.size()) ids.emplace_back(placementIds_[request]);
    }
    const auto keys = Qt::KeyboardModifiers(modifiers);
    (void)SelectObjects(ids, keys.testFlag(Qt::ControlModifier) ? Services::SelectionMode::Subtract
        : keys.testFlag(Qt::ShiftModifier) ? Services::SelectionMode::Add : Services::SelectionMode::Replace);
}

void WorldModule::SelectBox(const QVector<int>& instances, int modifiers) {
    std::vector<Services::WorldObjectId> ids;
    for (const auto instance : instances) {
        if (instance < 0 || !scene_ || static_cast<std::size_t>(instance) >= scene_->instanceRequests.size()) continue;
        const auto request = scene_->instanceRequests[static_cast<std::size_t>(instance)];
        if (request < placementIds_.size()) ids.emplace_back(placementIds_[request]);
    }
    const auto keys = Qt::KeyboardModifiers(modifiers);
    (void)SelectObjects(ids, keys.testFlag(Qt::ControlModifier) ? Services::SelectionMode::Subtract
        : keys.testFlag(Qt::ShiftModifier) ? Services::SelectionMode::Add : Services::SelectionMode::Replace);
}

void WorldModule::SelectGround(double x, double y, int modifiers) {
    // 辅助对象只在目标/边框上命中；区域内部留给实例中心框选。
    std::optional<Services::WorldObjectId> candidate;
    if (const auto start = PickStartLocation(x, y, false)) candidate = *start;
    if (!candidate && showCameras_->isChecked()) {
        double best = kCameraPickRadius;
        for (std::uint32_t i = 0; i < cameras_.size(); ++i) {
            const Services::WorldObjectId id = MapData::MakeCameraId(i, cameras_[i]);
            const auto flags = worldSelection_.Flags(id);
            if (!flags.visible || flags.locked) continue;
            const double distance = std::hypot(cameras_[i].targetX - x, cameras_[i].targetY - y);
            if (distance <= best) { best = distance; candidate = id; }
        }
    }
    if (!candidate && showRegions_->isChecked()) {
        double smallest = 0;
        for (std::uint32_t i = 0; i < regions_.size(); ++i) {
            const auto& region = regions_[i];
            const Services::WorldObjectId id = MapData::MakeRegionId(i, region);
            const auto flags = worldSelection_.Flags(id);
            if (!flags.visible || flags.locked || x < region.left - kCornerRadius || x > region.right + kCornerRadius
                || y < region.bottom - kCornerRadius || y > region.top + kCornerRadius) continue;
            const bool border = std::abs(x - region.left) <= kCornerRadius || std::abs(x - region.right) <= kCornerRadius
                || std::abs(y - region.bottom) <= kCornerRadius || std::abs(y - region.top) <= kCornerRadius;
            if (!border) continue;
            const double area = double(region.right - region.left) * double(region.top - region.bottom);
            if (!candidate || area < smallest) { smallest = area; candidate = id; }
        }
    }
    const auto keys = Qt::KeyboardModifiers(modifiers);
    (void)SelectObjects(candidate ? std::span(&*candidate, 1) : std::span<const Services::WorldObjectId>{},
        keys.testFlag(Qt::ControlModifier) ? Services::SelectionMode::Subtract
        : keys.testFlag(Qt::ShiftModifier) ? Services::SelectionMode::Add : Services::SelectionMode::Replace);
}

bool WorldModule::HandleAt(double x, double y) const {
    if (!SelectionEditable()) return false;
    if (PickStartLocation(x, y, true)) return true;
    if (const auto* region = SelectedRegion()) {
        const auto handle = PickRegionDragHandle(*region, x, y);
        return handle.corner >= 0 || handle.edge >= 0;
    }
    if (const auto* camera = SelectedCamera()) return std::hypot(camera->targetX - x, camera->targetY - y) <= kCameraPickRadius;
    return false;
}

// ── 拖动 ──

void WorldModule::StartDrag(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) { CancelDrag(); return; }
    if (!SelectionEditable() || !worldSelection_.Version()) return;
    dragStart_ = {x, y};
    dragGroup_.clear();
    rotationOrigin_.reset(); dragSelectionVersion_ = *worldSelection_.Version();
    dragMapKey_ = MapKey(); dragGeneration_ = generation_;
    regionCorner_ = -1;
    regionEdge_ = -1;
    regionDraft_.reset(); cameraDraft_.reset();
    if (const auto* region = SelectedRegion()) {
        // 与视口按下门禁共用命中规则，避免外角方形余区被误判为整区域平移。
        // 按下时确定手柄，拖过其他边也不切换；四角优先。
        const auto handle = PickRegionDragHandle(*region, x, y);
        regionCorner_ = handle.corner;
        regionEdge_ = handle.edge;
    }
}

void WorldModule::DragSelection(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) { CancelDrag(); return; }
    if (!SelectionEditable()) { CancelDrag(); return; }
    if (!dragStart_) StartDrag(x, y);
    if (!dragStart_ || !dragSelectionVersion_ || dragMapKey_ != MapKey() || dragGeneration_ != generation_
        || !worldSelection_.Snapshot(SelectionContext(), *dragSelectionVersion_).Succeeded()) { CancelDrag(); return; }
    const double dx = x - (*dragStart_)[0], dy = y - (*dragStart_)[1];
    if (const auto* region = SelectedRegion()) {
        std::array<float, 4> bounds{region->left, region->bottom, region->right, region->top};
        if (regionCorner_ < 0 && regionEdge_ < 0) {
            const float sx = Snap(dx), sy = Snap(dy);
            bounds = {bounds[0] + sx, bounds[1] + sy, bounds[2] + sx, bounds[3] + sy};
        } else {
            // 只吸附被拖动的边。对边即使来自表格的小数坐标也逐字保留；钳制点仍在网格上。
            const auto lower = [](double value, float opposite) {
                return std::min(Snap(value), static_cast<float>(std::floor((opposite - kRegionGrid) / kRegionGrid) * kRegionGrid));
            };
            const auto upper = [](double value, float opposite) {
                return std::max(Snap(value), static_cast<float>(std::ceil((opposite + kRegionGrid) / kRegionGrid) * kRegionGrid));
            };
            if (regionEdge_ == 0 || regionCorner_ == 0 || regionCorner_ == 3) bounds[0] = lower(region->left + dx, region->right);
            if (regionEdge_ == 1 || regionCorner_ == 0 || regionCorner_ == 1) bounds[1] = lower(region->bottom + dy, region->top);
            if (regionEdge_ == 2 || regionCorner_ == 1 || regionCorner_ == 2) bounds[2] = upper(region->right + dx, region->left);
            if (regionEdge_ == 3 || regionCorner_ == 2 || regionCorner_ == 3) bounds[3] = upper(region->top + dy, region->bottom);
        }
        regionDraft_ = bounds;
    } else if (const auto* camera = SelectedCamera()) {
        cameraDraft_ = std::array<float, 2>{static_cast<float>(camera->targetX + dx), static_cast<float>(camera->targetY + dy)};
    } else if (!selectedPlacements_.empty()) {
        if (!terrain_) return;
        const auto document = CurrentDocument();
        if (!document) return;
        auto planned = BuildSelectedPlacementOffsetMoves(*document, *terrain_, selectedPlacements_, {dx, dy});
        if (!planned.Succeeded()) return; // 一员无效，整组保持上一份合法草稿。
        auto moves = std::move(*planned.value);
        for (const auto& move : moves) {
            if (selection_ && move.id == *selection_) dragPosition_ = move.position;
            const auto found = std::find(placementIds_.begin(), placementIds_.end(), move.id);
            if (!scene_ || found == placementIds_.end()) continue;
            const auto index = static_cast<std::uint32_t>(found - placementIds_.begin());
            if (index >= requests_.size()) continue;
            const auto instance = InstanceOfRequest(index);
            if (!instance) continue;
            auto moved = requests_[index]; moved.position = WorldPlacementDisplayPosition(moved, move.position);
            const auto transform = Render::ModelInstanceTransform(moved);
            std::copy(transform.begin(), transform.end(), scene_->instances[*instance].transform);
        }
        dragGroup_ = std::move(moves);
        if (scene_) (void)viewport_->UpdateModelInstances();
    }
    UpdateOverlay(); UpdateSelectionText();
}

void WorldModule::StartRotation(double x, double y) {
    if (!SelectionEditable() || !selection_ || selectedPlacements_.empty() || !terrain_ || !worldSelection_.Version()) return;
    const auto document = CurrentDocument();
    if (!document) return;
    auto origin = CaptureWorldPlacementRotationOrigin(*document, selectedPlacements_, {x, y});
    if (!origin.Succeeded()) {
        project_.AddDiagnostics(origin.diagnostics); CancelDrag();
        selectionStatus_->setText(tr("不能旋转：%1").arg(WorldDiagnosticText(origin.diagnostics))); return;
    }
    dragStart_ = {x, y};
    dragRotation_.reset(); dragPosition_.reset(); dragGroup_.clear();
    rotationOrigin_ = std::move(*origin.value); dragSelectionVersion_ = *worldSelection_.Version();
    dragMapKey_ = MapKey(); dragGeneration_ = generation_;
}

void WorldModule::RotateSelection(double x, double y) {
    if (!rotationOrigin_ || !dragSelectionVersion_) return;
    if (!SelectionEditable() || dragMapKey_ != MapKey() || dragGeneration_ != generation_
        || rotationOrigin_->ids != selectedPlacements_
        || !worldSelection_.Snapshot(SelectionContext(), *dragSelectionVersion_).Succeeded()) { CancelDrag(); return; }
    if (std::hypot(x - rotationOrigin_->center[0], y - rotationOrigin_->center[1]) < 1.0) return;
    const auto document = CurrentDocument();
    if (!document || !terrain_) { CancelDrag(); return; }
    auto planned = BuildSelectedPlacementRotationMoves(*document, *terrain_, *rotationOrigin_, {x, y},
        rotationStep_ ? rotationStep_->currentData().toDouble() : 0.0);
    if (!planned.Succeeded()) {
        project_.AddDiagnostics(planned.diagnostics); CancelDrag();
        selectionStatus_->setText(tr("旋转已取消：%1").arg(WorldDiagnosticText(planned.diagnostics))); return;
    }
    // 先取得整组合法草稿，模型只是当前可显示项的预览；缺模型不会丢失 typed 对象的旋转。
    for (const auto& move : *planned.value) {
        if (selection_ && move.id == *selection_) { dragPosition_ = move.position; dragRotation_ = move.rotation; }
        const auto found = std::find(placementIds_.begin(), placementIds_.end(), move.id);
        if (found == placementIds_.end() || !scene_) continue;
        const auto request = static_cast<std::uint32_t>(found - placementIds_.begin());
        const auto instance = InstanceOfRequest(request);
        if (!instance || request >= requests_.size()) continue;
        auto preview = requests_[request]; preview.position = WorldPlacementDisplayPosition(preview, move.position);
        if (move.rotation) preview.angle = *move.rotation;
        const auto transform = Render::ModelInstanceTransform(preview);
        std::copy(transform.begin(), transform.end(), scene_->instances[*instance].transform);
    }
    dragGroup_ = std::move(*planned.value);
    if (scene_) (void)viewport_->UpdateModelInstances();
    UpdateOverlay(); UpdateSelectionText();
}

void WorldModule::CancelDrag() {
    const bool movedPlacement = dragPosition_.has_value() || dragRotation_.has_value() || !dragGroup_.empty();
    const auto affected = dragGroup_;
    dragStart_.reset(); dragPosition_.reset(); dragRotation_.reset(); dragGroup_.clear();
    rotationOrigin_.reset(); dragSelectionVersion_.reset(); dragMapKey_.clear();
    regionDraft_.reset(); cameraDraft_.reset();
    if (movedPlacement && scene_) {
        std::vector<MapData::PlacementId> restore;
        if (selection_) restore.push_back(*selection_);
        for (const auto& move : affected)
            if (std::find(restore.begin(), restore.end(), move.id) == restore.end()) restore.push_back(move.id);
        for (const auto& id : restore) {
            const auto found = std::find(placementIds_.begin(), placementIds_.end(), id);
            if (found == placementIds_.end()) continue;
            const auto request = static_cast<std::uint32_t>(found - placementIds_.begin());
            if (request >= requests_.size()) continue;
            const auto transform = Render::ModelInstanceTransform(requests_[request]);
            if (animation_ && animation_->Ready() && scene_ == animation_->Picking()) {
                // 临时变换可能已经进入动画状态；隐藏实例没有点选下标，仍须按稳定身份撤回。
                const auto data = animation_->Scene();
                if (request < data->specs.size()) {
                    Render::AnimationEdit restoreEdit;
                    restoreEdit.identity = data->specs[request].identity;
                    restoreEdit.transform = transform;
                    animation_->Edit(std::move(restoreEdit));
                }
            } else if (const auto instance = InstanceOfRequest(request)) {
                std::copy(transform.begin(), transform.end(), scene_->instances[*instance].transform);
            }
        }
        if (!animation_ || !animation_->Ready() || scene_ != animation_->Picking())
            (void)viewport_->UpdateModelInstances();
    }
    UpdateOverlay(); UpdateSelectionText();
}

void WorldModule::FinishDrag() {
    if (!SelectionEditable()) { CancelDrag(); return; }
    if (!project_.mapEdits_) { CancelDrag(); return; }
    if (!dragSelectionVersion_ || dragMapKey_ != MapKey() || dragGeneration_ != generation_
        || !worldSelection_.Snapshot(SelectionContext(), *dragSelectionVersion_).Succeeded()) { CancelDrag(); return; }
    Status status;
    QString text;
    std::string description;
    const auto beforeRevision = project_.mapEdits_->Revision();
    if (regionDraft_ && selectedRegion_) {
        const auto& b = *regionDraft_;
        status = MapData::SetRegionBounds(*project_.mapEdits_, *selectedRegion_, b[0], b[1], b[2], b[3]);
        text = (regionCorner_ < 0 && regionEdge_ < 0 ? tr("移动区域 %1") : tr("调整区域 %1")).arg(selectionName_);
        description = "修改区域";
    } else if (cameraDraft_ && selectedCamera_) {
        status = MapData::SetCameraTarget(*project_.mapEdits_, *selectedCamera_, (*cameraDraft_)[0], (*cameraDraft_)[1]);
        text = tr("移动镜头 %1").arg(selectionName_);
        description = "修改镜头";
    } else if (!dragGroup_.empty()) {
        status = MapData::MovePlacements(*project_.mapEdits_, dragGroup_);
        text = (rotationOrigin_ ? tr("旋转 %1 个摆放") : tr("移动 %1 个摆放")).arg(dragGroup_.size());
        description = rotationOrigin_ ? "旋转摆放组" : "移动摆放组";
    } else if ((dragPosition_ || dragRotation_) && selection_) {
        const auto current = MapData::FindPlacement(project_.mapEdits_->Snapshot(), *selection_);
        if (!current.Succeeded() || !current.value) {
            project_.AddDiagnostics(current.diagnostics);
            CancelDrag();
            return;
        }
        const MapData::PlacementMove move{*selection_, dragPosition_.value_or(current.value->position), dragRotation_};
        status = MapData::MovePlacements(*project_.mapEdits_, std::span(&move, 1));
        text = (dragRotation_ ? tr("旋转 %1") : tr("移动 %1")).arg(selectionName_);
        description = dragRotation_ ? "旋转摆放" : "移动摆放";
    } else { CancelDrag(); return; }
    if (!status.Succeeded()) {
        project_.AddDiagnostics(status.diagnostics);
        CancelDrag();
        selectionStatus_->setText(tr("修改未保存：%1").arg(WorldDiagnosticText(status.diagnostics)));
        return;
    }
    // 区域/镜头的名称与序号不变，选择保持；撤销栈变化会触发 RefreshPlacements 重新读取。
    dragStart_.reset(); dragRotation_.reset(); dragPosition_.reset(); dragGroup_.clear();
    rotationOrigin_.reset(); dragSelectionVersion_.reset(); dragMapKey_.clear();
    regionDraft_.reset(); cameraDraft_.reset();
    if (project_.mapEdits_->Revision() == beforeRevision) { UpdateOverlay(); UpdateSelectionText(); return; }
    (void)project_.ObserveMapDataEdit(description);
    QtProject::PushMapDataEdit(project_, text, description);
    if (!project_.mapDataUndoStack_) RefreshPlacements();
}

void WorldModule::DeleteSelection() {
    if (!SelectionEditable()) return;
    if (selectedRegion_ || selectedCamera_) {
        DeleteSelectedAuxiliary();
        return;
    }
    if (selectedPlacements_.empty() || !project_.mapEdits_) return;
    const auto status = MapData::DeletePlacements(*project_.mapEdits_, selectedPlacements_);
    if (!status.Succeeded()) {
        project_.AddDiagnostics(status.diagnostics);
        selectionStatus_->setText(tr("不能删除：%1").arg(WorldDiagnosticText(status.diagnostics)));
        return;
    }
    const auto name = selectedPlacements_.size() == 1 ? selectionName_
        : tr("%1 个摆放").arg(selectedPlacements_.size());
    ClearSelection();
    (void)project_.ObserveMapDataEdit("删除摆放");
    QtProject::PushMapDataEdit(project_, tr("删除 %1").arg(name), "删除摆放");
    if (!project_.mapDataUndoStack_) RefreshPlacements();
}

// ── 摆放 ──

void WorldModule::RefreshPalette() {
    if (!catalog_ || !palette_->isVisible()) return;
    QVector<QtApp::ObjectViewEntry> entries;
    for (auto& entry : QtApp::BuildObjectViewEntries(project_))
        if (PlaceableType(entry.kind)) entries.push_back(std::move(entry));
    catalog_->SetEntries(entries);
    catalog_->SetKindFilter(QString::fromUtf8(ObjectData::ToString(ObjectData::ObjectType::Unit)));
    catalog_->SetCatalogStatus(project_.standardCatalog_ ? QString{} : tr("原版目录未加载，只列出地图自定义物体"));
}

void WorldModule::ChoosePlacement(const QtApp::ObjectViewEntry& entry) {
    const auto type = PlaceableType(entry.kind);
    if (!type) return;
    pasting_ = false;
    placing_ = std::make_pair(*type, entry.id.toStdString());
    placingName_ = entry.name.isEmpty() ? entry.id : entry.name;
    ActivatePlacementTool();
    if (const auto& hover = viewport_->HoverCoordinate())
        previewHover_ = {static_cast<float>(hover->position[0]), static_cast<float>(hover->position[1]),
            static_cast<float>(hover->position[2])};
    previewRequests_.clear();
    ClearSelection();
    UpdateOverlay(); UpdateSelectionText();
    StartPlacementPreview();
}

void WorldModule::StopPlacing() {
    const bool hadPreview = placing_.has_value() || pasting_;
    if (animation_) animation_->CancelAssetPrefetch();
    pasting_ = false;
    placing_.reset(); placingName_.clear();
    previewHover_.reset(); previewRequests_.clear();
    if (viewport_) viewport_->SetPlacing(false);
    if (placeToggle_) {
        const QSignalBlocker blocker(placeToggle_);
        placeToggle_->setChecked(false);
    }
    UpdateToolState();
    UpdateOverlay();
    UpdateSelectionText();
    if (hadPreview && placementPreview_) placementPreview_->CancelRequest();
}

void WorldModule::PlaceAt(double x, double y, bool snap) {
    if (snap) { x = Snap(x); y = Snap(y); }
    if (pasting_) { PasteAt(x, y); return; }
    if (!placing_ || !project_.mapEdits_ || !terrain_) return;
    const auto report = [&](const std::vector<Diagnostic>& diagnostics) {
        project_.AddDiagnostics(diagnostics);
        selectionStatus_->setText(tr("不能摆放：%1").arg(WorldDiagnosticText(diagnostics)));
    };
    const auto ground = project_.mapEdits_->InspectTerrainPoint({x, y});
    if (!ground.Succeeded()) { report(ground.diagnostics); return; }
    auto context = QtProject::MakeObjectContextProvider(project_);
    const auto object = context(placing_->first);
    if (!object.Succeeded()) { report(object.diagnostics); return; }
    const auto owner = owner_ ? owner_->currentData().toInt() : 0;
    Services::PlacementCreateRequest creation;
    creation.type = placing_->first; creation.rawcode = placing_->second; creation.point = {x, y};
    if (creation.type == ObjectData::ObjectType::Unit) creation.owner = owner;
    auto prepared = Services::PreparePlacementCreation(creation, *object.value, *ground.value);
    if (!prepared.Succeeded()) { report(prepared.diagnostics); return; }
    auto placement = std::move(*prepared.value);
    // 只读取刚创建记录的同一物编父链，不为即时占位解析整个doo。
    const auto& transform = std::visit([](const auto& value) -> const MapData::PlacementTransform& { return value; }, placement);
    auto model = CapturePlacementPreview(project_, placing_->first, transform, owner);
    Render::ModelInstanceRequest request;
    if (model) { request = std::move(*model); request.previewGhost = false; }
    else { request.position = transform.position; request.angle = transform.rotation; request.scale = transform.scale; }
    placementInputStartedNs_ = WorldInstanceNowNs();
    const auto added = std::holds_alternative<MapData::UnitPlacement>(placement)
        ? MapData::AddUnitPlacement(*project_.mapEdits_, std::get<MapData::UnitPlacement>(std::move(placement)))
        : MapData::AddDoodadPlacement(*project_.mapEdits_, std::get<MapData::DoodadPlacement>(std::move(placement)));
    if (!added.Succeeded()) {
        placementInputStartedNs_ = 0;
        project_.AddDiagnostics(added.diagnostics);
        selectionStatus_->setText(tr("不能摆放：%1").arg(WorldDiagnosticText(added.diagnostics)));
        return;
    }
    RememberCreatedPlacements(std::span(&*added.value, 1), std::span(&request, 1));
    (void)project_.ObserveMapDataEdit("新增摆放");
    QtProject::PushMapDataEdit(project_, tr("摆放 %1").arg(placingName_), "新增摆放");
    if (!project_.mapDataUndoStack_) RefreshPlacements();
}

void WorldModule::CopySelection() {
    if (!SelectionEditable()) return;
    if (!project_.mapEdits_ || selectedPlacements_.empty()) return;
    const auto document = project_.mapEdits_->Snapshot();
    for (const auto& id : selectedPlacements_) {
        auto source = MapData::FindPlacement(document, id);
        if (!source.Succeeded() || !source.value || source.value->rawcode == "sloc") {
            selectionStatus_->setText(tr("复制失败：起始位置不可复制，或所选对象已过期"));
            return;
        }
    }
    clipboard_ = selectedPlacements_;
    const auto captured = CapturePlacementRows(project_, document, clipboard_, true);
    if (!captured.Succeeded() || captured.value->ids != clipboard_) {
        clipboard_.clear(); clipboardRequests_.clear(); clipboardPresentations_.clear();
        project_.AddDiagnostics(captured.diagnostics);
        selectionStatus_->setText(tr("复制失败：所选摆放的显示来源已失效"));
        return;
    }
    clipboardRequests_ = captured.value->requests;
    clipboardPresentations_ = captured.value->presentations;
    clipboardMapKey_ = MapKey();
    clipboardRevision_ = project_.mapEdits_->Revision();
    clipboardSession_ = project_.mapEdits_->SessionIdentity();
    clipboardPresentationCatalogRevision_ = selectionCatalogRevision_;
    clipboardAnchor_ = MapData::FindPlacement(document, clipboard_.front()).value->position;
    selectionStatus_->setText(tr("已复制 %1 个摆放 · Ctrl+V 粘贴").arg(clipboard_.size()));
}

void WorldModule::StartPaste() {
    if (!project_.mapEdits_ || clipboard_.empty() || clipboardMapKey_ != MapKey()
        || clipboardSession_ != project_.mapEdits_->SessionIdentity()) return;
    if (clipboardRevision_ != project_.mapEdits_->Revision()) {
        selectionStatus_->setText(tr("地图在复制后已修改，请重新复制所选对象"));
        return;
    }
    if (!clipboardPresentations_.empty() && clipboardPresentationCatalogRevision_ != selectionCatalogRevision_) {
        selectionStatus_->setText(tr("物编来源在复制后已改变，请重新复制所选对象"));
        return;
    }
    const auto document = project_.mapEdits_->Snapshot();
    for (const auto& id : clipboard_) {
        const auto source = MapData::FindPlacement(document, id);
        if (!source.Succeeded() || !source.value) {
            selectionStatus_->setText(tr("剪贴板对象已改变，请重新复制"));
            return;
        }
    }
    placing_.reset(); placingName_.clear(); pasting_ = true;
    ClearSelection();
    ActivatePlacementTool();
    if (const auto& hover = viewport_->HoverCoordinate())
        previewHover_ = {static_cast<float>(hover->position[0]), static_cast<float>(hover->position[1]),
            static_cast<float>(hover->position[2])};
    previewRequests_.clear();
    UpdateOverlay(); UpdateSelectionText();
    StartPlacementPreview();
}

void WorldModule::StartPlacementPreview() {
    if (!viewport_ || (!placing_ && !pasting_)) return;
    if (!placementPreview_) {
        placementPreview_ = new WorldPlacementPreview(*viewport_, this);
        connect(placementPreview_, &WorldPlacementPreview::Prepared, this, [this] {
            UpdatePlacementPreview(); UpdateOverlay();
        });
        connect(placementPreview_, &WorldPlacementPreview::Message, this, [this](const QString& text) {
            if (selectionStatus_) selectionStatus_->setText(text);
            UpdateOverlay();
        });
    }
    previewRequests_.clear();
    std::vector<WorldPlacementPresentationCandidate> presentations;
    if (placing_) {
        const auto owner = owner_ ? owner_->currentData().toInt() : 0;
        const auto placement = MakeNewPlacement(placing_->first, placing_->second, owner, {0, 0, 0});
        const auto& transform = std::visit([](const auto& value) -> const MapData::PlacementTransform& { return value; }, placement);
        WorldPlacementPresentationCandidate presentation;
        if (auto request = CapturePlacementPreview(project_, placing_->first, transform, owner, &presentation)) {
            previewRequests_.push_back(std::move(*request));
            if (presentation.fields) presentations.push_back(std::move(presentation));
        }
    } else {
        // Copy时查询的指定行请求由剪贴板拥有，不重新捕获整张地图。
        for (std::uint32_t i = 0; i < clipboardRequests_.size(); ++i) {
            const auto found = std::find_if(clipboardPresentations_.begin(), clipboardPresentations_.end(),
                [i](const auto& candidate) { return candidate.requestIndex == i; });
            if (clipboardRequests_[i].modelPath.empty() && found == clipboardPresentations_.end()) continue;
            if (found != clipboardPresentations_.end()) {
                auto candidate = *found; candidate.requestIndex = static_cast<std::uint32_t>(previewRequests_.size());
                candidate.owner.reset(); presentations.push_back(std::move(candidate)); // 类型预览，不冒称原行新身份。
            }
            previewRequests_.push_back(clipboardRequests_[i]);
        }
    }
    if (previewRequests_.empty()) {
        if (animation_) animation_->CancelAssetPrefetch();
        placementPreview_->Clear();
        selectionStatus_->setText(tr("所选物体没有可用的模型路径，仅显示落点标记"));
        UpdateOverlay(); return;
    }
    const auto sources = CaptureWorldTerrainSources(project_);
    if (!sources.Succeeded()) {
        if (animation_) animation_->CancelAssetPrefetch();
        selectionStatus_->setText(WorldDiagnosticText(sources.diagnostics)); placementPreview_->Clear();
        UpdateOverlay(); return;
    }
    const auto generation = generation_, revision = project_.mapEdits_ ? project_.mapEdits_->Revision() : 0;
    const auto catalogRevision = selectionCatalogRevision_, sessionGeneration = project_.editRevision_.Generation();
    const auto mapKey = MapKey();
    const auto presentationVersion = PlacementPresentationVersion();
    const auto helperIdentity = WorldPathingHelperResourceIdentity(*sources.value, presentationVersion.catalogRevision);
    if (!helperIdentity.Succeeded()) {
        if (animation_) animation_->CancelAssetPrefetch();
        project_.AddDiagnostics(helperIdentity.diagnostics); placementPreview_->Clear(); return;
    }
    if (animation_ && placing_ && previewRequests_.size() == 1 && !previewRequests_.front().modelPath.empty()) {
        const auto selectedType = placing_;
        const auto editSession = project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0;
        // 类型捕获的请求带预览标记。仅将这份未编译的物编请求切为正式材质策略，
        // 再由正式catalog独立准备；绝不将ghost几何、编译身份或运行状态用于正式实例。
        auto formalRequest = previewRequests_.front();
        formalRequest.previewGhost = false;
        animation_->PrefetchPlacementAsset(formalRequest, *sources.value,
            [this, generation, catalogRevision, sessionGeneration, mapKey, selectedType, editSession] {
                return page_ && placing_ == selectedType && generation_ == generation && MapKey() == mapKey
                    && selectionCatalogRevision_ == catalogRevision && project_.editRevision_.Generation() == sessionGeneration
                    && project_.mapEdits_ && project_.mapEdits_->SessionIdentity() == editSession;
            }, placementPreview_->PreloadedAssets(*sources.value));
    } else if (animation_) animation_->CancelAssetPrefetch();
    const auto oldGround = pasting_ && terrain_
        ? Render::SampleTerrainHeight(*terrain_, clipboardAnchor_[0], clipboardAnchor_[1]) : std::optional<float>{};
    auto transforms = PreviewTransforms(previewRequests_, previewHover_, pasting_, clipboardAnchor_, oldGround);
    placementPreview_->Show(previewRequests_, *sources.value, revision, [this, generation, catalogRevision, sessionGeneration, mapKey] {
        return page_ && (placing_ || pasting_) && generation_ == generation && MapKey() == mapKey
            && selectionCatalogRevision_ == catalogRevision && project_.editRevision_.Generation() == sessionGeneration;
    }, std::move(transforms), std::move(presentations), presentationVersion, *helperIdentity.value);
    UpdateOverlay();
}

void WorldModule::PreviewAt(double x, double y, double z, bool snap) {
    if (!placing_ && !pasting_) return;
    if (snap && terrain_) {
        x = Snap(x); y = Snap(y);
        const auto ground = Render::SampleTerrainHeight(*terrain_, x, y);
        if (!ground) { HidePreview(); return; }
        z = *ground;
    }
    previewHover_ = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    UpdatePlacementPreview(); UpdateOverlay();
}

void WorldModule::HidePreview() {
    previewHover_.reset();
    UpdatePlacementPreview(); UpdateOverlay();
}

void WorldModule::UpdatePlacementPreview() {
    if (!placementPreview_ || previewRequests_.empty()) return;
    const auto oldGround = pasting_ && terrain_
        ? Render::SampleTerrainHeight(*terrain_, clipboardAnchor_[0], clipboardAnchor_[1]) : std::optional<float>{};
    placementPreview_->SetTransforms(PreviewTransforms(previewRequests_, previewHover_, pasting_, clipboardAnchor_, oldGround));
}

void WorldModule::PasteAt(double x, double y) {
    if (!pasting_ || !project_.mapEdits_ || !terrain_) return;
    if (clipboardMapKey_ != MapKey() || clipboardRevision_ != project_.mapEdits_->Revision()
        || clipboardSession_ != project_.mapEdits_->SessionIdentity()
        || (!clipboardPresentations_.empty() && clipboardPresentationCatalogRevision_ != selectionCatalogRevision_)) {
        selectionStatus_->setText(tr("地图在复制后已修改，请重新复制所选对象"));
        StopPlacing();
        return;
    }
    const auto ground = Render::SampleTerrainHeight(*terrain_, x, y);
    const auto oldGround = Render::SampleTerrainHeight(*terrain_, clipboardAnchor_[0], clipboardAnchor_[1]);
    if (!ground || !oldGround) {
        selectionStatus_->setText(tr("粘贴组的锚点不在当前地形内"));
        return;
    }
    const std::array<float, 3> offset{static_cast<float>(x - clipboardAnchor_[0]),
        static_cast<float>(y - clipboardAnchor_[1]), *ground - *oldGround};
    placementInputStartedNs_ = WorldInstanceNowNs();
    const auto copied = MapData::CopyPlacements(*project_.mapEdits_, clipboard_, offset);
    if (!copied.Succeeded() || !copied.value) {
        placementInputStartedNs_ = 0;
        project_.AddDiagnostics(copied.diagnostics);
        selectionStatus_->setText(tr("粘贴失败：%1").arg(WorldDiagnosticText(copied.diagnostics)));
        return;
    }
    if (clipboardRequests_.size() == copied.value->size()) {
        auto requests = clipboardRequests_;
        for (auto& request : requests) for (std::size_t axis = 0; axis < 3; ++axis) request.position[axis] += offset[axis];
        RememberCreatedPlacements(*copied.value, requests);
    }
    clipboardRevision_ = project_.mapEdits_->Revision();
    (void)project_.ObserveMapDataEdit("复制摆放组");
    QtProject::PushMapDataEdit(project_, tr("粘贴 %1 个摆放").arg(copied.value->size()), "复制摆放组");
    if (!project_.mapDataUndoStack_) RefreshPlacements();
}

// ── 地形笔刷 ──

void WorldModule::StartTerrainStroke(double x, double y) {
    if (!project_.mapEdits_ || !terrainToggle_->isChecked() || !terrain_ || !viewport_) return;
    auto source = project_.mapEdits_->Terrain();
    if (!source || source != rawTerrain_) {
        selectionStatus_->setText(tr("地形来源已变化，请等待视图更新后再刷"));
        terrainToggle_->setChecked(false);
        return;
    }
    if (CurrentBrushKind() == MapData::TerrainBrushKind::CliffLevel && !WaterBrushCaptureCurrent()) {
        QueueWaterBrushCapture();
        selectionStatus_->setText(tr("请等待悬崖规则就绪后重新落笔")); return;
    }
    if (const auto kind = CurrentWaterBrushKind()) {
        if (!WaterBrushCaptureCurrent()) {
            QueueWaterBrushCapture();
            selectionStatus_->setText(tr("请等待水面规则就绪后重新落笔"));
            return;
        }
        if (terrainCliffTexture_->currentIndex() < 0) {
            selectionStatus_->setText(tr("当前地图没有可用的悬崖贴图")); return;
        }
        const auto anchor = MapData::CaptureTerrainWaterStrokeAnchor(*source, {x, y});
        if (!anchor.Succeeded()) {
            selectionStatus_->setText(tr("不能建立水刷锚点：%1").arg(WorldDiagnosticText(anchor.diagnostics))); return;
        }
        strokeWaterBrush_ = MapData::TerrainWaterBrush{*kind,
            static_cast<MapData::TerrainBrushShape>(std::max(0, terrainShapes_->checkedId())),
            static_cast<std::uint32_t>(terrainRadius_->value()),
            static_cast<std::uint8_t>(terrainCliffTexture_->currentIndex()),
            static_cast<std::uint32_t>(project_.mapEdits_->Revision()), *anchor.value};
        strokeWaterCapture_ = waterCapture_;
        // 共用采样器只借普通笔刷的格点尺寸；执行与预览都由waterBrush显式分派。
        strokeBrush_ = {};
        strokeBrush_.kind = MapData::TerrainBrushKind::Plateau;
        strokeBrush_.size = strokeWaterBrush_->size;
        strokeBrush_.shape = strokeWaterBrush_->shape;
        strokeBase_ = std::move(source);
        strokeRevision_ = project_.mapEdits_->Revision();
        strokePoints_.clear();
        ExtendTerrainStroke(x, y); UpdateSelectionText(); return;
    }
    strokeWaterBrush_.reset(); strokeWaterCapture_.reset();
    MapData::TerrainBrush brush;
    brush.kind = CurrentBrushKind();
    brush.shape = static_cast<MapData::TerrainBrushShape>(std::max(0, terrainShapes_->checkedId()));
    brush.size = static_cast<std::uint32_t>(terrainRadius_->value());
    brush.strength = static_cast<float>(terrainStrength_->value());
    brush.value = (brush.kind == MapData::TerrainBrushKind::Ramp ? terrainRampModes_
        : brush.kind == MapData::TerrainBrushKind::Boundary ? terrainBoundaryModes_
        : brush.kind == MapData::TerrainBrushKind::PathingRestriction ? terrainPathingModes_ : terrainBlightModes_)->checkedId() != 0;
    brush.pathingRestrictions = static_cast<std::uint8_t>(terrainPathingTypes_->checkedId());
    brush.levelDelta = terrainCliffDeltas_->checkedId();
    brush.cliffTexture = static_cast<std::uint8_t>(terrainCliffTexture_->currentIndex());
    brush.waterHeight = static_cast<float>(terrainWaterHeight_->value());
    const int variation = terrainVariation_->currentData().toInt();
    if (variation >= 0) brush.variation = static_cast<std::uint8_t>(variation);
    brush.randomizeVariation = variation < 0;
    if (const auto swatch = terrainSwatches_->checkedId(); swatch >= 0 && static_cast<std::size_t>(swatch) < swatchIds_.size())
        brush.groundTexture = swatchIds_[static_cast<std::size_t>(swatch)];
    brush.seed = static_cast<std::uint32_t>(project_.mapEdits_->Revision());
    if ((brush.kind == MapData::TerrainBrushKind::Texture && brush.groundTexture.size() != 4) ||
        ((brush.kind == MapData::TerrainBrushKind::CliffLevel || brush.kind == MapData::TerrainBrushKind::CliffTexture)
            && terrainCliffTexture_->currentIndex() < 0)) {
        selectionStatus_->setText(tr("当前地图没有可用的笔刷贴图"));
        terrainToggle_->setChecked(false);
        return;
    }
    const MapData::TerrainStrokePoint first{x, y};
    if (brush.kind == MapData::TerrainBrushKind::PathingRestriction
        && (!project_.mapEdits_->HasTerrainPathing() || !MapData::GetPathingRestrictionFootprint(source->header, brush, first))) {
        selectionStatus_->setText(tr("通行图缺失或落点超出通行格范围，未修改地图"));
        return;
    }
    if (brush.kind == MapData::TerrainBrushKind::Ramp) {
        const auto plan = MapData::PlanTerrainRamp(*source, first);
        if (!plan.Succeeded()) {
            selectionStatus_->setText(tr("不能创建或移除坡道：%1").arg(WorldDiagnosticText(plan.diagnostics)));
            return;
        }
    }
    const auto anchor = MapData::CaptureTerrainStrokeAnchor(*source, first);
    if (!anchor.Succeeded()) {
        selectionStatus_->setText(tr("不能从按下位置建立笔刷锚点：%1").arg(WorldDiagnosticText(anchor.diagnostics)));
        terrainToggle_->setChecked(false);
        return;
    }
    brush.anchor = *anchor.value;
    if (brush.kind == MapData::TerrainBrushKind::CliffLevel) strokeWaterCapture_ = waterCapture_;
    strokeBase_ = std::move(source);
    strokeRevision_ = project_.mapEdits_->Revision();
    strokeBrush_ = std::move(brush);
    strokePoints_.clear();
    ExtendTerrainStroke(x, y);
    UpdateSelectionText();
}

void WorldModule::ExtendTerrainStroke(double x, double y) {
    if (!strokeBase_ || !std::isfinite(x) || !std::isfinite(y)) return;
    if (strokeBrush_.kind == MapData::TerrainBrushKind::Ramp) {
        // 坡道按所在悬崖格取法向；吸附到最近顶点会把高侧半格错误移到平地。
        // 保留原落点，拖动按半格补采样；Core 仍对整笔预检，坏点不能留下半笔修改。
        if (strokePoints_.empty()) strokePoints_.push_back({x, y});
        else {
            const auto last = strokePoints_.back();
            const double distance = std::hypot(x - last.x, y - last.y);
            if (distance < 0.01) return;
            const double steps = std::ceil(distance / (MapData::TerrainGridSnapshot::tileSize * 0.5));
            const auto available = 65536 - strokePoints_.size();
            if (!std::isfinite(steps) || steps > static_cast<double>(available)) {
                CancelTerrainStroke();
                selectionStatus_->setText(tr("坡道笔划超过采样预算，本笔已取消"));
                return;
            }
            const auto count = static_cast<std::size_t>(steps);
            for (std::size_t step = 1; step <= count; ++step) {
                const double t = static_cast<double>(step) / static_cast<double>(count);
                strokePoints_.push_back({last.x + (x - last.x) * t, last.y + (y - last.y) * t});
            }
        }
        QueueTerrainRender(strokeBase_, true);
        return;
    }
    const auto& header = strokeBase_->header;
    const bool pathing = strokeBrush_.kind == MapData::TerrainBrushKind::PathingRestriction;
    if (pathing && !MapData::GetPathingRestrictionFootprint(header, strokeBrush_, {x, y})) {
        CancelTerrainStroke();
        selectionStatus_->setText(tr("落点超出通行格范围，本笔已取消"));
        return;
    }
    const double stepSize = pathing ? MapData::kPathingCellSize : MapData::TerrainGridSnapshot::tileSize;
    const auto grid = [&](double world, float offset, std::uint32_t extent) {
        const double coordinate = (world - offset) / stepSize;
        const double snapped = pathing ? std::floor(coordinate) : std::round(coordinate);
        return static_cast<int>(std::clamp(snapped, 0.0, static_cast<double>(extent - 1)));
    };
    const auto width = pathing ? (header.width - 1) * 4 : header.width;
    const auto height = pathing ? (header.height - 1) * 4 : header.height;
    const int tx = grid(x, header.offset[0], width), ty = grid(y, header.offset[1], height);
    const auto point = [&](int gx, int gy) {
        const double center = pathing ? 0.5 : 0;
        return MapData::TerrainStrokePoint{header.offset[0] + (gx + center) * stepSize,
            header.offset[1] + (gy + center) * stepSize};
    };
    bool added = false;
    if (strokePoints_.empty()) { strokePoints_.push_back(point(tx, ty)); added = true; }
    else {
        const auto& last = strokePoints_.back();
        int gx = grid(last.x, header.offset[0], width);
        int gy = grid(last.y, header.offset[1], height);
        const int dx = std::abs(tx - gx), dy = std::abs(ty - gy);
        int err = dx - dy;
        const int sx = gx < tx ? 1 : -1, sy = gy < ty ? 1 : -1;
        while ((gx != tx || gy != ty) && strokePoints_.size() < 65536) {
            const int twice = 2 * err;
            if (twice > -dy) { err -= dy; gx += sx; }
            if (twice < dx) { err += dx; gy += sy; }
            strokePoints_.push_back(point(gx, gy));
            added = true;
        }
        if (strokePoints_.size() == 65536) {
            if ((pathing || strokeWaterCapture_) && (gx != tx || gy != ty)) {
                CancelTerrainStroke(); selectionStatus_->setText(tr("笔划超过采样预算，本笔已取消")); return;
            }
            selectionStatus_->setText(tr("笔划已达到 65536 个采样点，请松开后继续"));
        }
    }
    if (added) {
        if (pathing) UpdateOverlay();
        else QueueTerrainRender(strokeBase_, true);
    }
}

void WorldModule::FinishTerrainStroke() {
    if (!strokeBase_ || !project_.mapEdits_ || project_.mapEdits_->Terrain() != strokeBase_ || strokePoints_.empty()) {
        CancelTerrainStroke(); return;
    }
    const auto before = project_.mapEdits_->Revision();
    const bool pathing = strokeBrush_.kind == MapData::TerrainBrushKind::PathingRestriction;
    const bool water = strokeWaterBrush_.has_value();
    const bool structural = water || strokeBrush_.kind == MapData::TerrainBrushKind::CliffLevel;
    if ((pathing || structural) && before != strokeRevision_) {
        CancelTerrainStroke(); selectionStatus_->setText(tr("地形编辑修订已变化，请重新落笔")); return;
    }
    if (structural && (!strokeWaterCapture_ || !QtProject::TerrainWaterEditRequestCurrent(project_,
            *strokeWaterCapture_->request, terrainEnforceWaterLimits_->isChecked()))) {
        CancelTerrainStroke(); InvalidateWaterBrushCapture(); QueueWaterBrushCapture();
        selectionStatus_->setText(tr("地形规则来源已变化，请等待更新后重新落笔")); return;
    }
    const auto terrainBefore = project_.mapEdits_->Terrain();
    if (structural) {
        auto commit = std::make_shared<WaterBrushCommit>(WaterBrushCommit{strokeWaterCapture_,
            water ? decltype(WaterBrushCommit::brush){*strokeWaterBrush_} : decltype(WaterBrushCommit::brush){strokeBrush_},
            std::move(strokePoints_), terrainToggle_->isChecked()});
        strokeBase_.reset(); strokePoints_.clear(); strokeWaterBrush_.reset(); strokeWaterCapture_.reset();
        ++terrainRenderSerial_; pendingTerrainRender_.reset();
        // toggled(false)同步调用Cancel；先关控件，再发布pending事务，避免自行取消。
        terrainToggle_->setChecked(false); terrainToggle_->setEnabled(false);
        waterCommit_ = std::move(commit); waterCommitPending_ = true;
        terrainWaterBrushStatus_->setText(tr("正在核对本笔地形规则…"));
        StartTerrainRender(); return;
    }
    auto edited = project_.mapEdits_->EditTerrain(strokeBrush_, strokePoints_, true);
    strokeBase_.reset(); strokePoints_.clear(); strokeWaterBrush_.reset(); strokeWaterCapture_.reset();
    PublishTerrainStrokeResult(std::move(edited), terrainBefore, before, pathing, false, terrainToggle_->isChecked());
}

void WorldModule::StartWaterBrushCommit() {
    if (worker_ || !waterCommitPending_ || !waterCommit_ || !page_) return;
    waterCommitPending_ = false;
    const auto request = waterCommit_;
    const auto generation = generation_;
    auto status = std::make_shared<Status>();
    auto elapsed = std::make_shared<qint64>(0);
    auto* worker = QThread::create([request, status, elapsed] {
        QElapsedTimer timer; timer.start();
        *status = QtProject::VerifyTerrainWaterEditResources(*request->capture);
        *elapsed = timer.elapsed();
    });
    worker_ = worker;
    connect(worker, &QThread::finished, this, [this, request, generation, status, elapsed] {
        worker_ = nullptr;
        if (reloadQueued_) { Refresh(); return; }
        if (!page_ || generation != generation_) return;
        if (waterCommit_ != request) { StartTerrainRender(); return; }
        waterCommit_.reset();
        terrainWaterBrushStatus_->setProperty("verifyMilliseconds", *elapsed);
        const bool current = QtProject::TerrainWaterEditRequestCurrent(project_, *request->capture->request,
            terrainEnforceWaterLimits_->isChecked());
        Result<MapData::TerrainStrokeResult> edited;
        if (!current) edited.diagnostics = Status::Failure("Terrain.Water", "TERRAIN_WATER_COMMIT_STALE",
            "水刷等待核验时地图或资源修订已变化，本笔未提交。").diagnostics;
        else if (!status->Succeeded()) edited.diagnostics = status->diagnostics;
        else {
            // live Session只在GUI线程使用。最终Prepare一次，来源I/O已在worker完成。
            QElapsedTimer timer; timer.start();
            auto plan = std::visit([&](const auto& brush) {
                if constexpr (std::is_same_v<std::decay_t<decltype(brush)>, MapData::TerrainWaterBrush>)
                    return project_.mapEdits_->PrepareTerrainWaterEdit(request->capture->resources, brush, request->points, true);
                else return project_.mapEdits_->PrepareTerrainCliffEdit(request->capture->resources, brush, request->points, true);
            }, request->brush);
            terrainWaterBrushStatus_->setProperty("prepareMilliseconds", timer.elapsed());
            if (plan.Succeeded()) edited = project_.mapEdits_->ApplyPreparedTerrainEdit(*plan.value);
            else edited.diagnostics = std::move(plan.diagnostics);
        }
        if (!status->Succeeded() || !current) InvalidateWaterBrushCapture();
        PublishTerrainStrokeResult(std::move(edited), request->capture->request->terrain,
            request->capture->request->revision, false, true, current && status->Succeeded() && request->resumeBrush);
        StartTerrainRender();
        UpdateWaterFrameControl();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}

void WorldModule::PublishTerrainStrokeResult(Result<MapData::TerrainStrokeResult> edited,
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrainBefore, std::uint64_t before,
    bool pathing, bool water, bool resumeBrush) {
    if (!pathing) {
        resumeBrushAfterRender_ = resumeBrush;
        if (terrainToggle_ && terrainToggle_->isChecked()) terrainToggle_->setChecked(false);
        if (terrainToggle_) terrainToggle_->setEnabled(false);
    }
    UpdateSelectionText();
    if (!edited.Succeeded()) {
        project_.AddDiagnostics(edited.diagnostics);
        selectionStatus_->setText(tr("地形未修改：%1").arg(WorldDiagnosticText(edited.diagnostics)));
        if (!pathing && project_.mapEdits_) QueueTerrainRender(project_.mapEdits_->Terrain(), false);
        return;
    }
    const auto next = project_.mapEdits_->Terrain();
    if (pathing) UpdateOverlay();
    else QueueTerrainRender(next, false);
    if (project_.mapEdits_->Revision() == before) return; // 未改变字节，不生成空撤销项。
    // 水刷Prepare刻意不检查保存资格；实际保存统一重建native通行，GUI不做必失败的全图等价扫描。
    terrainPathingPending_ = water || !project_.mapEdits_->VerifyTerrainPathingForSave().Succeeded();
    UpdateStatusIndicators();
    (void)project_.ObserveMapDataEdit("修改地形");
    pushingTerrainStrokeUndo_ = true;
    QtProject::PushMapDataEdit(project_, tr("地形笔刷"), "修改地形");
    pushingTerrainStrokeUndo_ = false;
    if (!project_.mapDataUndoStack_) RefreshPlacements();
    if (water) {
        // 水刷只允许同事务的Z跟随；不能调用下方会搬移XY或删除对象的补救对话框。
        selectionStatus_->setText(tr("水面已修改；水边对象的站立位置请另行检查，可撤销本笔"));
        return;
    }
    // 落笔事件结束后再询问，避免在鼠标释放处理中开模态窗口。
    const auto revision = project_.mapEdits_->Revision();
    QTimer::singleShot(0, this, [this, terrainBefore, revision] { ResolveStrandedUnits(terrainBefore, revision); });
}

void WorldModule::ResolveStrandedUnits(std::shared_ptr<const MapData::TerrainGridSnapshot> before, std::uint64_t revision) {
    if (!page_ || !before || !project_.mapEdits_ || project_.mapEdits_->Revision() != revision) return;
    const auto after = project_.mapEdits_->Terrain();
    const auto document = CurrentDocument();
    if (!after || after == before || !document || !document->Units()) return;
    struct Stranded { MapData::PlacementId id; std::array<float, 3> position; QString name; };
    std::vector<Stranded> stranded;
    const auto& units = document->Units()->units;
    for (std::uint32_t index = 0; index < units.size(); ++index) {
        const auto& unit = units[index];
        const double x = unit.position[0], y = unit.position[1];
        // 只处理这一笔造成的：原来能站、现在不能站。起始位置不是单位，不在此处理。
        if (unit.rawcode == "sloc" || !MapData::TerrainGroundStandable(*before, x, y)
            || MapData::TerrainGroundStandable(*after, x, y)) continue;
        if (PlacementType(project_, unit.rawcode, ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item)
            != ObjectData::ObjectType::Unit) continue;
        const auto movement = WorldUnitMovementType(project_, unit.rawcode);
        if (!movement || *movement == "fly" || *movement == "float") continue; // 飞行与海上单位不受悬崖面影响
        stranded.push_back({MapData::MakePlacementId(MapData::PlacementFile::Units, index, unit), unit.position,
            PlacementPresentation(project_, *document, unit.rawcode, ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item).name});
    }
    if (stranded.empty()) return;
    QStringList lines;
    for (std::size_t i = 0; i < stranded.size() && i < 8; ++i)
        lines << tr("%1（%2, %3）").arg(stranded[i].name.isEmpty() ? QString::fromStdString(stranded[i].id.rawcode) : stranded[i].name)
            .arg(stranded[i].position[0], 0, 'f', 0).arg(stranded[i].position[1], 0, 'f', 0);
    if (stranded.size() > 8) lines << tr("……另有 %1 个").arg(stranded.size() - 8);
    QMessageBox box(QMessageBox::Question, tr("单位无法落脚"),
        tr("这一笔让 %1 个地面单位落在悬崖面上，无法站立。").arg(stranded.size()), QMessageBox::NoButton, page_);
    box.setInformativeText(lines.join(QLatin1Char('\n')));
    auto* move = box.addButton(tr("移到最近可站立点"), QMessageBox::AcceptRole);
    auto* remove = box.addButton(tr("移除这些单位"), QMessageBox::DestructiveRole);
    auto* undo = box.addButton(tr("撤销这一笔"), QMessageBox::RejectRole);
    box.setDefaultButton(move); box.setEscapeButton(undo);
    box.exec();
    if (!page_ || !project_.mapEdits_ || project_.mapEdits_->Revision() != revision) return; // 期间地图已变化，不再按旧结果处理
    if (box.clickedButton() == undo) {
        if (project_.mapDataUndoStack_ && project_.mapDataUndoStack_->canUndo()) project_.mapDataUndoStack_->undo();
        return;
    }
    Status status;
    QString text; std::string description;
    if (box.clickedButton() == remove) {
        std::vector<MapData::PlacementId> ids;
        for (const auto& unit : stranded) ids.push_back(unit.id);
        ClearSelection();
        status = MapData::DeletePlacements(*project_.mapEdits_, ids);
        text = tr("移除 %1 个无法落脚的单位").arg(ids.size()); description = "删除摆放";
    } else if (box.clickedButton() == move) {
        std::vector<MapData::PlacementMove> moves;
        std::size_t unplaced = 0;
        for (const auto& unit : stranded) {
            const auto target = MapData::NearestTerrainGroundStandable(*after, unit.position[0], unit.position[1]);
            const auto oldGround = MapData::SampleGroundHeight(*after, unit.position[0], unit.position[1], MapData::TerrainHeightSource::RawWithRamps);
            const auto newGround = target ? MapData::SampleGroundHeight(*after, target->x, target->y, MapData::TerrainHeightSource::RawWithRamps) : std::nullopt;
            if (!target || !oldGround || !newGround) { ++unplaced; continue; }
            // 保持与地面的相对高度。
            moves.push_back({unit.id, {static_cast<float>(target->x), static_cast<float>(target->y),
                unit.position[2] - *oldGround + *newGround}, std::nullopt});
        }
        if (moves.empty()) { selectionStatus_->setText(tr("附近没有可站立的位置，单位保持原位")); return; }
        status = MapData::MovePlacements(*project_.mapEdits_, moves);
        text = tr("移动 %1 个无法落脚的单位").arg(moves.size()); description = "移动摆放组";
        if (unplaced) text += tr("（%1 个附近没有可站立位置，保持原位）").arg(unplaced);
    } else return;
    if (!status.Succeeded()) {
        project_.AddDiagnostics(status.diagnostics);
        selectionStatus_->setText(tr("单位未处理：%1").arg(WorldDiagnosticText(status.diagnostics)));
        return;
    }
    if (project_.mapEdits_->Revision() == revision) return;
    (void)project_.ObserveMapDataEdit(description);
    QtProject::PushMapDataEdit(project_, text, description);
    selectionStatus_->setText(text);
}

void WorldModule::CancelTerrainStroke() {
    const bool pendingWater = waterCommit_ != nullptr;
    waterCommit_.reset(); waterCommitPending_ = false;
    if (!strokeBase_ && !pendingWater) return;
    strokeBase_.reset(); strokePoints_.clear(); strokeWaterBrush_.reset(); strokeWaterCapture_.reset();
    if (strokeBrush_.kind == MapData::TerrainBrushKind::PathingRestriction) {
        UpdateOverlay(); UpdateSelectionText(); return;
    }
    resumeBrushAfterRender_ = terrainToggle_ && terrainToggle_->isChecked();
    if (resumeBrushAfterRender_) terrainToggle_->setChecked(false);
    if (terrainToggle_) terrainToggle_->setEnabled(false);
    if (project_.mapEdits_) QueueTerrainRender(project_.mapEdits_->Terrain(), false);
    UpdateSelectionText();
}

void WorldModule::EditTerrainTextureSlot(bool cliff, bool append) {
    if (!project_.mapEdits_ || strokeBase_) return;
    const auto source = project_.mapEdits_->Terrain();
    if (!source || source != rawTerrain_) return;
    const auto& table = cliff ? source->header.cliffTilesets : source->header.groundTilesets;
    const int slot = append ? static_cast<int>(table.size())
        : (cliff ? terrainCliffTexture_->currentIndex() : terrainSwatches_->checkedId());
    if (slot < 0 || (!append && static_cast<std::size_t>(slot) >= table.size())) return;
    const auto before = project_.mapEdits_->Revision();
    const QString previous = append ? QString{} : QString::fromLatin1(table[static_cast<std::size_t>(slot)].data(),
        static_cast<qsizetype>(table[static_cast<std::size_t>(slot)].size()));
    bool accepted = false;
    const QString value = QInputDialog::getText(page_, append ? tr("添加地形贴图") : tr("替换地形贴图"),
        tr("输入四字符贴图 ID。替换会影响此槽位的所有地形，可撤销。\n贴图通行性联动待核验，修改后可保存工程，暂不能保存地图。"),
        QLineEdit::Normal, previous, &accepted).trimmed();
    if (!accepted) return;
    if (!project_.mapEdits_ || project_.mapEdits_->Terrain() != source || project_.mapEdits_->Revision() != before) {
        selectionStatus_->setText(tr("地形来源已变化，请重新选择贴图槽"));
        return;
    }
    const auto edited = project_.mapEdits_->EditTerrainTextureSlot(
        cliff ? MapData::TerrainTextureTable::Cliff : MapData::TerrainTextureTable::Ground,
        static_cast<std::uint32_t>(slot), value.toStdString());
    if (!edited.Succeeded()) {
        project_.AddDiagnostics(edited.diagnostics);
        selectionStatus_->setText(tr("贴图未修改：%1").arg(WorldDiagnosticText(edited.diagnostics)));
        return;
    }
    if (project_.mapEdits_->Revision() == before) return;
    terrainPathingPending_ = !project_.mapEdits_->VerifyTerrainPathingForSave().Succeeded();
    (void)project_.ObserveMapDataEdit("替换地形贴图");
    pushingTerrainStrokeUndo_ = true;
    QtProject::PushMapDataEdit(project_, tr("地形贴图槽"), "替换地形贴图");
    pushingTerrainStrokeUndo_ = false;
    QueueTerrainRender(project_.mapEdits_->Terrain(), false);
    RefreshTerrainPalette();
    UpdateStatusIndicators();
}

// ── 显示 ──

void WorldModule::UpdateOverlay() {
    if (!viewport_) return;
    Render::OverlayLines lines;
    AppendStartLocationOverlay(lines);
    const auto* terrain = terrain_.get();
    const auto* region = SelectedRegion();
    const auto* camera = SelectedCamera();
    if (region && !worldSelection_.Flags(*selectedRegion_).visible) region = nullptr;
    if (camera && !worldSelection_.Flags(*selectedCamera_).visible) camera = nullptr;
    if (showRegions_ && showRegions_->isChecked()) {
        for (std::size_t index = 0; index < regions_.size(); ++index) {
            const auto& entry = regions_[index];
            if (&entry == region || !worldSelection_.Flags(MapData::MakeRegionId(static_cast<std::uint32_t>(index), entry)).visible) continue;
            Render::AppendGroundRectangle(lines, terrain, entry.left, entry.bottom, entry.right, entry.top,
                Render::RegionOverlayColor(entry));
        }
    }
    if (region) {
        const auto b = regionDraft_ ? *regionDraft_ : std::array<float, 4>{region->left, region->bottom, region->right, region->top};
        Render::AppendGroundRectangle(lines, terrain, b[0], b[1], b[2], b[3], kSelected, 6.0f);
        for (const auto& corner : Corners(b[0], b[1], b[2], b[3])) // 角手柄
            Render::AppendGroundRectangle(lines, terrain, static_cast<float>(corner[0] - 12), static_cast<float>(corner[1] - 12),
                static_cast<float>(corner[0] + 12), static_cast<float>(corner[1] + 12), kSelected, 8.0f);
    }
    if (showCameras_ && showCameras_->isChecked()) {
        for (std::size_t index = 0; index < cameras_.size(); ++index) {
            const auto& entry = cameras_[index];
            if (&entry == camera || !worldSelection_.Flags(MapData::MakeCameraId(static_cast<std::uint32_t>(index), entry)).visible) continue;
            Render::AppendCameraMarker(lines, terrain, entry, kCameraColor);
        }
    }
    if (camera) {
        auto shown = *camera;
        if (cameraDraft_) { shown.targetX = (*cameraDraft_)[0]; shown.targetY = (*cameraDraft_)[1]; }
        Render::AppendCameraMarker(lines, terrain, shown, kSelected);
    }
    if (scene_) for (const auto& id : selectedPlacements_) {
        const auto found = std::find(placementIds_.begin(), placementIds_.end(), id);
        if (found == placementIds_.end()) continue;
        const auto request = static_cast<std::uint32_t>(found - placementIds_.begin());
        // 单位使用真实MiscData选取圈；缺少尺寸也不回退成覆盖画面的巨大模型AABB。
        if (request < requests_.size() && requests_[request].selection) continue;
        const auto instance = InstanceOfRequest(request);
        if (instance) {
            if (const auto box = Render::ModelInstanceBox(*scene_, *instance)) Render::AppendBoxLines(lines, *box, kSelected);
        }
    }
    if ((placing_ || pasting_) && previewHover_ && placementPreview_ && !placementPreview_->ModelSubmitted()
        && viewport_->HasOverlaySupport()) {
        // 资产准备期间只标明有效落点，不把此十字当成模型尺寸或通行性占地。
        const auto& point = *previewHover_;
        const float z = point[2] + 8.0f;
        lines.push_back({{point[0] - 16.0f, point[1], z}, {point[0] + 16.0f, point[1], z},
            {kCameraColor[0], kCameraColor[1], kCameraColor[2]}});
        lines.push_back({{point[0], point[1] - 16.0f, z}, {point[0], point[1] + 16.0f, z},
            {kCameraColor[0], kCameraColor[1], kCameraColor[2]}});
    }
    if (CurrentTool() == Tool::Terrain && CurrentBrushKind() == MapData::TerrainBrushKind::Boundary)
        AppendTerrainBoundaryOverlay(lines);
    if (CurrentTool() == Tool::Terrain && CurrentBrushKind() == MapData::TerrainBrushKind::PathingRestriction)
        AppendPathingRestrictionOverlay(lines);
    // 笔刷落点：吸附到最近顶点，按实际作用范围画出（半径外半格为边界）。
    if (brushCursor_ && terrain && CurrentTool() == Tool::Terrain) {
        MapData::TerrainBrush brush;
        brush.kind = CurrentBrushKind();
        brush.size = static_cast<std::uint32_t>(terrainRadius_->value());
        const bool pathing = brush.kind == MapData::TerrainBrushKind::PathingRestriction;
        const double size = pathing ? MapData::kPathingCellSize : MapData::TerrainGridSnapshot::tileSize;
        const MapData::TerrainStrokePoint point{(*brushCursor_)[0], (*brushCursor_)[1]};
        if (const auto footprint = pathing ? MapData::GetPathingRestrictionFootprint(terrain->header, brush, point)
                                          : MapData::GetTerrainBrushFootprint(*terrain, brush, point)) {
            const double cx = footprint->centerX, cy = footprint->centerY, extent = footprint->radius;
            if (brush.kind == MapData::TerrainBrushKind::Ramp) {
                const auto& origin = terrain->header.offset;
                Render::AppendGroundRectangle(lines, terrain,
                    static_cast<float>(origin[0] + (footprint->x0 - 0.5) * size),
                    static_cast<float>(origin[1] + (footprint->y0 - 0.5) * size),
                    static_cast<float>(origin[0] + (footprint->x1 - 0.5) * size),
                    static_cast<float>(origin[1] + (footprint->y1 - 0.5) * size), kSelected, 6.0f);
            } else if (terrainShapes_->checkedId() == static_cast<int>(MapData::TerrainBrushShape::Square))
                Render::AppendGroundRectangle(lines, terrain, static_cast<float>(cx - extent), static_cast<float>(cy - extent),
                    static_cast<float>(cx + extent), static_cast<float>(cy + extent), kSelected, 6.0f);
            else Render::AppendGroundCircle(lines, terrain, cx, cy, extent, kSelected, 6.0f);
            Render::AppendGroundCircle(lines, terrain, cx, cy, size * 0.12, kSelected, 6.0f);
        }
    }
    if (const auto binding = CurrentPlacementBinding())
        pendingPlacements_.AppendLines(lines, *binding, placementCompletedScene_.get(), placementCompletedFrame_.get(),
            [this](const auto& row) { return worldSelection_.Flags(row).visible; });
    startLocationOverlaySubmitted_ = viewport_->SetOverlayLines(std::move(lines)) && StartLocationsCurrent();
    UpdateSelectionCircles();
}

// 选择或工具状态变化：状态栏提示、场景目录选中项与属性栏一起更新。
void WorldModule::UpdateSelectionText() {
    UpdateToolHint();
    SyncOutlinerSelection();
    UpdateInspector();
    if (startLocationMarkers_.omittedCount && selectionStatus_)
        selectionStatus_->setText(tr("起始位置标记超过显示上限，省略 %1 个；全部记录仍可在场景目录查看。")
            .arg(startLocationMarkers_.omittedCount));
}

} // namespace War3Map::Modules
