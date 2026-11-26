#include "EditorProject.h"
#include "../QtCommon/ApplicationPaths.h"
#include "War3MapCore/Services/MapRuntimePreparation.h"

namespace War3Map::QtProject {
namespace {
using Settings = Services::MapRuntimeSettings;
constexpr EditorProject::OwnedMemberSpec kRuntimeSettings{Services::kMapRuntimeSettingsMember, 4096,
    "MapRuntime", "RUNTIME_SETTINGS_INVALID", "运行库设置"};
Result<Settings> ReadSettings(const EditorProject& project) {
    const auto bytes = project.ReadOwnedMapMember(kRuntimeSettings);
    if (!bytes.Succeeded()) return {{}, bytes.diagnostics};
    return *bytes.value ? Services::ParseMapRuntimeSettings(**bytes.value) : Result<Settings>::Success({});
}
}

Result<Services::MapRuntimeSettings> EditorProject::QueryMapRuntimeSettings() const { return ReadSettings(*this); }

Status EditorProject::ReloadInstalledMapRuntimeCatalog() {
    if (savingMap_) return Status::Failure("MapRuntime", "RUNTIME_CATALOG_BUSY", "保存期间不能重新载入运行库安装目录。");
    const auto loaded = Services::LoadInstalledMapRuntimeSources(
        std::filesystem::path(QtCommon::InstallationRoot().toStdWString()).lexically_normal());
    runtimeCatalogDiagnostics_ = loaded.diagnostics;
    runtimePackageSources_ = loaded.Succeeded() ? *loaded.value : std::vector<Services::MapRuntimePackageSource>{};
    return {runtimeCatalogDiagnostics_};
}

Result<EditorProject::MapRuntimeState> EditorProject::QueryMapRuntimeState() const {
    using Output = Result<MapRuntimeState>;
    if (!editTrackingReady_ || editTrackingFailed_ || sourceChangeSuspected_ || sourceChangeConfirmed_)
        return Output::Failure("MapRuntime", "RUNTIME_REVISION_STALE", "工程修订或来源尚未核实，不能返回可用于启用的旧状态。");
    if (!runtimeCatalogDiagnostics_.empty()) return {{},runtimeCatalogDiagnostics_};
    MapRuntimeState state; state.generation = editRevision_.Generation(); state.revision = editRevision_.Revision();
    const auto settings = ReadSettings(*this);
    if (!settings.Succeeded()) return {{},settings.diagnostics};
    const auto packages = Services::QueryMapRuntimePackages(runtimePackageSources_);
    if (!packages.Succeeded()) return {{},packages.diagnostics};
    state.settings = *settings.value; state.packages = *packages.value;
    state.approved = !state.settings.packageSha256.empty()
        && state.settings.mode != Services::MapRuntimeMode::Disabled
        && runtimeApprovedPackageSha256_ == state.settings.packageSha256;
    if (state.generation != editRevision_.Generation() || state.revision != editRevision_.Revision())
        return Output::Failure("MapRuntime", "RUNTIME_REVISION_STALE", "查询运行库时工程修订已变化。");
    return Output::Success(std::move(state));
}

Status EditorProject::PrepareMapRuntimeSettings(AssetState& state, const Settings& settings,
    std::uint64_t expectedGeneration, std::uint64_t expectedRevision) const {
    if (!editTrackingReady_ || editTrackingFailed_ || sourceChangeSuspected_ || sourceChangeConfirmed_
        || editRevision_.Generation() != expectedGeneration || editRevision_.Revision() != expectedRevision)
        return Status::Failure("MapRuntime","RUNTIME_REVISION_STALE","启用决定的工程修订已变化；未提交旧确认。");
    const auto previous = ReadSettings(*this);
    if (!previous.Succeeded()) return {previous.diagnostics};
    const auto bytes = Services::SerializeMapRuntimeSettings(settings);
    if (!bytes.Succeeded()) return {bytes.diagnostics};
    const auto validated = Services::ValidateMapRuntimeDecision({settings, settings.mode != Services::MapRuntimeMode::Disabled},
        runtimePackageSources_, runtimeCatalogDiagnostics_);
    if (!validated.Succeeded()) return validated;
    if (settings.mode != Services::MapRuntimeMode::Disabled) {
        const auto selection = Services::ValidateMapRuntimeSelection(settings.packageSha256,
            previous.value->packageSha256, runtimePackageSources_);
        if (!selection.Succeeded()) return selection;
    }
    auto candidate = state;
    if (*previous.value != settings) {
        std::vector<ImportPayload> payloads{{QString::fromUtf8(Services::kMapRuntimeSettingsMember),
            std::make_shared<const std::vector<std::byte>>(*bytes.value)}};
        const auto problem = PrepareImportPayloads(candidate,payloads,true);
        if (!problem.isEmpty()) return Status::Failure("MapRuntime","RUNTIME_SETTINGS_CONFLICT",problem.toStdString());
    }
    candidate.runtimeApprovedPackageSha256 = settings.mode == Services::MapRuntimeMode::Disabled ? std::string{} : settings.packageSha256;
    state = std::move(candidate);
    return Status::Ok();
}
} // namespace War3Map::QtProject
