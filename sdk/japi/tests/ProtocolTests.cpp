#include "Test.h"
#include "aj/Protocol.h"
#include <bit>
#include <clocale>
#include <cmath>
#include <limits>
#include <random>

using namespace audastra::japi;
int failures = 0;
unsigned checks = 0;
namespace {
int32_t Parse(std::string args) {
    Request r;
    return ParseRequest("ajapi:2;1;1;test.call" + args, r);
}
void CodecTests() {
    Request r;
    Check(ParseRequest("ajapi:2;0;1;host.hello", r) == AJ_OK && r.epoch == 0 && r.id == 1, "hello request");
    Check(ParseRequest("ajapi:2;1;2;test.call;i:-2147483648;d:-1;b:1;r:1.25e-2;s:a%3Bb%25", r) == AJ_OK
        && r.arguments.size() == 5 && r.arguments[1].integer == -1 && r.arguments[4].text == "a;b%", "all argument types");
    for (const char* good : {";i:2147483647", ";i:-2147483648", ";i:0", ";d:-2147483648", ";r:.5", ";r:-0", ";r:1.", ";r:1e+2", ";s:", ";s:中文", ";s:a|b"})
        Check(Parse(good) == AJ_OK, good);
    for (const char* bad : {";i:+1", ";i: 1", ";i:1 ", ";i:2147483648", ";i:-2147483649", ";i:0x10", ";i:",
        ";r:nan", ";r:inf", ";r:0x1p2", ";r:+2", ";r:1e", ";r:1e999", ";r:1e-999", ";r:.e2", ";r:1_2",
        ";b:2", ";b:true", ";s:%", ";s:%0Z", ";s:%00", ";s:%1F", ";s:%7F", ";s:%C0%80", ";s:%ED%A0%80", ";s:%F4%90%80%80", ";s:%80", ";v:", ";"})
        Check(Parse(bad) != AJ_OK, bad);
    Check(Parse(";s:" + std::string(384, 'a')) == AJ_OK, "384 encoded bytes accepted");
    Check(Parse(";s:" + std::string(385, 'a')) == AJ_LIMIT, "385 encoded bytes rejected");
    Check(Parse(";s:" + std::string(500, 'a')) == AJ_LIMIT, "frame limit checked before parsing");
    const std::string fullPrefix = "ajapi:2;1;1;test.call;s:" + std::string(384, 'a') + ";s:";
    const std::string fullFrame = fullPrefix + std::string(AJ_MAX_FRAME - fullPrefix.size(), 'b');
    Check(ParseRequest(fullFrame, r) == AJ_OK && ParseRequest(fullFrame + "b", r) == AJ_LIMIT, "exact 512/513-byte boundary");
    std::string args;
    for (int i = 0; i < 16; ++i) args += ";i:1";
    Check(Parse(args) == AJ_OK && Parse(args + ";i:1") == AJ_LIMIT, "argument count boundary");
    for (const char* bad : {"ajapi:2;01;1;a.b", "ajapi:2;1;0;a.b", "ajapi:2;1;-1;a.b", "ajapi:2;1;1;a..b", "ajapi:2;1;1;a.B", "ajapi:2;1;1;a.b;"})
        Check(ParseRequest(bad, r) != AJ_OK, bad);
    Check(ParseRequest("ajapi:3;1;1;a.b", r) == AJ_VERSION, "unknown version fail closed");
    Check(ValidName("demo") && !ValidName("0demo") && !ValidName("../demo"), "module name grammar");
    Check(ValidFunction("AjDispatch") && !ValidFunction("fn;call"), "dispatcher identifier grammar");
    Check(ValidText("🙂中文") && !ValidText(std::string("a\0b", 3)), "UTF-8 and embedded NUL");
    Response response; response.epoch = 1; response.id = 9;
    response.value = {AJ_TEXT, 0, 0, "a;%中文|🙂"};
    Response decoded;
    Check(ParseResponse(EncodeResponse(response), decoded) == AJ_OK && decoded.value.text == response.value.text, "response UTF-8/escape round trip");
    response.value.text = std::string(AJ_MAX_OUTPUT_TEXT, 'a');
    Check(EncodeResponse(response).size() <= AJ_MAX_FRAME && ParseResponse(EncodeResponse(response), decoded) == AJ_OK
        && decoded.status == AJ_OK, "maximum response text fits frame");
    response.value.text += 'a';
    Check(ParseResponse(EncodeResponse(response), decoded) == AJ_OK && decoded.status == AJ_LIMIT, "output limit rejects, not truncates");
    response.value = {AJ_REAL, 0, std::numeric_limits<float>::infinity(), {}};
    Check(ParseResponse(EncodeResponse(response), decoded) == AJ_OK && decoded.status == AJ_BACKEND, "nonfinite output rejected");
    response.status = AJ_BUSY;
    Check(EncodeResponse(response).ends_with(";10;v:"), "error never masquerades as a success value");
    Check(ParseResponse("ajapi:2;1;2;10;i:0", decoded) != AJ_OK, "error value schema enforced");
    // Decimal parse/format is locale-independent and preserves every finite float sampled.
    Check(std::setlocale(LC_NUMERIC, "German_Germany.1252") != nullptr, "non-C numeric locale active for roundtrip test");
    std::mt19937 random(0xA02);
    response.status = AJ_OK;
    bool roundtrip = true;
    for (int i = 0; i < 20000; ++i) {
        const float f = std::bit_cast<float>(static_cast<uint32_t>(random()));
        if (!std::isfinite(f)) continue;
        response.value = {AJ_REAL, 0, f, {}};
        if (ParseResponse(EncodeResponse(response), decoded) != AJ_OK || decoded.status != AJ_OK
            || std::bit_cast<uint32_t>(decoded.value.real) != std::bit_cast<uint32_t>(f)) roundtrip = false;
    }
    Check(roundtrip, "20k float samples roundtrip independent of locale");
    std::setlocale(LC_NUMERIC, "C");
    // Deterministic malformed corpus: accepted frames must always satisfy the public invariants.
    bool invariant = true;
    for (int i = 0; i < 25000; ++i) {
        std::string wire = i % 2 ? "ajapi:2;1;1;test.call;s:" : "ajapi:2;";
        const auto n = random() % 550;
        for (unsigned j = 0; j < n; ++j) wire += static_cast<char>(random() & 255);
        if (ParseRequest(wire, r) == AJ_OK) {
            invariant &= wire.size() <= AJ_MAX_FRAME && ValidCommand(r.command) && r.arguments.size() <= AJ_MAX_ARGUMENTS;
            for (const auto& a : r.arguments) if (a.kind == AJ_TEXT) invariant &= ValidText(a.text);
        }
    }
    Check(invariant, "25k malformed-frame corpus invariant");
}
}
int main() {
    CodecTests();
    HostTests();
    std::printf("JAPI contract: %u checks, %d failures (plus 45000 deterministic codec samples)\n", checks, failures);
    return failures ? 1 : 0;
}
