#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Lua {

// YDWE Lua 的公开扩展语法：代码中的 |AHhb| 等同于 JASS 的 'AHhb'（四字符按大端拼成整数）。
// 标准 Lua 5.3 中 | 是按位或，因此只在“| + 恰好 4 个非 | 非换行字节 + |”时识别为四字码；
// 字符串、长字符串和注释内的内容不变。
// 逐个报告代码里的四字码：onRawcode(片段起始偏移, 整数值)。翻译与位置查询共用这一遍扫描。
template<class OnRawcode>
void ForEachYdweRawcode(std::string_view text, OnRawcode&& onRawcode) {
    std::size_t at = 0;
    const auto longBracket = [&](std::size_t start) -> std::size_t {
        // start 指向 '['；返回等号层数 + 1，非长括号返回 0。
        std::size_t level = 0, cursor = start + 1;
        while (cursor < text.size() && text[cursor] == '=') { ++level; ++cursor; }
        return cursor < text.size() && text[cursor] == '[' ? level + 1 : 0;
    };
    const auto skipLong = [&](std::size_t start, std::size_t level) {
        // 跳过 [==[ ... ]==]；未闭合时跳到末尾，交给 Lua 报原始错误。
        std::string close = "]" + std::string(level - 1, '=') + "]";
        const auto end = text.find(close, start + level + 1);
        return end == std::string_view::npos ? text.size() : end + close.size();
    };
    while (at < text.size()) {
        const char ch = text[at];
        if (ch == '-' && at + 1 < text.size() && text[at + 1] == '-') {
            if (at + 2 < text.size() && text[at + 2] == '[') {
                if (const auto level = longBracket(at + 2)) { at = skipLong(at + 2, level); continue; }
            }
            const auto end = text.find_first_of("\r\n", at);
            at = end == std::string_view::npos ? text.size() : end;
            continue;
        }
        if (ch == '[') {
            if (const auto level = longBracket(at)) { at = skipLong(at, level); continue; }
            ++at; continue;
        }
        if (ch == '"' || ch == '\'') {
            ++at;
            while (at < text.size() && text[at] != ch && text[at] != '\n' && text[at] != '\r') {
                if (text[at] == '\\' && at + 1 < text.size()) {
                    // \z 跳过后续空白（可含换行）；其余转义只跳过一个字符，换行续行也算一个。
                    if (text[at + 1] == 'z') {
                        at += 2;
                        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at;
                        continue;
                    }
                    at += 2; continue;
                }
                ++at;
            }
            if (at < text.size() && text[at] == ch) ++at;
            continue;
        }
        if (ch == '|' && at + 5 < text.size() && text[at + 5] == '|') {
            bool valid = true;
            std::uint32_t value = 0;
            for (std::size_t index = 1; index <= 4; ++index) {
                const auto byte = static_cast<unsigned char>(text[at + index]);
                if (byte == '|' || byte == '\r' || byte == '\n') { valid = false; break; }
                value = (value << 8) | byte;
            }
            if (valid) {
                onRawcode(at, value);
                at += 6;
                continue;
            }
        }
        ++at;
    }
}

// 改写只把四字码片段换成十进制整数，不增删换行，行号保持不变。
// 编辑器语法检查、Lua 测试副本与自有运行时共用此实现；保存时写入地图的仍是作者原文。
// 返回 nullopt 表示没有四字码，调用方直接使用原文。
[[nodiscard]] inline std::optional<std::string> TranslateYdweRawcodes(std::string_view text) {
    std::string result;
    std::size_t copied = 0;
    bool changed = false;
    ForEachYdweRawcode(text, [&](std::size_t offset, std::uint32_t value) {
        result.append(text.substr(copied, offset - copied));
        result += std::to_string(value);
        copied = offset + 6;
        changed = true;
    });
    if (!changed) return std::nullopt;
    result.append(text.substr(copied));
    return result;
}

struct YdweRawcodeUse {
    std::size_t line = 1;
    std::string code;       // 竖线之间的四个字节
    std::uint32_t value = 0;
};

// 四字码出现的位置（行号从 1 起），最多列 limit 处；total 返回总数。
[[nodiscard]] inline std::vector<YdweRawcodeUse> FindYdweRawcodes(std::string_view text, std::size_t limit,
    std::size_t* total = nullptr) {
    std::vector<YdweRawcodeUse> uses;
    std::size_t count = 0, line = 1, counted = 0;
    ForEachYdweRawcode(text, [&](std::size_t offset, std::uint32_t value) {
        ++count;
        if (uses.size() >= limit) return;
        for (; counted < offset; ++counted) if (text[counted] == '\n') ++line;
        uses.push_back({line, std::string(text.substr(offset + 1, 4)), value});
    });
    if (total) *total = count;
    return uses;
}

} // namespace War3Map::Lua
