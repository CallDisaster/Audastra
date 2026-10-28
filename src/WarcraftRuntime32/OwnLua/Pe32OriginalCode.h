#pragma once
#include <cstdint>
#include <span>
namespace Audastra::Runtime32::OwnLua {
// 调用方先锁定完整文件身份；按磁盘重定位表还原指定运行地址的原始代码。
bool Pe32OriginalCode(std::span<const unsigned char> file, std::uint32_t mappedBase,
    std::uint32_t rva, std::span<unsigned char> output) noexcept;
}
