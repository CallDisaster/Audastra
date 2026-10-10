#include "War3MapCore/Jass/Preprocessor.h"
#include "War3MapCore/Jass/VJass.h"
#include "../../src/Jass/JassCheckBudget.h"
#include "../../src/Jass/PpLexer.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

// 原创行号/预算回归：只有测试按唯一标记定位输出；产品映射由 IR 和 TextEdit 实际发射建立。
namespace {
using namespace War3Map;
using namespace War3Map::Jass;
using namespace War3Map::Jass::detail;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T> std::string Diagnostics(const T& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.code + ": " + diagnostic.message + "\n";
    return text;
}

std::uint32_t LineAt(std::string_view text, std::size_t at) {
    return 1 + static_cast<std::uint32_t>(std::count(text.begin(), text.begin() + at, '\n'));
}

void Mapping(const VJassResult& result, std::string_view source, std::string_view marker, std::string_view sourceMarker) {
    Require(result.completed && result.Succeeded(), "translation completes: " + Diagnostics(result) + result.unsupported);
    const auto output = result.text.find(marker);
    const auto original = source.find(sourceMarker);
    Require(output != std::string::npos && original != std::string::npos, "unique mapping marker exists");
    const auto position = result.sourceMap.Find(LineAt(result.text, output));
    Require(position && position->line == LineAt(source, original), "IR output marker maps to actual source line: " + std::string(marker));
    Require(position->file < result.sourceMap.Files().size() && result.sourceMap.Files()[position->file] == "map.j", "source identity retained");
}

void ByteMapping(const VJassResult& result, std::string_view source) {
    std::size_t end = 0;
    for (const auto& origin : result.byteOrigins) {
        Require(origin.byteLength != 0 && origin.outputOffset == end && origin.byteLength <= result.text.size() - end,
            "actual emitted byte origins are sorted, nonoverlapping and cover all output");
        Require(origin.sourceOffset <= source.size() && origin.sourceLine == LineAt(source, origin.sourceOffset), "actual source byte/line identity");
        if (origin.exact) {
            Require(origin.byteLength <= source.size() - origin.sourceOffset &&
                std::string_view(result.text).substr(origin.outputOffset, origin.byteLength) == source.substr(origin.sourceOffset, origin.byteLength),
                "exact origins retain original bytes rather than inferred marker matches");
            Require(LineAt(source, origin.sourceOffset + origin.byteLength - 1) == origin.sourceLine, "exact original spans split at source line boundaries");
        }
        end += origin.byteLength;
    }
    Require(end == result.text.size() && !result.FindByteOrigin(end), "origins cover complete output and reject EOF");
}

void TestLibraryReorderAndEdits() {
    const std::string source =
        "// before libraries\n"
        "library Z requires A\n"
        "private function Probe takes nothing returns integer\n"
        "    return MissingZ\n"
        "endfunction\n"
        "endlibrary\n"
        "// actual second library\n"
        "library A\n"
        "globals\n"
        "    private integer Value = 3\n"
        "endglobals\n"
        "private function Probe takes nothing returns integer\n"
        "    debug call Unreachable()\n"
        "    return Value /* first\n"
        " second */ + MissingA\n"
        "endfunction\n"
        "endlibrary\n";
    const auto translated = TranslateVJass(source, "map.j");
    ByteMapping(translated, source);
    Require(translated.text.find("function A__Probe") < translated.text.find("function Z__Probe"), "actual library dependency reorders functions");
    Mapping(translated, source, "function A__Probe", "private function Probe takes nothing returns integer\n    debug");
    Mapping(translated, source, "return A__Value", "    return Value");
    Mapping(translated, source, "return MissingZ", "    return MissingZ");
    Mapping(translated, source, "integer A__Value", "    private integer Value");
    Require(translated.text.find("Unreachable") == std::string::npos && translated.text.find("second */") == std::string::npos,
        "debug deletion and multiline block collapse happened before mapping assertions");
    const std::vector<JassSource> sources{{"translated.j", translated.text}};
    const auto checked = CheckJass(sources);
    Require(checked.completed && checked.semanticChecked && !checked.Succeeded(), "actual backend reports undefined source variable");
    bool saw = false, sawAfterCollapsedComment = false;
    for (const auto& diagnostic : checked.diagnostics) {
        if (!diagnostic.origin) continue;
        const auto byte = translated.FindByteOrigin(static_cast<std::size_t>(diagnostic.origin->offset));
        if (diagnostic.message.find("MissingA") != std::string::npos) {
            std::cerr << "actual_missing_a diagnosticOffset=" << diagnostic.origin->offset << " diagnosticLength=" << diagnostic.origin->length
                << " outputTokenOffset=" << translated.text.find("MissingA") << " sourceTokenOffset=" << source.find("MissingA");
            if (byte) std::cerr << " mappedOutput=" << byte->outputOffset << " mappedSource=" << byte->sourceOffset
                << " mappedLine=" << byte->sourceLine << " mappedColumn=" << byte->sourceColumn << " exact=" << byte->exact;
            std::cerr << '\n';
            Require(byte && byte->exact && byte->sourceOffset == source.find("MissingA") && byte->sourceLine == 15 && byte->sourceColumn == 14,
                "actual backend byte offset locates MissingA on second original comment line despite same output line");
            sawAfterCollapsedComment = true;
        }
        if (diagnostic.message.find("MissingZ") == std::string::npos) continue;
        const auto line = LineAt(translated.text, static_cast<std::size_t>(diagnostic.origin->offset));
        const auto mapped = translated.sourceMap.Find(line);
        Require(mapped && mapped->line == LineAt(source, source.find("    return MissingZ")), "actual JASS diagnostic maps through emitter");
        Require(byte && byte->exact && byte->sourceOffset == source.find("MissingZ"), "actual backend MissingZ offset has exact original byte provenance");
        saw = true;
    }
    Require(saw && sawAfterCollapsedComment, "both backend semantic diagnostics exercised rather than fabricated");
    const auto renamed = translated.FindByteOrigin(translated.text.find("A__Value", translated.text.find("return")));
    Require(renamed && !renamed->exact && renamed->sourceOffset == source.find("Value /*"), "generated rename has original identifier anchor and does not claim exactness");
}

void TestCachedBodiesAndGeneratedAnchors() {
    const std::string source =
        "struct Probe\n"
        "    integer value = 7\n"
        "    method onDestroy takes nothing returns nothing\n"
        "        set MissingBody = this\n"
        "    endmethod\n"
        "endstruct\n"
        "function main takes nothing returns nothing\n"
        "    call InitBlizzard()\n"
        "endfunction\n";
    const auto translated = TranslateVJass(source, "map.j");
    ByteMapping(translated, source);
    Require(translated.completed && translated.Succeeded(), "struct generation: " + Diagnostics(translated) + translated.unsupported);
    std::size_t at = 0, copies = 0;
    while ((at = translated.text.find("set MissingBody = this", at)) != std::string::npos) {
        const auto position = translated.sourceMap.Find(LineAt(translated.text, at));
        Require(position && position->line == 4, "each reused onDestroy body retains actual body line, not declaration anchor");
        const auto byte = translated.FindByteOrigin(at + 4);
        Require(byte && byte->exact && byte->sourceOffset == source.find("MissingBody") && byte->sourceLine == 4,
            "each cached body copy preserves adjusted output origins and exact original MissingBody bytes");
        ++copies; ++at;
    }
    Require(copies >= 2, "cached body was actually reused");
    const auto generated = translated.FindByteOrigin(translated.text.find("function s__Probe__allocate"));
    Require(generated && !generated->exact && generated->sourceLine == 1, "generated allocator explicitly reports nearby struct anchor");
    Mapping(translated, source, "constant integer si__Probe", "struct Probe");
    Mapping(translated, source, "set s__Probe_value[this]", "    integer value");
    Mapping(translated, source, "function s__Probe__allocate", "struct Probe");
}

void TestCrLfByteFragments() {
    const std::string source = "library A\r\nglobals\r\nprivate integer Value=3\r\nendglobals\r\n"
        "function Probe takes nothing returns integer\r\nreturn Value /* first\r\n second */ + Missing\r\nendfunction\r\nendlibrary\r\n";
    const auto translated = TranslateVJass(source, "map.j");
    Require(translated.Succeeded(), "CRLF fragment fixture translates");
    ByteMapping(translated, source);
    const auto at = translated.text.find("Missing");
    const auto byte = translated.FindByteOrigin(at + 2);
    Require(byte && byte->exact && byte->sourceOffset == source.find("Missing") + 2 && byte->sourceLine == 7 && byte->sourceColumn == 16,
        "CRLF original fragments and lookup delta retain actual second-line byte column");
}

void TestActualBackendByteRanges() {
    const std::string source = "native Sink takes integer value returns nothing\n"
        "function Probe takes nothing returns nothing\n    call Unknown(1)\n    call Sink(\"wrong\")\n"
        "    call Sink(1,2)\n    set Missing = 1\nendfunction\n"
        "function Broken takes nothing returns integer\nendfunction\n";
    const std::vector<JassSource> sources{{"backend.j", source}};
    const auto checked = CheckJass(sources);
    Require(checked.completed && checked.semanticChecked && !checked.Succeeded()
        && checked.diagnosticOriginExact.size() == checked.diagnostics.size(), "backend origin precision aligns every final diagnostic");
    for (const auto& [code, token] : std::vector<std::pair<std::string, std::string>>{
            {"JASS_UNDECLARED", "Unknown(1)"}, {"JASS_UNDECLARED", "Missing"},
            {"JASS_TYPE", "\"wrong\""}, {"JASS_ARGUMENTS", "Sink(1,2)"}}) {
        const auto expected = source.find(token);
        bool saw = false;
        for (std::size_t i = 0; i < checked.diagnostics.size(); ++i) {
            const auto& issue = checked.diagnostics[i];
            if (issue.code != code || !issue.origin || issue.origin->offset != expected) continue;
            Require(checked.diagnosticOriginExact[i] && issue.origin->length == token.size(), "backend range comes from actual IR expression: " + token);
            saw = true;
        }
        Require(saw, "actual backend precise diagnostic missing: " + code + " " + token);
    }
    bool fallback = false;
    for (std::size_t i = 0; i < checked.diagnostics.size(); ++i)
        if (checked.diagnostics[i].code == "JASS_MISSING_RETURN") {
            Require(!checked.diagnosticOriginExact[i], "line-only return-flow diagnostic does not invent exact token range");
            fallback = true;
        }
    Require(fallback, "line-only fallback was actually exercised");
    JassCheckOptions invalid;
    invalid.maxBodyWorkers = 0;
    const auto options = CheckJass(sources, invalid);
    Require(!options.completed && options.diagnosticOriginExact.size() == options.diagnostics.size()
        && !options.diagnosticOriginExact.front(), "early hard diagnostic retains aligned false precision");
    std::stop_source stop;
    stop.request_stop();
    invalid = {}; invalid.cancel = stop.get_token();
    const auto cancelled = CheckJass(sources, invalid);
    Require(!cancelled.completed && cancelled.diagnosticOriginExact.size() == cancelled.diagnostics.size()
        && std::none_of(cancelled.diagnosticOriginExact.begin(), cancelled.diagnosticOriginExact.end(), [](bool exact) { return exact; }),
        "cancelled hard diagnostic retains aligned false precision");
}

void TestIncludeCompositionKeepsFileIdentity() {
    PreprocessOptions options;
    options.includeProvider = [](const std::filesystem::path& path) {
        PreprocessIncludeContent content;
        content.exists = true;
        if (path.filename() == "z.j") content.text = "library Z requires A\nfunction ZProbe takes nothing returns integer\nreturn MissingZ\nendfunction\nendlibrary\n";
        else if (path.filename() == "a.j") content.text = "library A\nfunction AProbe takes nothing returns integer\nreturn MissingA\nendfunction\nendlibrary\n";
        else content.exists = false;
        return Result<PreprocessIncludeContent>::Success(std::move(content));
    };
    const auto pp = Preprocess("#include \"z.j\"\n#include \"a.j\"\n", "main.j", "owned", options);
    Require(pp.completed && pp.Succeeded(), "provider-only preprocessing: " + Diagnostics(pp));
    const auto translated = TranslateVJass(pp.text, "combined.j");
    ByteMapping(translated, pp.text);
    Require(translated.completed && translated.Succeeded(), "preprocessed library reordering");
    const auto map = translated.sourceMap.ComposeWith(pp.sourceMap, LineAt(translated.text, translated.text.size()));
    for (const auto& [marker, file] : std::vector<std::pair<std::string, std::string>>{{"return MissingZ", "z.j"}, {"return MissingA", "a.j"}}) {
        const auto at = translated.text.find(marker);
        Require(at != std::string::npos, "include marker emitted");
        const auto position = map.Find(LineAt(translated.text, at));
        Require(position && position->line == 3 && position->file < map.Files().size(), "same line in two includes remains actual line 3");
        Require(std::filesystem::u8path(map.Files()[position->file]).filename() == file, "include identity not grafted onto main/another include");
    }
}

template<class T> void Hard(const T& result, std::string_view label) {
    Require(!result.completed && !result.Succeeded() && result.text.empty() && result.sourceMap.Empty(), "hard incomplete result clears executable text/map: " + std::string(label));
    if constexpr (requires { result.byteOrigins; }) Require(result.byteOrigins.empty(), "hard failure clears byte provenance");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& item) { return item.severity == DiagnosticSeverity::Error; }),
        "hard failure remains error despite maxErrors/pragma: " + std::string(label));
}

void TestVJassBudgets() {
    const std::string small = "function Probe takes nothing returns integer\nreturn 1\nendfunction\n";
    Require(TranslateVJass(small, "map.j").Succeeded(), "ordinary native JASS still translates");
    VJassOptions options;
    options.check.maxTokens = 3;
    options.check.maxErrors = 0;
    Hard(TranslateVJass("//# nosyntaxerror\n" + small, "map.j", options), "vJass token limit");
    options = {};
    options.check.maxAstDepth = 16;
    std::string chain = "1";
    for (int i = 0; i < 100; ++i) chain += "+1";
    Hard(TranslateVJass("function Probe takes nothing returns integer\nreturn " + chain + "\nendfunction\n", "map.j", options), "flat real AST depth before renderer");
    options = {};
    options.check.maxNesting = 12;
    std::string scopes;
    for (int i = 0; i < 20; ++i) scopes += "scope S" + std::to_string(i) + "\n";
    for (int i = 0; i < 20; ++i) scopes += "endscope\n";
    Hard(TranslateVJass(scopes, "map.j", options), "scope recursive scanner");
    std::string libraries;
    for (int i = 0; i < 30; ++i) libraries += "library L" + std::to_string(i) + (i < 29 ? " requires L" + std::to_string(i + 1) : "") + "\nendlibrary\n";
    Hard(TranslateVJass(libraries, "map.j", options), "dependency recursive ordering");
    options = {};
    options.check.maxSources = 1;
    options.baseScripts.push_back({"base.j", ""});
    Hard(TranslateVJass(small, "map.j", options), "base scripts share source budget");
    options = {};
    options.check.deadline = std::chrono::steady_clock::now();
    Hard(TranslateVJass(small, "map.j", options), "vJass deadline");
    std::stop_source cancel;
    cancel.request_stop();
    options = {};
    options.check.cancel = cancel.get_token();
    Hard(TranslateVJass(small, "map.j", options), "vJass cancellation");
}

void TestPpBudgetsAndCacheReuse() {
    PreprocessOptions options;
    options.check.maxErrors = 0;
    options.check.maxNesting = 20;
    std::string expression(40, '(');
    expression += "1" + std::string(40, ')');
    Hard(Preprocess("#if " + expression + "\nkept\n#endif\n", "pp.j", {}, options), "#if parser recursion");
    std::string invocation = "1";
    for (int i = 0; i < 35; ++i) invocation = "ID(" + invocation + ")";
    Hard(Preprocess("#define ID(x) x\n" + invocation + "\n", "pp.j", {}, options), "macro argument recursion");
    options = {};
    options.check.maxAstNodes = 400;
    invocation = "1";
    for (int i = 0; i < 12; ++i) invocation = "TWICE(" + invocation + ")";
    Hard(Preprocess("#define TWICE(x) x x\n" + invocation + "\n", "pp.j", {}, options), "macro copied tokens/edges");
    options = {};
    options.check.maxSources = 3; // predefined + main + first include；第二次缓存命中仍消费来源预算。
    options.includeProvider = [](const std::filesystem::path&) { return Result<PreprocessIncludeContent>::Success({true, "integer source\n"}); };
    PreprocessCache cache;
    const auto repeated = Preprocess("#include \"same.j\"\n#include \"same.j\"\n", "pp.j", "owned", options, &cache);
    Hard(repeated, "cached include cannot bypass cumulative source budget");
    Require(cache.Entries() > 0, "first include really entered cache");
    options = {};
    options.maxOutputBytes = 256;
    const auto over = Preprocess(std::string(300, 'x'), "pp.j", {}, options);
    Hard(over, "explicit output budget");
    options = {};
    options.check.deadline = std::chrono::steady_clock::now();
    Hard(Preprocess("integer safe\n", "pp.j", {}, options), "PP deadline");
}

void TestLongPpTokenCancellationContract() {
    const std::string source = "/*" + std::string(4u * 1024u * 1024u, 'a') + "*/";
    JassCheckOptions options;
    std::stop_source cancel;
    options.cancel = cancel.get_token();
    CheckBudget budget(options);
    budget.RegisterSource(source.size());
    // 来源登记之后、进入词法器之前确定地取消；不依赖机器速度或线程调度。
    cancel.request_stop();
    bool saw = false;
    try { (void)LexPpBounded(source, budget); }
    catch (const CheckAborted& aborted) { saw = aborted.Cause() == CheckFailure::Cancelled; }
    Require(saw && budget.Failure() == CheckFailure::Cancelled && budget.Tokens() == 0,
        "pre-cancelled PP lexer rejects the long-token source with exact cancellation cause and no partial tokens");

    options = {};
    options.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    CheckBudget expired(options);
    saw = false;
    try { (void)LexPpBounded(source, expired); }
    catch (const CheckAborted& aborted) { saw = aborted.Cause() == CheckFailure::Deadline; }
    Require(saw && expired.Failure() == CheckFailure::Deadline && expired.Tokens() == 0,
        "expired PP lexer deadline rejects the long-token source with exact deadline cause and no partial tokens");

    options = {};
    CheckBudget active(options);
    active.RegisterSource(source.size());
    const auto completed = LexPpBounded(source, active);
    Require(completed.Succeeded() && completed.value && completed.value->size() == 1
        && completed.value->front().kind == PpKind::End && active.Failure() == CheckFailure::None
        && active.Tokens() == 1,
        "same long block comment finishes normally with only its complete End token when no stop is requested");
}
} // namespace

int main() {
    try {
        TestLibraryReorderAndEdits();
        TestCachedBodiesAndGeneratedAnchors();
        TestCrLfByteFragments();
        TestActualBackendByteRanges();
        TestIncludeCompositionKeepsFileIdentity();
        TestVJassBudgets();
        TestPpBudgetsAndCacheReuse();
        TestLongPpTokenCancellationContract();
        std::cout << "vJass actual emitter source maps and PP/shared budgets passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
