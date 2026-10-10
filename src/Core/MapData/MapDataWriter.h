#pragma once

#include "War3MapCore/Types.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::MapData::Detail {

struct WriteFailure {
    std::string code;
    std::string message;
};

// 经典地图成员共用的小端输出器。预算在写入前检查；异常由各格式入口转换为诊断。
class Writer {
public:
    explicit Writer(std::size_t maxBytes) : maxBytes_(maxBytes) {}

    void Byte(std::uint8_t value) {
        Reserve(1);
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void U32(std::uint32_t value) {
        Reserve(4);
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }

    void I32(std::int32_t value) { U32(std::bit_cast<std::uint32_t>(value)); }

    void F32(float value) {
        if (!std::isfinite(value)) throw WriteFailure{"MAP_WRITE_NONFINITE", "浮点字段不是有限数值"};
        U32(std::bit_cast<std::uint32_t>(value));
    }

    void Raw(std::string_view value, std::size_t expected) {
        if (value.size() != expected) throw WriteFailure{"MAP_WRITE_RAWCODE", "原始代码长度与格式不符"};
        Reserve(value.size());
        if (!value.empty()) {
            const auto* begin = reinterpret_cast<const std::byte*>(value.data());
            bytes_.insert(bytes_.end(), begin, begin + value.size());
        }
    }

    void Raw(std::span<const std::byte> value, std::size_t expected) {
        if (value.size() != expected) throw WriteFailure{"MAP_WRITE_RAWCODE", "原始代码长度与格式不符"};
        Reserve(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void String(std::string_view value, std::size_t maxStringBytes) {
        if (value.size() > maxStringBytes || value.find('\0') != std::string_view::npos)
            throw WriteFailure{"MAP_WRITE_STRING", "字符串超过预算或包含 NUL"};
        Reserve(value.size() + 1);
        if (!value.empty()) {
            const auto* begin = reinterpret_cast<const std::byte*>(value.data());
            bytes_.insert(bytes_.end(), begin, begin + value.size());
        }
        bytes_.push_back(std::byte{0});
    }

    void Count(std::size_t value, std::size_t maxCount) {
        if (value > maxCount || value > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()))
            throw WriteFailure{"MAP_WRITE_COUNT", "记录数量超过格式或解析预算"};
        I32(static_cast<std::int32_t>(value));
    }

    [[nodiscard]] std::vector<std::byte> Finish() && { return std::move(bytes_); }

private:
    void Reserve(std::size_t count) const {
        if (count > maxBytes_ - bytes_.size())
            throw WriteFailure{"MAP_WRITE_BUDGET", "地图成员超过输出字节预算"};
    }

    std::vector<std::byte> bytes_;
    std::size_t maxBytes_;
};

template <typename T>
Result<T> Failed(std::string_view member, const WriteFailure& error) {
    return Result<T>::Failure("MapData.Write", error.code,
        std::string(member) + "：" + error.message);
}

} // namespace War3Map::MapData::Detail
