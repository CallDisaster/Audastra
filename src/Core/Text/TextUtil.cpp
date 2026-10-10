#include "War3MapCore/Text/TextUtil.h"

#include <cctype>

namespace War3Map::Text {
namespace {

template <typename Predicate>
std::string_view TrimBy(std::string_view text, Predicate space) noexcept {
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

} // namespace

std::string ObjectScriptName(std::string_view name) {
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    std::string result;
    result.reserve(name.size() + 1);
    for (const unsigned char c : name) {
        const bool keep = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        result += keep ? static_cast<char>(c) : '_';
    }
    if (!result.empty() && result.back() == '_') result += 'u';
    return result;
}

std::string_view TrimHorizontal(std::string_view text) noexcept {
    return TrimBy(text, [](char c) { return c == ' ' || c == '\t'; });
}

std::string_view TrimLineWhitespace(std::string_view text) noexcept {
    return TrimBy(text, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
}

std::string TrimLineWhitespaceCopy(std::string_view text) {
    return std::string(TrimLineWhitespace(text));
}

std::string_view TrimCWhitespace(std::string_view text) noexcept {
    return TrimBy(text, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
}

std::string TrimCWhitespaceCopy(std::string_view text) {
    return std::string(TrimCWhitespace(text));
}

std::string TrimAsciiControlSpaceCopy(std::string_view text) {
    return std::string(TrimBy(text, [](char c) { return static_cast<unsigned char>(c) <= 0x20; }));
}

std::string LowerAscii(std::string_view text) {
    std::string result(text);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}

std::string UpperAscii(std::string_view text) {
    std::string result(text);
    for (char& c : result) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return result;
}

std::string LowerCType(std::string_view text) {
    std::string result(text);
    for (char& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::string UpperCType(std::string_view text) {
    std::string result(text);
    for (char& c : result) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}

std::vector<std::string_view> SplitPlain(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    for (;;) {
        const auto end = text.find(separator);
        parts.push_back(text.substr(0, end));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return parts;
}

ProfileSections ParseProfile(std::string_view text) {
    ProfileSections result;
    std::string section;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = TrimCWhitespace(text.substr(0, newline));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line.empty() || line.starts_with("//") || line.front() == ';') continue;
        if (line.front() == '[') {
            const auto closing = line.find(']');
            section = closing == std::string_view::npos ? std::string{} : LowerAscii(TrimCWhitespace(line.substr(1, closing - 1)));
        } else if (const auto equals = line.find('='); equals != std::string_view::npos) {
            result[section][LowerAscii(TrimCWhitespace(line.substr(0, equals)))] = std::string(TrimCWhitespace(line.substr(equals + 1)));
        }
    }
    return result;
}

} // namespace War3Map::Text
