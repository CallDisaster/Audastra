#include "EditorProject.h"

#include <algorithm>

namespace War3Map::QtProject {
namespace {
Result<Services::MinimapMarkerEdits> Read(const MapArchiveSession& archive, const AssetState& assets) {
    const auto key = EditorProject::AssetKey(QString::fromLatin1(Services::kMinimapMarkerProvenance));
    const auto found = std::find_if(assets.imports.begin(), assets.imports.end(), [&](const auto& item) {
        return EditorProject::AssetKey(item.archivePath) == key;
    });
    if (found != assets.imports.end() && (!found->ownedPayload || found->ownedPayload->empty()))
        return Result<Services::MinimapMarkerEdits>::Failure("Minimap.MarkerEdits", "MINIMAP_MARKER_PROVENANCE",
            "图标编辑数据未冻结。");
    return Services::MinimapMarkerEdits::Read(archive, found == assets.imports.end() ?
        std::span<const std::byte>{} : std::span<const std::byte>(*found->ownedPayload));
}
}

Result<Services::MinimapMarkerView> EditorProject::QueryMinimapMarkers() const {
    if (!archiveSession_ || sourceChangeSuspected_ || sourceChangeConfirmed_)
        return Result<Services::MinimapMarkerView>::Failure("Minimap.MarkerEdits", "MINIMAP_MARKER_STALE",
            "地图未打开或已变化，请重新打开。");
    auto state = Read(*archiveSession_, CaptureAssets());
    if (!state.Succeeded() || !state.value) return {{}, std::move(state.diagnostics)};
    return state.value->View();
}

Status EditorProject::EditMinimapMarkers(std::string_view revision, std::span<const Services::MinimapMarkerEdit> edits) {
    if (!archiveSession_ || sourceChangeSuspected_ || sourceChangeConfirmed_)
        return Status::Failure("Minimap.MarkerEdits", "MINIMAP_MARKER_STALE", "地图未打开或已变化，请重新打开。");
    auto state = Read(*archiveSession_, CaptureAssets());
    if (!state.Succeeded() || !state.value) return {std::move(state.diagnostics)};
    auto staged = state.value->Apply(revision, edits);
    if (!staged.Succeeded() || !staged.value) return {std::move(staged.diagnostics)};
    const auto import = QString::fromLatin1(Services::kMinimapMarkerProvenance);
    const auto key = AssetKey(import);
    const auto current = std::find_if(pendingImports_.begin(), pendingImports_.end(), [&](const auto& item) {
        return AssetKey(item.archivePath) == key;
    });
    if (current != pendingImports_.end() && current->ownedPayload && *current->ownedPayload == *staged.value)
        return Status::Ok();
    std::vector<ImportPayload> payloads{{import, std::make_shared<const std::vector<std::byte>>(std::move(*staged.value))}};
    const auto problem = AddImportPayloads(payloads, true, tr("编辑小地图图标"));
    return problem.isEmpty() ? Status::Ok() :
        Status::Failure("Minimap.MarkerEdits", "MINIMAP_MARKER_PUBLISH", problem.toStdString());
}
} // namespace War3Map::QtProject
