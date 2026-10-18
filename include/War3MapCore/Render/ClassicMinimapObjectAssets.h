#pragma once

#include "War3MapCore/Render/ClassicMinimapCandidate.h"
#include "War3MapCore/Render/TerrainMinimapAssets.h"
#include "War3MapCore/MapData/MapPlacements.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace War3Map::Render {

struct ClassicMinimapObjectAssetLimits {
    // 只收紧：此账包含已有Terrain快照载荷和本次新增捕获/快照/解析/SHA预留。
    // 不含已存在的edits历史、MapArchiveSession/MPQ/Game挂载内部、allocator或进程RSS。
    std::size_t maxWorkingBytes = 128u * 1024u * 1024u;
    std::size_t maxObjectMemberBytes = 64u * 1024u;
    std::size_t maxCurrentSnapshotBytes = 256u * 1024u; // Snapshot(kind)还复制/解析W3I
    std::size_t maxCurrentTerrainBytes = 4u * 1024u * 1024u;
};

enum class ClassicMinimapCapturedObjectState { Missing, ParsedEmpty };

struct ClassicMinimapObjectMemberFacts {
    std::string memberName;
    ClassicMinimapCapturedObjectState state = ClassicMinimapCapturedObjectState::Missing;
    std::size_t sourceBytes = 0, currentBytes = 0;
    std::string sourceSha256, currentSha256;
    std::int32_t version = 0, subversion = 0;
    MapData::PlacementRecordLayout recordLayout = MapData::PlacementRecordLayout::Undetermined;
    std::int32_t specialVersion = 0;
    std::size_t ordinaryPlacements = 0, specialPlacements = 0;
    bool parserFullyConsumed = false;
    bool sourceAndCurrentBytesEqual = false;
};

struct ClassicMinimapObjectAssetFacts {
    static constexpr const char* capturePolicy = "source-capture-parsed-empty-v1/native=false";
    static constexpr std::size_t snapshotScratchBytesPerWireByte = 128;
    std::array<ClassicMinimapObjectMemberFacts, 2> members; // units、doodads
    std::size_t retainedTerrainCapacityBytes = 0;
    std::size_t estimatedExtraPeakBytes = 0, estimatedPeakBytes = 0;
    std::size_t retainedAuxiliaryCapacityBytes = 0;
    // 只对这次源DOO与当前edits相等作证明。输入Terrain的game资源未再捕获/解码。
    bool resourcesCurrentReverified = false;
    bool nativeEditorOrGameVerified = false;
    bool formalSaveAuthorized = false;
};

class ClassicMinimapObjectAssets;

// 调用者串行排除edits写入。仅接受既有真实Terrain快照；源/当前两DOO必须存在性和
// 字节逐项相同，且Missing或唯一有界parser完整证明零记录（含特殊装饰物）。
// 失败不move terrain；成功才转移其owner。0字节present成员不能冒充合法ParsedEmpty。
// 本接口未提供Current/FrozenOutput/Readback生命周期，只供一次只读冻结候选，不接保存。
[[nodiscard]] Result<ClassicMinimapObjectAssets> CaptureClassicMinimapObjectAssets(
    TerrainMinimapAssetSnapshot&& terrain, const MapArchiveSession& source,
    const MapData::MapDataEditSession& edits, const ClassicMinimapObjectAssetLimits& limits = {});

class ClassicMinimapObjectAssets {
public:
    ClassicMinimapObjectAssets(ClassicMinimapObjectAssets&& other) noexcept
        : terrain_(std::move(other.terrain_)), domain_(std::move(other.domain_)), facts_(std::move(other.facts_)),
          sourceMembers_(std::move(other.sourceMembers_)), identitySha256_(std::move(other.identitySha256_)),
          captured_(std::exchange(other.captured_, false)) {}
    ClassicMinimapObjectAssets& operator=(ClassicMinimapObjectAssets&& other) noexcept {
        if (this != &other) {
            terrain_ = std::move(other.terrain_); domain_ = std::move(other.domain_); facts_ = std::move(other.facts_);
            sourceMembers_ = std::move(other.sourceMembers_); identitySha256_ = std::move(other.identitySha256_);
            captured_ = std::exchange(other.captured_, false);
        }
        return *this;
    }
    ClassicMinimapObjectAssets(const ClassicMinimapObjectAssets&) = delete;
    ClassicMinimapObjectAssets& operator=(const ClassicMinimapObjectAssets&) = delete;

    [[nodiscard]] const TerrainMinimapAssetSnapshot& TerrainAssets() const noexcept { return terrain_; }
    // ParsedEmpty事实只能从本sealed对象取得；不转换为旧caller-declared Empty。
    [[nodiscard]] const std::array<ClassicMinimapObjectMemberFacts, 2>& CapturedMembers() const noexcept { return facts_.members; }
    [[nodiscard]] const ClassicMinimapObjectAssetFacts& Facts() const noexcept { return facts_; }
    [[nodiscard]] const std::string& IdentitySha256() const noexcept { return identitySha256_; }

private:
    explicit ClassicMinimapObjectAssets(TerrainMinimapAssetSnapshot&& terrain) : terrain_(std::move(terrain)) {}
    TerrainMinimapAssetSnapshot terrain_;
    ClassicMinimapObjectDomain domain_;
    ClassicMinimapObjectAssetFacts facts_;
    std::array<std::vector<std::byte>, 2> sourceMembers_;
    std::string identitySha256_;
    bool captured_ = true; // move后源对象不能再次消费捕获资格，不依赖容器的未指定状态。
    friend Result<ClassicMinimapObjectAssets> CaptureClassicMinimapObjectAssets(
        TerrainMinimapAssetSnapshot&&, const MapArchiveSession&, const MapData::MapDataEditSession&,
        const ClassicMinimapObjectAssetLimits&);
    friend Result<ClassicMinimapCandidate> BuildClassicMinimapCandidate(
        const ClassicMinimapObjectAssets&, const TerrainMinimapPostprocessSettings&,
        const ClassicMinimapCandidateLimits&);
};

} // namespace War3Map::Render
