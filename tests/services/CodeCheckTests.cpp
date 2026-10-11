#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/Services/CodeCheckProject.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <stop_token>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Has(const CodeCheckResult& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
CodeCheckRequest Full(std::string text) {
    CodeCheckRequest request;
    request.source = {"WCT/trigger-17.j", std::move(text)};
    request.context.declarations = {{"Save/common.j", "type unit extends handle\n"
        "native ActualApi takes unit u, integer value returns boolean\n"}};
    request.context.complete = true; request.context.binding = "frozen-revision-7"; request.generation = 7;
    return request;
}
void CheckSemanticsAndActualIndex() {
    auto request = Full("function Test takes unit u returns boolean\nreturn ActualApi(u, 1)\nendfunction\n");
    auto checked = CheckCode(request);
    Require(checked.Succeeded() && checked.semanticChecked && checked.contextComplete, "Actual declaration context was not checked");
    Require(checked.generation == 7 && checked.contextBinding == request.context.binding, "Immutable request identity lost");
    Require(checked.declarations.completed && checked.declarations.symbols.size() == 3, "Production declaration parser index incomplete");
    const auto& native = checked.declarations.symbols[1];
    Require(native.kind == Jass::DeclarationKind::Native && native.name == "ActualApi" && native.type == "boolean"
        && native.source == "Save/common.j" && native.line == 2 && native.parameters.size() == 2
        && native.parameters[0].type == "unit" && native.parameters[0].name == "u", "Actual native signature was guessed or lost");
    Require(checked.externalSources.size() == 1 && checked.externalSources[0].text == request.context.declarations[0].text,
        "Frozen external source ownership lost");
    request.source.text = "function Test takes nothing returns nothing\ncall MissingNative()\nendfunction\n";
    auto full = CheckCode(request);
    Require(full.completed && full.semanticChecked && !full.Succeeded(), "Undefined call was accepted with full context");
    Require(std::any_of(full.diagnostics.begin(), full.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source == "WCT/trigger-17.j" && diagnostic.line == 2 && diagnostic.byteColumn > 1 && diagnostic.originMapped;
    }), "Actual source byte origin lost");
    request.context.complete = false; request.context.reason = "Actual runtime declarations unavailable";
    auto syntax = CheckCode(request);
    Require(syntax.Succeeded() && !syntax.semanticChecked && !syntax.contextComplete
        && syntax.reason == request.context.reason, "Partial context falsely claimed semantic proof");
}
void CheckOriginsAndGeneratedContext() {
    auto request = Full("function Test takes nothing returns nothing\ncall MissingNative()\nendfunction\n");
    request.prefix = "globals\ninteger value = 0\nendglobals\n";
    request.suffix = "function Generated takes nothing returns nothing\ncall MissingOther()\nendfunction\n";
    auto checked = CheckCode(request);
    Require(!checked.Succeeded(), "Invalid composed source passed");
    Require(std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source == "WCT/trigger-17.j" && diagnostic.line == 2;
    }), "Prefix offset was not mapped to original author line");
    Require(std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source.find("生成上下文") != std::string::npos;
    }), "Suffix error was attributed to author body");
    request.prefix.clear(); request.suffix.clear();
    const auto author = request.sourceMap.AddFile("WCT/trigger-91.j");
    request.sourceMap.Map(1, {author, 40});
    checked = CheckCode(request);
    Require(std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source == "WCT/trigger-91.j" && diagnostic.line == 41
            && diagnostic.byteColumn == 1 && diagnostic.byteLength == 0;
    }), "Frozen emitter source map or conservative rewritten column lost");
}
void CheckVJassLuaAndHardStops() {
    auto broken = Full("function Broken takes nothing returns nothing\n    set\nendfunction\n");
    broken.language = CodeLanguage::VJass;
    const auto brokenResult = CheckCode(broken);
    Require(brokenResult.completed && brokenResult.syntaxChecked && !brokenResult.Succeeded()
        && !brokenResult.semanticChecked && !brokenResult.diagnostics.empty(),
        "A completed actual vJass syntax rejection falsely claimed no syntax check was performed");
    auto included = Full("#include \"library.j\"\n#define COUNT 1\n"
        "function Test takes nothing returns integer\nreturn Included(COUNT)\nendfunction\n");
    included.language = CodeLanguage::VJass;
    included.context.includes = {{"includes/library.j", "native Included takes integer count returns integer\n"}};
    auto includeResult = CheckCode(included);
    Require(includeResult.Succeeded() && std::any_of(includeResult.declarations.symbols.begin(), includeResult.declarations.symbols.end(),
        [](const auto& symbol) { return symbol.name == "Included" && symbol.source == "includes/library.j" && symbol.line == 1; }),
        "Owned preprocessing or actual include declaration source was lost");
    included.context.includes.clear(); includeResult = CheckCode(included);
    Require(!includeResult.Succeeded(), "Missing owned include fell back to unbound filesystem");
    auto request = Full("scope Example\nfunction Test takes nothing returns nothing\ncall MissingNative()\nendfunction\nendscope\n");
    request.language = CodeLanguage::VJass;
    auto checked = CheckCode(request);
    Require(checked.completed && !checked.Succeeded(), "Expanded invalid vJass passed");
    Require(std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source == "WCT/trigger-17.j" && diagnostic.line == 3;
    }), "vJass output was not mapped back to original line");
    request = Full("globals\ninteger Value = 1\nendglobals\n"
        "function Test takes nothing returns integer\nreturn Value /* first\nsecond */ + MissingA\nendfunction\n");
    request.language = CodeLanguage::VJass;
    checked = CheckCode(request);
    Require(!checked.Succeeded() && std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source == "WCT/trigger-17.j" && diagnostic.line == 6 && diagnostic.originMapped;
    }), "Collapsed multiline expression reported the wrong author line for MissingA");
    request = {}; request.language = CodeLanguage::Lua53;
    request.source = {"module.lua", "while true do end\nerror('must only compile')\n"};
    checked = CheckCode(request);
    Require(checked.Succeeded() && !checked.semanticChecked && !checked.contextComplete
        && checked.reason.find("jass.*") != std::string::npos
        && checked.reason.find("目前只检查语法") != std::string::npos, "Lua compile-only interface boundary lost");
    request.source.text = "local x = 1\nlocal = 2\n";
    checked = CheckCode(request);
    Require(!checked.Succeeded() && Has(checked, "LUA_SYNTAX_ERROR") && checked.diagnostics.front().line == 2,
        "Lua exact syntax line lost");
    for (const auto* ending : {"\n", "\r\n"}) {
        request.source.text = std::string("function foo()") + ending;
        checked = CheckCode(request);
        Require(!checked.Succeeded() && checked.diagnostics.front().source == "module.lua"
            && checked.diagnostics.front().line == 2 && checked.diagnostics.front().originMapped,
            "Terminal newline EOF error was falsely labeled generated context");
    }
    request = Full("function Test takes nothing returns nothing\nendfunction\n");
    std::stop_source stop; stop.request_stop();
    checked = CheckCode(request, stop.get_token());
    Require(!checked.completed && checked.cancelled && Has(checked, "CODE_CHECK_CANCELLED"), "Worker cancellation formed passed evidence");
    request.jass.maxDuration = std::chrono::milliseconds(0);
    checked = CheckCode(request);
    Require(!checked.completed && Has(checked, "CODE_CHECK_DEADLINE"), "Zero deadline was ignored");
    request.jass = {}; request.jass.maxTokens = 3;
    checked = CheckCode(request);
    Require(!checked.completed && !checked.Succeeded(), "Shared JASS token budget was bypassed");
    request = Full("function Test takes nothing returns nothing\nendfunction\n");
    request.context.declarations[0].name = request.source.name;
    checked = CheckCode(request);
    Require(!checked.completed && Has(checked, "CODE_CHECK_SOURCE_AMBIGUOUS"), "Duplicate origin identity accepted");
    CodeCheckProjectCache cache;
    checked = CheckProjectCode({}, cache, {}, "function Test takes nothing returns nothing\nendfunction\n", CodeLanguage::Jass, 19);
    Require(checked.Succeeded() && !checked.contextComplete && !checked.semanticChecked && checked.generation == 19,
        "Unbound project fabricated full context or lost safe syntax fallback");
}
void CheckDeclarationMessagesUseAuthorLocations() {
    auto request = Full("function Early takes nothing returns nothing\ncall Later()\nendfunction\n"
        "function Later takes nothing returns nothing\nendfunction\n");
    request.prefix = "globals\ninteger generated = 0\nendglobals\n";
    auto checked = CheckCode(request);
    const auto order = [](const CodeCheckResult& result) -> const CodeCheckDiagnostic* {
        const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "JASS_DECLARATION_ORDER"; });
        return found == result.diagnostics.end() ? nullptr : &*found;
    };
    const auto* diagnostic = order(checked);
    Require(diagnostic && diagnostic->line == 2
        && diagnostic->message.find("定义于 WCT/trigger-17.j:4") != std::string::npos
        && diagnostic->message.find("WCT/trigger-17.j:7") == std::string::npos,
        "Embedded definition line still includes generated prefix");

    request.prefix.clear();
    const auto author = request.sourceMap.AddFile("WCT/header.j");
    request.sourceMap.Map(1, {author, 80});
    checked = CheckCode(request); diagnostic = order(checked);
    Require(diagnostic && diagnostic->line == 81
        && diagnostic->message.find("定义于 WCT/header.j:83") != std::string::npos,
        "Embedded definition location ignored the frozen emitter source map");

    request = Full("library Example\nfunction Early takes nothing returns nothing\ncall Later()\nendfunction\n"
        "function Later takes nothing returns nothing\nendfunction\nendlibrary\n");
    request.language = CodeLanguage::VJass;
    checked = CheckCode(request); diagnostic = order(checked);
    Require(diagnostic && diagnostic->line == 3
        && diagnostic->message.find("定义于 WCT/trigger-17.j:5") != std::string::npos,
        "vJass generated declaration position was not mapped back into author source");

    request = Full("function Fine takes nothing returns nothing\nendfunction\n");
    request.context.declarations = {{"Save/external.j", "function ExternalEarly takes nothing returns nothing\n"
        "call ExternalLater()\nendfunction\nfunction ExternalLater takes nothing returns nothing\nendfunction\n"}};
    const auto mapped = request.sourceMap.AddFile("WCT/other.j"); request.sourceMap.Map(1, {mapped, 200});
    checked = CheckCode(request); diagnostic = order(checked);
    Require(diagnostic && diagnostic->source == "Save/external.j"
        && diagnostic->message.find("定义于 Save/external.j:4") != std::string::npos,
        "External definition was incorrectly mapped as main author body");
}
void CheckDisabledDraftIsActuallyParsed() {
    auto document = std::make_shared<Trigger::TriggerDocument>();
    Trigger::TriggerRecord trigger;
    trigger.id = 91; trigger.type = 0; trigger.customTextFlag = 1; trigger.enabled = 0;
    document->triggers.push_back(trigger);
    auto snapshot = std::make_shared<CodeCheckProjectSnapshot>();
    snapshot->binding = "disabled-draft"; snapshot->triggers = document;
    CodeCheckProjectCache cache;
    const Trigger::CodeBlockTarget target{Trigger::CodeBlockKind::CustomTrigger, 91};
    auto malformed = CheckProjectCode(snapshot, cache, target,
        "function Broken takes nothing returns nothing\nlocal =\nendfunction\n", CodeLanguage::Jass, 20);
    Require(malformed.completed && !malformed.Succeeded() && !malformed.semanticChecked
        && !malformed.contextComplete && malformed.reason.find("已禁用") != std::string::npos,
        "Disabled draft was never parsed or falsely passed as production semantics");
    auto valid = CheckProjectCode(snapshot, cache, target,
        "function Fine takes nothing returns nothing\ncall UnknownApi()\nendfunction\n", CodeLanguage::Jass, 21);
    Require(valid.Succeeded() && !valid.semanticChecked && !valid.contextComplete
        && valid.reason.find("已禁用") != std::string::npos && document->triggers[0].enabled == 0
        && document->triggers[0].customText.text.empty(),
        "Disabled syntax-only draft changed live metadata or fabricated full context");
}
}
int main() {
    try {
        CheckSemanticsAndActualIndex(); CheckOriginsAndGeneratedContext(); CheckVJassLuaAndHardStops();
        CheckDeclarationMessagesUseAuthorLocations();
        CheckDisabledDraftIsActuallyParsed();
        std::cout << "Shared code-check semantics, emitter origins, vJass mapping, Lua compile-only and hard stops passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
