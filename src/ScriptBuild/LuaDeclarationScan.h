#pragma once

#include "War3MapCore/Lua/LuaSyntax.h"
#include <set>

namespace War3Map::Script::lua {
struct DeclarationToken { std::string_view text; std::size_t offset = 0; };
struct AuthorDeclarations { std::set<std::string> functions, locals; };
// 与作者代码资格检查共用有预算的词法扫描；失败抛 Diagnostic，由内部调用边界转为 Result。
[[nodiscard]] std::vector<DeclarationToken> ScanDeclarationTokens(std::string_view text, const Lua::SyntaxOptions& options);
[[nodiscard]] AuthorDeclarations CollectAuthorDeclarations(std::string_view text, const Lua::SyntaxOptions& options);
}
