#include "War3MapCore/Services/LuaTriggerCandidate.h"

#include <charconv>
#include <new>

namespace War3Map::Services {
Result<LuaCandidateProfile> ParseLuaCandidateProfile(std::string_view text) {
    using ParseResult = Result<LuaCandidateProfile>;
    const auto fail = [](std::size_t line, const char* message) {
        return ParseResult::Failure("Script.LuaCandidate", "LUA_PROFILE_FORMAT",
            "目标声明第 " + std::to_string(line) + " 行：" + message);
    };
    if (text.size() > 1024 * 1024 || text.find('\0') != text.npos)
        return fail(0, "输入超过 1 MiB 或包含 NUL。");
    try {
        LuaCandidateProfile result;
        bool identity = false;
        std::size_t number = 0;
        while (!text.empty()) {
            ++number;
            const auto end = text.find('\n');
            auto line = text.substr(0, end);
            text = end == text.npos ? std::string_view{} : text.substr(end + 1);
            if (line.ends_with('\r')) line.remove_suffix(1);
            if (number == 1) {
                if (line != "forge-lua-target-v1") return fail(number, "schema 不匹配。");
                continue;
            }
            if (line.empty()) continue;
            std::vector<std::string> fields;
            for (;;) {
                const auto tab = line.find('\t');
                const auto field = line.substr(0, tab);
                if (field.empty() || field.size() > 256 || field.find_first_of("\r\n") != field.npos || fields.size() >= 35)
                    return fail(number, "字段为空、过长或含无效换行/列数。");
                fields.emplace_back(field);
                if (tab == line.npos) break;
                line.remove_prefix(tab + 1);
            }
            if (fields[0] == "identity" && fields.size() == 2 && !identity) {
                result.target.identity = fields[1]; identity = true;
            } else if (fields[0] == "type" && fields.size() == 3 && result.target.handleTypes.size() < 512) {
                result.target.handleTypes.emplace_back(fields[1], fields[2]);
            } else if (fields[0] == "api" && fields.size() >= 3 && result.target.apis.size() < 4096) {
                result.target.apis.push_back({fields[1], fields[2], {fields.begin() + 3, fields.end()}});
            } else if (fields[0] == "constant" && fields.size() == 3 && result.target.constants.size() < 4096) {
                result.target.constants.push_back({fields[1], fields[2]});
            } else if (fields[0] == "callback" && fields.size() == 3 && result.callbacks.size() < 1024) {
                StableId id = 0;
                const auto [last, error] = std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), id);
                if (error != std::errc{} || last != fields[2].data() + fields[2].size() || id == 0)
                    return fail(number, "回调触发器 ID 不是正的 uint64。");
                result.callbacks.push_back({fields[1], id});
            } else return fail(number, "未知/重复记录、列数错误或记录预算超限。");
        }
        if (!identity) return fail(number, "缺少目标 identity。");
        // 类型、父链、保留名、重复 API 等语义仍由同一个 Lua 编译器校验。
        return ParseResult::Success(std::move(result));
    } catch (const std::bad_alloc&) {
        return fail(0, "无法分配目标声明。");
    }
}
} // namespace War3Map::Services
