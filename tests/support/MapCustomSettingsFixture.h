#pragma once
#include "TerrainTextureSlotSaveFixture.h"
#include "War3MapCore/Services/MapCustomSettingsSources.h"

namespace AudastraTest::CustomSettings {
using namespace War3Map;
using namespace AuxiliarySaveFixtures;
using TextureSlot::Slk;
inline MapData::SettingsRawcode Raw(std::string_view text) {
    MapData::SettingsRawcode id{}; Require(text.size() == 4, "raw4 fixture");
    for (std::size_t i = 0; i < 4; ++i) id[i] = std::byte(static_cast<unsigned char>(text[i])); return id;
}
struct Fixture {
    TextureSlot::Fixture base;
    ArchiveSaveResult source;
    std::vector<MapArchiveMember> members;
    Fixture() {
        auto original = Take(MapArchiveSession::Open(base.source.path));
        Require(!original.HasOpaqueMembers(), "original fixture has no anonymous members");
        members.assign(original.Members().begin(), original.Members().end());
        auto settings = *MapDocument::FromMembers(members).Settings();
        MapData::MapPlayerSettings player; player.number = 0; player.controller = 1; player.race = 1; player.name = "Original player";
        settings.players.push_back(player);
        std::find_if(members.begin(), members.end(), [](const auto& m) { return m.name == "war3map.w3i"; })->data = Take(settings.Serialize());
        const std::vector<MapArchiveMember> resources{
            {"Units/UnitData.slk", Slk({"unitID", "Name", "isbldg"}, {{"u001", "Original unit", "0"}, {"b001", "Original building", "1"}})},
            {"Units/ItemData.slk", Slk({"itemID", "Name"}, {{"i001", "Original item"}})},
            {"Units/AbilityData.slk", Slk({"alias", "Name", "code"}, {{"a001", "Original ability", "a001"}})},
            {"Units/UpgradeData.slk", Slk({"upgradeid", "Name", "maxlevel"}, {{"R001", "Original upgrade", "3"}, {"R002", "Other upgrade", "2"}})},
            {"Units/UnitMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "index", "useUnit", "useBuilding", "useHero"}, {{"ubdg", "isbldg", "UnitData", "bool", "0", "-1", "1", "1", "1"}})},
            {"Units/UpgradeMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "index"}, {{"glvl", "maxlevel", "UpgradeData", "int", "0", "-1"}})}};
        members.insert(members.end(), resources.begin(), resources.begin() + 4);
        auto declarations = JassDeclarations();
        declarations.common = "type playercolor extends handle\ntype racepreference extends handle\ntype mapcontrol extends handle\n" + declarations.common;
        declarations.common.insert(declarations.common.find("globals\n"),
            "native Player takes integer n returns player\n"
            "native SetPlayerTechMaxAllowed takes player p, integer id, integer level returns nothing\n"
            "native SetPlayerTechResearched takes player p, integer id, integer level returns nothing\n"
            "native ConvertPlayerColor takes integer n returns playercolor\n"
            "native SetPlayerStartLocation takes player p, integer n returns nothing\n"
            "native SetPlayerColor takes player p, playercolor c returns nothing\n"
            "native SetPlayerRacePreference takes player p, racepreference r returns nothing\n"
            "native SetPlayerRaceSelectable takes player p, boolean b returns nothing\n"
            "native SetPlayerController takes player p, mapcontrol c returns nothing\n"
            "native SetStartLocPrioCount takes integer n, integer count returns nothing\n"
            "native DefineStartLocation takes integer n, real x, real y returns nothing\n");
        declarations.common.insert(declarations.common.find("endglobals"), "constant racepreference RACE_PREF_HUMAN = null\nconstant mapcontrol MAP_CONTROL_USER = null\n");
        declarations.blizzard += "function SetPlayerSlotAvailable takes player p, mapcontrol c returns nothing\nendfunction\n";
        Write(base.inputs.tool / "compiler/jass/24/common.j", declarations.common);
        Write(base.inputs.tool / "compiler/jass/24/blizzard.j", declarations.blizzard);
        auto game = GameFiles(declarations);
        for (const auto& member : members) if ((member.name.ends_with(".slk") || member.name.starts_with("UI/")) && member.name != "Units/UnitData.slk"
            && std::none_of(resources.begin(), resources.end(), [&](const auto& r) { return r.name == member.name; })) game.push_back({member.name, member.data, false});
        for (const auto& member : resources) {
            auto found = std::find_if(game.begin(), game.end(), [&](const auto& f) { return f.name == member.name; });
            if (found != game.end()) game.erase(found);
            game.push_back({member.name, member.data, false});
        }
        base.inputs.game = base.root / "custom-game"; std::filesystem::create_directory(base.inputs.game);
        Require(Mpq::WriteMpqArchive(base.inputs.game / "war3.mpq", game).Succeeded(), "owned custom-settings game resources");
        source = Take(WriteVerifiedArchive(base.root / "custom-source.w3x", members));
    }
    Services::MapEditSaveRequest Request(std::string_view label) const {
        auto request = base.Request(label); request.sourceMap = source.path; request.expectedSourceSha256 = source.sha256; request.expectedTerrainSha256.clear(); return request;
    }
    Services::MapCustomSettingsEditRequest AppendUpgrade() const {
        auto map = MapDocument::FromMembers(members);
        Services::MapCustomSettingsCommand command; command.expectedCount = 0;
        command.value = MapData::MapUpgradeAvailability{1, Raw("R001"), 1, 2};
        return {Take(Services::MapCustomSettingsEditing::MemberIdentity(map)), {command}};
    }
};
} // namespace AudastraTest::CustomSettings
