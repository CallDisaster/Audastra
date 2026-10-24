#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/TerrainShadowRebuild.h"
#include "War3MapCore/MapData/TerrainShadowResources.h"

#include <memory>

namespace War3Map::MapData { class MapDataEditSession; class TerrainSavePlan; }

namespace War3Map::Services {

class TerrainShadowCasterResources;
struct MapSaveTask;

struct TerrainShadowBakeLimits {
    std::size_t maximumSourceBytes = 128u * 1024u * 1024u;
    std::size_t maximumWorkingBytes = 512u * 1024u * 1024u;
    std::size_t maximumWork = 256u * 1024u * 1024u;
    MapData::TerrainShadowRebuildLimits terrain;
};

// 显式“计算阴影”的共用结果。实际方向、地形、完整对象集合和模型都由
// 本次来源读取，不接受外部“已验证”结论。普通保存继续保留上次的SHD。
class TerrainShadowBake {
public:
    [[nodiscard]] static Result<std::shared_ptr<const TerrainShadowBake>> Capture(
        const MapArchiveSession& source, const std::filesystem::path& gameRoot,
        std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {},
        const TerrainShadowBakeLimits& limits = {}, std::stop_token cancellation = {});
    [[nodiscard]] const std::vector<std::byte>& Bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint32_t Width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t Height() const noexcept { return height_; }
    [[nodiscard]] std::size_t CasterCount() const noexcept { return casterCount_; }
    [[nodiscard]] Status AdmitToPlan(MapData::TerrainSavePlan& plan,
        const MapData::MapDataEditSession& edits) const;
    [[nodiscard]] Status VerifyFrozenOutput(const MapArchiveSession& source,
        std::span<const MemberReplacement> replacements, std::span<const std::string> removals = {}) const;
    [[nodiscard]] Status VerifyReadback(const MapArchiveSession& saved) const;

private:
    TerrainShadowBake() = default;
    std::string sourceSha256_, terrainSha256_;
    std::vector<std::byte> bytes_;
    std::uint32_t width_ = 0, height_ = 0;
    std::size_t casterCount_ = 0;
    std::optional<MapData::TerrainShadowResources> light_;
    std::shared_ptr<const TerrainShadowCasterResources> casters_;
};

// 最终脚本/物编列表形成后调用；失败不自动保留，因为这是作者显式请求的计算。
// 普通保存不调用此入口，计算不支持不会阻止普通保存地图。
[[nodiscard]] Status PrepareTerrainShadowBake(MapSaveTask& task, const std::filesystem::path& gameRoot,
    const MapData::MapDataEditSession* edits, std::shared_ptr<const MapData::TerrainSavePlan>& plan);

} // namespace War3Map::Services
