#include "War3MapCore/Services/MapScriptTargetSelection.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::Services;

void Require(bool value, const std::string& reason) {
    if (!value) throw std::runtime_error(reason);
}

bool BytesEqual(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool Code(const Result<PreparedLuaMapTarget>& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

void Rejected(const Result<PreparedLuaMapTarget>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value && Code(result, code),
        "expected no prepared target and diagnostic " + std::string(code));
}

void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(static_cast<bool>(file), "write original synthetic fixture");
}

std::vector<std::byte> Settings(std::int32_t version, std::int32_t language) {
    MapData::MapSettings value;
    value.formatVersion = version; value.scriptLanguage = language;
    value.tileset = 'L'; value.mapName = "Original target map"; value.description = "Original description";
    value.custom.emplace(); value.flags = 0x80000040u; value.cameraMargins = {1, 2, 3, 4};
    MapData::MapPlayerSettings player;
    player.number = 7; player.controller = 1; player.race = 2; player.name = "Player seven";
    player.startX = -256.5f; player.startY = 1024.0f; value.players.push_back(player);
    if (version == 31) {
        value.gameVersion = {1, 32, 10, 3}; value.graphicsModes = 3; value.gameDataVersion = 1;
        value.players[0].enemyLowPriorityMask = 2;
    }
    const auto encoded = value.Serialize();
    Require(encoded.Succeeded() && encoded.value.has_value(), "serialize real W3I fixture");
    const auto parsed = MapData::MapSettings::Parse(*encoded.value);
    Require(parsed.Succeeded() && parsed.value->SourceFormatVersion() == version, "fixture actually parses its source version");
    return *encoded.value;
}

class Fixture {
public:
    std::filesystem::path root, checkerA, checkerB;
    Trigger::UiCatalog catalog;
    Trigger::TriggerDocument triggers;
    LuaMapTargetRequest request;

    Fixture() {
        root = std::filesystem::temp_directory_path() / ("forge-lua-target-selection-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directory(root), "create unique target selection directory");
        root = std::filesystem::canonical(root);
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,整数,integer\n");
        for (const auto* name : {"event.txt", "condition.txt", "action.txt", "call.txt"}) Write(root / "ui/base" / name, "");
        auto loaded = Trigger::UiCatalog::Load(root);
        Require(loaded.Succeeded() && loaded.value.has_value(), "capture actual synthetic UI catalog identity");
        catalog = std::move(*loaded.value);
        std::vector<std::byte> wtg, wct;
        auto* destination = &wtg;
        const auto integer = [&](std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8) destination->push_back(static_cast<std::byte>((value >> shift) & 0xffu));
        };
        const auto string = [&](std::string_view value) {
            for (const char c : value) destination->push_back(static_cast<std::byte>(c));
            destination->push_back(std::byte{});
        };
        integer(0x21475457u); integer(7); integer(1); integer(1); string("Root"); integer(0);
        integer(2); integer(0); integer(1);
        string("MapStart"); string(""); integer(0); integer(1); integer(0); integer(0); integer(0); integer(1); integer(0);
        destination = &wct; integer(1); string(""); integer(0); integer(1); integer(0);
        auto parsed = Trigger::WtgWctCodec{}.Parse(wtg, wct, catalog);
        Require(parsed.Succeeded() && parsed.value && parsed.value->HasTrustedSourceSeal(), "obtain real WTG/WCT parser seal");
        triggers = std::move(*parsed.value);
        request.profile.target.identity = "original-target-selection-fixture/identity-only";
        request.profile.target.handleTypes = {{"unit", "handle"}};
        request.profile.target.apis = {{"TestNative", "nothing", {"unit"}}};
        request.profile.target.constants = {{"TEST_VALUE", "integer"}};
        request.profile.callbacks = {{"MapStart", triggers.triggers[0].id}};
        std::wstring executable(32768, L'\0');
        const DWORD count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        Require(count != 0 && count < executable.size(), "find test executable");
        executable.resize(count);
        checkerA = root / "checker-a.exe"; checkerB = root / "checker-b.exe";
        Require(std::filesystem::copy_file(executable, checkerA), "copy test executable for file identity only");
        Require(std::filesystem::copy_file(executable, checkerB), "copy second identity fixture");
        std::filesystem::create_directory(root / "runtime-a");
        std::filesystem::create_directory(root / "runtime-b");
        // 只核文件绑定，不执行这两个文件，也不把可捕获身份当成有效Lua解释器。
        request.checker = Capture(checkerA);
    }

    Script::LuaSyntaxChecker Capture(const std::filesystem::path& path,
        std::span<const std::filesystem::path> directories = {}) const {
        const auto hash = Sha256File(path);
        Require(hash.Succeeded() && hash.value, "hash checker identity fixture");
        const auto captured = Script::CaptureLuaSyntaxChecker(path, *hash.value, directories);
        Require(captured.Succeeded() && captured.value && captured.value->Identity(), "capture checker file identity");
        return *captured.value;
    }

    Result<PreparedLuaMapTarget> Prepare(std::span<const std::byte> source,
        std::span<const std::byte> final, std::uint64_t revision = 11) const {
        return PrepareMapScriptTargetSelection(source, final, triggers, catalog, request, revision);
    }

    void RemoveSuccessfulFixture() {
        // 只删除本测试成功创建且核验仍是同一路径的私有目录；失败时保留完整样例。
        const auto checked = std::filesystem::canonical(root);
        Require(checked == root && root.filename().wstring().starts_with(L"forge-lua-target-selection-"), "verify private cleanup root");
        std::filesystem::remove_all(root);
    }
};

void PreservesSettingsAndOwnsInputs(Fixture& fixture) {
    auto source = Settings(31, 1);
    source.insert(source.end(), {std::byte{0xd3}, std::byte{0}, std::byte{0xfe}, std::byte{0x7a}});
    auto edited = MapData::MapSettings::Parse(source);
    Require(edited.Succeeded() && edited.value->UnknownTail().size() == 4, "opaque tail parsed and retained");
    edited.value->mapName = "Authored final title";
    auto encoded = edited.value->Serialize(); Require(encoded.Succeeded(), "serialize authored final metadata");
    auto final = *encoded.value;
    const auto prepared = fixture.Prepare(source, final, 91);
    Require(prepared.Succeeded() && prepared.value && prepared.value->Identity() && prepared.value->Profile(), "prepare explicit Lua map target");
    const auto& target = *prepared.value;
    const auto& identity = *target.Identity();
    Require(BytesEqual(target.SelectedSettings(), final), "already Lua selected settings preserve exact final input bytes");
    Require(identity.sourceSettingsSha256 == *Sha256(source).value && identity.inputSettingsSha256 == *Sha256(final).value
        && identity.selectedSettingsSha256 == identity.inputSettingsSha256 && identity.documentRevision == 91
        && identity.sourceLanguage == 1 && identity.sourceSettingsSha256 != identity.inputSettingsSha256,
        "source, input, selected settings and document revision are individually bound");
    Require(Code(prepared, "W3I_UNKNOWN_TAIL"), "selection preserves opaque-tail diagnostics without claiming complete initialization");
    const auto restored = MapData::MapSettings::Parse(target.SelectedSettings());
    Require(restored.Succeeded() && restored.value->gameVersion == std::array<std::int32_t, 4>{1, 32, 10, 3}
        && restored.value->players[0].number == 7 && restored.value->players[0].enemyLowPriorityMask == 2
        && restored.value->flags == 0x80000040u && restored.value->UnknownTail() == edited.value->UnknownTail(),
        "target selection does not change game version, owner layout, unsupported execution fields or tail");
    const auto requestHash = identity.targetRequestSha256;
    const auto targetName = fixture.request.profile.target.identity;
    auto oldRequest = fixture.request;
    fixture.request.profile.target.apis[0].parameters[0] = "integer";
    fixture.request.profile.target.constants[0].type = "real";
    fixture.request.profile.callbacks[0].symbol = "Changed";
    fixture.request.checker = {};
    std::fill(source.begin(), source.end(), std::byte{}); std::fill(final.begin(), final.end(), std::byte{});
    Require(target.Profile()->target.identity == targetName && target.Profile()->target.apis[0].parameters[0] == "unit"
        && target.Profile()->target.constants[0].type == "integer" && target.Profile()->callbacks[0].symbol == "MapStart"
        && target.Profile()->checker.Identity() && target.Profile()->requestSha256 == requestHash
        && MapData::MapSettings::Parse(target.SelectedSettings()).value->mapName == "Authored final title", "prepared profile and settings own their data");
    fixture.request = std::move(oldRequest);
}

void RequiresExplicitLanguageAndActualVersion(Fixture& fixture) {
    const auto jass31 = Settings(31, 0), lua31 = Settings(31, 1), classic = Settings(25, 0);
    Rejected(fixture.Prepare(jass31, jass31), "LUA_MAP_TARGET_LANGUAGE_SELECTION_REQUIRED");
    Rejected(fixture.Prepare(jass31, lua31), "LUA_MAP_TARGET_LANGUAGE_SELECTION_REQUIRED");
    Rejected(fixture.Prepare(lua31, jass31), "LUA_MAP_TARGET_LANGUAGE_SELECTION_REQUIRED");
    fixture.request.changeLanguageToLua = true;
    const auto selected = fixture.Prepare(jass31, jass31);
    Require(selected.Succeeded() && selected.value->Identity()->sourceLanguage == 0
        && BytesEqual(selected.value->SelectedSettings(), lua31), "explicit v31 internal JASS to Lua changes only language bytes");
    Require(selected.value->Identity()->sourceSettingsSha256 == selected.value->Identity()->inputSettingsSha256
        && selected.value->Identity()->selectedSettingsSha256 != selected.value->Identity()->inputSettingsSha256,
        "language change has a distinct selected settings identity");
    Rejected(fixture.Prepare(classic, lua31), "LUA_MAP_TARGET_VERSION_UNSUPPORTED");
    Rejected(fixture.Prepare(lua31, classic), "LUA_MAP_TARGET_VERSION_UNSUPPORTED");
    Rejected(fixture.Prepare(Settings(31, 9), lua31), "LUA_MAP_TARGET_LANGUAGE_UNKNOWN");
    Rejected(fixture.Prepare(lua31, Settings(31, 9)), "LUA_MAP_TARGET_LANGUAGE_UNKNOWN");
    std::vector<std::byte> malformed{std::byte{31}, std::byte{}, std::byte{}, std::byte{}};
    const auto bad = fixture.Prepare(malformed, lua31);
    Require(!bad.Succeeded() && !bad.value, "header number alone is not a parsed v31 source");
    fixture.request.changeLanguageToLua = false;
}

void BindsFullRequest(Fixture& fixture) {
    const auto lua31 = Settings(31, 1);
    const auto baseline = fixture.Prepare(lua31, lua31);
    Require(baseline.Succeeded(), "prepare request fingerprint baseline");
    const auto baselineHash = baseline.value->Identity()->targetRequestSha256;
    const auto original = fixture.request;
    const auto differs = [&] {
        const auto changed = fixture.Prepare(lua31, lua31);
        Require(changed.Succeeded() && changed.value->Identity()->targetRequestSha256 != baselineHash,
            "full target request change must alter fingerprint");
        Require(BytesEqual(changed.value->SelectedSettings(), lua31), "profile selection must not rewrite player layout or W3I metadata");
        return changed.value->Identity()->targetRequestSha256;
    };
    fixture.request.profile.target.apis[0].parameters[0] = "integer"; differs(); fixture.request = original;
    fixture.request.profile.target.constants[0].type = "real"; differs(); fixture.request = original;
    fixture.request.profile.target.handleTypes.push_back({"widget", "handle"}); differs(); fixture.request = original;
    fixture.request.profile.callbacks[0].symbol = "AlternateMapStart"; differs(); fixture.request = original;
    fixture.request.playerLayout = Script::MapInitializationPlayerLayout::Extended24; differs(); fixture.request = original;
    fixture.request.changeLanguageToLua = true; differs(); fixture.request = original;
    fixture.request.checker = fixture.Capture(fixture.checkerB);
    const auto otherPath = differs(); fixture.request = original;
    std::ofstream(fixture.checkerB, std::ios::binary | std::ios::app).put('x');
    fixture.request.checker = fixture.Capture(fixture.checkerB);
    Require(differs() != otherPath, "same checker path with different bytes gets new request identity");
    fixture.request = original;
    std::vector<std::filesystem::path> directories{fixture.root / "runtime-a", fixture.root / "runtime-b"};
    fixture.request.checker = fixture.Capture(fixture.checkerA, directories);
    const auto ordered = differs();
    std::reverse(directories.begin(), directories.end());
    fixture.request.checker = fixture.Capture(fixture.checkerA, directories);
    Require(differs() != ordered, "runtime directory order is part of checker request identity");
    fixture.request = original;
    const auto repeat = fixture.Prepare(lua31, lua31), revised = fixture.Prepare(lua31, lua31, 12);
    Require(repeat.Succeeded() && repeat.value->Identity()->targetRequestSha256 == baselineHash
        && revised.Succeeded() && revised.value->Identity()->documentRevision != baseline.value->Identity()->documentRevision,
        "identical request deterministic and changed document revision remains explicit");
    Require(baseline.value->Profile()->target.apis[0].parameters[0] == "unit"
        && baseline.value->Profile()->checker.Identity()->executable == original.checker.Identity()->executable,
        "later request changes cannot mutate the frozen profile");
}

void RejectsUnboundInputs(Fixture& fixture) {
    const auto lua31 = Settings(31, 1);
    const auto original = fixture.request;
    fixture.request.checker = {};
    Rejected(fixture.Prepare(lua31, lua31), "LUA_MAP_TARGET_CHECKER_UNCAPTURED");
    fixture.request = original;
    fixture.request.profile.target.identity.clear();
    Rejected(fixture.Prepare(lua31, lua31), "LUA_MAP_TARGET_PROFILE_MISSING");
    fixture.request = original;
    fixture.request.playerLayout = static_cast<Script::MapInitializationPlayerLayout>(0xff);
    Rejected(fixture.Prepare(lua31, lua31), "LUA_MAP_TARGET_PLAYER_LAYOUT_UNKNOWN");
    fixture.request = original;
    const auto unsealed = PrepareMapScriptTargetSelection(lua31, lua31, Trigger::TriggerDocument{}, fixture.catalog, fixture.request, 11);
    Require(!unsealed.Succeeded() && !unsealed.value, "target preparation reuses sealed trigger input validation");
    const PreparedLuaMapTarget empty;
    Require(!empty.Identity() && !empty.Profile() && empty.SelectedSettings().empty(), "default target is visibly unprepared");
}

} // namespace

void ClassicTargetPreservesActualSettings(Fixture& fixture) {
    const auto previous = fixture.request;
    fixture.request.dialect = Script::LuaMapSaveDialect::Classic127YDWE;
    fixture.request.checker = {};
    fixture.request.changeLanguageToLua = false;
    fixture.request.playerLayout = Script::MapInitializationPlayerLayout::Classic12;
    const auto classic = Settings(25, 0);
    const auto prepared = fixture.Prepare(classic, classic);
    Require(prepared.Succeeded() && prepared.value && prepared.value->Profile()
        && prepared.value->Profile()->dialect == Script::LuaMapSaveDialect::Classic127YDWE
        && BytesEqual(prepared.value->SelectedSettings(), classic)
        && prepared.value->Identity()->sourceSettingsSha256 == prepared.value->Identity()->selectedSettingsSha256,
        "classic explicit target uses embedded compilation and preserves every v25 byte");
    fixture.request.changeLanguageToLua = true;
    Rejected(fixture.Prepare(classic, classic), "CLASSIC_LUA_TARGET_INPUT_UNSUPPORTED");
    fixture.request.changeLanguageToLua = false;
    fixture.request.playerLayout = Script::MapInitializationPlayerLayout::Extended24;
    Rejected(fixture.Prepare(classic, classic), "CLASSIC_LUA_TARGET_INPUT_UNSUPPORTED");
    fixture.request.playerLayout = Script::MapInitializationPlayerLayout::Classic12;
    Rejected(fixture.Prepare(Settings(31, 1), Settings(31, 1)), "LUA_MAP_TARGET_VERSION_UNSUPPORTED");
    fixture.request.dialect = static_cast<Script::LuaMapSaveDialect>(255);
    Rejected(fixture.Prepare(classic, classic), "LUA_MAP_TARGET_DIALECT_UNKNOWN");
    fixture.request = previous;
}

int main() {
    try {
        Fixture fixture;
        PreservesSettingsAndOwnsInputs(fixture);
        RequiresExplicitLanguageAndActualVersion(fixture);
        BindsFullRequest(fixture);
        RejectsUnboundInputs(fixture);
        ClassicTargetPreservesActualSettings(fixture);
        fixture.RemoveSuccessfulFixture();
        std::cout << "Lua map target selection: actual v31, explicit language, owned request and settings preservation passed; checker only bound, not executed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lua map target selection failure: " << error.what() << '\n';
        return 1;
    }
}
