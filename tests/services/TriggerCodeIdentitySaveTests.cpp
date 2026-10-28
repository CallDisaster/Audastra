#include "../support/TriggerCodeIdentitySaveFixture.h"
#include <iostream>

namespace {
using namespace AudastraTest::CodeIdentity;
template<class T> bool Code(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
void SaveAndReadback(bool copy) {
    Fixture f; auto request = f.Request(copy ? "identity-copy" : "identity-rename");
    const auto parsed = Take(ParseTriggerCodeIdentityEdit(Envelope(f.Edit("CodeNew", copy), true)));
    request.triggerCodeIdentity = parsed;
    auto preview = Take(SaveMapEditCandidate(request));
    Require(preview.previewOnly && !std::filesystem::exists(request.targetCandidate), "dry run has no map output");
    Require(preview.codeIdentityGraph && preview.codeIdentityGraph->before.currentWtgSha256 == parsed.request.expected.currentWtgSha256
        && preview.codeIdentityGraph->after.currentWtgSha256 != preview.codeIdentityGraph->before.currentWtgSha256,
        "preview reports exact before and candidate bindings without publishing");
    request.triggerCodeIdentity->dryRun = false;
    const auto saved = Take(SaveMapEditCandidate(request));
    Require(saved.codeIdentity && saved.codeIdentity->changed && !saved.previewOnly, "same normal save chain");
    auto archive = Take(MapArchiveSession::Open(saved.candidate));
    const auto wtg = Take(archive.ReadMember("war3map.wtg")), wct = Take(archive.ReadMember("war3map.wct"));
    Require(saved.codeIdentityGraph && saved.codeIdentityGraph->before.currentWtgSha256 == parsed.request.expected.currentWtgSha256
        && saved.codeIdentityGraph->after.currentWtgSha256 == Take(Sha256(wtg))
        && saved.codeIdentityGraph->after.currentWctSha256 == Take(Sha256(wct)), "reported final bindings match independent archive bytes");
    auto doc = Take(WtgWctCodec{}.Parse(wtg, wct, f.catalog));
    const auto row = std::find_if(doc.triggers.begin(), doc.triggers.end(), [](const auto& t) { return t.name.text == "CodeNew"; });
    Require(row != doc.triggers.end() && row->customText.text.find("function InitTrig_CodeNew") != std::string::npos, "independent pair reopens renamed initializer");
    Require(row->customText.text.find("// InitTrig_CodeSource stays in comment\r\n") != std::string::npos, "comment bytes retained in normal map");
    Require(doc.triggers.size() == f.triggers.triggers.size() + (copy ? 1 : 0), "copy count versus rename");
    const auto jassBytes = Take(archive.ReadMember("war3map.j"));
    const std::string jass(reinterpret_cast<const char*>(jassBytes.data()), jassBytes.size());
    Require(jass.find("call InitTrig_CodeNew(") != jass.npos && jass.find("function Trig_CodeNew_Action") != jass.npos, "actual normal generated script follows new identity");
    Require(Take(MapArchiveSession::Open(f.source.path)).SourceSha256() == f.source.sha256, "original map untouched");
    auto stale = f.Request("identity-stale"); stale.triggerCodeIdentity = parsed; stale.triggerCodeIdentity->dryRun = false;
    stale.triggerCodeIdentity->request.expectedCodeSha256.assign(64, '0');
    Require(Code(SaveMapEditCandidate(stale), "TRIGGER_CODE_SOURCE_STALE") && !std::filesystem::exists(stale.targetCandidate), "stale code does not publish");
    stale = f.Request("identity-lua"); stale.triggerCodeIdentity = parsed; stale.luaTarget.emplace();
    Require(Code(SaveMapEditCandidate(stale), "TRIGGER_CODE_LUA_UNSUPPORTED"), "actual selected Lua refused");
}
}
int main() { try { SaveAndReadback(false); SaveAndReadback(true); std::cout << "code identity normal-save cases passed\n"; return 0; }
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
