#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace War3Map::Mpq {

// 经典 1.27a 旁路候选的显式资源合同。所有预算均在读取成员正文前验证。
inline constexpr std::uint64_t kArchiveCandidateMaxPrefixBytes = 16ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kArchiveCandidateMaxArchiveBytes = 256ull * 1024ull * 1024ull;
inline constexpr std::size_t kArchiveCandidateMaxMemberCount = 32768;
inline constexpr std::uint64_t kArchiveCandidateMaxMemberBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kArchiveCandidateMaxDecodedBytes = 128ull * 1024ull * 1024ull;

struct ArchiveMemberReplacement {
    std::vector<std::byte> data;
    std::string expectedOriginalSha256;
};

struct ArchiveCandidateRequest {
    std::filesystem::path sourceArchivePath;
    std::string expectedSourceSha256;
    std::string uiCatalogSha256;
    std::uint64_t transactionId = 0;
    std::filesystem::path outputArchivePath;
    std::filesystem::path manifestPath;
    ArchiveMemberReplacement wtg;
    ArchiveMemberReplacement wct;
    // 提交前无副作用重算外部 Catalog/事务状态；空回调一律拒绝。
    std::function<Status()> finalGuard;
};

struct ArchiveCandidateResult {
    std::filesystem::path outputArchivePath;
    std::filesystem::path manifestPath;
    std::string candidateSha256;
    std::string wtgSha256;
    std::string wctSha256;
};

// 从只读源归档产生旁路 MPQ 候选。该产物不是 World Editor 真保存，也未经运行时验收。
// 服务只允许成对替换 war3map.wtg/war3map.wct，并在发布前后执行完整成员与 SHA 门禁。
[[nodiscard]] Result<ArchiveCandidateResult> ExportArchiveCandidate(
    const ArchiveCandidateRequest& request);

// 暴露确定性临时路径仅用于调用方预检与 hermetic 测试；路径本身不是授权令牌。
[[nodiscard]] std::filesystem::path ArchiveCandidateTempPath(
    const ArchiveCandidateRequest& request);
[[nodiscard]] std::filesystem::path ArchiveCandidateManifestTempPath(
    const ArchiveCandidateRequest& request);

} // namespace War3Map::Mpq
