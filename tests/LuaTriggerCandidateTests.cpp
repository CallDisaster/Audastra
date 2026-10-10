#include "War3MapCore/Services/LuaTriggerCandidate.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
void TestLuaTriggerCandidate(const Trigger::UiCatalog& catalog, const Script::LuaTargetProfile& target) {
    // 原创经典线格式，取得真实 seal；只含一变量和两个空 GUI 触发器。
    std::vector<std::byte> wtg, wct;
    auto* bytes = &wtg;
    const auto i = [&](std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) bytes->push_back(static_cast<std::byte>(value >> shift)); };
    const auto z = [&](std::string_view value) { for (const char c : value) bytes->push_back(static_cast<std::byte>(c)); bytes->push_back(std::byte{0}); };
    i(0x21475457); i(7); i(1); i(1); z("Category"); i(0); i(2); i(1);
    z("Count"); z("integer"); i(1); i(0); i(1); i(1); z("2"); i(2);
    for (const char* name : {"One", "Two"}) { z(name); z(""); i(0); i(1); i(0); i(0); i(0); i(1); i(0); }
    bytes = &wct; i(1); z(""); i(0); i(2); i(0); i(0);
    const auto parsed = Trigger::WtgWctCodec{}.Parse(wtg, wct, catalog);
    Require(parsed.Succeeded(), "candidate source parse");
    auto document = *parsed.value;
    LuaCandidateProfile profile{target, {}};
    const auto input = PrepareLuaTriggerInput(document, catalog, profile);
    Require(input.Succeeded(), "candidate snapshot");
    const auto revision = input.value->Identity()->revision;
    const auto candidate = CompileLuaTriggerCandidate(*input.value, revision);
    Require(candidate.Succeeded(), "candidate compile");
    const auto unit = candidate.value->Unit();
    const auto sourcePage = QueryLuaTriggerSources(*candidate.value, revision, unit->scriptSha256, 0, 0, 128);
    Require(sourcePage.Succeeded() && sourcePage.value->spans == unit->sourceSpans
        && sourcePage.value->sourceMapSha256.size() == 64, "source map enumeration and identity");
    const auto variable = std::find_if(unit->sourceSpans.begin(), unit->sourceSpans.end(),
        [&](const auto& span) { return span.nodeId == document.variables[0].id; });
    Require(variable != unit->sourceSpans.end(), "stable variable source absent");
    const auto line = 1u + static_cast<std::size_t>(std::count(unit->text.begin(),
        unit->text.begin() + static_cast<std::ptrdiff_t>(variable->byteOffset), '\n'));
    const auto location = QueryLuaTriggerSources(*candidate.value, revision, unit->scriptSha256, line, 0, 1);
    Require(location.Succeeded() && location.value->innermost == *variable
        && unit->text.substr(location.value->lineByteOffset, location.value->lineByteLength).starts_with("udg_Count"),
        "actual generated line must resolve to the variable identity");
    const auto eof = 1u + static_cast<std::size_t>(std::count(unit->text.begin(), unit->text.end(), '\n'));
    const auto emptyLine = QueryLuaTriggerSources(*candidate.value, revision, unit->scriptSha256, eof, 0, 1);
    Require(emptyLine.Succeeded() && !emptyLine.value->innermost && emptyLine.value->totalMatches == 0, "trailing empty line");
    for (const auto badLine : {eof + 1, std::numeric_limits<std::size_t>::max()})
        Require(!QueryLuaTriggerSources(*candidate.value, revision, unit->scriptSha256, badLine, 0, 1).Succeeded(), "invalid source line");
    Require(!QueryLuaTriggerSources(*candidate.value, revision, "wrong-script", line, 0, 1).Succeeded()
        && !QueryLuaTriggerSources(*candidate.value, "old-input", unit->scriptSha256, line, 0, 1).Succeeded()
        && !QueryLuaTriggerSources(*candidate.value, revision, unit->scriptSha256, 0, 0, 129).Succeeded(), "source map binding and cap");
    Require(!CompileLuaTriggerCandidate(*input.value, std::string(64, '0')).Succeeded(), "stale compile");
    document.variables[0].value.text = "3";
    profile.target.identity += "/changed";
    const auto changed = PrepareLuaTriggerInput(document, catalog, profile);
    Require(changed.Succeeded() && changed.value->Identity()->revision != revision, "edit identity unchanged");
    const auto repeated = CompileLuaTriggerCandidate(*input.value, revision);
    Require(repeated.Succeeded() && repeated.value->Unit()->text == candidate.value->Unit()->text
        && repeated.value->Unit()->targetIdentity == target.identity
        && repeated.value->Unit()->sourceSpans == unit->sourceSpans
        && repeated.value->Unit()->sourceMapSha256 == unit->sourceMapSha256, "snapshot borrowed mutable inputs");
    Require(!QueryLuaTriggerCandidate(*candidate.value, changed.value->Identity()->revision, 0, 32).Succeeded(), "stale candidate accepted");
    std::string reconstructed;
    for (std::size_t offset = 0; offset < candidate.value->Unit()->text.size(); offset += 31) {
        const auto page = QueryLuaTriggerCandidate(*candidate.value, revision, offset, 31);
        Require(page.Succeeded() && page.value->scriptSha256 == candidate.value->Unit()->scriptSha256, "candidate page");
        reconstructed.append(reinterpret_cast<const char*>(page.value->bytes.data()), page.value->bytes.size());
    }
    Require(reconstructed == candidate.value->Unit()->text, "candidate pages truncate data");
    Require(!QueryLuaTriggerCandidate(*candidate.value, revision, 0, 65537).Succeeded(), "page cap bypass");
    const auto beforeId = PrepareLuaTriggerInput(document, catalog, profile);
    document.triggers[0].id += 0x100000;
    const auto afterId = PrepareLuaTriggerInput(document, catalog, profile);
    Require(beforeId.Succeeded() && afterId.Succeeded() && beforeId.value->Identity()->wtgSha256 == afterId.value->Identity()->wtgSha256
        && beforeId.value->Identity()->revision != afterId.value->Identity()->revision, "stable symbol ID omitted from revision");
    document.triggers[0].customTextFlag = 1; document.triggers[0].customText.text = "// untranslated";
    const auto handwritten = PrepareLuaTriggerInput(document, catalog, profile);
    Require(handwritten.Succeeded(), "handwritten source snapshot");
    const auto rejected = CompileLuaTriggerCandidate(*handwritten.value, handwritten.value->Identity()->revision);
    Require(!rejected.Succeeded() && !rejected.sourceDiagnostics.empty()
        && rejected.sourceDiagnostics[0].triggerId == document.triggers[0].id, "JASS source diagnostic lost typed identity");
    Require(!PrepareLuaTriggerInput(Trigger::TriggerDocument{}, catalog, profile).Succeeded(), "unsealed candidate accepted");
    auto constantProfile = LuaCandidateProfile{target, {}};
    constantProfile.target.constants.push_back({"FORGE_TEST_ENGINE_CONSTANT", "integer"});
    const auto withConstant = PrepareLuaTriggerInput(*parsed.value, catalog, constantProfile);
    Require(withConstant.Succeeded() && withConstant.value->Identity()->targetRequestSha256
        != input.value->Identity()->targetRequestSha256, "typed constant omitted from frozen target identity");
    constantProfile.target.constants.back().type = "real";
    const auto withOtherType = PrepareLuaTriggerInput(*parsed.value, catalog, constantProfile);
    Require(withOtherType.Succeeded() && withOtherType.value->Identity()->targetRequestSha256
        != withConstant.value->Identity()->targetRequestSha256, "constant type omitted from frozen target identity");
    const std::string declaration = "forge-lua-target-v1\r\nidentity\toffline-stub\r\ntype\tunit\thandle\r\napi\tKillUnit\tnothing\tunit\r\ncallback\tDone\t12\r\n";
    const auto manifest = ParseLuaCandidateProfile(declaration);
    Require(manifest.Succeeded() && manifest.value->target.apis[0].parameters == std::vector<std::string>{"unit"}
        && manifest.value->callbacks[0].triggerId == 12, "profile TSV decode");
    const auto typedConstant = ParseLuaCandidateProfile(declaration + "constant\tCAMERA_FIELD_FARZ\tcamerafield\n");
    Require(typedConstant.Succeeded() && typedConstant.value->target.constants.size() == 1
        && typedConstant.value->target.constants[0].name == "CAMERA_FIELD_FARZ"
        && typedConstant.value->target.constants[0].type == "camerafield", "typed engine constant TSV decode");
    for (const auto suffix : {"identity\tx\n", "api\tBad\n", "unknown\tx\n", "callback\tF\t-1\n"})
        Require(!ParseLuaCandidateProfile(declaration + suffix).Succeeded(), "invalid profile accepted");
}
