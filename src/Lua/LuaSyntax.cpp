#include "War3MapCore/Lua/LuaSyntax.h"
#include "LuaCompileInspection.h"

extern "C" {
#include "lua.h"
}

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>

namespace War3Map::Lua {
namespace {
enum class StopReason { None, Memory, Cancelled, Deadline };
struct Budget {
    std::size_t used = 0, peak = 0, limit = 0;
    std::stop_token cancel;
    std::chrono::steady_clock::time_point deadline;
    StopReason stopped = StopReason::None;
    bool Poll() noexcept {
        if (stopped != StopReason::None) return false;
        if (cancel.stop_requested()) stopped = StopReason::Cancelled;
        else if (std::chrono::steady_clock::now() >= deadline) stopped = StopReason::Deadline;
        return stopped == StopReason::None;
    }
};
struct alignas(std::max_align_t) Allocation { std::size_t bytes; };
void* Allocate(void* opaque, void* block, std::size_t, std::size_t newBytes) noexcept {
    auto& budget = *static_cast<Budget*>(opaque);
    auto* allocation = block ? static_cast<Allocation*>(block) - 1 : nullptr;
    const auto oldBytes = allocation ? allocation->bytes : 0;
    if (newBytes == 0) {
        std::free(allocation);
        budget.used -= std::min(oldBytes, budget.used);
        return nullptr;
    }
    if (newBytes > std::numeric_limits<std::size_t>::max() - sizeof(Allocation)) {
        budget.stopped = StopReason::Memory; return nullptr;
    }
    const auto requested = newBytes + sizeof(Allocation);
    // Lua 分配器契约：缩小内存不能失败；取消或配额不能破坏此契约。
    if (allocation && requested <= oldBytes) {
        auto* smaller = static_cast<Allocation*>(std::realloc(allocation, requested));
        if (!smaller) return block; // 原分配仍有效，按物理大小继续计费。
        smaller->bytes = requested;
        budget.used -= oldBytes - requested;
        return smaller + 1;
    }
    if (!budget.Poll()) return nullptr;
    const auto retained = budget.used - std::min(oldBytes, budget.used);
    if (requested > budget.limit || retained > budget.limit - requested) {
        budget.stopped = StopReason::Memory;
        return nullptr;
    }
    auto* replacement = static_cast<Allocation*>(std::realloc(allocation, requested));
    if (!replacement) { budget.stopped = StopReason::Memory; return nullptr; }
    replacement->bytes = requested;
    budget.used = retained + requested;
    budget.peak = std::max(budget.peak, budget.used);
    return replacement + 1;
}
struct Reader {
    std::string_view source;
    std::size_t offset = 0;
    Budget* budget = nullptr;
};
const char* Read(lua_State*, void* opaque, std::size_t* bytes) noexcept {
    auto& reader = *static_cast<Reader*>(opaque);
    if (!reader.budget->Poll() || reader.offset == reader.source.size()) { *bytes = 0; return nullptr; }
    *bytes = std::min<std::size_t>(4096, reader.source.size() - reader.offset);
    const auto* result = reader.source.data() + reader.offset;
    reader.offset += *bytes;
    return result;
}
void Stopped(SyntaxResult& result, StopReason reason) {
    result.completed = false;
    if (reason == StopReason::Cancelled) result.diagnostics.push_back({"LUA_CHECK_CANCELLED", "Lua 语法检查已取消。"});
    else if (reason == StopReason::Deadline) result.diagnostics.push_back({"LUA_CHECK_DEADLINE", "Lua 语法检查超过时间预算。"});
    else result.diagnostics.push_back({"LUA_CHECK_MEMORY", "Lua 语法检查超过内存预算或内存分配失败。"});
}
SyntaxDiagnostic SyntaxError(std::string_view raw, std::string_view name) {
    SyntaxDiagnostic diagnostic{"LUA_SYNTAX_ERROR", std::string(raw.substr(0, 8192)), 0};
    // 使用受控 @文件名；文件名可包含 Windows 冒号，只读取完整名字后的十进制行号。
    const auto prefix = std::string(name) + ":";
    if (raw.starts_with(prefix)) {
        std::size_t position = prefix.size();
        std::uint64_t line = 0;
        while (position < raw.size() && raw[position] >= '0' && raw[position] <= '9') {
            line = std::min<std::uint64_t>(std::numeric_limits<std::uint32_t>::max(), line * 10 + raw[position++] - '0');
        }
        if (position < raw.size() && raw[position] == ':' && position > prefix.size()) diagnostic.line = static_cast<std::uint32_t>(line);
    }
    return diagnostic;
}
}

SyntaxResult Detail::CheckSyntaxWithInspection(std::string_view source, std::string_view name,
    const SyntaxOptions& options, const CompiledInspection& inspection) {
    SyntaxResult result;
    try {
        if (source.size() > options.maxSourceBytes) {
            result.completed = false; result.diagnostics.push_back({"LUA_CHECK_SOURCE_BUDGET", "Lua 代码超过字节预算。"}); return result;
        }
        if (name.empty() || name.size() > 1024 || name.find_first_of("\r\n\0", 0, 3) != std::string_view::npos) {
            result.completed = false; result.diagnostics.push_back({"LUA_CHECK_SOURCE_NAME", "Lua 检查来源名称无效。"}); return result;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::time_point::max() - now);
        const auto duration = std::clamp(options.maxDuration, std::chrono::milliseconds::zero(), remaining);
        Budget budget{0, 0, options.maxMemoryBytes, options.cancel, std::min(options.deadline, now + duration)};
        if (!budget.Poll()) { Stopped(result, budget.stopped); return result; }
        // Lua 会截短长 chunk 名；使用固定内部名取准确行号，随后呈现真实来源。
        constexpr std::string_view chunkName = "Audastra.lua";
        constexpr const char* chunk = "@Audastra.lua";
        std::unique_ptr<lua_State, decltype(&lua_close)> state(lua_newstate(Allocate, &budget), lua_close);
        if (!state) {
            result.peakMemoryBytes = budget.peak;
            Stopped(result, budget.stopped == StopReason::None ? StopReason::Memory : budget.stopped);
            return result;
        }
        Reader reader{source, 0, &budget};
        const int status = lua_load(state.get(), Read, &reader, chunk, "t");
        result.peakMemoryBytes = budget.peak;
        (void)budget.Poll();
        if (budget.stopped != StopReason::None) Stopped(result, budget.stopped);
        else if (status == LUA_ERRMEM) Stopped(result, StopReason::Memory);
        else if (status != LUA_OK) {
            std::size_t count = 0;
            const char* message = lua_tolstring(state.get(), -1, &count);
            auto diagnostic = SyntaxError(message ? std::string_view(message, count) : std::string_view("Lua 文本编译失败。"), chunkName);
            if (diagnostic.message.starts_with(chunkName)) diagnostic.message.replace(0, chunkName.size(), name);
            result.diagnostics.push_back(std::move(diagnostic));
        }
        if (status == LUA_OK && budget.stopped == StopReason::None && inspection) inspection(state.get());
        // 不 lua_call / lua_pcall：编译结果随 state 销毁，没有运行阶段。
    } catch (const std::exception&) {
        result.completed = false;
        result.diagnostics.push_back({"LUA_CHECK_INTERNAL", "Lua 检查器未能完成，未执行代码。"});
    }
    return result;
}
SyntaxResult CheckSyntax(std::string_view source, std::string_view name, const SyntaxOptions& options) {
    return Detail::CheckSyntaxWithInspection(source, name, options, {});
}
} // namespace War3Map::Lua
