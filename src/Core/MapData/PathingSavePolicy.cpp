#include "War3MapCore/MapData/PathingSavePolicy.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapPathing.h"
#include "War3MapCore/MapData/PathingRestrictionEditing.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace War3Map::MapData {
namespace {

constexpr std::size_t kPathingHeaderBytes = 16;

Status Reject(const char* code, std::string message, std::optional<std::size_t> cellOffset = {}) {
    auto status = Status::Failure("Pathing.SavePolicy", code, std::move(message));
    if (cellOffset) status.diagnostics.front().origin = OriginSpan{"war3map.wpm", *cellOffset, 1, {}};
    return status;
}

struct Failure { std::vector<Diagnostic> diagnostics; };

void Require(const Status& status) {
    if (!status.Succeeded()) throw Failure{status.diagnostics};
}

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}

bool ValidSha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool Equal(std::span<const std::byte> left, std::span<const std::byte> right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end());
}

} // namespace

Result<PathingPreservationProof> PathingPreservationProof::Prepare(const PathingPreservationBinding& binding,
    std::span<const std::byte> originalTerrain, std::span<const std::byte> originalPathing,
    std::span<const std::byte> currentTerrain, std::span<const std::byte> currentPathing,
    std::span<const std::byte> restrictionDraft, const PathingPreservationLimits& limits) {
    try {
        const PathingPreservationLimits hard;
        if (limits.maximumInputBytes > hard.maximumInputBytes
            || limits.maximumDecodedTerrainBytes > hard.maximumDecodedTerrainBytes
            || limits.maximumReportedCells > hard.maximumReportedCells)
            Require(Reject("PATHING_POLICY_LIMITS", "通行保留证明只能收紧输入、解码和摘要预算。"));
        std::size_t used = 0;
        for (const auto bytes : {originalTerrain, originalPathing, currentTerrain, currentPathing, restrictionDraft}) {
            if (bytes.size() > limits.maximumInputBytes - used)
                Require(Reject("PATHING_POLICY_BUDGET", "通行保留证明超过累计输入字节预算。"));
            used += bytes.size();
        }
        if (!ValidSha(binding.archiveSha256) || !ValidSha(binding.terrainSha256) || !ValidSha(binding.pathingSha256)
            || binding.sessionIdentity == 0 || binding.terrainSourceRevision == 0)
            Require(Reject("PATHING_POLICY_BINDING", "通行保留证明需要完整来源 SHA 和有效会话/来源代际。"));
        if (Take(Sha256(originalTerrain)) != binding.terrainSha256 || Take(Sha256(originalPathing)) != binding.pathingSha256)
            Require(Reject("PATHING_POLICY_SOURCE_MISMATCH", "原 W3E/WPM 字节不属于给定来源身份。"));
        if (!Equal(originalTerrain, currentTerrain))
            Require(Reject("PATHING_POLICY_TERRAIN_CHANGED", "显式附加限制证明只接受 W3E 全部原始字节不变的候选。"));
        TerrainReadLimits terrainLimits;
        terrainLimits.maxBytes = limits.maximumInputBytes;
        terrainLimits.maxDecodedBytes = limits.maximumDecodedTerrainBytes;
        const auto terrain = Take(ParseMapTerrain(originalTerrain, terrainLimits));
        if (terrain.Snapshot()->header.version != 11)
            Require(Reject("PATHING_POLICY_TERRAIN_VERSION", "显式附加限制证明只接受经典 W3E v11。"));
        std::uint32_t width = 0;
        {
            // 复用唯一 WPM 解析器，释放读取快照后再核声明，避免同时保留多份格数组。
            PathingReadLimits pathingLimits;
            pathingLimits.maxBytes = (std::min)(limits.maximumInputBytes, pathingLimits.maxBytes);
            const auto source = Take(ParseMapPathing(originalPathing, &terrain.Snapshot()->header, pathingLimits));
            width = source.Snapshot()->width;
        }
        if (currentPathing.size() != originalPathing.size()
            || !Equal(originalPathing.first(kPathingHeaderBytes), currentPathing.first((std::min)(currentPathing.size(), kPathingHeaderBytes))))
            Require(Reject("PATHING_POLICY_SHAPE", "候选 WPM 的头、尺寸或长度与来源不同。"));

        PathingPreservationProof proof;
        proof.binding_ = binding;
        proof.terrainBytes_ = currentTerrain.size(); proof.pathingBytes_ = currentPathing.size();
        proof.declarationBytes_ = restrictionDraft.size();
        proof.cells_.reserve(limits.maximumReportedCells);
        for (std::size_t offset = kPathingHeaderBytes; offset < originalPathing.size(); ++offset) {
            const auto before = std::to_integer<std::uint8_t>(originalPathing[offset]);
            const auto after = std::to_integer<std::uint8_t>(currentPathing[offset]);
            if ((before & kPathingRestrictionBits & ~after) != 0)
                Require(Reject("PATHING_POLICY_SOURCE_RESTRICTION_REMOVED", "候选清除了来源原有的通行限制。", offset));
            if (((before ^ after) & ~kPathingRestrictionBits) != 0)
                Require(Reject("PATHING_POLICY_UNSUPPORTED_BITS", "候选改动了附加限制以外的原始位，包括荒芜或未知位。", offset));
            if (before == after) continue;
            ++proof.changedCells_;
            const auto x = static_cast<std::uint32_t>((offset - kPathingHeaderBytes) % width);
            const auto y = static_cast<std::uint32_t>((offset - kPathingHeaderBytes) / width);
            if (!proof.bounds_) proof.bounds_ = PathingPreservationBounds{x, y, x + 1, y + 1};
            else {
                auto& bounds = *proof.bounds_;
                bounds.x0 = (std::min)(bounds.x0, x); bounds.y0 = (std::min)(bounds.y0, y);
                bounds.x1 = (std::max)(bounds.x1, x + 1); bounds.y1 = (std::max)(bounds.y1, y + 1);
            }
            if (proof.cells_.size() < limits.maximumReportedCells)
                proof.cells_.push_back({x, y, before, after, static_cast<std::uint8_t>(after & ~before)});
        }
        if (!restrictionDraft.empty()) {
            if (proof.changedCells_ == 0 && (restrictionDraft.size() < 4 || restrictionDraft[3] != std::byte{2}))
                Require(Reject("PATHING_POLICY_REDUNDANT_DECLARATION", "WPM 与来源原字相同，不能夹带附加限制声明或伪造脏状态。"));
            // 声明规则只有现有 APR 编解码这一处；不复制它的格式、SHA 或掩码验证。
            const auto stripped = Take(StripPathingRestrictionDraft(*terrain.Snapshot(), originalPathing, currentPathing, restrictionDraft));
            if (!Equal(stripped, originalPathing))
                Require(Reject("PATHING_POLICY_UNEXPLAINED_DELTA", "撤回声明的附加限制后仍有未说明的 WPM 差分。"));
            if (proof.changedCells_) proof.kind_ = PathingPreservationKind::ExplicitRestrictionCandidate;
        } else if (proof.changedCells_)
            Require(Reject("PATHING_POLICY_DECLARATION_MISSING", "WPM 附加限制缺少完整的来源绑定声明。"));
        proof.outputPathingSha256_ = Take(Sha256(currentPathing));
        proof.declarationSha256_ = Take(Sha256(restrictionDraft));
        return Result<PathingPreservationProof>::Success(std::move(proof));
    } catch (Failure& failure) {
        return {{}, std::move(failure.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Result<PathingPreservationProof>::Failure("Pathing.SavePolicy", "PATHING_POLICY_ALLOCATION", "通行保留证明分配失败，未返回部分结果。");
    } catch (const std::length_error&) {
        return Result<PathingPreservationProof>::Failure("Pathing.SavePolicy", "PATHING_POLICY_ALLOCATION", "通行保留证明超过容器容量，未返回部分结果。");
    }
}

Status PathingPreservationProof::VerifyCurrent(const PathingPreservationBinding& binding,
    std::span<const std::byte> currentTerrain, std::span<const std::byte> currentPathing,
    std::span<const std::byte> restrictionDraft) const {
    try {
        if (binding != binding_)
            return Reject("PATHING_POLICY_STALE_BINDING", "只读通行证明的来源、会话、编辑修订或来源代际已过期。");
        if (currentTerrain.size() != terrainBytes_ || currentPathing.size() != pathingBytes_ || restrictionDraft.size() != declarationBytes_)
            return Reject("PATHING_POLICY_STALE_OUTPUT", "只读通行证明的当前字节或声明长度已改变。");
        if (Take(Sha256(currentTerrain)) != binding_.terrainSha256 || Take(Sha256(currentPathing)) != outputPathingSha256_
            || Take(Sha256(restrictionDraft)) != declarationSha256_)
            return Reject("PATHING_POLICY_STALE_OUTPUT", "只读通行证明的当前 W3E/WPM 或声明字节已改变。");
        return Status::Ok();
    } catch (Failure& failure) {
        return {std::move(failure.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Reject("PATHING_POLICY_ALLOCATION", "无法核对只读通行证明，未授予任何保存资格。");
    } catch (const std::length_error&) {
        return Reject("PATHING_POLICY_ALLOCATION", "无法完整核对只读通行证明，未授予任何保存资格。");
    }
}

} // namespace War3Map::MapData
