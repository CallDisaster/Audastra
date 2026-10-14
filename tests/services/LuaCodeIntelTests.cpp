#include "War3MapCore/Lua/LuaCodeModel.h"
#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/Services/LuaRuntimeCatalog.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Lua;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// 原创合成声明，仅覆盖测试用到的形态。
constexpr std::string_view kCommon =
    "type agent extends handle\n"
    "type widget extends agent\n"
    "type unit extends widget\n"
    "type player extends agent\n"
    "\n"
    "constant native GetPlayerId takes player whichPlayer returns integer\n"
    "// 创建单位\n"
    "native CreateUnit takes player id, integer unitid, real x, real y, real face returns unit\n"
    "native KillUnit takes unit whichUnit returns nothing\n"
    "native Player takes integer number returns player\n"
    "native GetUnitName takes unit whichUnit returns string\n"
    "globals\n"
    "    constant integer PLAYER_NEUTRAL_AGGRESSIVE = 12\n"
    "endglobals\n";
constexpr std::string_view kBlizzard =
    "globals\n"
    "    integer bj_fixture = 1\n"
    "endglobals\n"
    "function FixtureHelper takes nothing returns nothing\n"
    "endfunction\n";

std::shared_ptr<const ApiCatalog> Catalog() {
    Services::LuaRuntimeScripts scripts{std::string(kCommon), std::string(kBlizzard), "合成 common.j"};
    return Services::BuildLuaRuntimeCatalog(&scripts, {{"udg_Hero", "unit"}});
}
const std::map<std::string, std::string, std::less<>>& Modules() {
    static const std::map<std::string, std::string, std::less<>> modules{
        {"lessons.hello", "local M = {}\n-- 打招呼\nfunction M.show(text)\n    print(text)\nend\nM.count = 3\nreturn M\n"},
        {"lessons.units", "local jass = require \"jass.common\"\nlocal M = {}\nfunction M.spawn()\n"
            "    return jass.CreateUnit(jass.Player(0), 1, 0, 0, 0)\nend\nreturn M\n"},
    };
    return modules;
}
ModuleSources Sources() {
    return {[] { std::vector<std::string> names; for (const auto& [name, text] : Modules()) names.push_back(name); return names; },
        [](std::string_view name) -> std::optional<std::string> {
            const auto found = Modules().find(name);
            return found == Modules().end() ? std::nullopt : std::optional<std::string>(found->second);
        }};
}
const CompletionItem* Find(const CompletionList& list, std::string_view label) {
    const auto found = std::find_if(list.items.begin(), list.items.end(), [&](const auto& item) { return item.label == label; });
    return found == list.items.end() ? nullptr : &*found;
}
bool HasWarning(const std::vector<CodeWarning>& warnings, std::string_view code, std::uint32_t line) {
    return std::any_of(warnings.begin(), warnings.end(), [&](const auto& warning) { return warning.code == code && warning.line == line; });
}

void CatalogFromDeclarations() {
    const auto catalog = Catalog();
    Require(catalog->nativesLoaded, "Natives were not loaded");
    const auto* common = catalog->Module("jass.common");
    Require(common && common->Find("CreateUnit") && common->Find("PLAYER_NEUTRAL_AGGRESSIVE"), "common.j members missing");
    Require(common->Find("CreateUnit")->documentation == "创建单位", "Comment above native not used as documentation");
    Require(!common->Find("FixtureHelper"), "blizzard.j function was presented as native");
    Require(catalog->Extends("unit", "handle") && !catalog->Extends("player", "widget"), "Type parents wrong");
    const auto runtime = Services::OwnedRuntimeSemantics(*catalog);
    Require(runtime.verified && runtime.commonLookupVerified && runtime.commonArguments.at("CreateUnit") == 5
        && runtime.presentModules.contains("jass.console"), "Owned runtime facts wrong");
    const auto empty = Services::BuildLuaRuntimeCatalog(nullptr, {});
    Require(!empty->nativesLoaded && !Services::OwnedRuntimeSemantics(*empty).verified, "Missing common.j was treated as verified");
}

void MemberCompletion() {
    CodeIntel intel(Catalog(), Sources(), "main");
    const std::string source = "local jass = require \"jass.common\"\njass.";
    auto list = intel.Complete(source, source.size());
    Require(list.member && list.replaceBegin == source.size() && list.replaceEnd == source.size(), "Member completion range wrong");
    const auto* create = Find(list, "CreateUnit");
    Require(create && create->kind == CompletionKind::Function && create->detail.find("player id") != std::string::npos,
        "jass.common native completion missing");
    Require(Find(list, "PLAYER_NEUTRAL_AGGRESSIVE") && !Find(list, "print"), "Member list mixed with scope");

    const std::string partial = "local jass = require \"jass.common\"\njass.Cre";
    list = intel.Complete(partial, partial.size());
    Require(list.replaceBegin == partial.size() - 3 && Find(list, "CreateUnit"), "Partial member range wrong");

    const std::string globals = "local g = require \"jass.globals\"\ng.";
    list = intel.Complete(globals, globals.size());
    Require(Find(list, "udg_Hero") && Find(list, "bj_fixture"), "jass.globals members missing");

    const std::string text = "local s = \"abc\"\ns:";
    list = intel.Complete(text, text.size());
    const auto* upper = Find(list, "upper");
    Require(upper && upper->kind == CompletionKind::Method, "String method completion missing");

    const std::string scope = "pri";
    Require(Find(intel.Complete(scope, scope.size()), "print"), "Scope completion missing print");
    const std::string declaring = "local pri";
    Require(intel.Complete(declaring, declaring.size()).items.empty(), "Completion offered for a new local name");
    const std::string comment = "-- jass.";
    Require(intel.Complete(comment, comment.size()).items.empty(), "Completion offered inside comment");
}

void RequireAndModules() {
    CodeIntel intel(Catalog(), Sources(), "main");
    const std::string name = "local m = require \"les";
    auto list = intel.Complete(name, name.size());
    Require(list.moduleName && Find(list, "lessons.hello") && Find(list, "jass.common"), "require module names missing");

    const std::string member = "local hello = require \"lessons.hello\"\nhello.";
    list = intel.Complete(member, member.size());
    const auto* show = Find(list, "show");
    Require(show && show->kind == CompletionKind::Function && show->documentation == "打招呼", "Cross-file member missing");
    Require(Find(list, "count"), "Cross-file field missing");

    // 跨文件返回值：spawn() 返回 CreateUnit 的 unit。
    const std::string chain = "local jass = require \"jass.common\"\nlocal units = require \"lessons.units\"\n"
        "local u = units.spawn()\njass.KillUnit(u)\njass.Player(u)\n";
    const auto warnings = intel.Check(chain);
    Require(!HasWarning(warnings, "LUA_NATIVE_ARGUMENT_TYPE", 4), "Inferred unit rejected by KillUnit");
    Require(HasWarning(warnings, "LUA_NATIVE_ARGUMENT_TYPE", 5), "Cross-file unit passed to integer not reported");
    const auto at = chain.find("u = units");
    const auto hover = intel.Hover(chain, at);
    Require(hover && hover->signature.find("unit") != std::string::npos, "Hover did not show inferred type");
}

void SignatureHelpCases() {
    CodeIntel intel(Catalog(), Sources(), "main");
    const std::string source = "local jass = require \"jass.common\"\njass.CreateUnit(jass.Player(0), ";
    const auto help = intel.Signature(source, source.size());
    Require(help && help->active == 1 && help->parameters.size() == 5, "Signature active parameter wrong");
    const auto [begin, end] = help->parameters[1];
    Require(help->label.substr(begin, end - begin) == "integer unitid", "Signature parameter range wrong");
    const std::string nested = "local jass = require \"jass.common\"\njass.CreateUnit(jass.Player(";
    const auto inner = intel.Signature(nested, nested.size());
    Require(inner && inner->label.find("Player") != std::string::npos && inner->active == 0, "Nested call signature wrong");
    const std::string definition = "local function f(a, ";
    Require(!intel.Signature(definition, definition.size()), "Definition parameter list treated as call");
}

void Checks() {
    CodeIntel intel(Catalog(), Sources(), "main");
    const std::string source =
        "local jass = require \"jass.common\"\n"   // 1
        "jass.KillUnit()\n"                        // 2
        "jass.KillUnit(1)\n"                       // 3
        "jass.Nope()\n"                            // 4
        "jass.Player(1.5)\n"                       // 5
        "local x = math.floor(2)\njass.Player(x)\n" // 6, 7
        "local missing = require \"lessons.missing\"\n" // 8
        "local u = jass.CreateUnit(jass.Player(0), 1, 0, 0, 0)\njass.KillUnit(u)\n" // 9, 10
        "local function many() return 1, 2 end\njass.CreateUnit(jass.Player(0), many())\n"; // 11, 12
    const auto warnings = intel.Check(source);
    Require(HasWarning(warnings, "LUA_NATIVE_ARGUMENT_COUNT", 2), "Missing argument not reported");
    Require(HasWarning(warnings, "LUA_NATIVE_ARGUMENT_TYPE", 3), "Integer passed to unit not reported");
    Require(HasWarning(warnings, "LUA_UNKNOWN_NATIVE", 4), "Unknown native not reported");
    Require(HasWarning(warnings, "LUA_NATIVE_ARGUMENT_TYPE", 5), "Float passed to integer not reported");
    Require(HasWarning(warnings, "LUA_MODULE_NOT_FOUND", 8), "Missing map module not reported");
    for (const std::uint32_t line : {1u, 6u, 7u, 9u, 10u, 12u})
        Require(std::none_of(warnings.begin(), warnings.end(), [&](const auto& warning) { return warning.line == line; }),
            "Valid line reported");
    const auto unknown = std::find_if(warnings.begin(), warnings.end(), [](const auto& warning) { return warning.code == "LUA_UNKNOWN_NATIVE"; });
    Require(unknown->byteColumn == 6 && unknown->byteLength == 4, "Warning column wrong");
}

void CodeCheckIntegration() {
    Services::CodeCheckRequest request;
    request.language = Services::CodeLanguage::Lua53;
    request.source = {"main.lua", "local jass = require \"jass.common\"\njass.KillUnit()\njass.Nope()\n"};
    request.context.luaCatalog = Catalog();
    request.context.luaSemantic.runtime = Services::OwnedRuntimeSemantics(*request.context.luaCatalog);
    request.context.luaModules = Modules();
    const auto result = Services::CheckCode(request);
    const auto count = [&](std::string_view code) {
        return std::count_if(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& item) { return item.code == code; });
    };
    Require(result.completed && count("LUA_UNKNOWN_NATIVE") == 1 && count("LUA_NATIVE_ARGUMENT_COUNT") == 1,
        "Source-level warnings missing from code check");
    Require(count("LUA_COMMON_ARGUMENT_COUNT") == 0, "Duplicate argument count warning");
}
// 设置 FORGE_GAME_ROOT 时用本机游戏的 common.j 再核对一次；未设置则跳过。
void RealGameCatalog() {
    char* value = nullptr;
    std::size_t length = 0;
    std::string root;
    if (_dupenv_s(&value, &length, "FORGE_GAME_ROOT") == 0 && value) { root = value; std::free(value); }
    if (root.empty()) { std::cout << "FORGE_GAME_ROOT not set; real common.j check skipped\n"; return; }
    const auto scripts = Services::ReadGameLuaRuntimeScripts(root);
    Require(scripts.Succeeded() && scripts.value, "Real common.j not readable");
    const auto catalog = Services::BuildLuaRuntimeCatalog(&*scripts.value, {});
    const auto* common = catalog->Module("jass.common");
    Require(catalog->nativesLoaded && common && common->members.size() > 900, "Real common.j natives missing");
    const auto* create = common->Find("CreateUnit");
    Require(create && create->parameters.size() == 5 && create->returns == "unit", "Real CreateUnit signature wrong");
    CodeIntel intel(catalog, {}, "main");
    const std::string source = "local jass = require \"jass.common\"\nlocal u = jass.CreateUnit(jass.Player(0), 1, 0, 0, 0)\n"
        "jass.KillUnit(u)\njass.SetUnitX(u, 1.5)\njass.Nope()\n";
    const auto warnings = intel.Check(source);
    Require(warnings.size() == 1 && warnings.front().code == "LUA_UNKNOWN_NATIVE", "Real common.j check result wrong");
    std::cout << "real common.j: " << common->members.size() << " members\n";
}
} // namespace

int main() {
    try {
        CatalogFromDeclarations();
        MemberCompletion();
        RequireAndModules();
        SignatureHelpCases();
        Checks();
        CodeCheckIntegration();
        RealGameCatalog();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Lua code intel tests passed\n";
    return 0;
}
