#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Lua {
struct ModuleRequire {
    std::string name;
    std::uint32_t line = 0;
    bool dynamic = false;
};
struct EmptyTableInitializer { std::size_t begin = 0, end = 0; };
[[nodiscard]] std::vector<EmptyTableInitializer> FindEmptyTableInitializers(
    std::string_view source, std::string_view variable);
// 词法证据，不执行代码；局部遮蔽和动态表达式不能当作依赖完整性证明。
[[nodiscard]] std::vector<ModuleRequire> ScanModuleRequires(std::string_view source,
    std::size_t maxTokens = 4 * 1024 * 1024);
} // namespace War3Map::Lua
