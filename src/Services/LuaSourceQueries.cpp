#include "War3MapCore/Services/LuaTriggerCandidate.h"

#include <algorithm>
#include <new>

namespace War3Map::Services {
namespace {
Result<LuaTriggerSourcePage> Fail(std::string code, std::string message) {
    return Result<LuaTriggerSourcePage>::Failure("LuaCandidate", std::move(code), std::move(message));
}
}

Result<LuaTriggerSourcePage> QueryLuaTriggerSources(const LuaTriggerCandidate& candidate,
    std::string_view currentInputRevision, std::string_view expectedScriptSha256,
    std::size_t line, std::size_t offset, std::size_t limit) {
    const auto unit = candidate.Unit();
    if (!unit || candidate.Identity().revision != currentInputRevision || unit->scriptSha256 != expectedScriptSha256)
        return Fail("LUA_SOURCE_MAP_STALE", "来源查询必须同时绑定当前输入修订和实际报错脚本的 SHA。");
    if (!limit || limit > 128 || line > unit->text.size() + 1)
        return Fail("LUA_SOURCE_MAP_RANGE", "来源页必须为 1..128 项，行号不能超过实际脚本范围。");
    try {
        LuaTriggerSourcePage page;
        page.inputRevision = candidate.Identity().revision;
        page.scriptSha256 = unit->scriptSha256;
        page.sourceMapSha256 = unit->sourceMapSha256;
        page.line = line; page.offset = offset; page.totalSpans = unit->sourceSpans.size();
        if (line) {
            for (std::size_t current = 1; current < line; ++current) {
                const auto newline = unit->text.find('\n', page.lineByteOffset);
                if (newline == std::string::npos) return Fail("LUA_SOURCE_MAP_RANGE", "指定行不存在。");
                page.lineByteOffset = newline + 1;
            }
            auto end = unit->text.find('\n', page.lineByteOffset);
            if (end == std::string::npos) end = unit->text.size();
            if (end > page.lineByteOffset && unit->text[end - 1] == '\r') --end;
            page.lineByteLength = end - page.lineByteOffset;
        }
        const auto matches = [&](const Script::LuaSourceSpan& span) {
            if (!line) return true;
            const auto end = span.byteOffset + span.byteLength;
            if (!page.lineByteLength) return span.byteOffset <= page.lineByteOffset && page.lineByteOffset < end;
            return span.byteOffset < page.lineByteOffset + page.lineByteLength && page.lineByteOffset < end;
        };
        // 编译器最多 262144 范围/8 MiB；不依赖调用方提供正确的计数或遍历终点。
        for (const auto& span : unit->sourceSpans) {
            if (!matches(span)) continue;
            ++page.totalMatches;
            if (line && (!page.innermost || span.byteLength < page.innermost->byteLength)) page.innermost = span;
        }
        if (offset > page.totalMatches) return Fail("LUA_SOURCE_MAP_RANGE", "来源页起点超过匹配范围数。");
        page.spans.reserve(std::min(limit, page.totalMatches - offset));
        std::size_t index = 0;
        for (const auto& span : unit->sourceSpans) {
            if (!matches(span)) continue;
            if (index++ < offset) continue;
            if (page.spans.size() == limit) break;
            page.spans.push_back(span);
        }
        return Result<LuaTriggerSourcePage>::Success(std::move(page));
    } catch (const std::bad_alloc&) {
        return Fail("LUA_CANDIDATE_ALLOCATION", "无法分配来源查询结果。");
    }
}

} // namespace War3Map::Services
