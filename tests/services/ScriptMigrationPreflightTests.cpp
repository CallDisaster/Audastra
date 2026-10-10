#include "War3MapCore/Services/ScriptMigrationPreflight.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data()); return {first, first + text.size()};
}
bool Code(const ScriptMigrationReport& report, std::string_view code) {
    return std::any_of(report.findings.begin(), report.findings.end(), [&](const auto& d) { return d.code == code; });
}
}
int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / ("forge-script-migration-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directory(root), "create unique fixture");
        Trigger::UiCatalog catalog;
        const auto words = [](std::vector<std::byte> data, std::initializer_list<std::uint32_t> values) {
            for (const auto value : values) for (unsigned shift = 0; shift < 32; shift += 8)
                data.push_back(static_cast<std::byte>((value >> shift) & 0xff));
            return data;
        };
        const auto wtg = words(Bytes("WTG!"), {7, 0, 2, 0, 0});
        auto wct = words({}, {1}); wct.push_back(std::byte{0}); wct = words(std::move(wct), {0, 0});
        MapData::MapSettings settings; settings.formatVersion = 25; settings.tileset = 'A'; settings.custom.emplace();
        const auto w3i = settings.Serialize(); Require(w3i.Succeeded(), "settings fixture");
        std::vector<MapArchiveMember> source{{"war3map.w3i", *w3i.value}, {"war3map.wtg", wtg},
            {"war3map.wct", wct}, {"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction")},
            {"war3mapUnits.doo", words(Bytes("W3do"), {8, 11, 0})}, {"war3map.doo", words(Bytes("W3do"), {8, 11, 0, 0, 0})}};
        unsigned serial = 0;
        const auto inspect = [&](const std::vector<MapArchiveMember>& members, ScriptMigrationTarget target,
            bool withCatalog = true) {
            const auto path = root / (std::to_string(++serial) + ".w3x");
            const auto written = WriteVerifiedArchive(path, members); Require(written.Succeeded(), "fixture write");
            auto opened = MapArchiveSession::Open(path); Require(opened.Succeeded(), "fixture open");
            const auto result = InspectScriptMigration(*opened.value, opened.value->SourceSha256(), target, withCatalog ? &catalog : nullptr);
            Require(result.Succeeded(), "inspection failed rather than report");
            Require(Sha256File(path).value == std::optional(written.value->sha256), "source mutated");
            return *result.value;
        };
        const auto classic = inspect(source, ScriptMigrationTarget::ClassicJass);
        Require(!classic.Blocked() && classic.triggerPairParsed && classic.settingsVersion == 25
            && classic.sourceLanguage == ScriptSourceLanguage::Jass && classic.members.size() == 27, "classic preflight");
        Require(classic.reportIdentity.size() == 64 && Code(classic, "MIGRATION_VALIDATION_REQUIRED"), "identity and boundary");
        const auto lua = inspect(source, ScriptMigrationTarget::Lua);
        Require(lua.Blocked() && Code(lua, "MIGRATION_LUA_MAP_BACKEND_UNAVAILABLE") && lua.reportIdentity != classic.reportIdentity, "Lua remains blocked");
        Require(Code(inspect(source, ScriptMigrationTarget::ClassicJass, false), "MIGRATION_UI_CATALOG_REQUIRED"), "missing UI not success");
        auto changed = source; changed.push_back({"scripts/war3map.lua", Bytes("function main() end")});
        const auto conflict = inspect(changed, ScriptMigrationTarget::Lua);
        Require(Code(conflict, "MIGRATION_SCRIPT_MEMBERS_CONFLICT") && Code(conflict, "MIGRATION_LANGUAGE_MEMBER_MISMATCH"), "language conflict");
        changed = source; changed.push_back({"scripts/war3map.j", Bytes("alternate")});
        Require(Code(inspect(changed, ScriptMigrationTarget::ClassicJass), "MIGRATION_SCRIPT_MEMBERS_CONFLICT"), "duplicate locations");
        changed = source; changed.push_back({"war3mapSkin.w3u", Bytes("opaque")});
        Require(Code(inspect(changed, ScriptMigrationTarget::ClassicJass), "MIGRATION_SKIN_MEMBER_UNSUPPORTED"), "skin cannot be folded");
        changed = source; changed.push_back({"other.ai", Bytes("memhack_init tiara.ai tiara.blp")});
        Require(!Code(inspect(changed, ScriptMigrationTarget::Lua), "MIGRATION_LUA_MEMHACK_UNSUPPORTED"), "extension/content is not a MemHack pair");
        changed = source; changed.push_back({"tiara.ai", {std::byte{0xff}, std::byte{0}, std::byte{0x80}}});
        Require(!Code(inspect(changed, ScriptMigrationTarget::Lua), "MIGRATION_LUA_MEMHACK_UNSUPPORTED"), "single import is not a confirmed pair");
        changed.push_back({"tiara.blp", {std::byte{0}, std::byte{0xfe}}});
        const auto memhack = inspect(changed, ScriptMigrationTarget::Lua);
        Require(Code(memhack, "MIGRATION_LUA_MEMHACK_UNSUPPORTED") && Code(memhack, "MIGRATION_LUA_MAP_BACKEND_UNAVAILABLE")
            && memhack.members.size() == classic.members.size(), "exact opaque pair requires explicit Lua refusal without reading payloads");
        const auto classicMemhack = inspect(changed, ScriptMigrationTarget::ClassicJass);
        Require(!classicMemhack.Blocked() && !Code(classicMemhack, "MIGRATION_LUA_MEMHACK_UNSUPPORTED"), "classic preflight must not check opaque AI payloads");
        changed = source; changed[0].data = Bytes("broken");
        Require(Code(inspect(changed, ScriptMigrationTarget::ClassicJass), "MIGRATION_SETTINGS_UNAVAILABLE"), "bad settings not guessed");
        changed = source; changed[1].data[4] = std::byte{0xff};
        Require(Code(inspect(changed, ScriptMigrationTarget::Lua), "MIGRATION_TRIGGER_PAIR_UNPARSED"), "unknown WTG retained");
        auto opened = MapArchiveSession::Open(root / "1.w3x"); Require(opened.Succeeded(), "reopen fixture");
        Require(!InspectScriptMigration(*opened.value, std::string(64, '0'), ScriptMigrationTarget::ClassicJass).Succeeded(), "stale SHA accepted");
        const auto repeat = InspectScriptMigration(*opened.value, opened.value->SourceSha256(), ScriptMigrationTarget::ClassicJass, &catalog);
        Require(repeat.Succeeded() && repeat.value->reportIdentity == classic.reportIdentity, "repeat identity changed");
        std::ofstream(root / "1.w3x", std::ios::binary | std::ios::app).put('x');
        Require(!InspectScriptMigration(*opened.value, opened.value->SourceSha256(), ScriptMigrationTarget::ClassicJass).Succeeded(), "changed source accepted");
        // 只清理本测试成功创建的唯一目录；失败保留以便取证。
        std::filesystem::remove_all(root);
        std::cout << "migration provenance/conflict/current-reader/readonly checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
