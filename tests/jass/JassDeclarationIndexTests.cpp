#include "War3MapCore/Jass/JassDeclarationIndex.h"

#include <iostream>
#include <stdexcept>
#include <stop_token>

namespace { void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); } }
int main() {
    using namespace War3Map::Jass;
    try {
        std::string text = "// native Fake takes nothing returns nothing\r\n"
            "type unit extends handle\r\nconstant native Actual takes unit who returns integer\r\n"
            "globals\r\nconstant integer Answer = 42\r\nunit array Instances\r\nendglobals\r\n"
            "function Work takes integer amount, unit target returns nothing\r\n"
            "local string noise = \"function Pretend takes nothing returns nothing\"\r\nendfunction\r\n";
        const JassSource input{"actual/common.j", text};
        auto index = IndexJassDeclarations(std::span(&input, 1));
        Require(index.completed && index.symbols.size() == 5, "Comments/strings were interpreted as declarations");
        Require(index.symbols[0].kind == DeclarationKind::Type && index.symbols[0].name == "unit"
            && index.symbols[0].type == "handle" && index.symbols[0].line == 2 && index.symbols[0].byteColumn == 6,
            "Type declaration source position incorrect");
        Require(index.symbols[1].constant && index.symbols[1].kind == DeclarationKind::Native
            && index.symbols[1].parameters[0].name == "who" && index.symbols[1].type == "integer", "Native contract lost");
        Require(index.symbols[2].constant && index.symbols[2].line == 5 && index.symbols[2].byteColumn == 18
            && index.symbols[3].array, "Globals contract or CRLF column lost");
        Require(index.symbols[4].parameters.size() == 2 && index.symbols[4].parameters[1].type == "unit"
            && index.symbols[4].line == 8, "Function signature lost");
        text.assign(text.size(), 'x');
        Require(index.symbols[4].name == "Work" && index.symbols[4].parameters[0].name == "amount",
            "Index borrowed caller source text");
        JassCheckOptions tiny; tiny.maxTokens = 2;
        const JassSource valid{"valid.j", "native Real takes nothing returns nothing\n"};
        Require(!IndexJassDeclarations(std::span(&valid, 1), tiny).completed, "Index ignored parser token budget");
        std::stop_source stop; stop.request_stop(); tiny = {}; tiny.cancel = stop.get_token();
        Require(!IndexJassDeclarations(std::span(&input, 1), tiny).completed, "Index ignored cancellation");
        const JassSource malformed{"bad.j", "native Broken takes integer returns\n"};
        auto partial = IndexJassDeclarations(std::span(&malformed, 1));
        Require(!partial.completed && !partial.reason.empty(), "Malformed top-level declaration reported complete index");
        std::cout << "Actual parser declaration signatures, owned source positions and incomplete gates passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
