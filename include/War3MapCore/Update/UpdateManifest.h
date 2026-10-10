#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <span>
#include <string_view>

namespace War3Map::Update {

inline constexpr std::size_t kMaxUpdateManifestBytes = 1024 * 1024;
inline constexpr std::size_t kMaxUpdateFiles = 4096;
inline constexpr std::uint64_t kMaxUpdatePackageBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kMaxUpdateFileBytes = 1024ULL * 1024 * 1024;
inline constexpr std::uint64_t kMaxUpdateExpandedBytes = 8ULL * 1024 * 1024 * 1024;

struct TrustedUpdateKey {
    std::array<std::byte, 32> bytes{};
    bool configured = false;
};

struct UpdateFileEntry {
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
};

struct UpdateManifest {
    std::string version;
    std::string minimumDirectUpgrade;
    std::string sourceCommit;
    std::string releaseNotes;
    std::string packageFilename;
    std::uint64_t packageBytes = 0;
    std::string packageSha256;
    std::vector<UpdateFileEntry> files;
};

class VerifiedUpdateManifest {
public:
    [[nodiscard]] const UpdateManifest& Manifest() const noexcept { return manifest_; }
    [[nodiscard]] const std::string& ManifestSha256() const noexcept { return manifestSha256_; }
    [[nodiscard]] const std::string& TrustedKeySha256() const noexcept { return trustedKeySha256_; }

private:
    VerifiedUpdateManifest(UpdateManifest manifest, std::string manifestSha256, std::string keySha256)
        : manifest_(std::move(manifest)), manifestSha256_(std::move(manifestSha256)),
          trustedKeySha256_(std::move(keySha256)) {}
    UpdateManifest manifest_;
    std::string manifestSha256_;
    std::string trustedKeySha256_;
    friend Result<VerifiedUpdateManifest> VerifyAndParseUpdateManifest(
        std::span<const std::byte>, std::span<const std::byte>, const TrustedUpdateKey&);
};

// 空字符串表示尚未配置；调用方只能传入构建期可信配置，不允许包自行提供公钥。
[[nodiscard]] Result<TrustedUpdateKey> ParseTrustedUpdateKeyHex(std::string_view hex);
// 未签名解析只提供数据检查，不产生安装授权。每个JSON对象拒绝重复解码后的key。
[[nodiscard]] Result<UpdateManifest> ParseUpdateManifest(std::span<const std::byte> rawBytes);
// Pure Ed25519验证原始完整字节，不转换BOM/换行或重序列化；sig必须恰好64字节。
[[nodiscard]] Result<VerifiedUpdateManifest> VerifyAndParseUpdateManifest(
    std::span<const std::byte> rawBytes, std::span<const std::byte> signature,
    const TrustedUpdateKey& key);

enum class UpdateChannel { Stable, Beta };
struct UpdateCheckResult {
    std::string currentVersion;
    std::string targetVersion;
    bool updateAvailable = false;
    bool directlyUpgradeable = false;
    std::string reason;
};
[[nodiscard]] Result<UpdateCheckResult> CheckUpdate(
    const VerifiedUpdateManifest& manifest, std::string_view currentVersion,
    UpdateChannel channel = UpdateChannel::Stable);

// 只接受明确的ASCII程序文件相对路径，返回原文；不接受Win32别名或隐式规范化。
[[nodiscard]] Result<std::string> ValidateUpdateRelativePath(std::string_view path);

} // namespace War3Map::Update
