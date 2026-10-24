#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/NativeMinimapMarkers.h"

#include <memory>

namespace War3Map::MapData { class TerrainSavePlan; class MapDataEditSession; }

namespace War3Map::Services {

struct TerrainMinimapMarkerLimits {
    std::size_t maximumSourceBytes = 64u * 1024u * 1024u;
    std::size_t maximumFileBytes = 8u * 1024u * 1024u;
    std::size_t maximumWorkingBytes = 512u * 1024u * 1024u;
    std::size_t maximumUnits = 65536;
    MapData::MinimapMarkerLimits markers;
};

enum class TerrainMinimapMarkerSource { Missing, Map, Game };
struct TerrainMinimapMarkerDependency {
    std::string name;
    TerrainMinimapMarkerSource source = TerrainMinimapMarkerSource::Missing;
    // Game为实际archiveName；Map的整图SHA独立绑定，不把输出图SHA变化当成依赖正文变化。
    std::string gameContainer, sha256;
    std::size_t bytes = 0;
    [[nodiscard]] bool operator==(const TerrainMinimapMarkerDependency&) const = default;
};

// 正常保存的MMP生成者；只接实际归档和宿主已冻结的最终成员，不接caller资格开关。
// 与MapBLP分开：MMP消费单位/能力语义、XY/owner/顺序和真实TeamColor，不消费地形Z。
class TerrainMinimapMarkers {
public:
    [[nodiscard]] static Result<std::shared_ptr<const TerrainMinimapMarkers>> Capture(
        const MapArchiveSession& source, const std::filesystem::path& gameRoot,
        std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {},
        const TerrainMinimapMarkerLimits& limits = {});
    [[nodiscard]] const std::vector<std::byte>& Bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::span<const TerrainMinimapMarkerDependency> Dependencies() const noexcept { return inputs_; }
    [[nodiscard]] std::size_t MarkerCount() const noexcept { return markerCount_; }
    // 保存编排实现；仅将本owner实际生成的正文送入同修订地形计划，不接caller字节。
    [[nodiscard]] Status AdmitToPlan(MapData::TerrainSavePlan& plan,
        const MapData::MapDataEditSession& edits) const;
    // 先确认实际源，再从同一最终视图重建；安装的MMP必须逐字等于owner输出。
    [[nodiscard]] Status VerifyFrozenOutput(const MapArchiveSession& source,
        std::span<const MemberReplacement> replacements, std::span<const std::string> removals = {}) const;
    // 真实输出归档重新捕获所有依赖及游戏winner，MMP正文与本次生成结果均须一致。
    [[nodiscard]] Status VerifyReadback(const MapArchiveSession& saved) const;
private:
    TerrainMinimapMarkers() = default;
    std::string sourceSha256_;
    std::filesystem::path gameRoot_;
    TerrainMinimapMarkerLimits limits_;
    std::vector<TerrainMinimapMarkerDependency> inputs_;
    std::vector<std::byte> bytes_;
    std::size_t markerCount_ = 0;
};

} // namespace War3Map::Services
