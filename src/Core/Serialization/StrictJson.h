#pragma once

#include "War3MapCore/Types.h"

#include <nlohmann/json.hpp>

#include <span>
#include <unordered_set>

namespace War3Map::Serialization {

struct StrictJsonLimits {
    std::size_t maxBytes = 1024 * 1024;
    std::size_t maxDepth = 24;
    std::size_t maxNodes = 65536;
    std::size_t maxStringBytes = 1024 * 1024;
    bool rejectFloatingPoint = false;
};

namespace Detail {

class StrictJsonSax final : public nlohmann::json_sax<nlohmann::json> {
public:
    explicit StrictJsonSax(StrictJsonLimits limits) : limits_(limits) {}
    bool null() override { return Node(); }
    bool boolean(bool) override { return Node(); }
    bool number_integer(number_integer_t) override { return Node(); }
    bool number_unsigned(number_unsigned_t) override { return Node(); }
    bool number_float(number_float_t, const string_t&) override {
        return !limits_.rejectFloatingPoint && Node();
    }
    bool string(string_t& value) override { return value.size() <= limits_.maxStringBytes && Node(); }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return Start(true); }
    bool key(string_t& value) override {
        return value.size() <= limits_.maxStringBytes && !frames_.empty() && frames_.back().object
            && frames_.back().keys.insert(value).second && Node();
    }
    bool end_object() override { return End(true); }
    bool start_array(std::size_t) override { return Start(false); }
    bool end_array() override { return End(false); }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }

private:
    struct Frame { bool object = false; std::unordered_set<std::string> keys; };
    bool Node() { return nodes_ < limits_.maxNodes && (++nodes_, true); }
    bool Start(bool object) {
        if (frames_.size() >= limits_.maxDepth || !Node()) return false;
        frames_.push_back(Frame{object, {}});
        return true;
    }
    bool End(bool object) {
        if (frames_.empty() || frames_.back().object != object) return false;
        frames_.pop_back();
        return true;
    }
    StrictJsonLimits limits_;
    std::size_t nodes_ = 0;
    std::vector<Frame> frames_;
};

} // namespace Detail

// 共用的有界标准JSON入口。SAX第一遍拒绝重复解码key/过深/过量，第二遍才构建DOM。
// nlohmann严格UTF-8/escape检查保持启用，不接受注释、BOM或尾部垃圾；不依赖Qt。
[[nodiscard]] inline Result<nlohmann::json> ParseStrictJson(
    std::span<const std::byte> bytes, StrictJsonLimits limits = {}) {
    using ParseResult = Result<nlohmann::json>;
    if (bytes.empty() || bytes.size() > limits.maxBytes || limits.maxDepth == 0 || limits.maxNodes == 0) {
        return ParseResult::Failure("Serialization.StrictJson", "STRICT_JSON_BUDGET",
            "JSON为空或超过允许的字节/结构容量。");
    }
    if (bytes.size() >= 3 && bytes[0] == std::byte{0xef} && bytes[1] == std::byte{0xbb}
        && bytes[2] == std::byte{0xbf}) return ParseResult::Failure(
            "Serialization.StrictJson", "STRICT_JSON_INVALID", "JSON必须是无BOM的完整UTF-8文本。");
    try {
        const auto* begin = reinterpret_cast<const char*>(bytes.data());
        const auto* end = begin + bytes.size();
        Detail::StrictJsonSax sax(limits);
        if (!nlohmann::json::sax_parse(begin, end, &sax, nlohmann::json::input_format_t::json, true)) {
            return ParseResult::Failure("Serialization.StrictJson", "STRICT_JSON_INVALID",
                "JSON语法、UTF-8、重复字段或结构容量不符合要求。");
        }
        return ParseResult::Success(nlohmann::json::parse(begin, end, nullptr, true, false));
    } catch (const std::exception&) {
        return ParseResult::Failure("Serialization.StrictJson", "STRICT_JSON_INVALID",
            "无法完整解析有界JSON，未返回部分数据。");
    }
}

} // namespace War3Map::Serialization
