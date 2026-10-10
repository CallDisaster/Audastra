#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

namespace War3Map::Runtime::detail {

struct PeImport { std::string library, name; };
struct PeSection { std::uint32_t rva, size, raw, flags, virtualSize; };
class Pe32 {
public:
    explicit Pe32(const std::vector<std::byte>& bytes, std::stop_token cancel = {},
        std::chrono::steady_clock::time_point deadline = (std::chrono::steady_clock::time_point::max)());
    [[nodiscard]] std::uint8_t Byte(std::size_t at) const;
    [[nodiscard]] std::uint16_t Word(std::size_t at) const;
    [[nodiscard]] std::uint32_t Dword(std::size_t at) const;
    [[nodiscard]] std::size_t Raw(std::uint32_t rva, std::size_t count = 1) const;
    [[nodiscard]] std::uint32_t Rva(std::size_t raw) const;
    [[nodiscard]] bool Executable(std::uint32_t rva) const;
    [[nodiscard]] bool ImageMapped(std::uint32_t rva, std::size_t count = 1) const;
    [[nodiscard]] std::string String(std::uint32_t va, std::size_t limit = 2048) const;
    [[nodiscard]] bool IsImport(std::uint32_t va, std::string_view library, std::string_view name) const;
    [[nodiscard]] bool HasRelocation(std::size_t raw) const;
    std::uint32_t imageBase = 0;
    std::uint32_t imageSize = 0;
    std::vector<PeSection> sections;
    std::map<std::uint32_t, PeImport> imports;
    std::map<std::string, std::uint32_t> exports;
    std::set<std::uint32_t> relocations;
    std::span<const std::byte> bytes;
private:
    void Poll() const;
    std::stop_token cancel_;
    std::chrono::steady_clock::time_point deadline_;
    mutable std::size_t metadataBytes_ = 0;
};

// 原创有界 x86 长度解码，仅覆盖经典 PE32；不认识的编码拒绝，不猜指令边界。
struct Instruction {
    std::size_t raw = 0, size = 0, modrmAt = 0, immediateAt = 0;
    std::uint8_t opcode = 0, second = 0, modrm = 0;
    std::size_t immediateSize = 0;
    bool twoByte = false;
};
[[nodiscard]] Instruction Decode32(const Pe32& pe, std::size_t at);

} // namespace War3Map::Runtime::detail
