#include "ScriptNames.h"

#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <array>
#include <cstdint>

// 名称转换按 WE / YDTrigger 的可观察输出行为独立实现（以参考脚本逐项核对）。
namespace War3Map::Script {
namespace {

bool AsciiAlnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string_view TrimTrailingSpaces(std::string_view value) {
    while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
    return value;
}

} // namespace

std::string ConvertObjectName(std::string_view name) {
    // 与触发器编辑层的重名校验共用同一规则。
    return Trigger::TriggerDirectoryEditor::ScriptName(name);
}

namespace detail {

std::string ConvertLocalName(std::string_view name) {
    static constexpr std::array<char, 62> kAlphabet = {
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
        'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
        'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z'};
    std::string result;
    // 非 ASCII 字节按有符号字节累加（与 YDTrigger 局部变量名的 32 位回绕一致），满 62 进位输出。
    std::uint32_t accumulator = 0;
    for (const char raw : TrimTrailingSpaces(name)) {
        const auto c = static_cast<unsigned char>(raw);
        if (AsciiAlnum(c) || c == '_') {
            result += raw;
            continue;
        }
        if (c == ' ') {
            result += '_';
            continue;
        }
        accumulator = accumulator * 256u + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<signed char>(raw)));
        while (accumulator > 62u) {
            result += kAlphabet[accumulator % 62u];
            accumulator /= 62u;
        }
    }
    if (accumulator != 0) result += kAlphabet[accumulator % 62u];
    return result;
}

bool IsJassIdentifier(std::string_view value) {
    if (value.empty() || value.back() == '_' || (value.front() >= '0' && value.front() <= '9')) return false;
    for (const unsigned char c : value) {
        if (!AsciiAlnum(c) && c != '_') return false;
    }
    return true;
}

std::string QuoteJassString(std::string_view value) {
    std::string output = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') output += '\\';
        output += c;
    }
    return output + '"';
}

// 与 WE 生成脚本一致：不去空白，只认双引号。右引号后缺逗号而紧跟其他文本时
// （"SCRIPT"显示名 写法），引号内容自成一列，脚本值不混入显示名。
std::vector<std::string> SplitDefineValue(std::string_view value) {
    std::vector<std::string> parts(1);
    bool quoted = false;
    bool closed = false;
    for (const char c : value) {
        if (c == '"') {
            if (quoted) closed = true;
            quoted = !quoted;
            continue;
        }
        if (c == ',' && !quoted) {
            parts.emplace_back();
            closed = false;
            continue;
        }
        if (closed && !quoted && c != ' ' && c != '\t') {
            parts.emplace_back();
            closed = false;
        }
        parts.back() += c;
    }
    return parts;
}

} // namespace detail
} // namespace War3Map::Script

namespace War3Map::Script {

// Jenkins lookup2（1996）；输入先把 a-z 转大写、'/' 转反斜杠，初值 0。按公开算法描述独立实现。
std::uint32_t StormStringHash(std::string_view bytes) {
    const auto mix = [](std::uint32_t& a, std::uint32_t& b, std::uint32_t& c) {
        a -= b; a -= c; a ^= c >> 13;
        b -= c; b -= a; b ^= a << 8;
        c -= a; c -= b; c ^= b >> 13;
        a -= b; a -= c; a ^= c >> 12;
        b -= c; b -= a; b ^= a << 16;
        c -= a; c -= b; c ^= b >> 5;
        a -= b; a -= c; a ^= c >> 3;
        b -= c; b -= a; b ^= a << 10;
        c -= a; c -= b; c ^= b >> 15;
    };
    const auto byteAt = [&](std::size_t index) -> std::uint32_t {
        auto c = static_cast<unsigned char>(bytes[index]);
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
        else if (c == '/') c = '\\';
        return c;
    };
    const auto word = [&](std::size_t index) {
        return byteAt(index) | byteAt(index + 1) << 8 | byteAt(index + 2) << 16 | byteAt(index + 3) << 24;
    };
    std::uint32_t a = 0x9e3779b9u;
    std::uint32_t b = 0x9e3779b9u;
    std::uint32_t c = 0;
    std::size_t at = 0;
    std::size_t remaining = bytes.size();
    while (remaining >= 12) {
        a += word(at);
        b += word(at + 4);
        c += word(at + 8);
        mix(a, b, c);
        at += 12;
        remaining -= 12;
    }
    c += static_cast<std::uint32_t>(bytes.size());
    // 尾部：c 的最低字节留给长度。
    for (std::size_t index = 0; index < remaining; ++index) {
        const std::uint32_t value = byteAt(at + index);
        if (index < 4) a += value << (8 * index);
        else if (index < 8) b += value << (8 * (index - 4));
        else c += value << (8 * (index - 7));
    }
    mix(a, b, c);
    return c;
}

} // namespace War3Map::Script
