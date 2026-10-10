#include "War3MapCore/Text/SourceEncoding.h"
#include "War3MapCore/Text/Utf8Scalar.h"
#include "War3MapCore/Text/WarcraftText.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace War3Map::Text;

namespace {

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void CheckRanges(const WarcraftTextResult& result, std::string_view source) {
    std::uint32_t previous = 0;
    for (const auto& run : result.runs) {
        Require(run.sourceOffset >= previous && run.sourceOffset <= source.size()
            && run.sourceBytes <= source.size() - run.sourceOffset, "source range escaped accepted input");
        Require(run.textOffset <= result.plainText.size() && run.textBytes <= result.plainText.size() - run.textOffset,
            "owned text range escaped result");
        previous = run.sourceOffset + run.sourceBytes;
    }
    Require(result.plainText.size() <= result.consumedSourceBytes, "output exceeded consumed source");
}

void Formatting() {
    const std::string source = "A|cff00ff00中文|rB|n|C10203040😀|R||cffbad <img src=x>\r\nZ\rQ\n";
    const auto original = source;
    const auto result = ParseWarcraftText(source);
    Require(result.Complete() && source == original, "formatting changed source or did not complete");
    Require(result.plainText == "A中文B\n😀|cffbad <img src=x>\nZ\nQ\n", "formatting or escaped pipe semantics lost text");
    Require(result.lineCount == 5 && result.diagnostics.empty(), "line count or valid escape diagnostics wrong");
    Require(result.runs.size() == 11 && result.runs[1].argb == 0xff00ff00u && result.runs[4].argb == 0x10203040u,
        "typed ARGB was not preserved or reset run lost");
    Require(!result.runs[2].argb && !result.runs[5].argb, "reset did not restore default style");
    CheckRanges(result, source);
    Require(WarcraftColourCode(0x12123456u) == "|cff123456", "colour insertion was not fixed ff RGB");
    Require(ParseWarcraftText("|cffffffff|r|n||").plainText == "\n|", "empty style runs or escape newline failed");
    const std::string malformed = "|c12G45678X|cshort|x|";
    const auto bad = ParseWarcraftText(malformed);
    Require(bad.Complete() && bad.plainText == malformed && bad.diagnostics.size() == 4,
        "malformed colour/unknown escape swallowed source bytes");
    Require(ParseWarcraftText("||n||r||cffffffff").plainText == "|n|r|cffffffff", "escaped codes were interpreted again");
    const std::string controls("A\0\x1a", 3);
    Require(ParseWarcraftText(controls).plainText == controls, "NUL or Ctrl-Z source was rewritten");
}

void BudgetsAndUtf8() {
    WarcraftTextLimits limits;
    limits.maxInputBytes = 3;
    Require(ParseWarcraftText("abc", limits).Complete(), "exact input boundary failed");
    const auto over = ParseWarcraftText("abcd", limits);
    Require(over.state == WarcraftTextState::Truncated && over.plainText == "abc" && over.consumedSourceBytes == 3,
        "input over-budget prefix was not honest");
    limits.maxInputBytes = 2;
    const auto split = ParseWarcraftText("中", limits);
    Require(split.state == WarcraftTextState::Truncated && split.plainText.empty(), "budget-split UTF-8 became invalid bytes");
    limits.maxInputBytes = 9;
    Require(ParseWarcraftText("|cff00ff00Z", limits).state == WarcraftTextState::Truncated,
        "partial colour marker at budget was accepted");
    limits = {}; limits.maxRuns = 2;
    const auto runs = ParseWarcraftText("A|cff00ff00B|rC", limits);
    Require(runs.state == WarcraftTextState::Truncated && runs.runs.size() == 2 && runs.plainText == "AB", "run cap was not enforced");
    limits = {}; limits.maxLines = 2;
    const auto lines = ParseWarcraftText("A|nB\nC", limits);
    Require(lines.state == WarcraftTextState::Truncated && lines.plainText == "A\nB" && lines.lineCount == 2, "line cap was not enforced");
    limits = {}; limits.maxDiagnostics = 1;
    const auto issues = ParseWarcraftText("|x|y|z", limits);
    Require(issues.Complete() && issues.diagnostics.size() == 1 && issues.omittedDiagnostics == 2 && issues.plainText == "|x|y|z",
        "diagnostic cap removed source or miscounted omissions");
    limits.maxRuns = 0;
    Require(ParseWarcraftText("unchanged", limits).state == WarcraftTextState::InvalidLimits, "zero required budget accepted");
    limits = {}; limits.maxInputBytes = 16u * 1024u * 1024u + 1u;
    Require(ParseWarcraftText("unchanged", limits).state == WarcraftTextState::InvalidLimits, "hard input cap bypassed");
    for (const auto& tail : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
        std::string("\xe4\xb8", 2), std::string("\x80", 1), std::string("\xe4X\xad", 3)}) {
        const std::string source = "A" + tail;
        const auto result = ParseWarcraftText(source);
        Require(result.state == WarcraftTextState::InvalidUtf8 && result.plainText == "A" && result.consumedSourceBytes == 1,
            "invalid UTF-8 was replaced, executed, or accepted");
        CheckRanges(result, source);
    }
    const std::string badColour = std::string("|cFFFF") + std::string("\x80", 1) + "FFF";
    Require(ParseWarcraftText(badColour).state == WarcraftTextState::InvalidUtf8, "bad colour hid invalid UTF-8");
}

void SourceCompatibility() {
    struct Valid { std::string bytes; std::uint32_t codepoint; std::uint8_t size; };
    for (const auto& fixture : std::vector<Valid>{{"A", 65, 1}, {std::string("\0", 1), 0, 1}, {"中", 0x4e2d, 3},
        {"😀", 0x1f600, 4}, {std::string("\xef\xbb\xbf", 3), 0xfeff, 3}}) {
        const auto helper = Detail::ReadUtf8Scalar(fixture.bytes, 0);
        const auto actual = ReadSourceScalar(fixture.bytes, 0, SourceEncoding::Utf8);
        Require(helper.Valid() && helper.codepoint == fixture.codepoint && helper.bytes == fixture.size,
            "shared helper changed valid scalar");
        Require(actual.Succeeded() && actual.value && actual.value->offset == 0 && actual.value->codepoint == helper.codepoint
            && actual.value->bytes == helper.bytes, "SourceEncoding delegation changed valid scalar contract");
    }
    struct Invalid { std::string bytes; const char* message; std::uint64_t length; };
    for (const auto& fixture : std::vector<Invalid>{{std::string("\xc0", 1), "UTF-8起始字节无效。", 1},
        {std::string("\xe4\xb8", 2), "UTF-8序列在来源末尾截断。", 2},
        {std::string("\xe4X\xad", 3), "UTF-8续字节无效。", 2},
        {std::string("\xed\xa0\x80", 3), "UTF-8过长编码、代理码点或越界码点无效。", 3}}) {
        const auto actual = ReadSourceScalar("A" + fixture.bytes, 1, SourceEncoding::Utf8);
        Require(!actual.Succeeded() && !actual.value && actual.diagnostics.size() == 1, "bad scalar accepted");
        const auto& diagnostic = actual.diagnostics.front();
        Require(diagnostic.code == "SOURCE_ENCODING_SEQUENCE_INVALID" && diagnostic.message == fixture.message
            && diagnostic.origin && diagnostic.origin->offset == 1 && diagnostic.origin->length == fixture.length,
            "delegation changed exact old error code/message/offset/length");
    }
    Require(ReadSourceScalar("", 0, SourceEncoding::Utf8).diagnostics.front().code == "SOURCE_ENCODING_OFFSET", "offset gate changed");
    Require(ReadSourceScalar("A", 0, static_cast<SourceEncoding>(999)).diagnostics.front().code == "SOURCE_ENCODING_INVALID", "enum gate changed");
    const std::string huge(16u * 1024u * 1024u + 1u, 'A');
    Require(ReadSourceScalar(huge, 0, SourceEncoding::Utf8).diagnostics.front().code == "SOURCE_ENCODING_BUDGET", "16MiB gate changed");
    const auto gbk = ReadSourceScalar(std::string("\xd6\xd0", 2), 0, SourceEncoding::Gbk);
    Require(gbk.Succeeded() && gbk.value && gbk.value->codepoint == 0x4e2d && gbk.value->bytes == 2, "unchanged CP936 path changed");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string group = argc > 1 ? argv[1] : "all";
        if (group == "format" || group == "all") Formatting();
        if (group == "budgets" || group == "all") BudgetsAndUtf8();
        if (group == "source" || group == "all") SourceCompatibility();
        if (group != "format" && group != "budgets" && group != "source" && group != "all") throw std::runtime_error("unknown test group");
        std::cout << "PASS Warcraft text " << group << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
