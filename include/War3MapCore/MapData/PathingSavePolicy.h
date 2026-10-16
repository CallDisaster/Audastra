#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace War3Map::MapData {

enum class PathingPreservationKind { PreserveExact, ExplicitRestrictionCandidate };

struct PathingPreservationBinding {
    // 归档 SHA 及代际由调用方从真实会话提供；本纯字节接口不能认证整个归档。
    std::string archiveSha256, terrainSha256, pathingSha256;
    std::uint64_t sessionIdentity = 0, revision = 0, terrainSourceRevision = 0;
    bool operator==(const PathingPreservationBinding&) const = default;
};

struct PathingPreservationLimits {
    // 输入累计字节与 W3E 解码载荷分别准入，不宣称是进程 RSS 上限。
    std::size_t maximumInputBytes = 64u * 1024u * 1024u;
    std::size_t maximumDecodedTerrainBytes = 64u * 1024u * 1024u;
    std::size_t maximumReportedCells = 64;
};

struct PathingPreservationCell {
    std::uint32_t x = 0, y = 0;
    std::uint8_t original = 0, current = 0, added = 0;
};

struct PathingPreservationBounds {
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // 半开通行格范围。
};

// 只读来源/掩码证明。成功仅表示逐格字节均有说明，绝不授予地图写出资格。
// 不写成员，不重建 WPM，不改变 MapDataEditSession 或 TerrainSavePlan 的保存门。
class PathingPreservationProof {
public:
    PathingPreservationProof(const PathingPreservationProof&) = default;
    PathingPreservationProof(PathingPreservationProof&&) noexcept = default;
    PathingPreservationProof& operator=(const PathingPreservationProof&) = default;
    PathingPreservationProof& operator=(PathingPreservationProof&&) noexcept = default;

    [[nodiscard]] static Result<PathingPreservationProof> Prepare(const PathingPreservationBinding& binding,
        std::span<const std::byte> originalTerrain, std::span<const std::byte> originalPathing,
        std::span<const std::byte> currentTerrain, std::span<const std::byte> currentPathing,
        std::span<const std::byte> restrictionDraft, const PathingPreservationLimits& limits = {});

    [[nodiscard]] PathingPreservationKind Kind() const noexcept { return kind_; }
    [[nodiscard]] const PathingPreservationBinding& Binding() const noexcept { return binding_; }
    [[nodiscard]] std::size_t ChangedCells() const noexcept { return changedCells_; }
    [[nodiscard]] const std::optional<PathingPreservationBounds>& Bounds() const noexcept { return bounds_; }
    [[nodiscard]] std::span<const PathingPreservationCell> ReportedCells() const noexcept { return cells_; }
    [[nodiscard]] const std::string& OutputPathingSha256() const noexcept { return outputPathingSha256_; }
    [[nodiscard]] const std::string& DeclarationSha256() const noexcept { return declarationSha256_; }
    [[nodiscard]] static constexpr std::uint32_t RuleVersion() noexcept { return 1; }
    [[nodiscard]] bool RequiresNativeVerification() const noexcept {
        return kind_ == PathingPreservationKind::ExplicitRestrictionCandidate;
    }

    // 只核该只读证明是否仍属于同一上下文及字节；相同字节不能复活旧会话修订。
    [[nodiscard]] Status VerifyCurrent(const PathingPreservationBinding& binding,
        std::span<const std::byte> currentTerrain, std::span<const std::byte> currentPathing,
        std::span<const std::byte> restrictionDraft) const;

private:
    PathingPreservationProof() = default;
    PathingPreservationBinding binding_;
    PathingPreservationKind kind_ = PathingPreservationKind::PreserveExact;
    std::string outputPathingSha256_, declarationSha256_;
    std::size_t terrainBytes_ = 0, pathingBytes_ = 0, declarationBytes_ = 0, changedCells_ = 0;
    std::optional<PathingPreservationBounds> bounds_;
    std::vector<PathingPreservationCell> cells_;
};

} // namespace War3Map::MapData
