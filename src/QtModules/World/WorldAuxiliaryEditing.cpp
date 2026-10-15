#include "WorldModule.h"
#include "WorldAuxiliaryDialog.h"
#include "WorldViewport.h"
#include "EditorProject.h"
#include "ProjectAuxiliaryEdits.h"
#include "Widgets/ElidedLabel.h"
#include "War3MapCore/Text/TextUtil.h"

#include <QAction>
#include <QLabel>
#include <QPushButton>
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace War3Map::Modules {
namespace {
void Report(QtProject::EditorProject& project, QLabel* status, const std::vector<Diagnostic>& diagnostics) {
    project.AddDiagnostics(diagnostics);
    if (status) status->setText(QObject::tr("操作未完成：%1").arg(WorldDiagnosticText(diagnostics)));
}
std::string UniqueName(std::string_view base, const MapDocument& source, MapDataKind kind, bool copy) {
    if (base.size() > 4078) return {};
    std::unordered_set<std::string> symbols;
    if (kind == MapDataKind::Regions && source.Regions())
        for (const auto& entry : source.Regions()->entries) symbols.insert(Text::ObjectScriptName(entry.name));
    if (kind == MapDataKind::Cameras && source.Cameras())
        for (const auto& entry : source.Cameras()->entries) symbols.insert(Text::ObjectScriptName(entry.name));
    for (int suffix = 1; suffix <= 100000; ++suffix) {
        const auto name = std::string(base) + (copy ? "_copy" : "") + std::to_string(suffix);
        if (!symbols.contains(Text::ObjectScriptName(name))) return name;
    }
    return {};
}
std::optional<std::int32_t> NextRegionNumber(const MapDocument& source) {
    std::int32_t highest = -1;
    if (source.Regions()) for (const auto& region : source.Regions()->entries) highest = std::max(highest, region.creationNumber);
    if (highest == std::numeric_limits<std::int32_t>::max()) return {};
    return highest + 1;
}
Result<Services::AuxiliaryEditRequest> ExistingRequest(const MapDocument& source,
    const std::optional<MapData::RegionId>& region, const std::optional<MapData::CameraId>& camera) {
    using Output = Result<Services::AuxiliaryEditRequest>;
    Services::AuxiliaryEditRequest request;
    if (region && source.Regions() && region->index < source.Regions()->entries.size()
        && MapData::MakeRegionId(region->index, source.Regions()->entries[region->index]) == *region) {
        request.kind = MapDataKind::Regions; request.sourceIndex = region->index;
        request.record = source.Regions()->entries[region->index];
    } else if (camera && source.Cameras() && camera->index < source.Cameras()->entries.size()
        && MapData::MakeCameraId(camera->index, source.Cameras()->entries[camera->index]) == *camera) {
        request.kind = MapDataKind::Cameras; request.sourceIndex = camera->index;
        request.record = source.Cameras()->entries[camera->index];
    } else return Output::Failure("World", "AUX_SELECTION_STALE", "所选区域或镜头已变化，请重新选择。");
    const auto member = Services::AuxiliaryEditing::MemberIdentity(source, request.kind);
    const auto key = MapData::MapFieldRowKey(source, request.kind, request.sourceIndex);
    if (!member.Succeeded()) return {std::nullopt, member.diagnostics};
    if (!key.Succeeded()) return {std::nullopt, key.diagnostics};
    request.expectedMemberSha256 = *member.value; request.expectedRowKey = *key.value;
    return Output::Success(std::move(request));
}
}

void WorldModule::SubmitAuxiliary(Services::AuxiliaryEditRequest request,
    const QtProject::AuxiliaryEditBinding& binding, const QString& label) {
    const auto result = QtProject::ApplyAuxiliaryEditWithResult(project_, binding, std::move(request), label);
    if (!result.Succeeded()) {
        Report(project_, selectionStatus_, result.diagnostics);
        return;
    }
    // 撤销栈和文档信号可能触发重入编辑；只恢复这个提交仍然拥有的选择身份。
    if (!QtProject::AuxiliaryEditBindingCurrent(project_, result.value->binding)) return;
    CaptureAuxiliary(project_.mapEdits_->Snapshot());
    std::optional<Services::WorldObjectId> selected;
    if (const auto* region = std::get_if<MapData::RegionId>(&result.value->object)) selected = *region;
    if (const auto* camera = std::get_if<MapData::CameraId>(&result.value->object)) selected = *camera;
    if (selected) (void)SelectObjects(std::span(&*selected, 1), Services::SelectionMode::Replace);
    else ClearSelection();
    RefreshOutliner(); UpdateOverlay(); UpdateSelectionText(); UpdateAuxiliaryActions();
}

void WorldModule::ShowAuxiliaryDialog(Services::AuxiliaryEditRequest request,
    const QtProject::AuxiliaryEditBinding& binding, const MapDocument& source) {
    WorldAuxiliaryRecord record;
    if (const auto* region = std::get_if<MapData::RegionRecord>(&request.record)) record = *region;
    else if (const auto* camera = std::get_if<MapData::CameraRecord>(&request.record)) record = *camera;
    else return;
    const bool creating = request.action == Services::AuxiliaryEditAction::Create;
    auto* dialog = new WorldAuxiliaryDialog(std::move(record), source, creating, page_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const auto selectionVersion = worldSelection_.Version();
    connect(dialog, &QDialog::accepted, this, [this, dialog, request = std::move(request), binding,
        selectionVersion, creating]() mutable {
        if (worldSelection_.Version() != selectionVersion) {
            if (selectionStatus_) selectionStatus_->setText(tr("选择或地图来源已变化，请重新打开属性。"));
            return;
        }
        const auto value = dialog->Record();
        if (!value.Succeeded()) {
            Report(project_, selectionStatus_, value.diagnostics);
            return;
        }
        std::visit([&](const auto& record) { request.record = record; }, *value.value);
        SubmitAuxiliary(std::move(request), binding, creating ? tr("新建区域或镜头") : tr("修改区域或镜头"));
    });
    dialog->open();
}

void WorldModule::CreateAuxiliary(MapDataKind kind) {
    if (kind != MapDataKind::Regions && kind != MapDataKind::Cameras) return;
    const auto binding = QtProject::CaptureAuxiliaryEditBinding(project_);
    if (!binding.Succeeded()) { Report(project_, selectionStatus_, binding.diagnostics); return; }
    const auto source = project_.mapEdits_->Snapshot();
    const auto member = Services::AuxiliaryEditing::MemberIdentity(source, kind);
    if (!member.Succeeded()) { Report(project_, selectionStatus_, member.diagnostics); return; }
    Services::AuxiliaryEditRequest request;
    request.kind = kind; request.action = Services::AuxiliaryEditAction::Create; request.expectedMemberSha256 = *member.value;
    float x = 0, y = 0;
    if (viewport_ && viewport_->SelectedCoordinate()) {
        x = viewport_->SelectedCoordinate()->position[0]; y = viewport_->SelectedCoordinate()->position[1];
    }
    if (kind == MapDataKind::Regions) {
        const auto number = NextRegionNumber(source);
        if (!number) { if (selectionStatus_) selectionStatus_->setText(tr("区域创建编号已用尽")); return; }
        MapData::RegionRecord region{x - 128, y - 128, x + 128, y + 128,
            UniqueName("Region", source, kind, false), *number, {}, {}, {255, 128, 0, 255}};
        request.record = std::move(region);
    } else {
        MapData::CameraRecord camera{x, y, 0, 90, 304, 1650, 0, 70, 5000, 100,
            UniqueName("Camera", source, kind, false), std::nullopt};
        if (source.Cameras() && !source.Cameras()->entries.empty())
            camera.nearClippingPlane = source.Cameras()->entries.front().nearClippingPlane;
        request.record = std::move(camera);
    }
    ShowAuxiliaryDialog(std::move(request), *binding.value, source);
}

void WorldModule::EditSelectedAuxiliary() {
    if (!SelectionEditable()) return;
    const auto binding = QtProject::CaptureAuxiliaryEditBinding(project_);
    if (!binding.Succeeded()) { Report(project_, selectionStatus_, binding.diagnostics); return; }
    const auto source = project_.mapEdits_->Snapshot();
    auto request = ExistingRequest(source, selectedRegion_, selectedCamera_);
    if (!request.Succeeded()) { Report(project_, selectionStatus_, request.diagnostics); return; }
    ShowAuxiliaryDialog(std::move(*request.value), *binding.value, source);
}

void WorldModule::DeleteSelectedAuxiliary() {
    if (!SelectionEditable()) return;
    const auto binding = QtProject::CaptureAuxiliaryEditBinding(project_);
    if (!binding.Succeeded()) { Report(project_, selectionStatus_, binding.diagnostics); return; }
    auto request = ExistingRequest(project_.mapEdits_->Snapshot(), selectedRegion_, selectedCamera_);
    if (!request.Succeeded()) { Report(project_, selectionStatus_, request.diagnostics); return; }
    request.value->action = Services::AuxiliaryEditAction::Delete; request.value->record = std::monostate{};
    SubmitAuxiliary(std::move(*request.value), *binding.value, tr("删除区域或镜头"));
}

void WorldModule::DuplicateSelectedAuxiliary() {
    if (!SelectionEditable()) return;
    const auto binding = QtProject::CaptureAuxiliaryEditBinding(project_);
    if (!binding.Succeeded()) { Report(project_, selectionStatus_, binding.diagnostics); return; }
    const auto source = project_.mapEdits_->Snapshot();
    auto request = ExistingRequest(source, selectedRegion_, selectedCamera_);
    if (!request.Succeeded()) { Report(project_, selectionStatus_, request.diagnostics); return; }
    if (auto* region = std::get_if<MapData::RegionRecord>(&request.value->record)) {
        const auto number = NextRegionNumber(source);
        if (!number) { if (selectionStatus_) selectionStatus_->setText(tr("区域创建编号已用尽")); return; }
        region->name = UniqueName(region->name, source, request.value->kind, true); region->creationNumber = *number;
    } else {
        auto& camera = std::get<MapData::CameraRecord>(request.value->record);
        camera.name = UniqueName(camera.name, source, request.value->kind, true);
    }
    request.value->action = Services::AuxiliaryEditAction::Create;
    request.value->sourceIndex = 0; request.value->expectedRowKey.clear();
    SubmitAuxiliary(std::move(*request.value), *binding.value, tr("复制区域或镜头"));
}

void WorldModule::UpdateAuxiliaryActions() {
    if (!page_) return;
    const bool available = QtProject::CaptureAuxiliaryEditBinding(project_).Succeeded();
    const bool auxiliary = available && (selectedRegion_ || selectedCamera_) && SelectionEditable();
    for (const char* name : {"worldAuxiliaryProperties", "worldAuxiliaryInspectorProperties", "worldDeleteAuxiliary"})
        if (auto* button = page_->findChild<QPushButton*>(QLatin1String(name))) button->setEnabled(auxiliary);
    if (auto* button = page_->findChild<QPushButton*>(QStringLiteral("worldDuplicateAuxiliary")))
        button->setEnabled(available && SelectionEditable() && std::none_of(selectedPlacements_.begin(), selectedPlacements_.end(),
            [](const auto& id) { return id.file == MapData::PlacementFile::Units && id.rawcode == "sloc"; }));
    if (auto* button = page_->findChild<QPushButton*>(QStringLiteral("worldNewAuxiliary"))) button->setEnabled(available);
}
} // namespace War3Map::Modules
