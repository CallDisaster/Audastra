#pragma once
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/WtgSource.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace War3Map::Trigger::Detail {
// WTG/WCT 共用的原始文本解码；继续保留原编码和原字节。
RawText DecodeSourceText(std::span<const std::byte> raw);
class SourceReadError final : public std::runtime_error {
public:
    SourceReadError(std::string diagnosticCode, std::size_t sourceOffset, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(diagnosticCode)), offset(sourceOffset) {}
    std::string code;
    std::size_t offset;
};
class FormatError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class MissingUiDefinitionError final : public std::runtime_error {
public:
    MissingUiDefinitionError(
        std::size_t offset,
        std::size_t endOffset,
        std::int32_t type,
        std::string rawName)
        : std::runtime_error(
            "UI Catalog 中不存在 " +
            std::string(ToString(UiDefinitionKindFromEcaType(type))) +
            " 原始函数 [" + rawName +
            "]；WTG 不记录参数数量，无法在缺少对应库时安全推断节点边界。"),
          offset_(offset),
          endOffset_(endOffset),
          rawName_(std::move(rawName)) {
    }

    [[nodiscard]] std::size_t Offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t EndOffset() const noexcept { return endOffset_; }
    [[nodiscard]] const std::string& RawName() const noexcept { return rawName_; }

private:
    std::size_t offset_ = 0;
    std::size_t endOffset_ = 0;
    std::string rawName_;
};

inline StableId MakeSourceId(std::uint8_t domain, std::size_t offset) {
    constexpr std::uint64_t kOffsetMask = 0x00ffffffffffffffULL;
    if (offset >= kOffsetMask) {
        throw FormatError("source offset exceeds stable id capacity");
    }
    return (static_cast<std::uint64_t>(domain) << 56U) |
        (static_cast<std::uint64_t>(offset) + 1U);
}

inline OriginSpan MakeOrigin(const char* source, std::size_t start, std::size_t end) {
    return OriginSpan{
        source,
        static_cast<std::uint64_t>(start),
        static_cast<std::uint64_t>(end - start),
        {},
    };
}

class Reader {
public:
    Reader(std::span<const std::byte> data, std::string source, const WtgSourceLimits* limits = nullptr)
        : data_(data), source_(std::move(source)), limits_(limits) {
    }

    [[nodiscard]] std::size_t Offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t Remaining() const noexcept { return data_.size() - offset_; }

    void Charge(std::size_t count, std::size_t size) {
        if (!limits_) return;
        if (decodedBytes_ > limits_->maxDecodedBytes || count > (limits_->maxDecodedBytes - decodedBytes_) / size)
            throw SourceReadError("WTG_SOURCE_MEMORY_LIMIT", offset_, "解码载荷超过预算");
        decodedBytes_ += count * size;
    }

    std::span<const std::byte> ReadExact(std::size_t size, std::string_view field) {
        if (size > Remaining()) {
            throw FormatError(source_ + "@" + std::to_string(offset_) + ": " +
                std::string(field) + " requires " + std::to_string(size) +
                " bytes; only " + std::to_string(Remaining()) + " remain");
        }
        const auto result = data_.subspan(offset_, size);
        offset_ += size;
        return result;
    }

    std::int32_t ReadI32(std::string_view field) {
        const auto bytes = ReadExact(4, field);
        const std::uint32_t value =
            std::to_integer<std::uint32_t>(bytes[0]) |
            (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
            (std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
            (std::to_integer<std::uint32_t>(bytes[3]) << 24U);
        return static_cast<std::int32_t>(value);
    }

    std::size_t ReadCount(std::string_view field, std::size_t minimumItemSize) {
        const std::int32_t signedValue = ReadI32(field);
        if (signedValue < 0) {
            throw FormatError(source_ + "@" + std::to_string(offset_ - 4) + ": " +
                std::string(field) + " is negative");
        }
        const std::size_t value = static_cast<std::size_t>(signedValue);
        if (limits_ && value > limits_->maxItems)
            throw SourceReadError("WTG_SOURCE_COUNT_LIMIT", offset_ - 4, "字段数量超过预算");
        if (minimumItemSize != 0 && value > Remaining() / minimumItemSize + 1) {
            throw FormatError(source_ + "@" + std::to_string(offset_ - 4) + ": " +
                std::string(field) + " is unreasonable: " + std::to_string(value));
        }
        return value;
    }

    RawText ReadCString(std::string_view field) {
        const auto begin = data_.begin() + static_cast<std::ptrdiff_t>(offset_);
        const auto end = std::find(begin, data_.end(), std::byte{0});
        if (end == data_.end()) {
            throw FormatError(source_ + "@" + std::to_string(offset_) + ": " +
                std::string(field) + " is missing its NUL terminator");
        }
        const std::size_t length = static_cast<std::size_t>(std::distance(begin, end));
        if (limits_ && length > limits_->maxStringBytes)
            throw SourceReadError("WTG_SOURCE_STRING_LIMIT", offset_, "原始字符串超过预算");
        Charge(length + 1, 8); // 原始字节、解码字符及临时转码的保守载荷预算
        const auto bytes = ReadExact(length, field);
        ReadExact(1, "NUL terminator");
        return DecodeSourceText(bytes);
    }

    void ExpectEof() const {
        if (Remaining() != 0) {
            throw FormatError(source_ + "@" + std::to_string(offset_) + ": " +
                std::to_string(Remaining()) + " trailing bytes remain");
        }
    }

private:
    std::span<const std::byte> data_;
    std::string source_;
    std::size_t offset_ = 0;
    const WtgSourceLimits* limits_ = nullptr;
    std::size_t decodedBytes_ = 0;
};

} // namespace War3Map::Trigger::Detail
