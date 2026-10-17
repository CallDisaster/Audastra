#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Render/TerrainMinimapCandidate.h"
#include "War3MapCore/Resources/TerrainTable.h"

namespace War3Map::Render {

struct TerrainMinimapAssetCaptureLimits {
    // 默认值也是硬上限，只能收紧。sourceBytes 计基线五成员、表和唯一 atlas 的实际读取，
    // 不计重复散列的整归档字节，也不计 GameDataSource 挂载的 MPQ/嵌套包缓冲。
    std::size_t maxSourceBytes = 64 * 1024 * 1024;
    std::size_t maxCurrentW3eBytes = 4 * 1024 * 1024;
    std::size_t maxTiles = 65536;
    std::size_t maxRetainedAtlasCapacityBytes = 64 * 1024 * 1024;
    Resources::TerrainTableLimits tables;
    TerrainAtlasLimits atlas;
};

struct TerrainMinimapAssetBinding {
    std::string archiveSha256, originalTerrainSha256, currentTerrainSha256;
    std::filesystem::path gameRoot;
    std::size_t originalTerrainBytes = 0;
    std::uint8_t mainTileset = 0; // 实际当前W3E，供无活跃edits的来源复核使用。
    std::uint64_t sessionIdentity = 0, revision = 0, terrainSourceRevision = 0;
    [[nodiscard]] bool operator==(const TerrainMinimapAssetBinding&) const = default;
};

enum class TerrainMinimapDependencyKind { Table, Atlas, Settings };

struct TerrainMinimapAssetDependency {
    TerrainMinimapDependencyKind kind = TerrainMinimapDependencyKind::Table;
    Resources::ResourceLocation winner;
    // 地图为真实 source SHA；游戏为 ReadFileBoundedWithSource 的实际 archiveName，
    // 其中嵌套 tileset 包包含父包/嵌套名/SHA。不能拿笼统 Game sourceId 代替。
    std::string containerIdentity, encodedSha256;
    std::size_t encodedBytes = 0;
    bool mapMemberPresent = false; // false 为本阶段准确批量捕获已证明 missing
    std::vector<TerrainMinimapMipFacts> decodedMips; // table为空；atlas来自本次唯一decoder
};

struct TerrainMinimapTableFacts {
    std::size_t dependencyIndex = 0, rows = 0;
    std::string idColumn, identitySha256;
};

struct TerrainMinimapGroundAsset {
    std::string rawcode, canonicalTexturePath;
    int tableRow = 0;
    std::size_t dependencyIndex = 0;
};

struct TerrainMinimapAssetCaptureFacts {
    // 成功固定3次：基线五成员、三表(显式crop同批带W3I)、唯一atlas；不按材质散列源图。
    std::size_t exactMemberCaptureBatches = 0, baselineSourceBytes = 0, mapDependencyBytes = 0, gameDependencyBytes = 0;
    std::size_t cumulativeSourceBytes = 0;
    std::size_t maximumMapBatchRetainedBytes = 0; // 本次phase编码字节size，不是容器capacity或完整内存峰值
    std::size_t uniqueAtlasCount = 0, retainedAtlasRgbaCapacityBytes = 0;
    // 新增 W3I 原字 capacity 与有界 Settings snapshot/解析的保守峰值；不是全进程峰值。
    std::size_t retainedW3iCapacityBytes = 0, estimatedSettingsWorkingBytes = 0;
    bool gameMounted = false;
    static constexpr const char* gameLookupPolicy = "GameDataSource-existing-priority-and-Mpq-FindBlock-v1";
    bool gameInternalInventoryIndependentlyVerified = false; // 未全量证明重复hash/locale等包内唯一性
    // 本单元不掌握 provider/MPQ/嵌套包完整容量，不声称全工作集128MiB或RSS帽。
    bool includesMountAndMpqMemory = false;
};

class TerrainMinimapAssetSnapshot;

// 调用者必须串行排除 edits 写入。只接真实 ArchiveAndGame 来源，不接任意SHA/atlas/callback。
// Prepare只复用来源基线匹配；其Result成功与SaveGate成功不同，本接口不执行或放宽保存。
[[nodiscard]] Result<TerrainMinimapAssetSnapshot> CaptureTerrainMinimapAssets(
    const MapArchiveSession& source, const MapData::MapDataEditSession& edits,
    const std::filesystem::path& gameRoot, const TerrainMinimapAssetCaptureLimits& limits = {},
    TerrainMinimapRangePolicy rangePolicy = TerrainMinimapRangePolicy::FullW3e);

class TerrainMinimapAssetSnapshot {
public:
    TerrainMinimapAssetSnapshot(TerrainMinimapAssetSnapshot&&) noexcept = default;
    TerrainMinimapAssetSnapshot& operator=(TerrainMinimapAssetSnapshot&&) noexcept = default;
    TerrainMinimapAssetSnapshot(const TerrainMinimapAssetSnapshot&) = delete;
    TerrainMinimapAssetSnapshot& operator=(const TerrainMinimapAssetSnapshot&) = delete;
    [[nodiscard]] const TerrainMinimapInput& Input() const noexcept { return input_; }
    [[nodiscard]] const TerrainMinimapAssetBinding& Binding() const noexcept { return binding_; }
    [[nodiscard]] const std::vector<TerrainMinimapAssetDependency>& Dependencies() const noexcept { return dependencies_; }
    [[nodiscard]] const std::vector<TerrainMinimapTableFacts>& Tables() const noexcept { return tables_; }
    [[nodiscard]] const std::vector<TerrainMinimapGroundAsset>& GroundAssets() const noexcept { return ground_; }
    [[nodiscard]] const TerrainMinimapAssetCaptureFacts& Facts() const noexcept { return facts_; }
    [[nodiscard]] const std::string& IdentitySha256() const noexcept { return identitySha256_; }
    static constexpr const char* sourcePolicy = "archive-and-game-capture-v3";
    static constexpr bool formalSaveAuthorized = false, nativeEditorOrGameVerified = false;
    static constexpr bool pendingImportReplaceRemoveCovered = false, w3iDooShdSemanticsCovered = false;
private:
    TerrainMinimapAssetSnapshot() = default;
    TerrainMinimapInput input_;
    TerrainMinimapAssetBinding binding_;
    std::vector<TerrainMinimapAssetDependency> dependencies_;
    std::vector<TerrainMinimapTableFacts> tables_;
    std::vector<TerrainMinimapGroundAsset> ground_;
    TerrainMinimapAssetCaptureFacts facts_;
    std::string identitySha256_;
    friend Result<TerrainMinimapAssetSnapshot> CaptureTerrainMinimapAssets(
        const MapArchiveSession&, const MapData::MapDataEditSession&, const std::filesystem::path&,
        const TerrainMinimapAssetCaptureLimits&, TerrainMinimapRangePolicy);
};

// 有界重新捕获并核对来源配置/修订和显式依赖；不以同像素或同W3E SHA复活旧会话。
// 临时来源guard在每次捕获返回时释放，不证明全核心MPQ字节/实体fileID或未读成员恒定。
// 复核仍要求调用者排除 edits 写入；没有长期来源租约或正式保存授权含义。
[[nodiscard]] Status VerifyTerrainMinimapAssetsCurrent(const TerrainMinimapAssetSnapshot& snapshot,
    const MapArchiveSession& source, const MapData::MapDataEditSession& edits,
    const std::filesystem::path& gameRoot, const TerrainMinimapAssetCaptureLimits& limits = {});

// 对保存任务实际最终列表核验所消费的W3E/Settings/Table/Atlas；不接caller填的SHA或来源。
// 准备线程先用VerifyCurrent核编辑代际；worker只消费owned snapshot和真实source/最终列表，
// 后续仍由原保存job的revision/Commit门检查代际。FullW3e不隐式增加W3I依赖。
// 预算累计当前W3E散列、源W3E/依赖编码重读和相关replacement散列，均在工作前预收；
// 无关成员只检查名字，不读正文。整源SHA、挂载/MPQ容量仍不在这个字节账内。
// 来源按W3E、三表(可选W3I)、atlas三批复核，不重解码，不证明完整game包内部唯一性。
// 成功只覆盖本次调用与给定实际列表；guard返回即释放，不授予长期租约、MapBLP或native资格。
[[nodiscard]] Status VerifyTerrainMinimapAssetsFrozenOutput(const TerrainMinimapAssetSnapshot& snapshot,
    const MapArchiveSession& source, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals = {}, const TerrainMinimapAssetCaptureLimits& limits = {});

// 回读真实输出归档的当前W3E与显式依赖；允许输出整档SHA不同于原图。
// map winner按saved身份核原字，game winner必须仍为map Missing并重读同一真实来源。
// FullW3e不请求W3I；同次guard/三批准确捕获只覆盖本次调用，不替代生成到发布的长期租约。
// 沿原64MiB预收current散列、输出W3E与依赖重读，不重解码/解析，不授予MapBLP/native资格。
[[nodiscard]] Status VerifyTerrainMinimapAssetsReadback(const TerrainMinimapAssetSnapshot& snapshot,
    const MapArchiveSession& saved, const TerrainMinimapAssetCaptureLimits& limits = {});

} // namespace War3Map::Render
