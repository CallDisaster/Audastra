#include "WorldModule.h"
#include "WorldNavigationBookmark.h"
#include "WorldViewport.h"
#include "WorldOutlinerModel.h"
#include "WorldViewportInternals.h"
#include "EditorProject.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QLineEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTreeView>
#include <QVariantAnimation>

#include <array>
#include <map>
#include <set>

namespace War3Map::Modules {
namespace {
bool CameraBookmarkValid(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const auto camera = value.toObject();
    for (const auto* key : {"x", "y", "z", "distance", "yaw", "pitch"}) {
        const auto item = camera.value(QLatin1String(key));
        if (!item.isDouble() || !std::isfinite(item.toDouble()) || std::abs(item.toDouble()) > kMaximumCoordinate) return false;
    }
    const auto distance = camera.value("distance").toDouble(), pitch = camera.value("pitch").toDouble();
    return distance >= kMinimumDistance && distance <= kMaximumDistance && pitch >= kMinimumPitch && pitch <= kMaximumPitch;
}

int LoadedRows(const WorldOutlinerModel& model, const QModelIndex& parent = {}) {
    int count = 0;
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const auto index = model.index(row, 0, parent);
        count += model.RowAt(index) ? 1 : LoadedRows(model, index);
    }
    return count;
}

const Services::WorldObjectRow* FirstRow(const WorldOutlinerModel& model, const QModelIndex& parent) {
    if (const auto* row = model.RowAt(parent)) return row;
    for (int index = 0; index < model.rowCount(parent); ++index)
        if (const auto* row = FirstRow(model, model.index(index, 0, parent))) return row;
    return nullptr;
}

QJsonObject GroupKey(const WorldOutlinerModel& model, const QModelIndex& index) {
    const auto* row = FirstRow(model, index);
    if (!row || model.RowAt(index)) return {};
    const bool nested = index.parent().isValid();
    return {{"category", static_cast<int>(row->category)}, {"nested", nested},
        {"owner", nested && row->owner ? QJsonValue(*row->owner) : QJsonValue(QJsonValue::Null)}};
}

void CollectGroups(const WorldOutlinerModel& model, std::map<QByteArray, QModelIndex>& groups,
    const QModelIndex& parent = {}) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const auto index = model.index(row, 0, parent);
        if (model.RowAt(index)) continue;
        const auto key = GroupKey(model, index);
        if (!key.isEmpty()) groups.emplace(QJsonDocument(key).toJson(QJsonDocument::Compact), index);
        CollectGroups(model, groups, index);
    }
}
}

bool WorldModule::PrepareNavigationLeave() {
    if (dragStart_ || dragPosition_ || dragRotation_ || rotationOrigin_ || regionDraft_ || cameraDraft_
        || strokeBase_ || !strokePoints_.empty() || pasting_ || placing_ || (viewport_ && viewport_->StrokeActive())) return false;
    if (inspectorBody_) for (const auto* edit : inspectorBody_->findChildren<QLineEdit*>())
        if ((edit->hasFocus() && edit->isModified()) || edit->property("worldEdited").toBool()) return false;
    if (inspectorBody_) for (const auto* spin : inspectorBody_->findChildren<QDoubleSpinBox*>())
        if (spin->property("worldEdited").toBool()) return false;
    return true;
}

std::optional<QJsonObject> WorldModule::CaptureNavigationBookmark() const {
    if (!page_ || !viewport_ || !outliner_ || !outlinerModel_ || !outlinerFilter_ || !outlinerCategory_) return std::nullopt;
    const auto map = MapKey();
    if (map.size() > 4096 || outlinerFilter_->text().size() > WorldNavigation::MaximumText
        || (project_.mapDocument_ && shownMap_ != map)) return std::nullopt;
    QJsonArray selection;
    QJsonValue primary(QJsonValue::Null);
    if (project_.mapDocument_) {
        if (!worldSelection_.Version() || !selectionMapData_
            || selectionMapKey_ != map || (project_.mapEdits_ && selectionMapData_->Binding().revision != project_.mapEdits_->Revision())) return std::nullopt;
        const auto snapshot = worldSelection_.Snapshot(SelectionContext(), *worldSelection_.Version());
        if (!snapshot.Succeeded() || !snapshot.value) return std::nullopt;
        for (const auto& id : snapshot.value->selected) selection.push_back(WorldNavigation::Encode(id));
        if (snapshot.value->primary) primary = WorldNavigation::Encode(*snapshot.value->primary);
    }
    QJsonArray expanded;
    std::map<QByteArray, QModelIndex> groups;
    CollectGroups(*outlinerModel_, groups);
    for (const auto& [key, index] : groups) if (outliner_->isExpanded(index)) expanded.push_back(QJsonDocument::fromJson(key).object());
    QJsonObject state{{"schema", 1}, {"map", map}, {"selection", selection}, {"primary", primary},
        {"camera", viewport_->SaveCamera()}, {"search", outlinerFilter_->text()},
        {"category", outlinerCategory_->currentData().toInt()}, {"horizontal", outliner_->horizontalScrollBar()->value()},
        {"vertical", outliner_->verticalScrollBar()->value()}, {"loadedRows", LoadedRows(*outlinerModel_)}, {"expanded", expanded}};
    return WorldNavigation::Bounded(state) ? std::optional(state) : std::nullopt;
}

Shell::NavigationRestoreResult WorldModule::RestoreNavigationBookmark(const QJsonObject& state) {
    using Restore = Shell::NavigationRestoreResult;
    if (!PrepareNavigationLeave()) return Restore::Blocked;
    const auto schema = WorldNavigation::Integer(state.value("schema"), 1, 1);
    const auto category = WorldNavigation::Integer(state.value("category"), -1, static_cast<int>(Services::WorldCategory::Unknown));
    const auto horizontal = WorldNavigation::Integer(state.value("horizontal")), vertical = WorldNavigation::Integer(state.value("vertical"));
    const auto loadedRows = WorldNavigation::Integer(state.value("loadedRows"), 0, 4096);
    if (!schema || !category || !horizontal || !vertical || !loadedRows || !state.value("expanded").isArray() || !state.value("map").isString()
        || state.value("map").toString().size() > 4096 || state.value("map").toString() != MapKey()
        || !state.value("search").isString() || state.value("search").toString().size() > WorldNavigation::MaximumText
        || !state.value("selection").isArray() || !CameraBookmarkValid(state.value("camera"))
        || !WorldNavigation::Bounded(state)) return Restore::Invalid;
    const auto encoded = state.value("selection").toArray();
    if (encoded.size() > static_cast<qsizetype>(Services::WorldSelection::MaxSelection)) return Restore::Invalid;
    std::vector<Services::WorldObjectId> ids;
    ids.reserve(static_cast<std::size_t>(encoded.size()));
    std::set<QByteArray> unique;
    const auto document = CurrentDocument();
    for (const auto& value : encoded) {
        const auto id = WorldNavigation::Decode(value);
        if (!id || !document || !WorldNavigation::Contains(*document, *id)
            || !unique.insert(QJsonDocument(WorldNavigation::Encode(*id)).toJson(QJsonDocument::Compact)).second) return Restore::Invalid;
        const auto flags = worldSelection_.Flags(*id);
        if (!flags.visible || flags.locked) return Restore::Invalid;
        ids.push_back(*id);
    }
    std::optional<Services::WorldObjectId> primary;
    if (!state.value("primary").isNull()) {
        primary = WorldNavigation::Decode(state.value("primary"));
        if (!primary || std::find(ids.begin(), ids.end(), *primary) == ids.end()) return Restore::Invalid;
    }
    if (ids.empty() != !primary) return Restore::Invalid;
    const auto expanded = state.value("expanded").toArray();
    if (expanded.size() > 4104) return Restore::Invalid;
    std::set<QByteArray> expansions;
    for (const auto& group : expanded) {
        if (!group.isObject()) return Restore::Invalid;
        const auto key = group.toObject();
        if (key.size() != 3 || !WorldNavigation::Integer(key.value("category"), 0, static_cast<int>(Services::WorldCategory::Unknown))
            || !key.value("nested").isBool() || (!key.value("owner").isNull()
                && !WorldNavigation::Integer(key.value("owner"), std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()))
            || (!key.value("nested").toBool() && !key.value("owner").isNull())
            || !expansions.insert(QJsonDocument(key).toJson(QJsonDocument::Compact)).second) return Restore::Invalid;
    }
    if (!page_ || !viewport_ || !outliner_ || !outlinerCategory_ || !outlinerFilter_
        || (project_.mapDocument_ && shownMap_ != MapKey())) return Restore::Blocked;
    const int categoryIndex = outlinerCategory_->findData(static_cast<int>(*category));
    if (categoryIndex < 0) return Restore::Invalid;
    if (project_.mapDocument_ && (!RefreshSelectionContext() || !worldSelection_.Version())) return Restore::Blocked;
    // Replace 与 primary 的 Add 最多占两次选择修订，先预留，不能集合已改后才拒绝 primary。
    if (project_.mapDocument_ && worldSelection_.Version()->editorRevision > std::numeric_limits<std::uint64_t>::max() - 2)
        return Restore::Blocked;
    Services::WorldObjectFilter filter;
    filter.searchUtf8 = state.value("search").toString().toStdString();
    if (*category >= 0) filter.category = static_cast<Services::WorldCategory>(*category);
    // 用同一只读 provider 预检已加载范围和展开组；失败时不先改变正式目录、相机或选择。
    if (project_.mapDocument_ && *loadedRows > 0) {
        WorldOutlinerModel prepared;
        prepared.SetFilter(filter);
        prepared.SetSource([this](MapDataKind kind, const Services::WorldObjectFilter& requested,
            const std::optional<Services::WorldObjectCursor>& cursor, const Services::WorldObjectQueryOptions& options) {
            return worldSelection_.QueryObjects(SelectionContext(), *worldSelection_.Version(), kind, requested, cursor, options);
        });
        for (int count = 0; LoadedRows(prepared) < *loadedRows && prepared.canFetchMore({}) && count < 256; ++count) prepared.fetchMore({});
        if (!prepared.Version() || LoadedRows(prepared) < *loadedRows) return Restore::Blocked;
        std::map<QByteArray, QModelIndex> preparedGroups;
        CollectGroups(prepared, preparedGroups);
        for (const auto& key : expansions) if (!preparedGroups.contains(key)) return Restore::Invalid;
    } else if (*loadedRows != 0 || !expansions.empty()) return Restore::Invalid;
    const auto oldCamera = viewport_->SaveCamera();
    // 结构、完整 typed 目标与所有视图值先检查；相机拒绝不改变选择或筛选。
    if (!viewport_->RestoreCamera(state.value("camera").toObject())) return Restore::Blocked;
    if (project_.mapDocument_) {
        const auto selected = worldSelection_.Select(SelectionContext(), *worldSelection_.Version(), Services::SelectionMode::Replace, ids);
        if (!selected.Succeeded() || !selected.value) { (void)viewport_->RestoreCamera(oldCamera); return Restore::Blocked; }
        auto restored = *selected.value;
        // Add 已在集合中的完整 ID 不重排集合，沿现有服务更新 primary；不实现第二套选择状态。
        if (primary && restored.primary != primary) {
            const std::array<Services::WorldObjectId, 1> focused{*primary};
            const auto focus = worldSelection_.Select(SelectionContext(), *worldSelection_.Version(), Services::SelectionMode::Add, focused);
            if (!focus.Succeeded() || !focus.value) { (void)viewport_->RestoreCamera(oldCamera); return Restore::Blocked; }
            restored = *focus.value;
        }
        ApplySelectionSnapshot(restored);
    }
    if (cameraFlight_) cameraFlight_->stop();
    {
        const QSignalBlocker searching(outlinerFilter_), filtering(outlinerCategory_);
        outlinerFilter_->setText(state.value("search").toString()); outlinerCategory_->setCurrentIndex(categoryIndex);
    }
    RefreshOutliner();
    outlinerModel_->SetFilter(std::move(filter));
    if (worldSelection_.Version() && outlinerModel_->Version() && outlinerModel_->Version() != worldSelection_.Version()) outlinerModel_->Refresh();
    for (int count = 0; LoadedRows(*outlinerModel_) < *loadedRows && outlinerModel_->canFetchMore({}) && count < 256; ++count) outlinerModel_->fetchMore({});
    outliner_->collapseAll();
    std::map<QByteArray, QModelIndex> restoredGroups;
    CollectGroups(*outlinerModel_, restoredGroups);
    for (const auto& [key, index] : restoredGroups) outliner_->setExpanded(index, expansions.contains(key));
    PresentSelection(); SyncOutlinerSelection();
    outliner_->doItemsLayout();
    outliner_->horizontalScrollBar()->setValue(static_cast<int>(*horizontal));
    outliner_->verticalScrollBar()->setValue(static_cast<int>(*vertical));
    return Restore::Restored;
}

} // namespace War3Map::Modules
