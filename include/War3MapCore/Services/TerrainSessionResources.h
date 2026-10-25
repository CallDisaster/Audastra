#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"

namespace War3Map::Services {
namespace TerrainReplayDetail { struct ResourceSource; }

// 从真实来源和可信typed会话建立当前结构读取视图。原物理来源单独复核，
// 当前W3E/W3I/DOO绝不冒充原地图；不能由协议传入reader或最终字节。
class TerrainSessionResources {
public:
    [[nodiscard]] static Result<TerrainSessionResources> Capture(const WorkspaceSnapshot& source,
        const MapData::MapDataEditSession& session, const std::filesystem::path& gameRoot,
        std::size_t maximumBytes = 64u * 1024u * 1024u);
    [[nodiscard]] static Result<TerrainSessionResources> Capture(std::shared_ptr<const MapArchiveSession> source,
        std::shared_ptr<const MapData::MapDataEditSession> session, const std::filesystem::path& gameRoot,
        std::size_t maximumBytes = 64u * 1024u * 1024u, std::size_t additionalOwnedBytes = 0);
    // worker复用GUI已冻结会话；additionalOwnedBytes计入请求仍保留的深复制与目录清单。
    [[nodiscard]] static Result<TerrainSessionResources> Capture(const WorkspaceSnapshot& source,
        std::shared_ptr<const MapData::MapDataEditSession> session, const std::filesystem::path& gameRoot,
        std::size_t maximumBytes = 64u * 1024u * 1024u, std::size_t additionalOwnedBytes = 0);
    [[nodiscard]] static Result<TerrainSessionResources> Capture(std::shared_ptr<const MapArchiveSession> source,
        const MapData::MapDataEditSession& session, const std::filesystem::path& gameRoot,
        std::size_t maximumBytes = 64u * 1024u * 1024u);
    [[nodiscard]] Result<MapData::TerrainPathingResources> CaptureWater(bool enforceWaterLimits,
        std::size_t maximumBytes = 64u * 1024u * 1024u) const;
    [[nodiscard]] Result<MapData::TerrainPathingResources> CaptureWater(const MapData::TerrainGridSnapshot& after,
        bool enforceWaterLimits, std::size_t maximumBytes = 64u * 1024u * 1024u) const;
    [[nodiscard]] Result<MapData::TerrainPathingResources> CaptureNative(const MapData::TerrainGridSnapshot& after,
        const std::vector<std::byte>* currentDoodads = nullptr,
        std::size_t maximumBytes = 64u * 1024u * 1024u) const;
    [[nodiscard]] Status Verify(const MapData::TerrainPathingResources& resources,
        std::size_t maximumBytes = 64u * 1024u * 1024u) const;
    [[nodiscard]] std::size_t OwnedCapacityBytes() const noexcept;

private:
    struct State;
    using MemberReader = MapData::TerrainPathingResources::HeightSourceReader;
    explicit TerrainSessionResources(std::shared_ptr<const State> state) : state_(std::move(state)) {}
    [[nodiscard]] static Result<TerrainSessionResources> CaptureInputs(const MemberReader& reader,
        std::shared_ptr<const MapData::MapDataEditSession> session,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes,
        std::size_t additionalOwnedBytes = 0);
    std::shared_ptr<const State> state_;
    friend class TerrainPathingSave;
    friend struct TerrainReplayDetail::ResourceSource;
};

} // namespace War3Map::Services
