#include "War3MapCore/Jass/JassChecker.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map::Jass;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
const SymbolReference& ReferenceAt(const SymbolIndex& index, std::uint32_t file, std::size_t offset) {
    const auto found = std::find_if(index.references.begin(), index.references.end(), [&](const auto& item) {
        return item.position.key.file == file && item.position.key.offset == offset;
    });
    Require(found != index.references.end(), "Actual semantic reference missing"); return *found;
}
void CheckScopeAndUses() {
    const std::string text =
        "globals\r\ninteger g = 1\rinteger array values\ninteger before = g\nendglobals\n"
        "function Sink takes code cb returns nothing\nendfunction\n"
        "function First takes integer g returns integer\nlocal integer g = 2\n"
        "set g = g + 1\nset values[g] = g\nset g = First(g)\nreturn g\nendfunction\n"
        "function Second takes nothing returns integer\nlocal integer g = 3\nreturn g\nendfunction\n"
        "function Caller takes nothing returns nothing\nset g = First(g)\ncall Sink(function Caller)\nendfunction\n";
    const JassSource source{"scoped.j", text}; JassCheckOptions options; options.collectSymbols = true;
    const auto result = CheckJass(std::span(&source, 1), options);
    for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.message << '\n';
    Require(result.Succeeded() && result.symbols.completed, "Valid scoped program index was incomplete");
    const auto definition = [&](std::string_view prefix, std::size_t delta) {
        return SymbolKey{0, static_cast<std::uint32_t>(text.find(prefix) + delta)};
    };
    const auto global = definition("integer g = 1", 8);
    const auto firstLocal = definition("local integer g = 2", 14);
    const auto secondLocal = definition("local integer g = 3", 14);
    Require(global != firstLocal && firstLocal != secondLocal, "Same-name definitions lost separate identities");
    Require(ReferenceAt(result.symbols, 0, text.find("before = g") + 9).target == global, "Global initializer wrong scope");
    Require(ReferenceAt(result.symbols, 0, text.find("set g = g") + 4).target == firstLocal, "Local write bound to parameter/global");
    Require(ReferenceAt(result.symbols, 0, text.find("set g = g") + 8).target == firstLocal, "Local read wrong scope");
    Require(ReferenceAt(result.symbols, 0, text.find("return g", text.find("function Second")) + 7).target == secondLocal,
        "Same-name locals across functions were combined");
    const auto arrayUse = text.find("set values[g]") + 4;
    Require(ReferenceAt(result.symbols, 0, arrayUse).use == SymbolUse::Write, "Array write mistaken for read");
    Require(ReferenceAt(result.symbols, 0, arrayUse + 7).target == firstLocal, "Array index variable not collected");
    Require(ReferenceAt(result.symbols, 0, text.find("= First(g)") + 2).target == definition("function First", 9),
        "Recursive call did not use actual checker self rule");
    Require(ReferenceAt(result.symbols, 0, text.find("function Caller)") + 9).use == SymbolUse::FunctionReference,
        "Code function reference mistaken for call");
    Require(result.symbols.scopes.size() == 4 && result.symbols.definitions[0].position.line == 2,
        "Function scopes or mixed CR/LF/CRLF line positions lost");
    options.maxSymbolReferences = 1;
    const auto quota = CheckJass(std::span(&source, 1), options);
    Require(quota.Succeeded() && !quota.symbols.completed && quota.symbols.references.size() == 1,
        "Independent display quota changed valid save check or reported complete index");
    options = {}; const auto ordinary = CheckJass(std::span(&source, 1), options);
    Require(ordinary.Succeeded() && ordinary.symbols.definitions.empty() && ordinary.symbols.references.empty(),
        "Default save check collected optional display index");
}
void CheckInvalidVisibilityAndParallel() {
    const JassSource invalid{"bad.j", "globals\ninteger self = self\nendglobals\n"
        "//# +nosemanticerror\nfunction Before takes nothing returns nothing\ncall Later()\nendfunction\n"
        "function Later takes nothing returns nothing\nendfunction\n"};
    JassCheckOptions options; options.collectSymbols = true;
    const auto result = CheckJass(std::span(&invalid, 1), options);
    Require(!result.Succeeded() && !result.symbols.completed && result.errorsIgnored == 1,
        "Invalid/ignored semantic errors yielded complete index");
    Require(!result.symbols.references[0].target && !result.symbols.references[1].target,
        "Global self or forward function incorrectly resolved");
    const JassSource ignored{"ignored.j", "//# +nosemanticerror\nfunction Wrong takes nothing returns integer\nreturn true\nendfunction\n"};
    const auto ignoredResult = CheckJass(std::span(&ignored, 1), options);
    Require(ignoredResult.Succeeded() && ignoredResult.errorsIgnored == 1 && !ignoredResult.symbols.completed,
        "Ignored return type error alone yielded complete semantic index");
    const JassSource valid{"simple.j", "function Okay takes nothing returns nothing\nendfunction\n"};
    for (const bool definitions : {false, true}) {
        auto tiny = options; if (definitions) tiny.maxSymbolDefinitions = 0; else tiny.maxSymbolBytes = 0;
        const auto bounded = CheckJass(std::span(&valid, 1), tiny);
        Require(bounded.Succeeded() && !bounded.symbols.completed, "Zero display quota changed save result or granted full index");
    }
    auto syntax = options; syntax.syntaxOnly = true;
    Require(!CheckJass(std::span(&valid, 1), syntax).symbols.completed, "Syntax-only granted full semantic index");
    std::stop_source stop; stop.request_stop(); auto cancelled = options; cancelled.cancel = stop.get_token();
    const auto stopped = CheckJass(std::span(&valid, 1), cancelled);
    Require(!stopped.completed && !stopped.symbols.completed, "Cancelled check granted semantic index");
    std::string text = "globals\ninteger shared = 0\nendglobals\n";
    for (int i = 0; i < 130; ++i) text += "function F" + std::to_string(i) +
        " takes nothing returns integer\nlocal integer item = shared\nreturn item\nendfunction\n";
    const JassSource source{"parallel.j", text}; options.threads = 1;
    const auto serial = CheckJass(std::span(&source, 1), options); options.threads = 4;
    const auto parallel = CheckJass(std::span(&source, 1), options);
    Require(serial.Succeeded() && parallel.Succeeded() && serial.symbols.completed && parallel.symbols.completed
        && parallel.threadsUsed > 1, "Parallel semantic fixture did not run multiple workers");
    Require(serial.symbols.definitions == parallel.symbols.definitions && serial.symbols.references == parallel.symbols.references
        && serial.symbols.scopes == parallel.symbols.scopes, "Parallel merge changed symbol identities/positions/targets");
}
}
int main() {
    try { CheckScopeAndUses(); CheckInvalidVisibilityAndParallel();
        std::cout << "Actual checker scopes, reads/writes/calls, independent index quota and stable parallel identities passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
