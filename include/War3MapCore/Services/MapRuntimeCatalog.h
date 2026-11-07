#pragma once

#include "War3MapCore/Services/MapRuntimePackage.h"
namespace War3Map::Trigger { class UiCatalog; }

namespace War3Map::Services {

// 安装者维护的固定目录，不读取地图、工程或Agent给出的路径。
// 不存在表示未安装任何包；损坏、越界或歧义返回错误，不降级为空目录。
[[nodiscard]] Result<std::vector<MapRuntimePackageSource>> LoadInstalledMapRuntimeSources(
    const std::filesystem::path& installationRoot);

// 面向UI/CLI/Agent的只读信息，不暴露包路径、不授予本工程安装批准。
struct MapRuntimePackageInfo {
    std::string packageSha256, id, version, payloadSha256;
    bool internalOnly = true;
    bool available = false;
    std::vector<Diagnostic> diagnostics;
    std::string modulePayloadSha256;
    std::string clientSha256;
    bool current = false;
};
[[nodiscard]] std::string_view BundledMapRuntimeClientSha256();
[[nodiscard]] Result<std::vector<MapRuntimePackageInfo>> QueryMapRuntimePackages(
    std::span<const MapRuntimePackageSource> sources);

// 旧版只能保留地图已有的锁定身份；批准与来源核对仍由原有事务执行。
[[nodiscard]] Status ValidateMapRuntimeSelection(std::string_view selectedIdentity,
    std::string_view lockedIdentity, std::span<const MapRuntimePackageSource> sources);
[[nodiscard]] std::string MissingMapRuntimePackageMessage(std::string_view identity);

enum class MapRuntimeCompatibilityAction { None, SwitchRuntime, UpdateLibrary };
struct MapRuntimeCompatibility {
    std::string lockedVersion, libraryVersion, suggestedPackageSha256;
    MapRuntimeCompatibilityAction action = MapRuntimeCompatibilityAction::None;
    std::vector<Diagnostic> diagnostics;
};
// hashes 必须来自实际注入器消费的输入；此查询不更改锁定身份或授予批准。
[[nodiscard]] Result<MapRuntimeCompatibility> QueryMapRuntimeCompatibility(
    const MapRuntimePackage& locked, std::span<const std::string> injectedSourceSha256,
    std::span<const MapRuntimePackageSource> sources);
[[nodiscard]] Result<MapRuntimeCompatibility> QueryMapRuntimeLibraryCompatibility(
    const MapRuntimeSettings& settings, std::span<const MapRuntimePackageSource> sources,
    const std::filesystem::path& toolRoot, const std::filesystem::path& uiRoot,
    const std::filesystem::path& mapDirectory, const Trigger::UiCatalog& catalog);

} // namespace War3Map::Services
