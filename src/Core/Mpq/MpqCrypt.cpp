#include "MpqCrypt.h"

namespace War3Map::Mpq {

MpqCrypt::MpqCrypt() noexcept {
    std::uint32_t seed = 0x00100001;
    for (std::uint32_t index1 = 0; index1 < 0x100; ++index1) {
        std::uint32_t index2 = index1;
        for (int i = 0; i < 5; ++i, index2 += 0x100) {
            seed = (seed * 125 + 3) % 0x2AAAAB;
            const std::uint32_t temp1 = (seed & 0xFFFF) << 16;
            seed = (seed * 125 + 3) % 0x2AAAAB;
            const std::uint32_t temp2 = (seed & 0xFFFF);
            table_[index2] = temp1 | temp2;
        }
    }
}

const MpqCrypt& MpqCrypt::Instance() {
    static const MpqCrypt instance;
    return instance;
}

std::uint32_t MpqCrypt::HashString(
    std::string_view name, HashType type) const noexcept {
    std::uint32_t seed1 = 0x7FED7FED;
    std::uint32_t seed2 = 0xEEEEEEEE;
    const std::uint32_t base = static_cast<std::uint32_t>(type) << 8;
    for (const char rawChar : name) {
        auto ch = static_cast<unsigned char>(rawChar);
        // 归一：小写转大写，正斜杠转反斜杠（与 Storm_HashString 完全一致）。
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<unsigned char>(ch - 32);
        } else if (ch == '/') {
            ch = '\\';
        }
        seed1 = table_[base + ch] ^ (seed1 + seed2);
        seed2 = ch + seed1 + seed2 + (seed2 << 5) + 3;
    }
    return seed1;
}

void MpqCrypt::DecryptBlock(
    void* data, std::size_t byteLength, std::uint32_t key) const noexcept {
    auto* words = static_cast<std::uint32_t*>(data);
    const std::size_t count = byteLength / 4;
    std::uint32_t seed = 0xEEEEEEEE;
    for (std::size_t i = 0; i < count; ++i) {
        seed += table_[0x400 + (key & 0xFF)];
        const std::uint32_t plain = words[i] ^ (key + seed);
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        seed = plain + seed + (seed << 5) + 3;
        words[i] = plain;
    }
}

void MpqCrypt::EncryptBlock(
    void* data, std::size_t byteLength, std::uint32_t key) const noexcept {
    auto* words = static_cast<std::uint32_t*>(data);
    const std::size_t count = byteLength / 4;
    std::uint32_t seed = 0xEEEEEEEE;
    for (std::size_t i = 0; i < count; ++i) {
        seed += table_[0x400 + (key & 0xFF)];
        const std::uint32_t plain = words[i];
        words[i] = plain ^ (key + seed);
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        seed = plain + seed + (seed << 5) + 3;
    }
}

} // namespace War3Map::Mpq
