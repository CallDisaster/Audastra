#include "../src/ScriptBuild/ScriptJapiPlaceholder.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Jass/Preprocessor.h"
#include "War3MapCore/Script/ScriptPipeline.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

// 原创内存夹具；不需要本机YDWE、私有地图、磁盘写入或外部编译器。
namespace {
using namespace War3Map;
using namespace War3Map::Script;
using namespace War3Map::Script::Detail;

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
std::string Stub(std::string_view name = "AttachPulse", std::string_view parameters = "unit who, trigger callback, integer mode, integer key") {
    return "function " + std::string(name) + " takes " + std::string(parameters)
        + " returns nothing\n    JapiPlaceHolder\nendfunction\n";
}
std::string Real(std::string_view name = "AttachPulse", std::string_view parameters = "unit who, trigger callback, integer keys, integer mode") {
    return "function " + std::string(name) + " takes " + std::string(parameters)
        + " returns nothing\n    call ExecuteFunc(\"OriginalBridge\")\nendfunction\n";
}
std::string CrLf(std::string_view source) {
    std::string result;
    for (char c : source) { if (c == '\n') result += '\r'; result += c; }
    return result;
}
struct Fixture {
    std::string generated, input;
    std::vector<JapiPlaceholderInclude> includes;
    Jass::PreprocessResult pp;
    Fixture(std::string raw = Stub(), std::string implementation = Real(), std::string injection = {})
        : generated(std::move(raw)), input(generated + injection) {
        const auto path = std::filesystem::path("C:/japi-placeholder-original-fixture/bridge.j");
        const auto hash = Sha256Text(implementation);
        Require(hash.Succeeded(), "fixture hash");
        includes.push_back({path, std::move(implementation), *hash.value});
        Jass::PreprocessOptions options;
        options.defines = {{"JapiPlaceHolder", "call ConvertRace(0) YDNL return"}, {"YDNL", "<?='\\n'?>"}};
        options.forceIncludes = {"bridge.j"};
        options.includeProvider = [&](const std::filesystem::path& candidate) {
            if (candidate.lexically_normal() == path) return Result<Jass::PreprocessIncludeContent>::Success({true, includes.front().text});
            return Result<Jass::PreprocessIncludeContent>::Success({false, {}});
        };
        pp = Jass::Preprocess(input, "war3map.j", path.parent_path(), options);
        Require(pp.Succeeded(), "fixture preprocess");
    }
    JapiPlaceholderRequest Request() const { return {generated, input, pp.text, &pp.sourceMap, "war3map.j", includes}; }
    Result<JapiPlaceholderResult> Run() const { return ReconcileJapiPlaceholders(Request()); }
};
void Reject(const Fixture& fixture, std::string_view why) {
    const auto result = fixture.Run();
    Require(!result.Succeeded() && !result.value.has_value(), why);
}
void Noop(const Fixture& fixture, std::string_view why) {
    const auto result = fixture.Run();
    Require(result.Succeeded() && result.value->retained.empty() && result.value->text == fixture.pp.text, why);
}

void TestSuccessfulBatch() {
    Fixture fixture(Stub() + Stub("ClearPulse"), Real() + Real("ClearPulse"));
    const auto source = fixture.generated, ppSource = fixture.pp.text;
    const auto result = fixture.Run();
    Require(result.Succeeded() && result.value->retained.size() == 2, "two independent generic names reconciled");
    Require(fixture.generated == source && fixture.pp.text == ppSource, "caller source bytes unchanged");
    Require(result.value->text.size() == ppSource.size(), "mask preserves all offsets");
    for (std::size_t i = 0; i < ppSource.size(); ++i) {
        if (ppSource[i] == '\n' || ppSource[i] == '\r') Require(result.value->text[i] == ppSource[i], "mask preserves newlines");
        else Require(result.value->text[i] == ppSource[i] || result.value->text[i] == ' ', "only erase proven span");
    }
    Require(result.value->retained[0].parameterNamesDiffer, "formal names differ without swapping actual arguments");
    Require(result.diagnostics.size() == 2, "formal-name differences recorded");
    const auto evaluated = EvaluateTemplates(result.value->text, {});
    Require(evaluated.Succeeded() && VerifyJapiPlaceholderResult(evaluated.text, result.value->retained).Succeeded(), "post-template identity");
    Require(VerifyJapiPlaceholderResult(CrLf(evaluated.text), result.value->retained).Succeeded(), "post-template CRLF normalization");
    Fixture comments("// function Bogus takes nothing returns nothing\n" + Stub(), "/* JapiPlaceHolder */\n" + Real());
    Require(comments.Run().Succeeded(), "comments around actual bodies are not fake candidates");
    auto commented = Stub();
    commented.replace(commented.find("    JapiPlaceHolder"), std::string("    JapiPlaceHolder").size(),
        "    /* complete marker */ JapiPlaceHolder // ordinary comment");
    Require(Fixture(commented).Run().Succeeded(), "only insignificant comments around lone marker are permitted");
    Require(Fixture("#if 1\n" + Stub() + "#endif\n").Run().Succeeded(), "active conditional maps back to actual source span");
    Fixture crlf(CrLf(Stub()), CrLf(Real()));
    Require(crlf.Run().Succeeded(), "CRLF source line maps");
    Fixture bom(Stub(), std::string("\xef\xbb\xbf") + Real());
    Require(bom.Run().Succeeded(), "include BOM keeps physical SHA and PP line identity");
}

void TestNoopBoundaries() {
    Fixture inactive("#if 0\n" + Stub() + "#endif\n");
    Noop(inactive, "inactive source placeholder cannot count as removed");
    Fixture ordinary("function AttachPulse takes unit who, trigger callback, integer mode, integer key returns nothing\n"
        "    call ConvertRace(0)\n    return\nendfunction\n");
    Noop(ordinary, "ordinary nop body is never guessed to be placeholder");
    Fixture stringOnly("function AttachPulse takes unit who, trigger callback, integer mode, integer key returns nothing\n"
        "    call ExecuteFunc(\"JapiPlaceHolder\")\nendfunction\n");
    Noop(stringOnly, "string marker is not a declaration contract");
    Fixture commentOnly("// JapiPlaceHolder\n" + Real());
    Noop(commentOnly, "comment marker cannot nominate ordinary function");
    Fixture extra("function AttachPulse takes unit who, trigger callback, integer mode, integer key returns nothing\n"
        "    JapiPlaceHolder\n    call ExecuteFunc(\"KeepMe\")\nendfunction\n");
    Noop(extra, "marker plus actual statement is never erased");
    Fixture injected("function Unrelated takes nothing returns nothing\nendfunction\n", Real(), Stub());
    Noop(injected, "injected-only marker is outside original generated ownership");
    Fixture directiveBody("function AttachPulse takes unit who, trigger callback, integer mode, integer key returns nothing\n"
        "#define Added 1\n    JapiPlaceHolder\nendfunction\n");
    Noop(directiveBody, "body directive is not complete lone marker");
}

void TestAmbiguityAndAbi() {
    Noop(Fixture(Stub(), {}), "single runtime hook stub does not require an unrelated replacement");
    Reject(Fixture(Stub() + Stub()), "multiple source stubs fail closed");
    Reject(Fixture(Stub(), Real() + Real()), "multiple real implementations fail closed");
    Reject(Fixture(Stub() + Stub("ClearPulse"), Real() + Real("ClearPulse", "unit who, trigger callback, real key, integer mode")),
        "one mismatched colliding implementation rejects entire batch without partial edits");
    Reject(Fixture(Stub() + Real()), "ordinary author function beside marker is not erased");
    Reject(Fixture(Stub(), Real("AttachPulse", "unit who, trigger callback, real keys, integer mode")), "parameter type mismatch");
    auto returning = Real();
    returning.replace(returning.find("returns nothing"), std::string("returns nothing").size(), "returns integer");
    Reject(Fixture(Stub(), returning), "return type mismatch");
    Reject(Fixture(Stub(), "native AttachPulse takes nothing returns nothing\n" + Real()), "native collision");
    Reject(Fixture(Stub(), Stub()), "owned placeholder is not real implementation");
    auto macroReal = Real();
    macroReal.replace(macroReal.find("ExecuteFunc"), std::string("ExecuteFunc").size(), "ForwardCall");
    Reject(Fixture(Stub(), "#define ForwardCall ExecuteFunc\n" + macroReal), "macro changed real body is explicitly unsupported in initial contract");
}

void TestScopes() {
    for (const auto& scope : {std::string("scope"), std::string("library"), std::string("struct"), std::string("module"), std::string("interface")}) {
        Reject(Fixture(scope + " Outer\n" + Stub() + "end" + scope + "\n"), "scoped raw stub is not global");
        Reject(Fixture(Stub(), scope + " Outer\n" + Real() + "end" + scope + "\n"), "scoped include implementation is not global");
    }
    Reject(Fixture("static if true then\n" + Stub() + "endif\n"), "static-if unresolved scope is not global");
    Reject(Fixture("private " + Stub()), "private stub is outside plain-global contract");
    Reject(Fixture(Stub(), "private " + Real()), "private implementation is outside plain-global contract");
    Reject(Fixture("//# +nosemanticerror\n" + Stub()), "removal cannot reattach a checker pragma to a later function");
    Reject(Fixture("//! textmacro Hidden\n//! endtextmacro\n" + Stub()), "unexpanded vJass directives forbid proof of uniqueness");
    Reject(Fixture("#define BeginScope library Hidden\nBeginScope\n" + Stub() + "endlibrary\n"), "preprocessor scope change cannot turn unknown raw scope into deletion authority");
    const auto background = "library Unrelated\nglobals\ninteger counter\nendglobals\nprivate struct Holder\n"
        "static method run takes nothing returns nothing\nif true then\ncall ExecuteFunc(\"endstruct\")\nendif\nendmethod\nendstruct\nendlibrary\n";
    Fixture safe(std::string(background) + Stub());
    Require(safe.Run().Succeeded(), "balanced unrelated vJass scopes and methods remain supported");
}

void TestInterfacePrototypes() {
    const std::string interfaceText = "interface PulseSink\n"
        "    method run takes nothing returns nothing\n"
        "    method AttachPulse takes unit who, integer key returns nothing defaults nothing\n"
        "endinterface\n";
    Fixture generatedInterface(interfaceText + Stub());
    const auto result = generatedInterface.Run();
    Require(result.Succeeded() && result.value->retained.size() == 1,
        "unrelated interface prototypes coexist with active global stub through real PP");
    Require(result.value->text.find(interfaceText) != std::string::npos,
        "interface method sharing a global name is preserved and not counted as global implementation");
    const auto evaluated = EvaluateTemplates(result.value->text, {});
    Require(evaluated.Succeeded() && VerifyJapiPlaceholderResult(evaluated.text, result.value->retained).Succeeded(),
        "interface prototypes also pass independent post-template scope verification");
    Require(Fixture(Stub(), interfaceText + Real()).Run().Succeeded(),
        "owned include may contain unrelated interface prototypes before global implementation");
    Reject(Fixture("interface\nmethod run takes nothing returns nothing\nendinterface\n" + Stub()),
        "interface requires a name");
    Reject(Fixture("interface PulseSink\nmethod run nothing returns nothing\nendinterface\n" + Stub()),
        "interface method requires strict takes syntax");
    Reject(Fixture("interface PulseSink\nmethod run takes unit returns nothing\nendinterface\n" + Stub()),
        "interface method requires parameter names");
    Reject(Fixture("interface PulseSink\nmethod run takes nothing returns\nendinterface\n" + Stub()),
        "interface method requires a return type");
    Reject(Fixture("interface PulseSink\nmethod run takes nothing returns nothing defaults\nendinterface\n" + Stub()),
        "defaults clause cannot be incomplete");
    Reject(Fixture("interface PulseSink\nmethod run takes nothing returns integer defaults 0\nendinterface\n" + Stub()),
        "unsupported default expression remains fail closed");
    Reject(Fixture("interface PulseSink\nmethod run takes nothing returns nothing\ncall ExecuteFunc(\"Body\")\nendmethod\nendinterface\n" + Stub()),
        "interface prototype cannot conceal a method body");
    Reject(Fixture("interface PulseSink\nmethod run takes nothing returns nothing\nendinterface trailing\n" + Stub()),
        "interface closing line cannot contain unknown trailing tokens");
    Reject(Fixture("interface PulseSink\nmethod run takes nothing returns nothing\nendstruct\n" + Stub()),
        "interface must close with matching scope");
    Reject(Fixture("method run takes nothing returns nothing\nendmethod\n" + Stub()),
        "top-level method is never accepted as unrelated safe syntax");
    Reject(Fixture("struct Holder\nmethod run takes nothing returns nothing\ncall ExecuteFunc(\"Body\")\nendstruct\n" + Stub()),
        "ordinary struct method still requires endmethod");
}

void TestActiveProbe() {
    Fixture active;
    Require(HasActiveJapiPlaceholders(active.Request()), "complete raw global marker with active PP span is detected");
    auto noSources = active.Request();
    noSources.includes = {};
    Require(HasActiveJapiPlaceholders(noSources), "probe does not consume or require owned include content");
    noSources.sourceMap = nullptr;
    Require(!HasActiveJapiPlaceholders(noSources), "probe cannot prove activity without sourceMap");
    Require(!HasActiveJapiPlaceholders(Fixture("#if 0\n" + Stub() + "#endif\n").Request()),
        "inactive marker does not enable owned-provider semantics");
    Require(!HasActiveJapiPlaceholders(Fixture("// JapiPlaceHolder\n" + Real()).Request()),
        "comment-only marker does not enable owned-provider semantics");
    Require(!HasActiveJapiPlaceholders(Fixture("function AuthorCode takes nothing returns nothing\n"
        "call ExecuteFunc(\"JapiPlaceHolder\")\nendfunction\n").Request()),
        "string-only marker does not enable owned-provider semantics");
    Require(!HasActiveJapiPlaceholders(Fixture("/*\n" + Stub() + "*/\n" + Real()).Request()),
        "entire fake marker declaration in block comment does not enable owned-provider semantics");
    Require(!HasActiveJapiPlaceholders(Fixture("scope Hidden\n" + Stub() + "endscope\n").Request()),
        "scoped marker is not an active ordinary global candidate");
    Require(!HasActiveJapiPlaceholders(Fixture("function Unrelated takes nothing returns nothing\nendfunction\n", Real(), Stub()).Request()),
        "only injected marker does not enable owned-provider semantics");
    Fixture missingReal(Stub(), {});
    Require(!HasActiveJapiPlaceholders(missingReal.Request()), "uncollided runtime hook does not enable owned-provider semantics");
    Noop(missingReal, "uncollided runtime hook remains exact PP bytes");
    Fixture nativeConflict(Stub(), "native AttachPulse takes nothing returns nothing\n");
    Require(HasActiveJapiPlaceholders(nativeConflict.Request()) && !nativeConflict.Run().Succeeded(),
        "single marker colliding with native still triggers fail-closed coordination");
    Fixture mismatch(Stub(), Real("AttachPulse", "integer who, trigger callback, integer keys, integer mode"));
    Require(HasActiveJapiPlaceholders(mismatch.Request()) && !mismatch.Run().Succeeded(),
        "active probe leaves replacement ABI checks to full reconciliation");
    Fixture interfaceCase("interface Observer\nmethod run takes nothing returns nothing defaults nothing\nendinterface\n" + Stub());
    Require(HasActiveJapiPlaceholders(interfaceCase.Request()), "unrelated legitimate interface does not hide active global marker");
}

void TestUncollidedHooks() {
    const auto hooks = Stub("SelectModeHook") + Stub("SelectRoleHook");
    Fixture singles(hooks, {});
    Require(!HasActiveJapiPlaceholders(singles.Request()), "two unique hook stubs are not duplicate candidates");
    Noop(singles, "all-single hooks retain exact PP source");
    auto noMap = singles.Request();
    noMap.sourceMap = nullptr;
    noMap.includes = {};
    const auto untracked = ReconcileJapiPlaceholders(noMap);
    Require(untracked.Succeeded() && untracked.value->text == singles.pp.text && untracked.value->retained.empty(),
        "single hooks do not need new source ownership proof");
    Fixture unknown("//! runtextmacro Later()\n" + hooks, {});
    Require(!HasActiveJapiPlaceholders(unknown.Request()), "unknown scope with only single hooks does not change provider policy");
    Noop(unknown, "unknown unrelated syntax in all-single script stays with original compiler");
    Fixture mixed(Stub() + hooks + Stub("ClearPulse"), Real() + Real("ClearPulse"));
    Require(HasActiveJapiPlaceholders(mixed.Request()), "two duplicate names trigger coordination despite two independent hooks");
    const auto result = mixed.Run();
    Require(result.Succeeded() && result.value->retained.size() == 2, "only two colliding markers are recorded");
    for (const auto name : {"SelectModeHook", "SelectRoleHook"}) {
        const auto begin = mixed.pp.text.find("function " + std::string(name) + " takes");
        const auto end = mixed.pp.text.find("endfunction", begin);
        Require(begin != std::string::npos && end != std::string::npos, "fixture contains original single hook");
        const auto length = end + std::string_view("endfunction").size() - begin;
        Require(result.value->text.substr(begin, length) == mixed.pp.text.substr(begin, length),
            "noncolliding expanded hook definition remains byte-for-byte at original offset");
        Require(std::none_of(result.value->retained.begin(), result.value->retained.end(),
            [name](const auto& retained) { return retained.name == name; }), "single hooks are never replacement receipts");
    }
    const auto evaluated = EvaluateTemplates(result.value->text, {});
    Require(evaluated.Succeeded() && VerifyJapiPlaceholderResult(evaluated.text, result.value->retained).Succeeded(),
        "independent hook template expansion coexists with verified duplicate resolution");
}

void TestExpansionContract() {
    for (const auto body : {"call ActualSideEffect()", "call ConvertRace(0) YDNL return 0",
            "local integer hidden YDNL call ConvertRace(0) YDNL return",
            "debug call ConvertRace(0) YDNL return", "call ConvertRace(0) YDNL return YDNL call ActualSideEffect()"}) {
        Fixture changed("#undef JapiPlaceHolder\n#define JapiPlaceHolder " + std::string(body) + "\n" + Stub());
        Require(HasActiveJapiPlaceholders(changed.Request()), "probe does not confuse activity with safe macro semantics");
        const auto result = changed.Run();
        Require(!result.Succeeded() && !result.value.has_value()
            && std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.code == "JAPI_PLACEHOLDER_EXPANSION_UNSUPPORTED";
            }), "redefined marker's nondefault semantics are never erased");
    }
    Reject(Fixture("#undef JapiPlaceHolder\n" + Stub()), "unexpanded bare marker is not approved default PP body");
    Fixture noCollision("#undef JapiPlaceHolder\n#define JapiPlaceHolder call ActualSideEffect()\n" + Stub(), {});
    Noop(noCollision, "noncolliding custom macro retains original behavior");
    Fixture defaultRedefinition("#undef JapiPlaceHolder\n#define JapiPlaceHolder call ConvertRace(0) YDNL return\n" + Stub());
    Require(defaultRedefinition.Run().Succeeded(), "exact approved expansion is recognized only with original marker provenance");
}

void TestPureNewlineTemplates() {
    // 从真实内置PP产物只替换stub中的模板块；重建同源body行映射，以独立覆盖模板谓词，
    // 不让物理换行造成的行号变化先触发其它来源门禁而掩盖词法错误。
    const auto replaceTemplate = [](Fixture& fixture, std::string_view replacement) {
        const auto at = fixture.pp.text.rfind("<?='\\n'?>");
        Require(at != std::string::npos, "fixture default newline template");
        const auto originalMap = fixture.pp.sourceMap;
        fixture.pp.text.replace(at, std::string_view("<?='\\n'?>").size(), replacement);
        const auto first = fixture.pp.text.rfind("function AttachPulse takes");
        const auto last = fixture.pp.text.rfind("endfunction");
        Require(first != std::string::npos && last != std::string::npos, "fixture stub boundaries");
        const auto lineOf = [&](std::size_t offset) {
            return static_cast<std::uint32_t>(1 + std::count(fixture.pp.text.begin(),
                fixture.pp.text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
        };
        const auto firstLine = lineOf(first), lastLine = lineOf(last), lines = lineOf(fixture.pp.text.size());
        Jass::SourceMap mapped;
        for (const auto& file : originalMap.Files()) mapped.AddFile(file);
        const auto main = mapped.AddFile("war3map.j");
        for (std::uint32_t line = 1; line <= lines; ++line) {
            if (line < firstLine) {
                if (const auto position = originalMap.Find(line)) mapped.Map(line, *position);
            } else mapped.Map(line, {main, line == firstLine ? 1u : line < lastLine ? 2u : 3u + line - lastLine});
        }
        fixture.pp.sourceMap = std::move(mapped);
    };
    for (const auto block : {"<?='\\n'?>", "<?= \"\\n\" ?>", "<?=\t'\\n' \t?>"}) {
        Fixture fixture;
        replaceTemplate(fixture, block);
        const auto result = fixture.Run();
        Require(result.Succeeded() && result.value->retained.size() == 1,
            "exact newline literal permits whitespace only outside expression literal");
        Require(EvaluateTemplates(fixture.pp.text, {}).Succeeded(), "accepted template is a real supported expression");
    }
    for (const auto block : {"<? = '\\n' ?>", "<?\t= '\\n' ?>", "<?='\\ n'?>", "<?=' \\n'?>",
            "<?='\\n '?>", "<?=\"\\ n\"?>", "<?='\\\tn'?>", "<?='\\\nn'?>", "<?='\\n\n'?>"}) {
        Fixture fixture;
        replaceTemplate(fixture, block);
        const auto result = fixture.Run();
        Require(!result.Succeeded() && !result.value.has_value()
            && std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.code == "JAPI_PLACEHOLDER_EXPANSION_UNSUPPORTED";
            }), "template prefix spacing, literal internal whitespace and physical newlines are not normalized away");
    }
    Fixture invalidStatement("#undef YDNL\n#define YDNL <? = '\\n' ?>\n" + Stub());
    Require(!EvaluateTemplates(invalidStatement.pp.text, {}).Succeeded(), "real template evaluator rejects spaced equals statement block");
    Reject(invalidStatement, "coordination cannot conceal original template syntax error");
    Fixture singleUnknown("#undef YDNL\n#define YDNL <? = '\\n' ?>\n" + Stub(), {});
    Noop(singleUnknown, "uncollided stub keeps unsupported template for original evaluator to report");
}

void TestCollisionBudget() {
    std::string repeated;
    for (std::size_t i = 0; i < 4096; ++i) repeated += Stub();
    Fixture ambiguous(std::move(repeated), {});
    Require(HasActiveJapiPlaceholders(ambiguous.Request()), "probe returns after first mapped colliding marker without Cartesian product");
    ambiguous.includes.clear();
    const auto rejected = ambiguous.Run();
    Require(!rejected.Succeeded() && !rejected.value.has_value()
        && std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "JAPI_PLACEHOLDER_AMBIGUOUS";
        }), "thousands of equal names reject as one ambiguity before owned source processing");
    std::string stubs, implementations;
    for (std::size_t i = 0; i < 256; ++i) {
        const auto name = "Pulse_" + std::to_string(i);
        stubs += Stub(name);
        implementations += Real(name);
    }
    Fixture atLimit(stubs, implementations);
    const auto allowed = atLimit.Run();
    Require(allowed.Succeeded() && allowed.value->retained.size() == 256,
        "256 unique collisions share one validated include scan and name index");
    Fixture overLimit(stubs + Stub("BeyondBudget"), implementations + Real("BeyondBudget"));
    const auto over = overLimit.Run();
    Require(!over.Succeeded() && !over.value.has_value()
        && std::any_of(over.diagnostics.begin(), over.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "JAPI_PLACEHOLDER_BUDGET";
        }), "257 collisions reject whole batch before any partial output");
}

void TestProvenance() {
    Fixture missing;
    missing.includes.clear();
    Reject(missing, "source map path is not owned content proof");
    Fixture changed;
    changed.includes.front().sha256.assign(64, '0');
    Reject(changed, "snapshot content hash is checked");
    Fixture duplicate;
    duplicate.includes.push_back(duplicate.includes.front());
    Reject(duplicate, "duplicate owned include identity rejected");
    Fixture sourceMoved;
    sourceMoved.includes.front().text = "// new first line\n" + sourceMoved.includes.front().text;
    sourceMoved.includes.front().sha256 = *Sha256Text(sourceMoved.includes.front().text).value;
    Reject(sourceMoved, "same real body at different source position is insufficient");
    Fixture mapped;
    Jass::SourceMap corrupt;
    for (const auto& file : mapped.pp.sourceMap.Files()) corrupt.AddFile(file);
    const auto lines = static_cast<std::uint32_t>(1 + std::count(mapped.pp.text.begin(), mapped.pp.text.end(), '\n'));
    for (std::uint32_t line = 1; line <= lines; ++line) {
        if (auto position = mapped.pp.sourceMap.Find(line)) {
            if (line == 2) position->line = 999999;
            corrupt.Map(line, *position);
        }
    }
    auto request = mapped.Request();
    request.sourceMap = &corrupt;
    const auto invalid = ReconcileJapiPlaceholders(request);
    Require(!invalid.Succeeded(), "every mapped line is bounded, not just declaration header");
    request.sourceMap = nullptr;
    Require(!ReconcileJapiPlaceholders(request).Succeeded(), "missing map rejects placeholder coordination");
    Fixture sideEffect("#undef YDNL\n#define YDNL <? dangerous = 7 ?>\n" + Stub());
    Reject(sideEffect, "removal cannot suppress template side effects in macro-expanded stub");
}

void TestTemplateGate() {
    Fixture fixture;
    const auto result = fixture.Run();
    Require(result.Succeeded(), "template fixture reconciliation");
    const auto& retained = result.value->retained;
    const auto evaluated = EvaluateTemplates(result.value->text, {});
    Require(evaluated.Succeeded(), "template fixture evaluation");
    Require(!VerifyJapiPlaceholderResult(evaluated.text + Real(), retained).Succeeded(), "template-created duplicate rejected");
    Require(!VerifyJapiPlaceholderResult("library Hidden\n" + evaluated.text + "endlibrary\n", retained).Succeeded(), "template scope wrapping rejected");
    Require(!VerifyJapiPlaceholderResult("native AttachPulse takes nothing returns nothing\n" + evaluated.text, retained).Succeeded(), "template-created native collision rejected");
    auto edited = evaluated.text;
    edited.replace(edited.find("OriginalBridge"), std::string("OriginalBridge").size(), "DifferentBridge");
    Require(!VerifyJapiPlaceholderResult(edited, retained).Succeeded(), "template body mutation rejected");
    Require(!VerifyJapiPlaceholderResult("//! runtextmacro Unknown()\n" + evaluated.text, retained).Succeeded(), "late textmacro identity unknown");
    Require(!VerifyJapiPlaceholderResult("<?='pending'?>\n" + evaluated.text, retained).Succeeded(), "unresolved template rejected");
    Require(VerifyJapiPlaceholderResult("arbitrary unchanged legacy input", {}).Succeeded(), "no coordination means no added broad legacy restrictions");
}

void TestLiteralDisabledDocumentation() {
    // 原创复现：注入库在关闭分支中列出只有函数头、没有endfunction的接口文档。
    const std::string docs = "function DocumentationOne takes nothing returns nothing\n"
        "function DocumentationTwo takes integer index returns string\n";
    const std::string disabled = "#if /* ordinary comment */ 0 // known closed\n" + docs + "#endif\n";
    Fixture injected(Stub(), Real(), disabled);
    Require(HasActiveJapiPlaceholders(injected.Request()), "literal-disabled injection docs do not hide unrelated active collision");
    const auto originalInput = injected.input, originalPp = injected.pp.text;
    const auto result = injected.Run();
    Require(result.Succeeded() && result.value->retained.size() == 1,
        "proven closed malformed documentation does not reject an otherwise owned replacement");
    Require(injected.input == originalInput && injected.pp.text == originalPp,
        "raw scan does not rewrite input or actual preprocessor output");
    const auto evaluated = EvaluateTemplates(result.value->text, {});
    Require(evaluated.Succeeded() && VerifyJapiPlaceholderResult(evaluated.text, result.value->retained).Succeeded(),
        "literal-disabled documentation preserves post-template retained identity gate");
    Require(Fixture(disabled + Stub()).Run().Succeeded(), "same narrow rule applies to generated source");
    Require(Fixture(Stub(), disabled + Real()).Run().Succeeded(), "same narrow rule applies to owned include scan");
    Require(Fixture(CrLf(disabled + Stub()), CrLf(Real())).Run().Succeeded(), "closed-block masks preserve CRLF source mapping");
    Require(Fixture("/* prefix comment begins\ncloses here */ " + disabled + Stub()).Run().Succeeded(),
        "mask does not erase a preceding multiline block-comment terminator");
    const auto nested = "#if 0\n#if UnknownCondition\n" + docs + "#else\n" + docs + "#endif\n#endif\n";
    Require(Fixture(Stub(), Real(), nested).Run().Succeeded(), "nested alternatives cannot activate an outer literal-zero block");
    Fixture deadMarker("#if 0\n" + Stub() + "#endif\n" + Stub());
    Require(HasActiveJapiPlaceholders(deadMarker.Request()) && deadMarker.Run().Succeeded(),
        "closed marker is excluded from raw ownership indexes rather than creating a false duplicate");
    Fixture hooks(Stub("SelectModeHook"), {}, disabled);
    Require(!HasActiveJapiPlaceholders(hooks.Request()), "closed docs do not enable provider for a single hook");
    Noop(hooks, "single hook with closed docs keeps actual PP bytes exactly");

    for (const auto expression : {"1", "0 + 0", "(0)", "DisabledMacro"}) {
        Fixture unsupported(Stub(), Real(), "#define DisabledMacro 0\n#if " + std::string(expression) + "\n" + docs + "#endif\n");
        Require(!HasActiveJapiPlaceholders(unsupported.Request()), "probe does not evaluate active or unsupported conditional expressions");
        Reject(unsupported, "malformed docs outside an exact literal-zero proof remain structural errors");
    }
    for (const auto alternative : {"#else\n", "#elif 1\n", "#elif 0\n", "#elseif 1\n"}) {
        Fixture branched(Stub(), Real(), "#if 0\n" + docs + alternative + docs + "#endif\n");
        Require(!HasActiveJapiPlaceholders(branched.Request()), "same-level alternative prevents whole-block masking");
        Reject(branched, "same-level else/elif/unsupported elseif cannot grant closed-block proof");
    }
    const std::vector<std::string> continuedOrTemplate = {
        "#if 0 \\\n+ 0\n" + docs + "#endif\n",
        "#if 0\n#define DocumentValue \\\n1\n" + docs + "#endif\n",
        "#if 0\n" + docs + "#el\\\nse\n" + docs + "#endif\n",
        "#if 0\n" + docs + "#en\\\ndif\n",
        "#if 0\n<?='\\n'?>\n" + docs + "#endif\n"
    };
    for (const auto& tail : continuedOrTemplate) {
        Fixture unsupported(Stub(), Real(), tail);
        Require(!HasActiveJapiPlaceholders(unsupported.Request()), "continuation or template does not grant closed-block proof");
        Reject(unsupported, "all physical splices and templates anywhere in closed range are unsupported");
    }

    // 直接请求负例：真实PP应先报错的输入也不能让服务相信部分来源或恢复后删除。
    const auto rejectRawTail = [](std::string tail, std::string_view why) {
        Fixture fixture;
        fixture.input += tail;
        Require(!HasActiveJapiPlaceholders(fixture.Request()), why);
        Reject(fixture, why);
    };
    rejectRawTail("#if 0\n" + docs, "unclosed raw conditional cannot be accepted from partial scan");
    rejectRawTail("#endif\n", "unmatched closing conditional remains an error");
    rejectRawTail("#if 0\n#else\n#else\n#endif\n", "duplicate alternatives remain an error");
    rejectRawTail("// continued comment \\\n#if 0\n" + docs + "#endif\n",
        "physical opening hidden by spliced line comment is not a real PP directive");
    rejectRawTail("/\\\n* comment\n#if 0\n*/\n" + docs + "#endif\n",
        "spliced block-comment opener cannot manufacture closed-block ownership");
    rejectRawTail("<? generatedPrefix = 1 ?> #if 0\n" + docs + "#endif\n",
        "template masking cannot manufacture a directive at logical line start");

    std::string depthAtLimit;
    for (int i = 0; i < 256; ++i) depthAtLimit += "#if 0\n";
    depthAtLimit += docs;
    for (int i = 0; i < 256; ++i) depthAtLimit += "#endif\n";
    Require(Fixture(Stub(), Real(), depthAtLimit).Run().Succeeded(), "bounded 256 nested literal-zero blocks are supported");
    Fixture tooDeep(Stub(), Real(), "#if 0\n" + depthAtLimit + "#endif\n");
    Require(!HasActiveJapiPlaceholders(tooDeep.Request()), "257 conditional levels reject probe before owned provider");
    Reject(tooDeep, "conditional-depth overflow rejects full coordination without partial masking");
    std::string tooManyBlocks;
    for (int i = 0; i < 65537; ++i) tooManyBlocks += "#if 0\n#endif\n";
    Fixture overBudget(Stub(), Real(), tooManyBlocks);
    Require(!HasActiveJapiPlaceholders(overBudget.Request()), "conditional count budget rejects probe before owned provider");
    Reject(overBudget, "conditional count overflow does not accept partial raw scan");
}
} // namespace

int main() {
    try {
        TestSuccessfulBatch();
        TestNoopBoundaries();
        TestAmbiguityAndAbi();
        TestScopes();
        TestInterfacePrototypes();
        TestActiveProbe();
        TestUncollidedHooks();
        TestExpansionContract();
        TestPureNewlineTemplates();
        TestCollisionBudget();
        TestProvenance();
        TestTemplateGate();
        TestLiteralDisabledDocumentation();
        std::cout << "ScriptJapiPlaceholder: 13 original scenario groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ScriptJapiPlaceholder: " << error.what() << '\n';
        return 1;
    }
}
