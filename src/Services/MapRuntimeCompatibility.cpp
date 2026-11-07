#include "War3MapCore/Services/MapRuntimeCatalog.h"
#include "MapRuntimeBuildIdentity.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include <algorithm>

namespace War3Map::Services {
std::string_view BundledMapRuntimeClientSha256() { return AUDASTRA_BUNDLED_RUNTIME_CLIENT_SHA256; }

std::string MissingMapRuntimePackageMessage(std::string_view identity) {
    // 此表仅用于诊断，不提供包来源、分发批准或成员所有权。
    std::string label(identity.substr(0, 12));
    if (identity == "4f7cd50b95973d7780b8d7762a0d3a803b6a37c2d3e8ac8a8914c2b879f11b91") label = "0.3.0";
    if (identity == "f023365ad3628989d0b7d5c42d3fd426b2961f047c6351411fa448bf49c5c00c") label = "0.4.0";
    return "本机缺少地图锁定的运行库 " + label + "；请使用带有这个包的安装目录。未自动改用其他包，地图未保存。";
}

Status ValidateMapRuntimeSelection(std::string_view selectedIdentity, std::string_view lockedIdentity,
    std::span<const MapRuntimePackageSource> sources) {
    const auto packages = QueryMapRuntimePackages(sources);
    if (!packages.Succeeded()) return {packages.diagnostics};
    for (const auto& package : *packages.value) if (package.packageSha256 == selectedIdentity) {
        if (!package.available) return {package.diagnostics};
        if (package.current || selectedIdentity == lockedIdentity) return Status::Ok();
        return Status::Failure("MapRuntime", "RUNTIME_LEGACY_SELECTION_REJECTED",
            "旧版运行库 " + package.version + " 只供已锁定该包的地图保留使用；请为新启用或升级选择当前运行库。");
    }
    return Status::Failure("MapRuntime", "RUNTIME_PACKAGE_MISSING", MissingMapRuntimePackageMessage(selectedIdentity));
}

Result<MapRuntimeCompatibility> QueryMapRuntimeCompatibility(const MapRuntimePackage& locked,
    std::span<const std::string> hashes, std::span<const MapRuntimePackageSource> sources) {
    MapRuntimeCompatibility result; result.lockedVersion = locked.Version();
    if (locked.VerifyScriptInputs(hashes).Succeeded()) {
        result.libraryVersion = locked.Version();
        return Result<MapRuntimeCompatibility>::Success(std::move(result));
    }
    const auto packages = QueryMapRuntimePackages(sources);
    if (!packages.Succeeded()) return {{}, packages.diagnostics};
    const MapRuntimePackageInfo* matched = nullptr;
    for (const auto& package : *packages.value) {
        if (!package.available || package.packageSha256 == locked.Identity()
            || std::count(hashes.begin(), hashes.end(), package.clientSha256) != 1) continue;
        if (matched) { matched = nullptr; break; }
        matched = &package;
    }
    if (matched) {
        result.libraryVersion = matched->version;
        if (matched->current && locked.ClientSha256() != BundledMapRuntimeClientSha256()) {
            result.action = MapRuntimeCompatibilityAction::SwitchRuntime;
            result.suggestedPackageSha256 = matched->packageSha256;
            result.diagnostics.push_back({DiagnosticSeverity::Error, "MapRuntime", "RUNTIME_UPGRADE_REQUIRED",
                "这张图的运行库是 " + locked.Version() + "，函数库 AudastraUI 已是 " + matched->version
                + "；请把这张图的运行库切换到 " + matched->version + " 并保存。建议操作：map_runtime_set，明确批准包 " + matched->packageSha256 + "。"});
            return Result<MapRuntimeCompatibility>::Success(std::move(result));
        }
    }
    result.action = MapRuntimeCompatibilityAction::UpdateLibrary;
    result.diagnostics.push_back({DiagnosticSeverity::Error, "MapRuntime", "RUNTIME_SCRIPT_INPUT_MISMATCH",
        "这张图的运行库是 " + locked.Version() + "，函数库 AudastraUI "
        + (matched ? "是 " + matched->version : "的输入身份未知或重复")
        + "；请在触发器的“函数库…”里更新为与地图运行库配套的 AudastraUI 后再保存。"});
    return Result<MapRuntimeCompatibility>::Success(std::move(result));
}

Result<MapRuntimeCompatibility> QueryMapRuntimeLibraryCompatibility(const MapRuntimeSettings& settings,
    std::span<const MapRuntimePackageSource> sources, const std::filesystem::path& toolRoot,
    const std::filesystem::path& uiRoot, const std::filesystem::path& mapDirectory, const Trigger::UiCatalog& catalog) {
    using Output = Result<MapRuntimeCompatibility>;
    if (settings.packageSha256.empty() || settings.mode == MapRuntimeMode::Disabled) return Output::Success({});
    const MapRuntimePackageSource* selected = nullptr;
    for (const auto& source : sources) if (source.manifestSha256 == settings.packageSha256) {
        if (selected) return Output::Failure("MapRuntime", "RUNTIME_PACKAGE_CONFLICT", "可信目录存在重复包身份。");
        selected = &source;
    }
    if (!selected) return Output::Failure("MapRuntime", "RUNTIME_PACKAGE_MISSING", MissingMapRuntimePackageMessage(settings.packageSha256));
    const auto locked = MapRuntimePackage::Capture(*selected, MapRuntimeSavePurpose::InternalValidation);
    if (!locked.Succeeded()) return {{}, locked.diagnostics};
    const auto capability = QueryScriptRebuildCapability(toolRoot, uiRoot);
    if (!capability.layout) return {{}, capability.diagnostics};
    auto profile = Script::MakeLegacyFrameworkProfile(*capability.layout, catalog,
        Trigger::UiCatalog::ResolveUiRoot(uiRoot), mapDirectory);
    if (!profile.Succeeded()) return {{}, profile.diagnostics};
    const auto framework = Script::CaptureScriptFrameworkInputs(std::move(*profile.value), *capability.layout, catalog);
    if (!framework.Succeeded()) return {{}, framework.diagnostics};
    std::vector<std::string> hashes;
    for (const auto& choice : framework.value->injection.choices)
        if (choice.key == "AudastraUIIsReady") hashes.push_back(framework.value->injection.files.at(choice.selectedFile).binding.sha256);
    return QueryMapRuntimeCompatibility(**locked.value, hashes, sources);
}
} // namespace War3Map::Services
