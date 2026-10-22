#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"
#include "War3MapCore/Workspace.h"

#include <functional>
#include <map>

namespace War3Map::MapData {

// 保存时实际读取的通行语义输入；只能由真实地图/游戏来源建立。
// 旧Capture证明等价修改；CaptureNative另捕获完整生成输入，仍由保存计划
// 比较旧WPM六位并保留0x11与显式工程意图，不从合成位猜作者来源。
class TerrainPathingResources {
public:
    [[nodiscard]] static bool Needed(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after);
    [[nodiscard]] static Result<TerrainPathingResources> Capture(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNative(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNative(const WorkspaceSnapshot& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    // 只允许本次捕获的旧/新W3E内容；输出不含头、源WPM及工程意图。
    [[nodiscard]] Result<std::vector<std::byte>> GenerateNative(const TerrainGridSnapshot& terrain,
        std::size_t maximumBytes) const;
    [[nodiscard]] bool EquivalentGround(std::string_view before, std::string_view after) const;
    // 无坡道顶点的原版 D=0/1/2 和严格覆水状态都必须相等；不使用显示网格高度。
    [[nodiscard]] bool EquivalentWater(const TerrainVertex& before, const TerrainVertex& after) const;
    [[nodiscard]] Status VerifyFrozenOutput(std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals) const;
    [[nodiscard]] Status VerifyReadback(const MapArchiveSession& saved, std::size_t maximumBytes) const;
    [[nodiscard]] Status VerifyReadback(const WorkspaceSnapshot& saved, std::size_t maximumBytes) const;
    [[nodiscard]] std::size_t SourceBytes() const noexcept { return sourceBytes_; }
    [[nodiscard]] std::size_t OwnedCapacityBytes() const noexcept;

private:
    TerrainPathingResources() = default;
    using MemberReader = std::function<Result<std::vector<ArchiveExactMember>>(
        std::span<const std::string>, const ArchiveExactMemberLimits&)>;
    [[nodiscard]] static Result<TerrainPathingResources> CaptureInputs(const MemberReader& reader,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, bool native);
    [[nodiscard]] Status VerifyReadbackMembers(const MemberReader& reader, std::size_t maximumBytes) const;
    [[nodiscard]] Status VerifyGameDependencies(std::size_t remainingBytes) const;
    struct Dependency {
        std::string name, sha256, gameArchive;
        std::size_t bytes = 0;
        bool mapPresent = false;
        bool mapOnly = false;
    };
    std::vector<Dependency> dependencies_;
    std::map<std::string, std::array<bool, 3>, std::less<>> ground_;
    std::filesystem::path gameRoot_;
    std::uint8_t tileset_ = 0;
    std::size_t sourceBytes_ = 0;
    std::size_t capturedMaximumBytes_ = 0;
    std::vector<TerrainNativeGroundRule> nativeGround_;
    std::optional<TerrainNativeWaterRules> waterRules_;
    std::optional<TerrainNativePlayableBounds> nativeBounds_;
    std::vector<std::byte> nativeDoodads_;
    std::array<std::string, 2> nativeTerrainSha_;
    std::array<std::size_t, 2> nativeTerrainBytes_{};
};

} // namespace War3Map::MapData
