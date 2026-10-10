#include "War3MapCore/Services/CommandRegistry.h"

#include <Windows.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace War3Map::Services {
namespace {

constexpr std::size_t MaxTextBytes = 16384;
constexpr std::size_t MaxDetailBytes = 4096;

std::optional<std::wstring> Utf16(std::string_view text) {
    if (text.size() > MaxTextBytes) return std::nullopt;
    if (text.empty()) return std::wstring{};
    const int size = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (count <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), count) != count) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::wstring> Lower(std::string_view text) {
    const auto source = Utf16(text);
    if (!source || source->empty()) return source;
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source->data(),
        static_cast<int>(source->size()), nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0 || count > static_cast<int>(MaxTextBytes * 2)) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source->data(),
        static_cast<int>(source->size()), result.data(), count, nullptr, nullptr, 0) != count) {
        return std::nullopt;
    }
    return result;
}

// ECMAScript trim / \s：U+180E、U+0085 均不属于这里的空白。
bool Space(wchar_t value) {
    return (value >= 0x9 && value <= 0xD) || value == 0x20 || value == 0xA0 || value == 0x1680 ||
        (value >= 0x2000 && value <= 0x200A) || value == 0x2028 || value == 0x2029 ||
        value == 0x202F || value == 0x205F || value == 0x3000 || value == 0xFEFF;
}

std::optional<std::int64_t> Score(std::wstring_view query, std::wstring_view text) {
    std::int64_t total = 0;
    for (std::size_t begin = 0; begin < query.size();) {
        if (Space(query[begin])) { ++begin; continue; }
        std::size_t end = begin + 1;
        while (end < query.size() && !Space(query[end])) ++end;
        const auto token = query.substr(begin, end - begin);
        begin = end;
        const auto length = static_cast<std::int64_t>(text.size());
        if (text == token) { total += 1000; continue; }
        if (text.starts_with(token)) { total += 800 - length; continue; }
        const auto at = text.find(token);
        if (at != std::wstring_view::npos) {
            total += 600 - static_cast<std::int64_t>(at) * 4 - length;
            continue;
        }
        std::size_t from = 0, first = std::wstring_view::npos, last = 0;
        for (std::size_t index = 0; index < token.size();) {
            std::size_t width = 1;
            if (token[index] >= 0xD800 && token[index] <= 0xDBFF && index + 1 < token.size() &&
                token[index + 1] >= 0xDC00 && token[index + 1] <= 0xDFFF) width = 2;
            const auto found = text.find(token.substr(index, width), from);
            if (found == std::wstring_view::npos) return std::nullopt;
            if (first == std::wstring_view::npos) first = found;
            last = found;
            from = found + 1; // 原型 indexOf(ch, from) 使用 UTF16 索引并仅 +1。
            index += width;
        }
        total += 200 - (static_cast<std::int64_t>(last) - static_cast<std::int64_t>(first) -
            static_cast<std::int64_t>(token.size())) * 3 - static_cast<std::int64_t>(first);
    }
    return total;
}

bool ValidText(std::string_view text, std::size_t maxBytes) {
    return text.size() <= maxBytes && text.find('\0') == std::string_view::npos && Utf16(text).has_value();
}

CommandAvailability Availability(const CommandDefinition& definition) {
    try {
        auto result = definition.availability ? definition.availability() : CommandAvailability{};
        if (!ValidText(result.reason, MaxDetailBytes)) return {false, "命令可用状态说明无效"};
        return result;
    } catch (...) {
        return {false, "命令可用状态检查失败"};
    }
}

CommandQueryResult InvalidQuery(std::string error) {
    CommandQueryResult result;
    result.valid = false;
    result.error = std::move(error);
    return result;
}

} // namespace

std::optional<std::int64_t> FuzzyCommandScore(std::string_view query, std::string_view text) {
    if (query.size() > CommandRegistry::MaxQueryBytes || text.size() > MaxTextBytes) return std::nullopt;
    const auto loweredQuery = Lower(query), loweredText = Lower(text);
    if (!loweredQuery || !loweredText) return std::nullopt;
    return Score(*loweredQuery, *loweredText);
}

bool CommandRegistry::Register(CommandDefinition definition, std::string* error) {
    if (error) error->clear();
    const auto fail = [&](const char* reason) { if (error) *error = reason; return false; };
    if (commands_.size() >= MaxCommands) return fail("命令数量已达到上限");
    if (definition.id.empty() || definition.title.empty() || !definition.execute ||
        !ValidText(definition.id, 128) || !ValidText(definition.title, 1024) ||
        !ValidText(definition.group, 1024) || !ValidText(definition.shortcut, 128) ||
        !ValidText(definition.keywords, 4096)) return fail("命令元数据或执行函数无效");
    if (std::any_of(commands_.begin(), commands_.end(), [&](const auto& item) { return item->id == definition.id; })) {
        return fail("命令 ID 已注册");
    }
    commands_.push_back(std::make_shared<const CommandDefinition>(std::move(definition)));
    return true;
}

bool CommandRegistry::Remove(std::string_view id) {
    if (id.size() > 128) return false;
    const std::string stableId(id);
    const auto item = std::find_if(commands_.begin(), commands_.end(), [&](const auto& entry) { return entry->id == stableId; });
    if (item == commands_.end()) return false;
    commands_.erase(item);
    std::erase(recent_, stableId);
    return true;
}

void CommandRegistry::Clear() {
    commands_.clear();
    recent_.clear();
}

CommandQueryResult CommandRegistry::List(std::size_t offset, std::size_t limit, bool includeUnavailable) const {
    return Query({}, offset, limit, includeUnavailable, false);
}

CommandQueryResult CommandRegistry::Search(std::string_view query, std::size_t offset, std::size_t limit,
    bool includeUnavailable) const {
    return Query(query, offset, limit, includeUnavailable, true);
}

CommandQueryResult CommandRegistry::Query(std::string_view query, std::size_t offset, std::size_t limit,
    bool includeUnavailable, bool ranked) const {
    if (offset > MaxCommands || limit == 0 || limit > MaxPageSize || query.size() > MaxQueryBytes) {
        return InvalidQuery("命令查询参数超出范围");
    }
    const auto loweredQuery = Lower(query);
    if (!loweredQuery) return InvalidQuery("查询文本不是有效 UTF8 或小写转换失败");
    // 快照只复制最多4096个指针；回调可增删命令，不使当前遍历或处理器悬空。
    const auto commands = commands_;
    const auto recent = recent_;
    struct Match { std::shared_ptr<const CommandDefinition> definition; CommandAvailability availability; std::int64_t score; };
    std::vector<Match> matches;
    for (const auto& definition : commands) {
        std::int64_t score = 0;
        if (ranked) {
            const auto text = Lower(definition->title + " " + definition->group + " " + definition->keywords);
            if (!text) return InvalidQuery("命令检索文本小写转换失败");
            const auto fuzzy = Score(*loweredQuery, *text);
            if (!fuzzy) continue;
            score = *fuzzy;
            const auto found = std::find(recent.begin(), recent.end(), definition->id);
            if (found != recent.end()) score += 60 - static_cast<std::int64_t>(found - recent.begin()) * 4;
        }
        const auto availability = Availability(*definition);
        if (includeUnavailable || availability.available) matches.push_back({definition, availability, score});
    }
    if (ranked) std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    CommandQueryResult result;
    result.totalMatches = matches.size();
    for (std::size_t index = offset; index < matches.size() && result.items.size() < limit; ++index) {
        const auto& match = matches[index];
        result.items.push_back({*match.definition, match.availability, match.score});
    }
    result.hasMore = offset < matches.size() && result.items.size() < matches.size() - offset;
    return result;
}

CommandExecution CommandRegistry::Execute(std::string_view id) {
    if (id.empty() || !ValidText(id, 128)) return {CommandExecutionStatus::InvalidRequest, {}, "命令 ID 无效"};
    const std::string stableId(id);
    const auto found = std::find_if(commands_.begin(), commands_.end(), [&](const auto& entry) { return entry->id == stableId; });
    if (found == commands_.end()) return {CommandExecutionStatus::Unknown, stableId, "命令未注册"};
    const CommandDefinition definition = **found;
    const auto availability = Availability(definition);
    if (!availability.available) return {CommandExecutionStatus::Disabled, stableId, availability.reason};
    try {
        auto result = definition.execute();
        result.commandId = stableId;
        if (!ValidText(result.message, MaxDetailBytes)) return {CommandExecutionStatus::Failed, stableId, "命令结果说明无效"};
        if (result.Succeeded()) {
            std::erase(recent_, stableId);
            recent_.insert(recent_.begin(), stableId);
            if (recent_.size() > RecentLimit) recent_.resize(RecentLimit);
        }
        return result;
    } catch (...) {
        return {CommandExecutionStatus::Failed, stableId, "命令处理器抛出异常"};
    }
}

bool CommandRegistry::SetRecent(const std::vector<std::string>& ids, std::string* error) {
    if (error) error->clear();
    if (ids.size() > RecentLimit || std::any_of(ids.begin(), ids.end(), [](const auto& id) { return id.empty() || !ValidText(id, 128); })) {
        if (error) *error = "最近命令 ID 列表无效或超出上限";
        return false;
    }
    std::vector<std::string> unique;
    for (const auto& id : ids) if (std::find(unique.begin(), unique.end(), id) == unique.end()) unique.push_back(id);
    recent_ = std::move(unique);
    return true;
}

} // namespace War3Map::Services
