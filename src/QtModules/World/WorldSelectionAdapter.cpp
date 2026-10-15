#include "WorldModule.h"
#include "WorldOutlinerModel.h"
#include "WorldViewport.h"
#include "WorldAssetBrowser.h"
#include "WorldPlacementModels.h"
#include "WorldSelectionProperties.h"
#include "Widgets/ElidedLabel.h"
#include "EditorProject.h"
#include "ProjectQuerySources.h"
#include "ObjectNames.h"
#include "Commands/MapDataCommands.h"
#include "War3MapCore/MapArchiveSession.h"

#include <QApplication>
#include <QComboBox>
#include <QItemSelectionModel>
#include <QLabel>
#include <QUndoStack>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTreeView>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace War3Map::Modules {
namespace {
Services::WorldObjectFilter ReadOutlinerFilter(const QLineEdit* search, const QComboBox* category) {
    Services::WorldObjectFilter filter;
    if (search) filter.searchUtf8 = search->text().toStdString();
    if (category && category->currentIndex() > 0)
        filter.category = static_cast<Services::WorldCategory>(category->currentData().toInt());
    return filter;
}
}

Services::WorldSelectionContext WorldModule::SelectionContext() const {
    Services::WorldSelectionContext context;
    context.document = selectionDocument_ ? &*selectionDocument_ : nullptr;
    context.mapData = selectionMapData_ ? &*selectionMapData_ : nullptr;
    if (context.mapData) context.binding = context.mapData->Binding();
    context.catalogRevision = selectionCatalogRevision_;
    context.objects = selectionObjectSources_;
    context.standardObjects = project_.standardCatalog_.get();
    if (context.standardObjects) context.standardState = Services::ProjectSearchState::Ready;
    context.objectNameReader = [this](const ObjectData::ObjectKey& key) {
        const auto index = static_cast<std::size_t>(key.type);
        const auto shown = project_.objectDocuments_[index]
            ? ResolveObjectPresentation(key, project_.objectDocuments_[index].get(), project_.standardCatalog_.get(), selectionDocument_ ? &*selectionDocument_ : nullptr)
            : ResolveNativeObjectPresentation(key, project_.nativeObjectDocuments_[index].get(), project_.standardCatalog_.get(), selectionDocument_ ? &*selectionDocument_ : nullptr);
        Services::MapDataQueryText text;
        text.rawBytes = key.rawcode;
        text.displayBytes = shown.name.toStdString();
        if (!shown.issue.isEmpty()) text.state = Services::MapDataQueryTextState::CatalogUnavailable;
        return Result<Services::MapDataQueryText>::Success(std::move(text));
    };
    return context;
}

bool WorldModule::RefreshSelectionContext(std::shared_ptr<const MapDocument> owned) {
    if (!project_.mapDocument_) {
        worldSelection_.Clear(); selectionDocument_.reset(); selectionMapData_.reset();
        startLocationMarkers_ = {}; startLocationBinding_.reset(); startLocationOverlaySubmitted_ = false;
        if (viewport_) (void)viewport_->SetOverlayLines({});
        return false;
    }
    const auto sessionIdentity = project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0;
    if (selectionDocumentId_.empty() || selectionSessionIdentity_ != sessionIdentity)
        selectionDocumentId_ = "world-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto mapKey = MapKey();
    const auto revision = project_.mapEdits_ ? project_.mapEdits_->Revision() : 0;
    const auto generation = std::max<std::uint64_t>(1, project_.editRevision_.Generation());
    if (!selectionMapData_ || selectionSessionIdentity_ != sessionIdentity
        || selectionMapKey_ != mapKey || selectionMapData_->Binding().revision != revision
        || selectionMapData_->Binding().generation != generation || selectionBoundCatalogRevision_ != selectionCatalogRevision_) {
        auto captured = QtProject::CaptureProjectMapDataQuery(project_, selectionDocumentId_, generation, std::move(owned));
        if (!captured.Succeeded()) {
            startLocationMarkers_ = {}; startLocationBinding_.reset(); startLocationOverlaySubmitted_ = false;
            if (viewport_) (void)viewport_->SetOverlayLines({});
            if (selectionStatus_) selectionStatus_->setText(tr("场景查询不可用：%1").arg(WorldDiagnosticText(captured.diagnostics)));
            return false;
        }
        selectionMapData_ = std::move(*captured.value);
        selectionDocument_ = selectionMapData_->OwnedDocument();
        selectionObjectSources_ = QtProject::CaptureProjectObjectSources(project_);
        selectionMapKey_ = mapKey;
        selectionSessionIdentity_ = sessionIdentity;
        selectionBoundCatalogRevision_ = selectionCatalogRevision_;
        startLocationMarkers_ = {}; startLocationBinding_.reset(); startLocationOverlaySubmitted_ = false;
        const auto markers = CaptureWorldStartLocations(*selectionDocument_);
        if (markers.Succeeded()) {
            startLocationMarkers_ = *markers.value; startLocationBinding_ = selectionMapData_->Binding();
        } else project_.AddDiagnostics(markers.diagnostics);
    }
    const auto bound = worldSelection_.Bind(SelectionContext());
    if (!bound.Succeeded() || !worldSelection_.Version()) return false;
    const auto snapshot = worldSelection_.Snapshot(SelectionContext(), *worldSelection_.Version());
    if (!snapshot.Succeeded()) return false;
    ApplySelectionSnapshot(*snapshot.value);
    return true;
}

void WorldModule::ApplySelectionSnapshot(const Services::WorldSelectionSnapshot& snapshot) {
    selectedPlacements_.clear(); selection_.reset(); selectedRegion_.reset(); selectedCamera_.reset();
    for (const auto& id : snapshot.selected)
        if (const auto* placement = std::get_if<MapData::PlacementId>(&id)) selectedPlacements_.push_back(*placement);
    if (snapshot.primary) {
        if (const auto* placement = std::get_if<MapData::PlacementId>(&*snapshot.primary)) selection_ = *placement;
        else if (snapshot.selected.size() == 1) {
            if (const auto* region = std::get_if<MapData::RegionId>(&*snapshot.primary)) selectedRegion_ = *region;
            if (const auto* camera = std::get_if<MapData::CameraId>(&*snapshot.primary)) selectedCamera_ = *camera;
        }
    }
}

bool WorldModule::SelectObjects(std::span<const Services::WorldObjectId> ids, Services::SelectionMode mode) {
    if (!RefreshSelectionContext() || !worldSelection_.Version()) return false;
    const auto selected = worldSelection_.Select(SelectionContext(), *worldSelection_.Version(), mode, ids);
    if (!selected.Succeeded()) {
        if (selectionStatus_) selectionStatus_->setText(tr("选择被拒绝：%1").arg(WorldDiagnosticText(selected.diagnostics)));
        SyncOutlinerSelection(); return false;
    }
    ApplySelectionSnapshot(*selected.value);
    PresentSelection();
    return true;
}

bool WorldModule::SelectionEditable() const {
    if ((viewport_ && !viewport_->ModelPresentationCurrent()) || project_.savingMap_
        || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_
        || !selectionMapData_ || !worldSelection_.Version() || !project_.mapEdits_
        || selectionSessionIdentity_ != project_.mapEdits_->SessionIdentity()
        || selectionMapKey_ != MapKey() || selectionMapData_->Binding().generation != std::max<std::uint64_t>(1, project_.editRevision_.Generation())
        || selectionMapData_->Binding().revision != project_.mapEdits_->Revision()
        || selectionBoundCatalogRevision_ != selectionCatalogRevision_) return false;
    return worldSelection_.EditStatus(SelectionContext(), *worldSelection_.Version()).Succeeded();
}

std::uint64_t WorldModule::SelectionRevision() const {
    return worldSelection_.Version() ? worldSelection_.Version()->editorRevision : 0;
}

void WorldModule::RefreshOutliner(std::shared_ptr<const MapDocument> owned) {
    if (!outlinerModel_ || !outliner_) return;
    outlinerDirty_ = true;
    // 摆放页与收起的工具栏不需要重建不可见的目录，但 typed 选择、显隐和修订
    // 仍立即绑定到当前文档，不能让隐藏 UI 延迟影响视口和后续编辑的身份检查。
    if (!RefreshSelectionContext(std::move(owned))) return;
    // 只有辅助对象/起点的地图没有主模型 scene；同一文档事务仍须刷新标记和属性。
    if (!scene_ && terrain_ && viewport_) { UpdateOverlay(); UpdateInspector(); }
    if (!outliner_->isVisible()) return;
    const QSignalBlocker selectionBlocker(outliner_->selectionModel());
    const auto boundVersion = *worldSelection_.Version();
    const bool configured = outlinerModel_->Version().has_value();
    // provider 借用的是调用时的当前 overlay。先应用隐藏期间最后一次筛选；它若
    // 已经取得新修订便不再 Refresh，避免 SetSource 后又因筛选变化重读同一页。
    outlinerModel_->SetFilter(ReadOutlinerFilter(outlinerFilter_, outlinerCategory_));
    if (!configured) outlinerModel_->SetSource([this](MapDataKind kind, const Services::WorldObjectFilter& filter,
        const std::optional<Services::WorldObjectCursor>& cursor, const Services::WorldObjectQueryOptions& options) {
        if (!worldSelection_.Version()) return Result<Services::WorldObjectPage>::Failure("World", "WORLD_QUERY_UNBOUND", "场景没有当前修订。");
        return worldSelection_.QueryObjects(SelectionContext(), *worldSelection_.Version(), kind, filter, cursor, options);
    }, [this](const Services::WorldSelectionVersion& expected, std::span<const Services::WorldEditorFlagEdit> edits) {
        if (viewport_ && viewport_->RendererReady() && viewport_->HasModelInstances()
            && viewport_->HasModelSupport() && !viewport_->HasEditorVisibilitySupport()) {
            for (const auto& edit : edits)
                if (std::holds_alternative<MapData::PlacementId>(edit.id)
                    && worldSelection_.Flags(edit.id).visible != edit.value.visible)
                    return Result<Services::WorldSelectionSnapshot>::Failure("World", "WORLD_VISIBILITY_UNSUPPORTED", "当前渲染插件不支持模型显隐，请更新插件。");
        }
        auto result = worldSelection_.SetEditorFlags(SelectionContext(), expected, edits);
        if (result.Succeeded()) QTimer::singleShot(0, this, [this, snapshot = *result.value] {
            if (worldSelection_.Version() == snapshot.version) {
                ApplySelectionSnapshot(snapshot); PresentSelection();
                RefreshBuildingSplats(false);
                if (viewport_ && viewport_->RendererReady() && !viewport_->RefreshEditorVisibility())
                    selectionStatus_->setText(tr("显隐状态待呈现：%1").arg(viewport_->StatusText()));
            }
        });
        return result;
    });
    else if (const auto& version = outlinerModel_->Version(); version
        && (version->document != boundVersion.document || version->catalogRevision != boundVersion.catalogRevision
            || version->editorRevision > boundVersion.editorRevision)) outlinerModel_->Refresh();
    outlinerModel_->SetDiagnosticReader([this](const Services::WorldSelectionVersion& expected,
        const Services::WorldObjectId& id) {
        const auto* placement = std::get_if<MapData::PlacementId>(&id);
        return placement ? InstanceDiagnosticText(expected, *placement) : QString{};
    });
    const auto& version = outlinerModel_->Version();
    outlinerDirty_ = !version || version->document != boundVersion.document || version->catalogRevision != boundVersion.catalogRevision
        || version->editorRevision > boundVersion.editorRevision || worldSelection_.Version() != boundVersion;
    if (outlinerStatus_) outlinerStatus_->setText(outlinerModel_->StatusText());
    if (outlinerDirty_) return; // 查询失败或同步重入后身份改变，不能误清待刷新状态。
    outliner_->expandAll();
    SyncOutlinerSelection();
}

void WorldModule::FilterOutliner(const QString& text) {
    if (!outlinerModel_) return;
    if (!outliner_->isVisible()) { outlinerDirty_ = true; return; }
    if (outlinerDirty_) { RefreshOutliner(); return; }
    const QSignalBlocker selectionBlocker(outliner_->selectionModel());
    auto filter = ReadOutlinerFilter(outlinerFilter_, outlinerCategory_);
    filter.searchUtf8 = text.toStdString();
    outlinerModel_->SetFilter(std::move(filter));
    outliner_->expandAll();
    if (outlinerStatus_) outlinerStatus_->setText(outlinerModel_->StatusText());
    outlinerDirty_ = !outlinerModel_->Version();
    SyncOutlinerSelection();
}

void WorldModule::SyncOutlinerSelection() {
    RefreshInstanceDiagnostics();
    if (!outliner_ || !outlinerModel_ || !outliner_->selectionModel() || !worldSelection_.Version()
        || outlinerDirty_ || !outliner_->isVisible()) return;
    const auto snapshot = worldSelection_.Snapshot(SelectionContext(), *worldSelection_.Version());
    if (!snapshot.Succeeded()) return;
    if (!outlinerModel_->ApplySnapshot(*snapshot.value)) return;
    const QSignalBlocker blocker(outliner_->selectionModel());
    outliner_->selectionModel()->clearSelection();
    for (const auto& id : snapshot.value->selected) {
        const auto index = outlinerModel_->Find(id);
        if (index.isValid()) outliner_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    if (snapshot.value->primary) {
        const auto index = outlinerModel_->Find(*snapshot.value->primary);
        if (index.isValid()) outliner_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    }
}

void WorldModule::ActivateOutlinerItem(const QModelIndex& index, bool center) {
    const auto* row = outlinerModel_ ? outlinerModel_->RowAt(index) : nullptr;
    if (!row || !row->id || !outlinerModel_->Version() || outlinerModel_->Version() != worldSelection_.Version()) return;
    const auto id = *row->id;
    const auto position = row->positionWorld;
    const auto modifiers = QApplication::keyboardModifiers();
    const auto mode = modifiers.testFlag(Qt::ControlModifier) ? Services::SelectionMode::Subtract
        : modifiers.testFlag(Qt::ShiftModifier) ? Services::SelectionMode::Add : Services::SelectionMode::Replace;
    std::vector<Services::WorldObjectId> range;
    if (mode == Services::SelectionMode::Add && outliner_->selectionModel())
        for (const auto& selected : outliner_->selectionModel()->selectedRows())
            if (const auto* selectedRow = outlinerModel_->RowAt(selected); selectedRow && selectedRow->id)
                range.push_back(*selectedRow->id);
    if (!SelectObjects(range.empty() ? std::span(&id, 1) : std::span<const Services::WorldObjectId>(range), mode)) return;
    if (center && position) FlyTo((*position)[0], (*position)[1], std::nullopt);
}

void WorldModule::RefreshAssetBrowser() {
    if (!assetBrowser_) return;
    const WorldAssetBinding binding{MapKey(), generation_, project_.mapEdits_ ? project_.mapEdits_->Revision() : 0};
    assetBrowser_->SetContext(project_, binding, [this, binding] {
        return binding.mapKey == MapKey() && binding.generation == generation_
            && binding.revision == (project_.mapEdits_ ? project_.mapEdits_->Revision() : 0);
    });
}

void WorldModule::SelectAllWorldObjects() {
    if (!RefreshSelectionContext() || !worldSelection_.Version()) return;
    std::vector<Services::WorldObjectId> ids;
    std::size_t examined = 0;
    Services::WorldObjectFilter filter;
    if (outlinerFilter_) filter.searchUtf8 = outlinerFilter_->text().toStdString();
    if (outlinerCategory_ && outlinerCategory_->currentIndex() > 0)
        filter.category = static_cast<Services::WorldCategory>(outlinerCategory_->currentData().toInt());
    for (const auto kind : {MapDataKind::Units, MapDataKind::Doodads, MapDataKind::Regions, MapDataKind::Cameras}) {
        std::optional<Services::WorldObjectCursor> cursor;
        do {
            const auto page = worldSelection_.QueryObjects(SelectionContext(), *worldSelection_.Version(), kind, filter, cursor);
            if (!page.Succeeded()) {
                selectionStatus_->setText(tr("全选被拒绝：%1").arg(WorldDiagnosticText(page.diagnostics))); return;
            }
            examined += page.value->examined;
            for (const auto& row : page.value->rows)
                if (row.id && row.flags.visible && !row.flags.locked) ids.push_back(*row.id);
            if (ids.size() > Services::WorldSelection::MaxSelection || examined > 32768) {
                selectionStatus_->setText(tr("全选超过预算，请先按类别缩小范围；未部分替换选择")); return;
            }
            cursor = page.value->next;
        } while (cursor);
    }
    (void)SelectObjects(ids, Services::SelectionMode::Replace);
}

void WorldModule::NudgeSelection(double dx, double dy) {
    if (!SelectionEditable() || selectedPlacements_.empty() || !selectionDocument_ || !terrain_) return;
    auto moves = BuildSelectedPlacementOffsetMoves(*selectionDocument_, *terrain_, selectedPlacements_, {dx, dy});
    if (!moves.Succeeded()) { selectionStatus_->setText(WorldDiagnosticText(moves.diagnostics)); return; }
    const auto status = MapData::MovePlacements(*project_.mapEdits_, *moves.value);
    if (!status.Succeeded()) { project_.AddDiagnostics(status.diagnostics); selectionStatus_->setText(WorldDiagnosticText(status.diagnostics)); return; }
    (void)project_.ObserveMapDataEdit("微调摆放组");
    QtProject::PushMapDataEdit(project_, tr("微调 %1 个摆放").arg(selectedPlacements_.size()), "微调摆放组");
    if (!project_.mapDataUndoStack_) RefreshMapEdits();
}

void WorldModule::DuplicateSelection() {
    if (!SelectionEditable()) return;
    if (selectedRegion_ || selectedCamera_) { DuplicateSelectedAuxiliary(); return; }
    if (selectedPlacements_.empty()) return;
    auto copied = MapData::CopyPlacements(*project_.mapEdits_, selectedPlacements_, {0, 0, 0});
    if (!copied.Succeeded()) { project_.AddDiagnostics(copied.diagnostics); selectionStatus_->setText(WorldDiagnosticText(copied.diagnostics)); return; }
    (void)project_.ObserveMapDataEdit("就地复制摆放组");
    QtProject::PushMapDataEdit(project_, tr("副本 %1 个摆放").arg(copied.value->size()), "就地复制摆放组");
    std::vector<Services::WorldObjectId> ids;
    for (const auto& id : *copied.value) ids.emplace_back(id);
    (void)SelectObjects(ids, Services::SelectionMode::Replace);
    if (!project_.mapDataUndoStack_) RefreshMapEdits();
}

} // namespace War3Map::Modules
