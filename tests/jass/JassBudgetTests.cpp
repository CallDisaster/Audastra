#include "War3MapCore/Jass/JassChecker.h"
#include "../../src/Jass/JassCheckBudget.h"
#include "../../src/Jass/JassLexer.h"
#include "../../src/Jass/JassSyntax.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

// 原创危险输入：每个硬门禁都有合法正例或精确边界；不执行脚本或启动游戏。
namespace {

using namespace War3Map;
using namespace War3Map::Jass;
using namespace War3Map::Jass::detail;

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

bool Has(const JassCheckResult& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& item) { return item.severity == DiagnosticSeverity::Error && item.code == code; });
}

JassCheckResult Check(std::string_view text, const JassCheckOptions& options = {}) {
    const std::vector<JassSource> sources{{"budget.j", text}};
    return CheckJass(sources, options);
}

void Hard(const JassCheckResult& result, std::string_view code, const std::string& label) {
    if (result.completed || result.Succeeded() || !Has(result, code)) {
        std::string actual;
        for (const auto& item : result.diagnostics) actual += item.code + ": " + item.message + "\n";
        throw std::runtime_error(label + ": expected hard " + std::string(code) + "\n" + actual);
    }
}

std::string Returning(std::string_view expression) {
    return "function Probe takes nothing returns integer\nreturn " + std::string(expression) + "\nendfunction\n";
}

std::string Chain(std::size_t terms) {
    std::string result = "1";
    for (std::size_t i = 1; i < terms; ++i) result += "+1";
    return result;
}

void TestBytesSourcesTokens() {
    const auto text = Returning("1");
    auto options = JassCheckOptions{};
    options.maxSourceBytes = text.size();
    options.maxTotalBytes = text.size();
    Require(Check(text, options).Succeeded(), "exact source and total bytes pass");
    --options.maxSourceBytes;
    Hard(Check(text, options), "JASS_SOURCE_BYTES_BUDGET", "source one byte over");
    options.maxSourceBytes = text.size();
    --options.maxTotalBytes;
    Hard(Check(text, options), "JASS_TOTAL_BYTES_BUDGET", "total one byte over");

    const std::vector<JassSource> empty{{"one.j", ""}, {"two.j", ""}};
    options = {};
    options.maxSources = 2;
    options.maxTokens = 2;
    const auto exact = CheckJass(empty, options);
    Require(exact.Succeeded() && exact.tokens == 2, "each empty source consumes an End token");
    options.maxSources = 1;
    Hard(CheckJass(empty, options), "JASS_SOURCE_COUNT_BUDGET", "source count one over");
    options.maxSources = 2;
    options.maxTokens = 1;
    Hard(CheckJass(empty, options), "JASS_TOKEN_BUDGET", "global token count includes both End tokens");
    options = {};
    const auto baseline = Check(text);
    Require(baseline.Succeeded() && baseline.tokens > 1, "token baseline is an actual completed check");
    options.maxTokens = baseline.tokens;
    Require(Check(text, options).Succeeded(), "exact token budget passes");
    --options.maxTokens;
    Hard(Check(text, options), "JASS_TOKEN_BUDGET", "token one over returns no partial success");
    options = {};
    options.maxSourceWorkers = 0;
    Hard(Check(text, options), "JASS_CHECK_OPTIONS", "zero worker limit is invalid");
}

void TestAstAndPools() {
    const auto text = Returning("1");
    const auto baseline = Check(text);
    Require(baseline.Succeeded() && baseline.astNodes > 0 && baseline.maxAstDepthObserved == 2,
        "return literal reports expression and statement depth");
    auto options = JassCheckOptions{};
    options.maxAstNodes = baseline.astNodes;
    Require(Check(text, options).Succeeded(), "exact AST budget passes");
    --options.maxAstNodes;
    Hard(Check(text, options), "JASS_AST_NODE_BUDGET", "AST one over");
    options = {};
    options.maxPoolNodes = 2;
    Require(Check(text, options).Succeeded(), "one expression and one statement fit exact pool budget");
    options.maxPoolNodes = 1;
    Hard(Check(text, options), "JASS_POOL_NODE_BUDGET", "pool one over");

    options = {};
    options.maxAstDepth = 16;
    Require(Check(Returning(Chain(15)), options).Succeeded(), "flat binary chain fits real depth16 including return");
    Hard(Check(Returning(Chain(2000)), options), "JASS_AST_DEPTH_BUDGET", "unparenthesized left-deep AST is bounded");
    options.maxAstDepth = 8;
    const std::string call = "native Sink takes integer n returns nothing\nfunction Probe takes nothing returns nothing\ncall Sink(" + Chain(8) + ")\nendfunction\n";
    Hard(Check(call, options), "JASS_AST_DEPTH_BUDGET", "call child arguments contribute depth");

    options = {};
    options.maxAstDepth = 3;
    const std::string branch = "function Probe takes nothing returns nothing\nif true then\nelse\nif true then\ncall Missing()\nendif\nendif\nendfunction\n";
    options.syntaxOnly = true;
    Hard(Check(branch, options), "JASS_AST_DEPTH_BUDGET", "else branch statement subtree contributes depth");
    options.maxAstDepth = 4;
    Require(Check(branch, options).Succeeded(), "same else subtree passes exact real depth4 syntax gate");

    options = {};
    options.maxPoolNodes = 8;
    options.syntaxOnly = true;
    Hard(Check("function Probe takes nothing returns nothing\ncall Missing(1,2,3,4,5)\nendfunction\n", options),
        "JASS_POOL_NODE_BUDGET", "call argument edges count toward pool budget");
}

void TestRecursiveNesting() {
    auto options = JassCheckOptions{};
    options.maxNesting = 8;
    Require(Check(Returning("((((1))))"), options).Succeeded(), "legal nested parentheses fit budget");
    Hard(Check(Returning(std::string(2000, '(') + "1" + std::string(2000, ')')), options),
        "JASS_NESTING_BUDGET", "parentheses guard runs before recursion");
    Hard(Check(Returning(std::string(2000, '-') + "1"), options), "JASS_NESTING_BUDGET", "unary guard before recursion");
    std::string call = "1";
    for (int i = 0; i < 40; ++i) call = "Identity(" + call + ")";
    Hard(Check("native Identity takes integer n returns integer\n" + Returning(call), options),
        "JASS_NESTING_BUDGET", "nested call parameters guard before recursion");
    std::string body = "function Probe takes nothing returns nothing\n";
    for (int i = 0; i < 40; ++i) body += "loop\n";
    for (int i = 0; i < 40; ++i) body += "endloop\n";
    body += "endfunction\n";
    Hard(Check(body, options), "JASS_NESTING_BUDGET", "nested statement parsing cannot exhaust stack");
}

void TestDiagnosticsAndSyntaxMode() {
    const std::string unknown = "function Probe takes nothing returns integer\ncall Unknown()\nreturn missing\nendfunction\n";
    auto options = JassCheckOptions{};
    const auto full = Check(unknown, options);
    Require(full.completed && full.semanticChecked && !full.Succeeded() && Has(full, "JASS_UNDECLARED"),
        "default checker still performs complete declaration semantics");
    options.syntaxOnly = true;
    const auto syntax = Check(unknown, options);
    Require(syntax.completed && !syntax.semanticChecked && syntax.Succeeded(), "syntaxOnly explicitly skips missing declaration noise");
    const auto malformed = Check("function Probe takes nothing returns nothing\nif true then\nendfunction\n", options);
    Require(malformed.completed && !malformed.semanticChecked && !malformed.Succeeded() && Has(malformed, "JASS_SYNTAX"),
        "syntaxOnly retains actual syntax failures");

    options = {};
    options.maxProducedDiagnostics = 2;
    Require(Check(unknown, options).completed, "exact two produced diagnostics completes with normal errors");
    options.maxProducedDiagnostics = 1;
    options.maxErrors = 0;
    Hard(Check("//# +nosemanticerror +nosyntaxerror\n" + unknown, options), "JASS_DIAGNOSTIC_BUDGET",
        "pragma and maxErrors0 cannot hide diagnostic production failure");
    options = {};
    options.maxErrors = 0;
    Require(Has(Check(unknown, options), "JASS_TOO_MANY_ERRORS"), "normal errors remain failures when display cap is zero");
    options.syntaxOnly = true;
    options.maxAstDepth = 8;
    Hard(Check("//# +nosemanticerror +nosyntaxerror\n" + Returning(Chain(1000)), options), "JASS_AST_DEPTH_BUDGET",
        "syntax-only budget failure survives both pragmas and zero display cap");
}

void TestWorkersAndControl() {
    std::string many;
    for (int i = 0; i < 400; ++i) many += "function F" + std::to_string(i) + " takes nothing returns nothing\ncall Missing()\nendfunction\n";
    std::vector<JassSource> sources{{"first.j", many}, {"second.j", ""}, {"third.j", ""}};
    auto options = JassCheckOptions{};
    options.threads = 1;
    options.maxSourceWorkers = 1;
    options.maxErrors = 1000;
    const auto serial = CheckJass(sources, options);
    options.threads = 64;
    options.maxSourceWorkers = 2;
    options.maxBodyWorkers = 3;
    const auto parallel = CheckJass(sources, options);
    Require(serial.completed && parallel.completed && serial.threadsUsed == 1 && serial.sourceThreadsUsed == 1
        && parallel.threadsUsed == 3 && parallel.sourceThreadsUsed == 2, "worker pools honor both explicit ceilings");
    Require(serial.diagnostics.size() == parallel.diagnostics.size() && serial.astNodes == parallel.astNodes
        && serial.tokens == parallel.tokens, "serial and bounded parallel checks produce same counts");
    for (std::size_t i = 0; i < serial.diagnostics.size(); ++i) {
        Require(serial.diagnostics[i].code == parallel.diagnostics[i].code
            && serial.diagnostics[i].message == parallel.diagnostics[i].message, "parallel normal diagnostics remain deterministic");
    }
    options.maxAstNodes = 32;
    Hard(CheckJass(sources, options), "JASS_AST_NODE_BUDGET", "source worker budget exception is caught");
    options = {};
    options.threads = 4;
    options.maxPoolNodes = 1;
    Hard(CheckJass(sources, options), "JASS_POOL_NODE_BUDGET", "body worker budget exception is caught");

    std::stop_source stop;
    stop.request_stop();
    options = {};
    options.cancel = stop.get_token();
    options.maxErrors = 0;
    Hard(Check(Returning("1"), options), "JASS_CHECK_CANCELLED", "already cancelled returns explicit incomplete error");
    options = {};
    options.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    Hard(Check(Returning("1"), options), "JASS_CHECK_DEADLINE", "expired caller deadline");
    options = {};
    options.maxDuration = std::chrono::milliseconds::zero();
    Hard(Check(Returning("1"), options), "JASS_CHECK_DEADLINE", "zero duration does not disable the clock budget");
}

void TestDiagnosticTextAmplificationAndLexicalClassification() {
    const std::string parameter(8192, 'p');
    std::string text = "native F takes integer " + parameter + " returns nothing\nfunction Probe takes nothing returns nothing\n";
    for (int i = 0; i < 16; ++i) text += "call F(null)\n";
    text += "endfunction\n";
    auto options = JassCheckOptions{};
    const auto result = Check(text, options);
    Require(result.completed && Has(result, "JASS_TYPE"), "long parameter fixture produces actual type errors");
    for (const auto& item : result.diagnostics) {
        Require(item.message.size() <= options.maxDiagnosticMessageBytes && item.message.find(parameter) == std::string::npos,
            "names are summarized before repeated diagnostic construction");
    }
    options.maxDiagnosticBytes = 1024;
    options.maxErrors = 0;
    auto ignored = text;
    ignored.insert(ignored.find("function Probe"), "//# +nosemanticerror\n");
    Hard(Check(ignored, options), "JASS_DIAGNOSTIC_BYTES_BUDGET",
        "small total diagnostic byte budget prevents repeated long-name amplification");

    const std::string badUtf8 = "//" + std::string(1, static_cast<char>(0xff));
    options = {};
    CheckBudget budget(options);
    const auto lexical = LexJassBounded(badUtf8, Text::SourceEncoding::Utf8, {}, 32, &budget);
    Require(!lexical.Succeeded() && !lexical.diagnostics.empty(), "encoded lexer rejects actual invalid source bytes");
    const auto originalCode = lexical.diagnostics.front().code;
    try { budget.RejectLexical(lexical.diagnostics); Require(false, "reject lexical must abort"); }
    catch (const CheckAborted& error) { Require(error.Cause() == CheckFailure::Lexical, "bad encoding is not token budget"); }
    const auto retained = budget.TakeLexicalDiagnostics();
    Require(!retained.empty() && retained.front().code == originalCode && retained.front().origin
        && retained.front().origin->offset == 2, "original encoded-source diagnostic and byte origin remain available");
}

// 固定取消/截止时间合同；不依赖长 token 扫描和另一个线程的调度比赛。
void LongTokenStopContract(std::string text, const LexOptions& lexerOptions,
    TokenKind expected, const std::string& label) {
    std::stop_source stop;
    JassCheckOptions options;
    options.cancel = stop.get_token();
    CheckBudget budget(options);
    budget.RegisterSource(text.size());
    stop.request_stop();
    bool interrupted = false;
    try { (void)LexJassBounded(text, lexerOptions, 16, &budget); }
    catch (const CheckAborted& error) { interrupted = error.Cause() == CheckFailure::Cancelled; }
    Require(interrupted && budget.Failure() == CheckFailure::Cancelled && budget.Tokens() == 0,
        label + " pre-cancellation must report exact hard cause without partial token accounting");

    options = {};
    options.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    CheckBudget expired(options);
    interrupted = false;
    try { (void)LexJassBounded(text, lexerOptions, 16, &expired); }
    catch (const CheckAborted& error) { interrupted = error.Cause() == CheckFailure::Deadline; }
    Require(interrupted && expired.Failure() == CheckFailure::Deadline && expired.Tokens() == 0,
        label + " expired deadline must report exact hard cause without partial token accounting");

    options = {};
    CheckBudget active(options);
    active.RegisterSource(text.size());
    const auto completed = LexJassBounded(text, lexerOptions, 16, &active);
    const auto count = expected == TokenKind::End ? std::size_t{1} : std::size_t{2};
    Require(completed.Succeeded() && completed.value && completed.value->size() == count
        && completed.value->front().kind == expected && completed.value->back().kind == TokenKind::End
        && completed.value->back().offset == text.size() && active.Failure() == CheckFailure::None,
        label + " active lexer must finish the entire original input with exact token kinds and End offset");
    // JASS lexer 由调用者在完整成功后记账；PP lexer 则在内部 push 时记账。
    active.ConsumeTokens(completed.value->size());
    Require(active.Tokens() == count, label + " successful complete token accounting must retain its actual count");
}

void TestLongTokenCancellationAndLargeCompatibility() {
    constexpr std::size_t bytes = 16 * 1024 * 1024;
    LongTokenStopContract("//" + std::string(bytes, 'x'), {}, TokenKind::End, "single long line comment");
    LongTokenStopContract("/*" + std::string(bytes, 'x') + "*/", {true, false}, TokenKind::End, "single long block comment");
    LongTokenStopContract("\"" + std::string(bytes, 'x') + "\"", {}, TokenKind::String, "single long string");
    LongTokenStopContract(std::string(bytes, 'x'), {}, TokenKind::Identifier, "single long identifier");
    LongTokenStopContract("'" + std::string(bytes, 'x') + "'", {}, TokenKind::Rawcode, "single long rawcode");
    // 8MiB 可接受来源保留小语法树；证明字节预算不误挡真实大小，真实地图上下文另行 opt-in。
    const std::string large = "//" + std::string(8 * 1024 * 1024, 'x') + "\n" + Returning("1");
    const auto result = Check(large);
    Require(result.Succeeded() && result.completed && result.semanticChecked && result.tokens < 64,
        "8MiB legal source completes with sparse tokens and bounded initial reservation");
    std::cout << "large_source_bytes=" << large.size() << " tokens=" << result.tokens << " ast=" << result.astNodes
        << " maxDepth=" << result.maxAstDepthObserved << '\n';
}

} // namespace

int main() {
    try {
        TestBytesSourcesTokens();
        TestAstAndPools();
        TestRecursiveNesting();
        TestDiagnosticsAndSyntaxMode();
        TestWorkersAndControl();
        TestDiagnosticTextAmplificationAndLexicalClassification();
        TestLongTokenCancellationAndLargeCompatibility();
        std::cout << "JASS budget tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JASS budget tests failed: " << error.what() << '\n';
        return 1;
    }
}
