#pragma once

#include "War3MapCore/Services/MapRuntimePackage.h"

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
};
[[nodiscard]] Result<std::vector<MapRuntimePackageInfo>> QueryMapRuntimePackages(
    std::span<const MapRuntimePackageSource> sources);

} // namespace War3Map::Services
