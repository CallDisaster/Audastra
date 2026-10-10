#pragma once

#include "War3MapCore/Jass/JassChecker.h"

namespace War3Map::Jass {

enum class DeclarationKind : std::uint8_t { Type, Global, Native, Function };
struct DeclarationParameter { std::string type, name; };
struct DeclarationSymbol {
    DeclarationKind kind = DeclarationKind::Function;
    std::string name, type, source;
    std::vector<DeclarationParameter> parameters;
    std::uint32_t line = 0, byteColumn = 1;
    bool constant = false, array = false;
};
struct DeclarationIndex {
    bool completed = false;
    std::vector<DeclarationSymbol> symbols;
    std::string reason;
};

// 复用生产Parser真实声明，不把词法词、UiCatalog GUI名或猜测运行库名称作为函数。
// 仅顶层type/global/native/function；局部scope/全图引用由调用方明确另行覆盖。
[[nodiscard]] DeclarationIndex IndexJassDeclarations(std::span<const JassSource> sources,
    const JassCheckOptions& options = {});

} // namespace War3Map::Jass
