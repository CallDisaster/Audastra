#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapTerrain.h"

#include <map>

namespace War3Map::MapData {

// 保存时实际读取的通行语义输入；只能由真实地图/游戏来源建立。
// 它仅证明等价修改能保留原 WPM，不授予清除静态对象或作者限制的能力。
class TerrainPathingResources {
public:
    [[nodiscard]] static bool Needed(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after);
    [[nodiscard]] static Result<TerrainPathingResources> Capture(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    [[nodiscard]] bool EquivalentGround(std::string_view before, std::string_view after) const;
    // 无坡道顶点的原版 D=0/1/2 和严格覆水状态都必须相等；不使用显示网格高度。
    [[nodiscard]] bool EquivalentWater(const TerrainVertex& before, const TerrainVertex& after) const;
    [[nodiscard]] Status VerifyFrozenOutput(std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals) const;
    [[nodiscard]] Status VerifyReadback(const MapArchiveSession& saved, std::size_t maximumBytes) const;
    [[nodiscard]] std::size_t SourceBytes() const noexcept { return sourceBytes_; }
    [[nodiscard]] std::size_t OwnedCapacityBytes() const noexcept;

private:
    TerrainPathingResources() = default;
    struct Dependency {
        std::string name, sha256, gameArchive;
        std::size_t bytes = 0;
        bool mapPresent = false;
    };
    std::vector<Dependency> dependencies_;
    std::map<std::string, std::array<bool, 3>, std::less<>> ground_;
    std::filesystem::path gameRoot_;
    std::uint8_t tileset_ = 0;
    std::size_t sourceBytes_ = 0;
    double waterOffset_ = 0, deepThreshold_ = 0;
    bool waterConfigured_ = false;
};

} // namespace War3Map::MapData
