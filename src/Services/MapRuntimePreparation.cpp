#include "War3MapCore/Services/MapRuntimePreparation.h"
#include "War3MapCore/Resources/ImportTable.h"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace War3Map::Services {
std::optional<MapRuntimeSavePurpose> ParseMapRuntimeSavePurpose(std::string_view name) {
    if (name == "normal") return MapRuntimeSavePurpose::Normal;
    if (name == "internal_validation") return MapRuntimeSavePurpose::InternalValidation;
    if (name == "test_run") return MapRuntimeSavePurpose::TestRun;
    return {};
}
std::string_view MapRuntimeSavePurposeName(MapRuntimeSavePurpose purpose) {
    switch (purpose) {
    case MapRuntimeSavePurpose::Normal: return "normal";
    case MapRuntimeSavePurpose::InternalValidation: return "internal_validation";
    case MapRuntimeSavePurpose::TestRun: return "test_run";
    }
    return "invalid";
}
Status ValidateMapRuntimeDecision(const MapRuntimeDecision& decision, std::span<const MapRuntimePackageSource> sources,
    std::span<const Diagnostic> catalogDiagnostics) {
    const auto& settings = decision.settings;
    const auto encoded = SerializeMapRuntimeSettings(settings);
    if (!encoded.Succeeded()) return {encoded.diagnostics};
    if (settings.mode == MapRuntimeMode::Disabled) {
        if (decision.approveNativeRuntime || !settings.packageSha256.empty())
            return Status::Failure("MapRuntime", "RUNTIME_ARGUMENT_INVALID", "关闭运行库不能同时选择包或授予批准。");
        return Status::Ok();
    }
    if (!decision.approveNativeRuntime || settings.packageSha256.empty())
        return Status::Failure("MapRuntime", "RUNTIME_CONSENT_REQUIRED", "必须明确批准本图使用固定身份的原生运行库；地图声明不能自授权。");
    if (!catalogDiagnostics.empty()) return {std::vector<Diagnostic>(catalogDiagnostics.begin(), catalogDiagnostics.end())};
    const MapRuntimePackageSource* selected = nullptr;
    for (const auto& source : sources) if (source.manifestSha256 == settings.packageSha256) {
        if (selected) return Status::Failure("MapRuntime", "RUNTIME_PACKAGE_CONFLICT", "可信目录存在重复包身份。");
        selected = &source;
    }
    if (!selected) return Status::Failure("MapRuntime", "RUNTIME_PACKAGE_MISSING", "本机缺少所选可信包；不使用地图路径或最新版代替。");
    const auto captured = MapRuntimePackage::Capture(*selected, MapRuntimeSavePurpose::InternalValidation);
    return {captured.diagnostics};
}
Result<MapRuntimeSettings> ReadMapRuntimeSettings(const MapArchiveSession& archive) {
    ArchiveExactMemberLimits limits; limits.memberCount = 1; limits.memberBytes = limits.totalBytes = 4096;
    const auto captured = archive.CaptureExactMembers(std::vector<std::string>{std::string(kMapRuntimeSettingsMember)}, limits);
    if (!captured.Succeeded()) return {{}, captured.diagnostics};
    return captured.value->front().present ? ParseMapRuntimeSettings(captured.value->front().bytes)
        : Result<MapRuntimeSettings>::Success({});
}
Status PrepareMapRuntimeInputs(MapSaveTask& task, const MapRuntimeSaveOptions& options,
    std::span<const MapRuntimePackageSource> sources, std::span<const Diagnostic> catalogDiagnostics) {
    if (MapRuntimeSavePurposeName(options.purpose) == "invalid")
        return Status::Failure("MapRuntime", "RUNTIME_SAVE_PURPOSE_INVALID", "未知保存用途不能授予内部包使用资格。");
    if (!task.archive) return Status::Failure("MapRuntime", "RUNTIME_SOURCE_REQUIRED", "运行库决定须绑定已冻结的源地图。");
    std::optional<std::vector<std::byte>> encoded;
    if (options.decision) {
        const auto checked = ValidateMapRuntimeDecision(*options.decision, sources, catalogDiagnostics);
        if (!checked.Succeeded()) return checked;
        const auto before = ReadMapRuntimeSettings(*task.archive);
        if (!before.Succeeded()) return {before.diagnostics};
        const auto settingsKey = Resources::CanonicalImportPath(kMapRuntimeSettingsMember);
        for (const auto& item : task.replacements) {
            const auto key = Resources::CanonicalImportPath(item.name);
            if (!key.Succeeded() || key.value == settingsKey.value)
                return Status::Failure("MapRuntime", "RUNTIME_SETTINGS_CONFLICT", "其他保存领域已占用运行库设置成员。");
        }
        for (const auto& name : task.removals) {
            const auto key = Resources::CanonicalImportPath(name);
            if (!key.Succeeded() || key.value == settingsKey.value)
                return Status::Failure("MapRuntime", "RUNTIME_SETTINGS_CONFLICT", "其他保存领域正在移除运行库设置成员。");
        }
        if (*before.value != options.decision->settings) {
            encoded = *SerializeMapRuntimeSettings(options.decision->settings).value;
        }
    }
    task.runtimePurpose = options.purpose;
    task.testCopyOnly = options.purpose != MapRuntimeSavePurpose::Normal;
    task.runtimePackageSources.assign(sources.begin(), sources.end());
    task.runtimeCatalogDiagnostics.assign(catalogDiagnostics.begin(), catalogDiagnostics.end());
    task.runtimeApprovedPackageSha256 = options.decision && options.decision->settings.mode != MapRuntimeMode::Disabled
        ? options.decision->settings.packageSha256 : std::string{};
    if (encoded) task.replacements.push_back({std::string(kMapRuntimeSettingsMember), std::move(*encoded)});
    return Status::Ok();
}
Result<MapRuntimeArchiveState> QueryMapRuntimeArchiveState(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, std::span<const MapRuntimePackageSource> sources) {
    using Output = Result<MapRuntimeArchiveState>;
    const auto archive = MapArchiveSession::Open(source);
    if (!archive.Succeeded()) return {{}, archive.diagnostics};
    if (expectedSourceSha256 != archive.value->SourceSha256())
        return Output::Failure("MapRuntime", "RUNTIME_SOURCE_STALE", "源地图身份已变化；未返回可用于批准的旧状态。");
    const auto settings = ReadMapRuntimeSettings(*archive.value);
    if (!settings.Succeeded()) return {{}, settings.diagnostics};
    const auto packages = QueryMapRuntimePackages(sources);
    if (!packages.Succeeded()) return {{}, packages.diagnostics};
    const auto current = archive.value->VerifySourceUnchanged();
    if (!current.Succeeded()) return {{}, current.diagnostics};
    return Output::Success({archive.value->SourceSha256(), *settings.value, *packages.value});
}
std::string SerializeMapRuntimeArchiveState(const MapRuntimeArchiveState& state) {
    using Json = nlohmann::json;
    Json packages = Json::array();
    for (const auto& package : state.packages) {
        Json diagnostics = Json::array();
        for (const auto& diagnostic : package.diagnostics)
            diagnostics.push_back({{"code", diagnostic.code}, {"message", diagnostic.message}});
        packages.push_back({{"package_sha256", package.packageSha256}, {"id", package.id}, {"version", package.version},
            {"payload_sha256", package.payloadSha256}, {"internal_only", package.internalOnly},
            {"available", package.available}, {"diagnostics", diagnostics}});
    }
    return Json{{"ok", true}, {"source_sha256", state.sourceSha256}, {"approved", false},
        {"mode", state.settings.mode == MapRuntimeMode::Automatic ? "automatic" : state.settings.mode == MapRuntimeMode::Enabled ? "enabled" : "disabled"},
        {"package_sha256", state.settings.packageSha256}, {"packages", packages}}.dump();
}
} // namespace War3Map::Services
