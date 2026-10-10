// 发布者本机工具；不随便携应用发布，不生成或内置正式私钥。
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Update/UpdateManifest.h"

#include <Windows.h>
#include <sodium.h>

#include <array>
#include <iostream>
#include <map>

namespace {
struct SecretKey {
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> expanded{};
    ~SecretKey() { sodium_memzero(expanded.data(), expanded.size()); }
};
struct KeyInput {
    std::vector<std::byte> bytes;
    ~KeyInput() { if (!bytes.empty()) sodium_memzero(bytes.data(), bytes.size()); }
};
void Diagnostics(const std::vector<War3Map::Diagnostic>& values) {
    for (const auto& value : values) std::cerr << value.code << ": " << value.message << '\n';
}
int Fail(const char* reason) { std::cerr << "Update.Sign: " << reason << '\n'; return 1; }
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::map<std::wstring, std::filesystem::path> options;
        if (argc != 7) return Fail("需要 --manifest <json> --key-file <binary-seed32-or-key64> --signature-out <new-sig>。");
        for (int i = 1; i < argc; i += 2) {
            const std::wstring name = argv[i];
            if ((name != L"--manifest" && name != L"--key-file" && name != L"--signature-out")
                || options.contains(name) || !*argv[i + 1]) return Fail("未知、重复或缺值的参数。");
            options.emplace(name, argv[i + 1]);
        }
        auto manifest = War3Map::IO::ReadFileBytes(options.at(L"--manifest"), War3Map::Update::kMaxUpdateManifestBytes);
        if (!manifest.Succeeded()) { Diagnostics(manifest.diagnostics); return 1; }
        auto parsed = War3Map::Update::ParseUpdateManifest(*manifest.value);
        if (!parsed.Succeeded()) { Diagnostics(parsed.diagnostics); return 1; }
        auto input = War3Map::IO::ReadFileBytes(options.at(L"--key-file"), crypto_sign_SECRETKEYBYTES);
        if (!input.Succeeded()) return Fail("无法读取发布者本机二进制私钥文件。");
        KeyInput seed{std::move(*input.value)};
        if (seed.bytes.size() != crypto_sign_SEEDBYTES && seed.bytes.size() != crypto_sign_SECRETKEYBYTES)
            return Fail("私钥必须是32字节二进制种子或64字节libsodium Ed25519密钥；不接受文本。");
        if (sodium_init() < 0) return Fail("签名库初始化失败。");
        SecretKey key;
        std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> publicKey{};
        // 64字节形式也从其seed派生并交叉校验后半公钥，拒绝损坏的expanded key。
        if (crypto_sign_seed_keypair(publicKey.data(), key.expanded.data(),
                reinterpret_cast<const unsigned char*>(seed.bytes.data())) != 0) return Fail("无法派生Ed25519密钥。");
        if (seed.bytes.size() == crypto_sign_SECRETKEYBYTES && sodium_memcmp(seed.bytes.data(), key.expanded.data(), key.expanded.size()) != 0)
            return Fail("64字节私钥的种子与公钥身份不匹配。");
        std::array<unsigned char, crypto_sign_BYTES> signature{};
        unsigned long long signatureBytes = 0;
        if (crypto_sign_detached(signature.data(), &signatureBytes,
                reinterpret_cast<const unsigned char*>(manifest.value->data()), manifest.value->size(), key.expanded.data()) != 0
            || signatureBytes != signature.size()) return Fail("签名失败。");
        if (crypto_sign_verify_detached(signature.data(), reinterpret_cast<const unsigned char*>(manifest.value->data()),
                manifest.value->size(), publicKey.data()) != 0) return Fail("签名回验失败。");
        const HANDLE output = CreateFileW(options.at(L"--signature-out").c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return Fail("签名输出已存在或不可写；未覆盖原文件。");
        DWORD written = 0;
        const bool success = WriteFile(output, signature.data(), static_cast<DWORD>(signature.size()), &written, nullptr)
            && written == signature.size() && FlushFileBuffers(output);
        CloseHandle(output);
        if (!success) return Fail("签名输出未完整写入；保留失败文件供检查，请使用新输出路径重试。");
        constexpr char hex[] = "0123456789abcdef";
        std::string publicHex;
        for (const auto byte : publicKey) { publicHex.push_back(hex[byte >> 4]); publicHex.push_back(hex[byte & 15]); }
        std::cout << "Ed25519 signature written (64 bytes). Public key: " << publicHex << '\n';
        return 0;
    } catch (const std::exception&) {
        return Fail("签名操作未完成；未输出私钥内容。");
    }
}
