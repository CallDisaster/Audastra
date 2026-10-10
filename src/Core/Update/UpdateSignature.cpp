#include "War3MapCore/Update/UpdateManifest.h"
#include "War3MapCore/Hash.h"

#include <sodium.h>

namespace War3Map::Update {
namespace {

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

Result<TrustedUpdateKey> ParseTrustedUpdateKeyHex(std::string_view hex) {
    TrustedUpdateKey key;
    if (hex.empty()) return Result<TrustedUpdateKey>::Success(key);
    if (hex.size() != key.bytes.size() * 2) return Result<TrustedUpdateKey>::Failure(
        "Update", "UPDATE_TRUST_KEY_INVALID", "可信发布公钥必须是32字节的完整十六进制编码。");
    for (std::size_t i = 0; i < key.bytes.size(); ++i) {
        const auto high = Hex(hex[2 * i]), low = Hex(hex[2 * i + 1]);
        if (high < 0 || low < 0) return Result<TrustedUpdateKey>::Failure(
            "Update", "UPDATE_TRUST_KEY_INVALID", "可信发布公钥包含无效十六进制字符。");
        key.bytes[i] = static_cast<std::byte>(high * 16 + low);
    }
    key.configured = true;
    return Result<TrustedUpdateKey>::Success(key);
}

Result<VerifiedUpdateManifest> VerifyAndParseUpdateManifest(std::span<const std::byte> rawBytes,
    std::span<const std::byte> signature, const TrustedUpdateKey& key) {
    using VerifyResult = Result<VerifiedUpdateManifest>;
    if (!key.configured) return VerifyResult::Failure("Update", "UPDATE_TRUST_NOT_CONFIGURED",
        "尚未配置可信发布公钥，拒绝授权更新安装。");
    if (rawBytes.empty() || rawBytes.size() > kMaxUpdateManifestBytes) return VerifyResult::Failure(
        "Update", "UPDATE_MANIFEST_BUDGET", "更新清单为空或超过1MiB容量，拒绝验签。");
    if (signature.size() != crypto_sign_BYTES) return VerifyResult::Failure("Update", "UPDATE_SIGNATURE_MISSING_OR_INVALID",
        "更新清单必须附带恰好64字节的原始Ed25519签名。");
    static const int initialized = sodium_init();
    if (initialized < 0) return VerifyResult::Failure("Update", "UPDATE_SIGNATURE_PROVIDER_FAILED", "Ed25519验证库初始化失败。");
    if (crypto_sign_verify_detached(reinterpret_cast<const unsigned char*>(signature.data()),
        reinterpret_cast<const unsigned char*>(rawBytes.data()), static_cast<unsigned long long>(rawBytes.size()),
        reinterpret_cast<const unsigned char*>(key.bytes.data())) != 0) return VerifyResult::Failure(
            "Update", "UPDATE_SIGNATURE_INVALID", "更新清单签名与可信发布公钥不符，拒绝安装。");
    auto parsed = ParseUpdateManifest(rawBytes);
    if (!parsed.Succeeded()) return {std::nullopt, std::move(parsed.diagnostics)};
    auto hash = Sha256(rawBytes);
    if (!hash.Succeeded()) return {std::nullopt, std::move(hash.diagnostics)};
    auto keyHash = Sha256(key.bytes);
    if (!keyHash.Succeeded()) return {std::nullopt, std::move(keyHash.diagnostics)};
    return VerifyResult::Success(VerifiedUpdateManifest(std::move(*parsed.value),
        std::move(*hash.value), std::move(*keyHash.value)));
}

} // namespace War3Map::Update
