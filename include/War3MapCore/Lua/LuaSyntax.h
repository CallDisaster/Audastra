#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Lua {

struct SyntaxOptions {
    std::size_t maxSourceBytes = 16 * 1024 * 1024;
    std::size_t maxMemoryBytes = 64 * 1024 * 1024;
    std::chrono::milliseconds maxDuration{30000};
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    std::stop_token cancel;
};
struct SyntaxDiagnostic {
    std::string code;
    std::string message;
    std::uint32_t line = 0;
};
struct SyntaxResult {
    bool completed = true;
    std::size_t peakMemoryBytes = 0;
    std::vector<SyntaxDiagnostic> diagnostics;
    [[nodiscard]] bool Succeeded() const noexcept { return completed && diagnostics.empty(); }
};

// Lua 5.3 文本编译：不执行字节码，不安装标准库或运行时 jass 接口。
[[nodiscard]] SyntaxResult CheckSyntax(std::string_view source, std::string_view name,
    const SyntaxOptions& options = {});

} // namespace War3Map::Lua
