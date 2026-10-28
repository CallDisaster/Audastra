#include "MapEditSaveCommand.h"
#include "MapCustomSettingsCommand.h"
#include "../src/Services/MapEditSaveValidation.h"
#include "support/TerrainResizeFixture.h"
#include "support/TerrainTextureSlotSaveFixture.h"
#include "support/MapCustomSettingsFixture.h"
#include "support/TriggerCodeIdentitySaveFixture.h"
#include "War3MapCore/Services/MapEditSaveService.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Invocation {
    int exitCode = 0;
    std::string output, error;
};

Invocation Invoke(std::vector<std::wstring> arguments, int (*run)(int, wchar_t**) = RunMapEditSave) {
    std::vector<wchar_t*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    std::ostringstream output, error;
    auto* previousOutput = std::cout.rdbuf(output.rdbuf());
    auto* previousError = std::cerr.rdbuf(error.rdbuf());
    try {
        const auto exitCode = run(static_cast<int>(argv.size()), argv.data());
        std::cout.rdbuf(previousOutput);
        std::cerr.rdbuf(previousError);
        return {exitCode, output.str(), error.str()};
    } catch (...) {
        std::cout.rdbuf(previousOutput);
        std::cerr.rdbuf(previousError);
        throw;
    }
}

std::vector<std::wstring> Arguments() {
    // 参数门禁必须在打开此不存在的来源之前终止；不依赖本机地图、游戏或工具。
    return {L"war3-map-cli", L"map-edit-save", L"missing-minimap-cli-source.w3x",
        std::wstring(64, L'a'), std::wstring(64, L'b'), L"unused-tool", L"unused-game",
        L"missing-minimap-cli-candidate.w3x"};
}

void Rejected(const Invocation& result, std::string_view detail) {
    Require(result.exitCode == 2 && result.output.empty(), "invalid option must fail before any receipt");
    Require(result.error.find("SAVE_ARGUMENT_INVALID") != std::string::npos
        && result.error.find(detail) != std::string::npos, "argument rejection must identify its exact scope");
}

void LegacyMinimapSwitchesAreRemoved() {
    auto arguments = Arguments();
    arguments.push_back(L"--generate-missing-minimap");
    Rejected(Invoke(arguments), "unknown map-edit-save option");

    arguments = Arguments();
    arguments.push_back(L"--generate-missing-playable-minimap");
    Rejected(Invoke(arguments), "unknown map-edit-save option");
}

void ShadowCalculationUsesTheSharedSaveService() {
    auto arguments = Arguments(); arguments.push_back(L"--rebuild-shadows");
    const auto result = Invoke(arguments);
    Require(result.exitCode != 0 && result.output.empty()
        && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "explicit shadow calculation without strokes must reach the shared source identity gate");
    arguments.push_back(L"--rebuild-shadows");
    Rejected(Invoke(arguments), "may appear only once");
    arguments = Arguments(); arguments[4] = L"-"; arguments.push_back(L"--rebuild-shadows");
    const auto missing = Invoke(arguments);
    Require(missing.exitCode != 0 && missing.error.find("SAVE_REQUEST_INVALID") != std::string::npos,
        "shadow-only save must require the actual W3E identity");
}

void TypedTextureAndWaterReachTheSaveService() {
    const auto stroke = [](std::wstring kind, std::wstring value) {
        auto args = Arguments();
        args.insert(args.end(), {L"--stroke", std::move(kind), L"circle", L"4", L"4", L"0", std::move(value)});
        return Invoke(std::move(args));
    };
    for (const auto result : {stroke(L"texture", L"Lgrs"), stroke(L"water-height", L"0"),
            stroke(L"water_height", L"-1.25")}) {
        Require(result.exitCode != 0 && result.output.empty()
            && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos,
            "valid typed terrain parameters must reach the shared source checks");
    }
    Rejected(stroke(L"texture", L"bad"), "four-byte ASCII rawcode");
    Rejected(stroke(L"texture", L"中文"), "four-byte ASCII rawcode");
    Rejected(stroke(L"water-height", L"nan"), "water height must be finite");
    Rejected(stroke(L"water-height", L"2048"), "water height must be finite");
}

void RemoveWaterUsesOnlyItsFootprintAndSharedSaveService() {
    const auto base = [] {
        auto args = Arguments();
        args.insert(args.end(), {L"--remove-water", L"square", L"0", L"4", L"16"});
        return args;
    };
    auto args = base();
    for (const auto shape : {L"circle", L"square"}) {
        args[9] = shape;
        Require(Invoke(args).error.find("SOURCE_LEASE_FAILED") != std::string::npos,
            "remove-water reaches source checks without a water-height, seed, cliff slot or value");
    }
    for (const auto [index, value] : std::vector<std::pair<std::size_t, std::wstring>>{
            {9, L"triangle"}, {10, L"-1"}, {10, L"4294967296"}, {11, L"nan"}, {12, L"17"}}) {
        args = base(); args[index] = value; Rejected(Invoke(args), "radius 0..16");
    }
    args = base(); args.pop_back(); Rejected(Invoke(args), "requires shape");
    args = base(); args.push_back(L"true"); Rejected(Invoke(args), "unknown map-edit-save option");
    args = Arguments();
    for (unsigned i = 0; i < 129; ++i)
        args.insert(args.end(), {L"--remove-water", L"circle", L"4", L"4", L"0"});
    Rejected(Invoke(args), "at most 128 ordered strokes");
    args = base(); args.insert(args.end(), {L"--stroke", L"raise", L"circle", L"4", L"4", L"0", L"8"});
    Require(Invoke(args).error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "remove-water and height commands keep one ordered request");
}

void NativeWaterUsesExplicitTypedArguments() {
    const auto base = [] {
        auto args = Arguments();
        args.insert(args.end(), {L"--water-stroke", L"shallow", L"circle", L"32", L"32",
            L"1", L"0", L"4294967295", L"true"});
        return args;
    };
    auto args = base();
    for (const auto kind : {L"shallow", L"deep"}) {
        args[9] = kind;
        const auto result = Invoke(args);
        Require(result.exitCode != 0 && result.output.empty()
            && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos,
            "typed water stroke must reach shared source checks without being treated as WaterHeight");
    }
    args = base(); args[13] = L"9";
    Rejected(Invoke(args), "size 1..8");
    args = base(); args[13] = L"0";
    Rejected(Invoke(args), "size 1..8");
    args = base(); args[14] = L"15";
    Rejected(Invoke(args), "cliff slot 0..14");
    args = base(); args[15] = L"4294967296";
    Rejected(Invoke(args), "uint32 vertex indices and seed");
    args = base(); args[16] = L"1";
    Rejected(Invoke(args), "enforce-limits must be true or false");
    args = base(); args.pop_back();
    Rejected(Invoke(args), "requires eight arguments");
}

void CliffUsesTypedClassicSizeAndDoesNotAliasTextureToLevel() {
    const auto base = [](bool textureOnly) {
        auto args = Arguments();
        args.insert(args.end(), {textureOnly ? L"--cliff-texture" : L"--cliff-stroke",
            L"square", L"32", L"32", L"2", L"1"});
        if (!textureOnly) args.insert(args.end(), {L"-1", L"4294967295"});
        return args;
    };
    for (const bool textureOnly : {false, true}) {
        const auto result = Invoke(base(textureOnly));
        Require(result.exitCode != 0 && result.output.empty()
            && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos
            && result.error.find("SAVE_STROKE_KIND") == std::string::npos,
            "typed cliff level and pure texture must pass kind validation and reach the actual source gate");
        for (const auto size : {L"0", L"9", L"1.5", L"-1"}) {
            auto args = base(textureOnly); args[12] = size;
            Rejected(Invoke(args), "size 1..8");
        }
        for (const auto slot : {L"15", L"-1", L"0.5"}) {
            auto args = base(textureOnly); args[13] = slot;
            Rejected(Invoke(args), "cliff slot 0..14");
        }
        auto args = base(textureOnly); args[10] = L"4294967296";
        Rejected(Invoke(args), "uint32 vertex indices");
        args = base(textureOnly); args[9] = L"triangle";
        Rejected(Invoke(args), "shape must be circle or square");
        args = base(textureOnly); args.pop_back();
        Rejected(Invoke(args), textureOnly ? "requires five arguments" : "requires seven arguments");
    }
    for (const auto delta : {L"-2", L"2", L"0.5"}) {
        auto args = base(false); args[14] = delta;
        Rejected(Invoke(args), "level delta -1..1");
    }
    for (const auto seed : {L"-1", L"4294967296", L"1.5"}) {
        auto args = base(false); args[15] = seed;
        Rejected(Invoke(args), "uint32 seed");
    }
    auto args = base(true); args.push_back(L"4294967295");
    Rejected(Invoke(args), "unknown map-edit-save option");
    args = Arguments();
    for (int i = 0; i < 129; ++i)
        args.insert(args.end(), {L"--cliff-texture", L"circle", L"32", L"32", L"1", L"0"});
    Rejected(Invoke(args), "at most 128 ordered strokes");
}

void RampUsesExplicitBooleanAndTheSharedSaveGate() {
    const auto base = [] {
        auto args = Arguments();
        args.insert(args.end(), {L"--stroke", L"ramp", L"circle", L"4", L"4", L"0", L"true"});
        return args;
    };
    auto args = base();
    for (const auto value : {L"true", L"false"}) {
        args[14] = value;
        const auto result = Invoke(args);
        Require(result.exitCode != 0 && result.output.empty()
            && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos
            && result.error.find("SAVE_STROKE_KIND") == std::string::npos,
            "typed ramp creation and removal must pass shared stroke validation and reach actual source checks");
    }
    for (const auto value : {L"1", L"0", L"True", L"set", L"nan"}) {
        args = base(); args[14] = value;
        Rejected(Invoke(args), "ramp requires circle, radius 0 and a true or false value");
    }
    args = base(); args[13] = L"1";
    Rejected(Invoke(args), "ramp requires circle, radius 0 and a true or false value");
    args = base(); args[10] = L"square";
    Rejected(Invoke(args), "ramp requires circle, radius 0 and a true or false value");
    args = base(); args[9] = L"unsupported";
    Rejected(Invoke(args), "stroke kind must be");
    args = base(); args.pop_back();
    Rejected(Invoke(args), "requires six arguments");
}

void BoundaryUsesStrictBooleanAndSharedOrderedSave() {
    const auto base = [] {
        auto args = Arguments();
        args.insert(args.end(), {L"--stroke", L"boundary", L"circle", L"4", L"4", L"0", L"true"});
        return args;
    };
    for (const auto value : {L"true", L"false"}) for (const auto shape : {L"circle", L"square"})
        for (const auto radius : {L"0", L"16"}) {
            auto args = base(); args[14] = value; args[10] = shape; args[13] = radius;
            const auto result = Invoke(args);
            Require(result.exitCode != 0 && result.output.empty()
                && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos
                && result.error.find("SAVE_STROKE_KIND") == std::string::npos,
                "boundary paint/clear must pass typed validation and reach the shared source gate");
        }
    for (const auto value : {L"1", L"0", L"True", L"set", L"nan"}) {
        auto args = base(); args[14] = value; Rejected(Invoke(args), "boundary value must be true or false");
    }
    for (const auto radius : {L"-1", L"17", L"0.5"}) {
        auto args = base(); args[13] = radius; Rejected(Invoke(args), "radius must be 0..16");
    }
    for (const auto x : {L"-1", L"1.5", L"4294967296"}) {
        auto args = base(); args[11] = x; Rejected(Invoke(args), "unsigned vertex indices");
    }
    auto args = base(); args[10] = L"triangle"; Rejected(Invoke(args), "stroke shape must be circle or square");
    args = base(); args.pop_back(); Rejected(Invoke(args), "requires six arguments");
    args = base(); args.push_back(L"extra"); Rejected(Invoke(args), "unknown map-edit-save option");
    args = base(); args.insert(args.end(), {L"--terrain-bounds", L"1", L"2", L"17", L"14"});
    Require(Invoke(args).error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "boundary and playable bounds retain distinct intents in one ordered save request");
    args = Arguments();
    for (unsigned i = 0; i < 129; ++i)
        args.insert(args.end(), {L"--stroke", L"boundary", L"square", L"4", L"4", L"0", L"false"});
    Rejected(Invoke(args), "at most 128 ordered strokes");
}

void CornerRampUsesOneWorldPointAndStrictBoolean() {
    const auto base = [] {
        auto args = Arguments();
        args.insert(args.end(), {L"--corner-ramp", L"1056", L"1056", L"true"});
        return args;
    };
    for (const auto value : {L"true", L"false"}) {
        auto args = base(); args[11] = value;
        const auto result = Invoke(args);
        Require(result.exitCode != 0 && result.output.empty()
            && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos
            && result.error.find("SAVE_STROKE_KIND") == std::string::npos,
            "corner creation/removal must reach the actual shared map source gate");
    }
    for (const auto value : {L"nan", L"inf", L"16777217", L"-16777217"}) {
        auto args = base(); args[9] = value;
        Rejected(Invoke(args), "requires finite world");
    }
    auto args = base(); args[11] = L"1";
    Rejected(Invoke(args), "requires finite world");
    args = base(); args.pop_back();
    Rejected(Invoke(args), "requires world x, world y");
}

void BoundsUseIntegerHalfOpenCellsAndShareOrderedCommandLimit() {
    const auto base = [] {
        auto args = Arguments(); args.insert(args.end(), {L"--terrain-bounds", L"1", L"2", L"17", L"14"}); return args;
    };
    const auto result = Invoke(base());
    Require(result.exitCode != 0 && result.output.empty() && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "valid half-open bounds reach the actual shared source identity gate");
    for (const auto value : {L"-1", L"0.5", L"nan", L"4294967296"}) {
        auto args = base(); args[9] = value;
        Rejected(Invoke(args), "nonempty integer half-open");
    }
    for (const auto value : {L"0", L"1", L"2049"}) {
        auto args = base(); args[11] = value;
        Rejected(Invoke(args), "nonempty integer half-open");
    }
    auto incomplete = base(); incomplete.pop_back(); Rejected(Invoke(incomplete), "requires west south east north");
    auto tooMany = Arguments();
    for (unsigned i = 0; i < 129; ++i) tooMany.insert(tooMany.end(), {L"--terrain-bounds", L"1", L"2", L"17", L"14"});
    Rejected(Invoke(tooMany), "at most 128 terrain");
    auto mixed = base(); mixed.insert(mixed.end(), {L"--stroke", L"raise", L"circle", L"4", L"4", L"0", L"8"});
    Require(Invoke(mixed).error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "bounds and brush share one ordered service request without separate save routes");
}

#include "CliStraightRampCases.h"
#include "CliTerrainBlockCases.h"
#include "CliTerrainPathingCases.h"
#include "CliTerrainResizeCases.h"
#include "CliTerrainTextureSlotCases.h"
#include "CliTriggerCodeIdentityCases.h"

} // namespace

namespace {
void CustomSettingsUseSharedSave() {
    namespace C = AudastraTest::CustomSettings;
    C::Fixture fixture; auto direct = fixture.Request("direct-tech"); direct.settingsCustomEdits = fixture.AppendUpgrade();
    std::vector<std::wstring> queryArgs{L"war3-map-cli", L"map-settings-custom-query", fixture.source.path.wstring(), std::wstring(fixture.source.sha256.begin(), fixture.source.sha256.end())};
    const auto query = Invoke(queryArgs, RunMapCustomSettingsQuery);
    Require(query.exitCode == 0 && query.output.find("\"native_v25_writable\":true") != std::string::npos, "CLI independent custom query reads actual native source");
    queryArgs[3].assign(64, L'a'); Require(Invoke(queryArgs, RunMapCustomSettingsQuery).exitCode != 0, "CLI query rejects stale source SHA");
    C::Take(War3Map::Services::SaveMapEditCandidate(direct));
    const auto json = fixture.base.root / "commands.json";
    const auto request = *direct.settingsCustomEdits;
    C::Write(json, "{\"expected_member_sha256\":\"" + request.expectedMemberSha256 + "\",\"operations\":[{\"section\":\"upgrades\",\"op\":\"append\",\"expected_count\":0,\"value\":{\"player_mask\":1,\"id_hex\":\"52303031\",\"level\":1,\"availability\":2}}]}");
    const auto target = fixture.base.root / "cli-tech.w3x";
    std::vector<std::wstring> args{L"war3-map-cli", L"map-edit-save", fixture.source.path.wstring(),
        std::wstring(fixture.source.sha256.begin(), fixture.source.sha256.end()), L"-", fixture.base.inputs.tool.wstring(), fixture.base.inputs.game.wstring(), target.wstring(),
        L"--settings-custom-edit-file", json.wstring()};
    const auto result = Invoke(args); if (result.exitCode != 0) throw std::runtime_error(result.error);
    auto actual = C::Take(War3Map::MapArchiveSession::Open(target)); auto expected = C::Take(War3Map::MapArchiveSession::Open(direct.targetCandidate));
    for (const auto* member : {"war3map.w3i", "war3map.j", "war3map.w3e", "war3map.wpm", "unchanged.bin"})
        Require(C::Take(actual.ReadMember(member)) == C::Take(expected.ReadMember(member)), "CLI custom settings and common save agree");
    args[7] = (fixture.base.root / "cli-no-prefix.w3x").wstring();
    C::Write(json, "{\"expected_member_sha256\":\"" + request.expectedMemberSha256 + "\",\"operations\":[{\"section\":\"upgrades\",\"op\":\"append\",\"expected_count\":1,\"value\":{\"player_mask\":1,\"id_hex\":\"52303031\",\"level\":1,\"availability\":2}}]}");
    Require(Invoke(args).exitCode != 0 && !std::filesystem::exists(fixture.base.root / "cli-no-prefix.w3x"), "CLI stale CAS does not publish");
    if (War3Map::Script::RandomMapInitializationAvailable()) {
        auto random = fixture.Request("direct-random"); random.settingsCustomEdits = fixture.AppendRandomUnits();
        C::Take(War3Map::Services::SaveMapEditCandidate(random));
        C::Write(json, "{\"expected_member_sha256\":\"" + random.settingsCustomEdits->expectedMemberSha256
            + "\",\"operations\":" + C::Fixture::RandomUnitOperationsJson() + "}");
        args[7] = (fixture.base.root / "cli-random.w3x").wstring();
        const auto saved = Invoke(args); Require(saved.exitCode == 0, saved.error.c_str());
        auto cliRandom = C::Take(War3Map::MapArchiveSession::Open(fixture.base.root / "cli-random.w3x"));
        auto directRandom = C::Take(War3Map::MapArchiveSession::Open(random.targetCandidate));
        for (const auto* member : {"war3map.w3i", "war3map.j", "unchanged.bin"})
            Require(C::Take(cliRandom.ReadMember(member)) == C::Take(directRandom.ReadMember(member)), "CLI random table normal save matches the same service");
    }
}
}
int main() {
    try {
        LegacyMinimapSwitchesAreRemoved();
        ShadowCalculationUsesTheSharedSaveService();
        TypedTextureAndWaterReachTheSaveService();
        RemoveWaterUsesOnlyItsFootprintAndSharedSaveService();
        NativeWaterUsesExplicitTypedArguments();
        CliffUsesTypedClassicSizeAndDoesNotAliasTextureToLevel();
        RampUsesExplicitBooleanAndTheSharedSaveGate();
        BoundaryUsesStrictBooleanAndSharedOrderedSave();
        CornerRampUsesOneWorldPointAndStrictBoolean();
        StraightRampUsesItsOwnWorldPointCommand();
        TerrainBlockCopyUsesBoundedTypedCommand();
        PathingRestrictionUsesTypedSharedSave();
        BoundsUseIntegerHalfOpenCellsAndShareOrderedCommandLimit();
        TerrainResizeUsesSignedBeforeCanvasEdges();
        TerrainTextureSlotsUseSharedOrderedSave();
        CustomSettingsUseSharedSave();
        CustomCodeIdentityCliUsesNormalSave();
        std::cout << "Map edit save CLI minimap option gates passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
