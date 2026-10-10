#include "War3MapCore/Jass/JassDeclarationIndex.h"
#include "JassParser.h"
#include "JassCheckBudget.h"

#include <algorithm>
#include <stdexcept>

namespace War3Map::Jass {
DeclarationIndex IndexJassDeclarations(std::span<const JassSource> sources, const JassCheckOptions& options) {
    DeclarationIndex result;
    constexpr std::size_t maxSymbols = 32768, maxBytes = 8 * 1024 * 1024, maxName = 1024, maxParams = 256;
    std::size_t bytes = 0;
    try {
        detail::CheckBudget budget(options);
        for (const auto& source : sources) {
            budget.Poll();
            if (source.name.size() > 4096) throw std::runtime_error("name");
            const auto program = detail::ParseProgram(source.text, &budget);
            if (!program.errors.empty()) { result.reason = "顶层声明含语法错误，索引不完整。"; return result; }
            std::size_t lineStart = 0, scanned = 0;
            const auto append = [&](DeclarationSymbol symbol, std::string_view originalName) {
                budget.Poll();
                if (symbol.name.empty() || symbol.name.size() > maxName || symbol.type.size() > maxName
                    || symbol.parameters.size() > maxParams || result.symbols.size() >= maxSymbols) throw std::runtime_error("symbols");
                const auto begin = reinterpret_cast<std::uintptr_t>(source.text.data());
                const auto name = reinterpret_cast<std::uintptr_t>(originalName.data());
                if (name < begin || name - begin > source.text.size() || originalName.size() > source.text.size() - (name - begin))
                    throw std::runtime_error("origin");
                const auto offset = static_cast<std::size_t>(name - begin);
                if (offset < scanned) { scanned = 0; lineStart = 0; }
                for (; scanned < offset; ++scanned) {
                    budget.PollOccasionally(scanned);
                    if (source.text[scanned] == '\r' || source.text[scanned] == '\n') lineStart = scanned + 1;
                }
                symbol.byteColumn = static_cast<std::uint32_t>(offset - lineStart + 1);
                symbol.source = source.name;
                std::size_t needed = sizeof(DeclarationSymbol) + symbol.name.size() + symbol.type.size() + symbol.source.size();
                for (const auto& parameter : symbol.parameters) {
                    if (parameter.name.size() > maxName || parameter.type.size() > maxName) throw std::runtime_error("parameters");
                    needed += sizeof(DeclarationParameter) + parameter.name.size() + parameter.type.size();
                }
                if (needed > maxBytes - bytes) throw std::runtime_error("bytes");
                bytes += needed; result.symbols.push_back(std::move(symbol));
            };
            for (const auto& declaration : program.order) {
                budget.Poll();
                DeclarationSymbol symbol;
                std::string_view name;
                switch (declaration.kind) {
                case detail::Declaration::Kind::Type: {
                    const auto& type = program.types.at(declaration.index);
                    if (type.parent.size() > maxName) throw std::runtime_error("type");
                    symbol.kind = DeclarationKind::Type; name = type.name; symbol.type = type.parent; symbol.line = type.line; break;
                }
                case detail::Declaration::Kind::Global: {
                    const auto& global = program.globals.at(declaration.index);
                    if (global.type.size() > maxName) throw std::runtime_error("type");
                    symbol.kind = DeclarationKind::Global; name = global.name; symbol.type = global.type;
                    symbol.line = global.line; symbol.constant = global.constant; symbol.array = global.array; break;
                }
                case detail::Declaration::Kind::Native:
                case detail::Declaration::Kind::Function: {
                    const auto setFunction = [&](const auto& function) {
                        if (function.returns.size() > maxName || function.name.size() > maxName) throw std::runtime_error("type");
                        name = function.name; symbol.type = function.returns.empty() ? "nothing" : function.returns;
                        symbol.line = function.line; symbol.constant = function.constant;
                        if (function.params.size() > maxParams) throw std::runtime_error("parameters");
                        for (const auto& parameter : function.params) {
                            if (parameter.type.size() > maxName || parameter.name.size() > maxName) throw std::runtime_error("parameter");
                            symbol.parameters.push_back({std::string(parameter.type), std::string(parameter.name)});
                        }
                    };
                    if (declaration.kind == detail::Declaration::Kind::Native) {
                        symbol.kind = DeclarationKind::Native; setFunction(program.natives.at(declaration.index));
                    } else { symbol.kind = DeclarationKind::Function; setFunction(program.functions.at(declaration.index)); }
                    break;
                }
                }
                if (name.size() > maxName) throw std::runtime_error("name");
                symbol.name = name; append(std::move(symbol), name);
            }
        }
        budget.Poll(); result.completed = true;
    } catch (const std::exception&) {
        result.reason = "声明索引已取消或达到名称、数量、文本/解析预算；不完整索引不能代表全图引用覆盖。";
    }
    return result;
}
} // namespace War3Map::Jass
