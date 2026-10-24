#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapTerrain.h"

#include <memory>

namespace War3Map::Services {

struct TerrainShadowCasterLimits {
    std::size_t maximumSourceBytes = 64u * 1024u * 1024u;
    std::size_t maximumFileBytes = 32u * 1024u * 1024u;
    std::size_t maximumWorkingBytes = 512u * 1024u * 1024u;
    std::size_t maximumPlacements = 65536;
    std::size_t maximumModels = 4096;
};

enum class TerrainShadowCasterSource { Missing, Map, Game };
struct TerrainShadowCasterDependency {
    std::string name;
    TerrainShadowCasterSource source = TerrainShadowCasterSource::Missing;
    std::string gameContainer, sha256;
    std::size_t bytes = 0;
    [[nodiscard]] bool operator==(const TerrainShadowCasterDependency&) const = default;
};

struct TerrainShadowCasterModel {
    std::string path;
    // 每个实际路径只持有一份原字节；实例通过modelIndex共享。不提供预览替代模型。
    std::vector<std::byte> bytes;
    std::string sha256;
};
struct TerrainShadowCasterInstance {
    std::size_t sourceRow = 0, modelIndex = 0;
    MapData::DoodadPlacement placement;
    // 真实标准表的变换参数；不从显示transform反推。初始支持域不含倾斜、
    // 固定转角或漂浮，最终Z须等于同份实际普通无水地形的中心高度。
    float defaultScale = 0, maximumPitch = 0, maximumRoll = 0, fixedRotation = 0;
    bool floating = false;
    // 已核同一实际地形中心高与wire Z的普通非浮动/无倾斜域，行主序3x4。
    std::array<float, 12> nativeMatrix{};
};

// 捕获原版显式SHD计算的普通DOO模型集合：DestructableData存在优先排除，
// 剩余对象须在Doodads表中唯一存在，shadow非零才读取实际file/numVar模型。
// 初始支持域为标准对象；任何非空W3B/W3D先明确拒绝，不假装已解释覆盖。
// 模型只接受无扩展名file、numVar1..10及显式有效变体；不借预览路径的
// 去扩展名/十进制编号策略扩张原版单字符变体规则。
// 本owner不授予几何/姿势资格。调用者还须通过原生静态模型验证，不能
// 以成功读取MDX或UI模型预览代替阴影完整性。特殊DOO另供接收面owner验证。
class TerrainShadowCasterResources {
public:
    [[nodiscard]] static Result<std::shared_ptr<const TerrainShadowCasterResources>> Capture(
        const MapArchiveSession& source, const std::filesystem::path& gameRoot,
        std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {},
        const TerrainShadowCasterLimits& limits = {}, std::stop_token cancellation = {});
    [[nodiscard]] std::span<const TerrainShadowCasterInstance> Casters() const noexcept { return casters_; }
    [[nodiscard]] std::span<const TerrainShadowCasterModel> Models() const noexcept { return models_; }
    [[nodiscard]] const MapData::TerrainGridSnapshot& Terrain() const noexcept { return *terrain_; }
    [[nodiscard]] std::span<const MapData::SpecialDoodadPlacement> SpecialDoodads() const noexcept { return special_; }
    [[nodiscard]] std::span<const TerrainShadowCasterDependency> Dependencies() const noexcept { return inputs_; }
    [[nodiscard]] std::size_t DestructableCount() const noexcept { return destructableCount_; }
    [[nodiscard]] std::size_t NonShadowDoodadCount() const noexcept { return nonShadowCount_; }
    [[nodiscard]] std::size_t SourceBytes() const noexcept { return sourceBytes_; }
    [[nodiscard]] std::size_t WorkingBytes() const noexcept { return workingBytes_; }
    // 同一冻结最终成员视图重新捕获，核源归档、实际winner/absence与每份原字节。
    // 不接caller的“已验证”开关；真实I/O错误或来源变化保持失败。
    [[nodiscard]] Status VerifySources(const MapArchiveSession& source,
        std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {},
        std::stop_token cancellation = {}) const;
    // 保存结果从真实归档重新读取；允许归档SHA改变，所有被消费依赖须仍相同。
    [[nodiscard]] Status VerifyReadback(const MapArchiveSession& saved, std::stop_token cancellation = {}) const;

private:
    TerrainShadowCasterResources() = default;
    std::string sourceSha256_;
    std::filesystem::path gameRoot_;
    TerrainShadowCasterLimits limits_;
    std::vector<TerrainShadowCasterDependency> inputs_;
    std::vector<TerrainShadowCasterInstance> casters_;
    std::vector<TerrainShadowCasterModel> models_;
    std::vector<MapData::SpecialDoodadPlacement> special_;
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain_;
    std::size_t destructableCount_ = 0, nonShadowCount_ = 0, sourceBytes_ = 0, workingBytes_ = 0;
};

} // namespace War3Map::Services
