#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace War3Map::Game {
namespace {



// 值两端成对的双引号是 INI 写法的一部分（WESTRING_X="--"），不属于文本。
std::string Unquote(std::string_view value) {
    value = Text::TrimLineWhitespace(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    return std::string(value);
}

} // namespace

std::size_t WorldEditStrings::AddText(std::string_view source, std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::size_t added = 0;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = text.find('\n', start);
        const auto line = Text::TrimLineWhitespace(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        start = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty() || line.front() == '[' || line.front() == ';' || line.starts_with("//")) continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const auto key = Text::TrimLineWhitespace(line.substr(0, equals));
        if (key.empty()) continue;
        if (AddEntry(key, Unquote(line.substr(equals + 1)))) ++added;
    }
    sources_.emplace_back(source);
    return added;
}

bool WorldEditStrings::AddEntry(std::string_view key, std::string value) {
    return entries_.try_emplace(Text::UpperCType(Text::TrimLineWhitespace(key)), std::move(value)).second;
}

const std::string* WorldEditStrings::Find(std::string_view key) const {
    const auto found = entries_.find(Text::UpperCType(Text::TrimLineWhitespace(key)));
    return found == entries_.end() ? nullptr : &found->second;
}

bool WorldEditStrings::IsKey(std::string_view text) noexcept {
    text = Text::TrimLineWhitespace(text);
    constexpr std::string_view prefix = "WESTRING_";
    if (text.size() <= prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::toupper(static_cast<unsigned char>(text[index])) != prefix[index]) return false;
    }
    return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(prefix.size()), text.end(),
        [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
}

std::string WorldEditStrings::Resolve(std::string_view text) const {
    std::string current(text);
    std::unordered_set<std::string> visited;
    while (IsKey(current) && visited.insert(Text::UpperCType(Text::TrimLineWhitespace(current))).second) {
        const std::string* found = Find(current);
        if (found == nullptr) break;
        current = *found;
    }
    // 链尾仍是找不到的键时，返回调用方给出的原始键，便于界面按原键提示。
    return IsKey(current) ? std::string(text) : current;
}

bool WorldEditStrings::CanResolve(std::string_view text) const {
    return !IsKey(Resolve(text));
}

} // namespace War3Map::Game
