#pragma once
#include "War3MapCore/Services/MapRuntimeCatalog.h"
#include "War3MapCore/Services/MapSaveService.h"

namespace War3Map::Services {
struct MapRuntimeDecision {
    MapRuntimeSettings settings;
    bool approveNativeRuntime = false;
};
struct MapRuntimeSaveOptions {
    std::optional<MapRuntimeDecision> decision;
    MapRuntimeSavePurpose purpose = MapRuntimeSavePurpose::Normal;
};
[[nodiscard]] std::optional<MapRuntimeSavePurpose> ParseMapRuntimeSavePurpose(std::string_view name);
[[nodiscard]] std::string_view MapRuntimeSavePurposeName(MapRuntimeSavePurpose purpose);
[[nodiscard]] Status ValidateMapRuntimeDecision(const MapRuntimeDecision& decision,
    std::span<const MapRuntimePackageSource> sources, std::span<const Diagnostic> catalogDiagnostics = {});
// 冻结源图已由宿主核对身份；设置/本次批准/用途在脚本准备前统一传入，不读取黑盒正文。
[[nodiscard]] Status PrepareMapRuntimeInputs(MapSaveTask& task, const MapRuntimeSaveOptions& options,
    std::span<const MapRuntimePackageSource> sources, std::span<const Diagnostic> catalogDiagnostics = {});
[[nodiscard]] Result<MapRuntimeSettings> ReadMapRuntimeSettings(const MapArchiveSession& archive);
struct MapRuntimeArchiveState {
    std::string sourceSha256;
    MapRuntimeSettings settings;
    std::vector<MapRuntimePackageInfo> packages;
};
[[nodiscard]] Result<MapRuntimeArchiveState> QueryMapRuntimeArchiveState(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, std::span<const MapRuntimePackageSource> sources);
[[nodiscard]] std::string SerializeMapRuntimeArchiveState(const MapRuntimeArchiveState& state);
} // namespace War3Map::Services
