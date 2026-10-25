#pragma once

#include "War3MapCore/MapData/TerrainShadowRebuild.h"
#include "War3MapCore/MapData/TerrainShadowResources.h"
#include "War3MapCore/Services/TerrainShadowCasterResources.h"

namespace War3Map::Services {

struct TerrainShadowReceiverLimits {
    std::size_t maximumSourceBytes = 64u * 1024u * 1024u;
    std::size_t maximumFileBytes = 32u * 1024u * 1024u;
    std::size_t maximumWorkingBytes = 256u * 1024u * 1024u;
    std::size_t maximumWork = 256u * 1024u * 1024u;
    std::size_t maximumRecords = 65536, maximumModels = 4096;
};

// 显式计算阴影的真实接收面。普通面仍走既有采样；普通异层悬崖和坡道使用
// 原GEOS/group次序、实际模型变体、坡道anchor及逐顶点起伏，不借显示mesh或旧SHD。
// 明确拒绝未闭合坡道、special DOO、未知模型控制器及复杂面上的投影DOO。
// 普通保存不捕获本对象，因此本能力边界不阻止普通保存地图。
class TerrainShadowReceiverResources {
public:
    [[nodiscard]] static Result<std::shared_ptr<const TerrainShadowReceiverResources>> Capture(
        const MapArchiveSession& source, const std::filesystem::path& gameRoot,
        std::shared_ptr<const TerrainShadowCasterResources> casters,
        const MapData::TerrainShadowResources& light,
        std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {},
        const TerrainShadowReceiverLimits& limits = {}, std::stop_token cancellation = {});
    [[nodiscard]] Result<MapData::TerrainShadowReceiverGrid> Sample(
        const MapData::TerrainShadowRebuildLimits& limits = {}, std::stop_token cancellation = {}) const;
    [[nodiscard]] Status VerifySources(const MapArchiveSession& source,
        std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {},
        const TerrainShadowCasterVerificationLimits& limits = {}) const;
    [[nodiscard]] Status VerifyReadback(const MapArchiveSession& saved,
        const TerrainShadowCasterVerificationLimits& limits = {}) const;
    [[nodiscard]] std::span<const TerrainShadowCasterDependency> Dependencies() const noexcept;
    [[nodiscard]] std::size_t RecordCount() const noexcept;
    // 仅本owner新增的资源/有效工作存储；借用caster/light由调用方原预算计入。
    [[nodiscard]] std::size_t SourceBytes() const noexcept;
    [[nodiscard]] std::size_t WorkingBytes() const noexcept;
    [[nodiscard]] std::size_t Work() const noexcept;
private:
    struct State;
    explicit TerrainShadowReceiverResources(std::shared_ptr<const State> state) : state_(std::move(state)) {}
    std::shared_ptr<const State> state_;
};

} // namespace War3Map::Services
