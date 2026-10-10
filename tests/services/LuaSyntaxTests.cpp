#include "War3MapCore/Lua/LuaSyntax.h"

#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <stop_token>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Code(const War3Map::Lua::SyntaxResult& result, std::string_view code) {
    return result.diagnostics.size() == 1 && result.diagnostics.front().code == code;
}
}
int main() {
    using namespace War3Map::Lua;
    try {
        const auto marker = std::filesystem::temp_directory_path()
            / ("Audastra-Lua-MustNotExecute-" + std::to_string(GetCurrentProcessId()) + ".txt");
        Require(!std::filesystem::exists(marker), "Marker already exists; refuse to overwrite");
        const auto code = "os.execute([[cmd /c echo executed > \"" + marker.string() + "\"]])\n"
            "error('compile must not execute')\nwhile true do end\n";
        const auto started = std::chrono::steady_clock::now();
        auto safe = CheckSyntax(code, "compile-only.lua");
        Require(safe.Succeeded(), "Valid dangerous-looking source must compile without execution");
        Require(!std::filesystem::exists(marker), "Lua checker executed os.execute");
        Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "Checker executed infinite loop or stalled");
        Require(safe.peakMemoryBytes > 0 && safe.peakMemoryBytes <= SyntaxOptions{}.maxMemoryBytes, "Physical allocator peak missing");
        Require(CheckSyntax("local x = 7 // 2\nx = (x & 3) << 1\nreturn x\n", "lua53.lua").Succeeded(), "Lua 5.3 operators unsupported");
        const std::string longName = "D:/" + std::string(160, 'a') + "/source.lua";
        const auto bad = CheckSyntax("local x = 1\nlocal y = 2\nlocal = 3\n", longName);
        Require(bad.completed && Code(bad, "LUA_SYNTAX_ERROR") && bad.diagnostics.front().line == 3, "Long source name lost exact syntax line");
        Require(bad.diagnostics.front().message.starts_with(longName), "Diagnostic must show actual source name");
        const auto binary = CheckSyntax(std::string("\x1bLua\0", 5), "binary.lua");
        Require(binary.completed && Code(binary, "LUA_SYNTAX_ERROR"), "Text checker accepted bytecode");
        SyntaxOptions small; small.maxSourceBytes = 3;
        auto bounded = CheckSyntax("return 1", "bytes.lua", small);
        Require(!bounded.completed && Code(bounded, "LUA_CHECK_SOURCE_BUDGET"), "Source byte budget was ignored");
        for (const std::size_t memory : {std::size_t{0}, std::size_t{4096}, std::size_t{8192}}) {
            SyntaxOptions quota; quota.maxMemoryBytes = memory;
            std::string source = "return {";
            for (unsigned i = 0; i < 2048; ++i) source += "'value',";
            source += "}\n";
            auto result = CheckSyntax(source, "quota.lua", quota);
            Require(!result.completed && Code(result, "LUA_CHECK_MEMORY"), "Memory quota did not fail safely");
            Require(result.peakMemoryBytes <= memory, "Allocator exceeded hard physical quota");
        }
        std::stop_source cancelled; cancelled.request_stop();
        SyntaxOptions cancellation; cancellation.cancel = cancelled.get_token();
        auto result = CheckSyntax("return 1", "cancelled.lua", cancellation);
        Require(!result.completed && Code(result, "LUA_CHECK_CANCELLED"), "Pre-cancel did not report incomplete");
        SyntaxOptions expired; expired.maxDuration = std::chrono::milliseconds::zero();
        result = CheckSyntax("return 1", "expired.lua", expired);
        Require(!result.completed && Code(result, "LUA_CHECK_DEADLINE"), "Deadline did not report incomplete");
        SyntaxOptions past; past.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        result = CheckSyntax("return 1", "past.lua", past);
        Require(!result.completed && Code(result, "LUA_CHECK_DEADLINE"), "Absolute deadline was ignored");
        std::string comment = "--[[" + std::string(3 * 1024 * 1024, 'x') + "]]\nreturn 1\n";
        Require(CheckSyntax(comment, "large-comment.lua").Succeeded(), "Chunked reader broke a long token");
        Require(!std::filesystem::exists(marker), "Compile-only marker changed during tests");
        std::cout << "Lua 5.3 compile-only, line mapping, text mode and bounded allocator passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
