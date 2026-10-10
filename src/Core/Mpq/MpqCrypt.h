#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace War3Map::Mpq {

// MPQ 经典加密表（0x500 个 DWORD）。使用 Blizzard 的确定性 PRNG 生成：
//   seed = 0x00100001; 对 index1∈[0,0x100)，index2 每次 +0x100 共 5 组：
//     seed = (seed*125 + 3) % 0x2AAAAB; temp1 = (seed & 0xFFFF) << 16;
//     seed = (seed*125 + 3) % 0x2AAAAB; temp2 = (seed & 0xFFFF);
//     table[index2] = temp1 | temp2;
// 首值 table[0] == 0x55C636E2（与 StormLib/w3x2lni/HiveWE 完全一致）。
class MpqCrypt {
public:
    // 哈希类型：0=哈希表桶偏移，1=名字哈希 A，2=名字哈希 B，3=文件加密 key。
    enum class HashType : std::uint32_t {
        TableOffset = 0,
        NameA = 1,
        NameB = 2,
        FileKey = 3,
    };

    // 进程内单例，首次访问时构建加密表。
    static const MpqCrypt& Instance();

    // 经典字符串哈希：大写归一 + '/'→'\'，逐字符混合。
    [[nodiscard]] std::uint32_t HashString(
        std::string_view name, HashType type) const noexcept;

    // 原地解密一段 DWORD 数据（长度按字节给出，仅处理完整的 4 字节块）。
    void DecryptBlock(void* data, std::size_t byteLength, std::uint32_t key) const noexcept;

    // 原地加密一段 DWORD 数据（DecryptBlock 的逆操作，种子用明文更新）。
    void EncryptBlock(void* data, std::size_t byteLength, std::uint32_t key) const noexcept;

    // 直接访问表项（供文件 key 计算等使用）。
    [[nodiscard]] std::uint32_t At(std::size_t index) const noexcept {
        return table_[index];
    }

private:
    MpqCrypt() noexcept;

    std::array<std::uint32_t, 0x500> table_{};
};

} // namespace War3Map::Mpq
