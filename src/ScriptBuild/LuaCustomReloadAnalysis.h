#pragma once

#include "War3MapCore/Script/LuaAuthorCode.h"
#include <set>

namespace War3Map::Script::lua {
struct CustomReloadSource { std::string_view text, initializer; };
struct CustomReloadContext { std::set<std::string, std::less<>> functions; };
struct CustomReloadAnalysis {
    std::string text;
    // 保留声明、形参、顺序和完整 initializer；只挖去普通函数体。
    std::string shape;
};
[[nodiscard]] Result<CustomReloadContext> CaptureCustomReloadContext(std::string_view header,
    std::span<const CustomReloadSource> sources, const Lua::SyntaxOptions& options = {});
[[nodiscard]] Result<CustomReloadAnalysis> AnalyzeCustomReload(CustomReloadSource source,
    const CustomReloadContext& context, const Lua::SyntaxOptions& options = {});
}
