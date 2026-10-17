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
    std::uint64_t sessionIdentity = 0, revision = 0, terrainSourceRevision = 0;
    [[nodiscard]] bool operator==(const TerrainMinimapAssetBinding&) const = default;
};

enum class TerrainMinimapDependencyKind { Table, Atlas };

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
    // 成功固定3次：TerrainSavePlan基线五成员、三表批量、唯一atlas批量；不按材质逐次散列源图。
    std::size_t exactMemberCaptureBatches = 0, baselineSourceBytes = 0, mapDependencyBytes = 0, gameDependencyBytes = 0;
    std::size_t cumulativeSourceBytes = 0;
    std::size_t maximumMapBatchRetainedBytes = 0; // 本次phase编码字节size，不是容器capacity或完整内存峰值
    std::size_t uniqueAtlasCount = 0, retainedAtlasRgbaCapacityBytes = 0;
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
    const std::filesystem::path& gameRoot, const TerrainMinimapAssetCaptureLimits& limits = {});

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
    static constexpr const char* sourcePolicy = "archive-and-game-capture-v1";
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
        const TerrainMinimapAssetCaptureLimits&);
};

// 有界重新捕获并核对来源配置/修订和显式依赖；不以同像素或同W3E SHA复活旧会话。
// 临时来源guard在每次捕获返回时释放，不证明全核心MPQ字节/实体fileID或未读成员恒定。
// 复核仍要求调用者排除 edits 写入；没有长期来源租约或正式保存授权含义。
[[nodiscard]] Status VerifyTerrainMinimapAssetsCurrent(const TerrainMinimapAssetSnapshot& snapshot,
    const MapArchiveSession& source, const MapData::MapDataEditSession& edits,
    const std::filesystem::path& gameRoot, const TerrainMinimapAssetCaptureLimits& limits = {});

} // namespace War3Map::Render
