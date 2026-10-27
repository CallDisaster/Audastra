#include "../support/MapCustomSettingsFixture.h"
#include "../MapDataFixtures.h"
#include "War3MapCore/Services/MapCustomSettingsQuery.h"
#include <nlohmann/json.hpp>
#include <iostream>

namespace {
using namespace AudastraTest::CustomSettings;
using namespace War3Map::Services;
using Json = nlohmann::json;
template<class R> bool Code(const R& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
void QueryAndReadOnly() {
    const MapDataQueryBinding binding{"original", 1, 0, std::string(64, 'a')};
    auto fixture = MapDataFixtures::FullMapSettingsV25();
    auto document = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3i", fixture.bytes}});
    auto summary = Json::parse(Take(QueryMapCustomSettings(document, binding, R"({"section":"summary"})")));
    Require(summary["native_v25_writable"] == true && summary["counts"]["random_units"] == 1, "literal independent W3I query");
    Json query{{"section", "random_units"}, {"settings_binding", summary["settings_binding"]}, {"part", "rows"}, {"number", 4}, {"limit", 1}};
    auto page = Json::parse(Take(QueryMapCustomSettings(document, binding, query.dump())));
    Require(page["rows"].size() == 1 && page["next_offset"] == 1, "bounded random rows and cursor");
    auto stale = binding; ++stale.revision;
    Require(Code(QueryMapCustomSettings(document, stale, query.dump()), "SETTINGS_QUERY_STALE"), "old revision page rejected");
    Require(!QueryMapCustomSettings(document, binding, R"({"section":"upgrades"})").Succeeded(), "non-summary requires binding");
    auto tail = fixture.bytes; tail.push_back(std::byte{7});
    auto partial = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3i", tail}});
    Require(!MapCustomSettingsEditing::WriteQualification(partial).Succeeded(), "unknown tail read only");
    fixture.bytes.resize(fixture.offsets.at("upgrades")); fixture.bytes.push_back(std::byte{255});
    auto omitted = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3i", fixture.bytes}});
    Require(Json::parse(Take(QueryMapCustomSettings(omitted, binding, R"({"section":"summary"})")))["custom_present"] == false, "FF omission is not empty");
    for (const auto version : {18, 28, 31, 32, 33}) {
        const auto classic = MapDataFixtures::FullMapSettingsV25(); auto versioned = classic;
        const auto insert = [&](std::size_t offset, std::initializer_list<std::uint32_t> values) {
            MapDataFixtures::Fixture encoded; for (auto value : values) encoded.UInt(value);
            versioned.bytes.insert(versioned.bytes.begin() + static_cast<std::ptrdiff_t>(offset), encoded.bytes.begin(), encoded.bytes.end());
        };
        if (version == 18) {
            versioned.bytes.resize(classic.offsets.at("camera") + 61);
            for (auto number : {17, 29}) { versioned.Int(number); for (const auto* text : {"Text", "Title", "Subtitle"}) versioned.String(text); }
            versioned.bytes.insert(versioned.bytes.end(), classic.bytes.begin() + classic.offsets.at("players"), classic.bytes.begin() + classic.offsets.at("itemTables"));
        } else {
            const auto players = classic.offsets.at("players"), first = players + 4;
            const auto second = first + 16 + std::string_view("Human slot").size() + 1 + 8 + 8;
            const auto end = second + 16 + std::string_view("Computer slot").size() + 1 + 8 + 8;
            if (version >= 31) { insert(end, {0x80000000u, 0x01020304u}); insert(second, {0x40000000u, 0x05060708u}); }
            if (version == 33) insert(players, {0x80000000u});
            if (version >= 32) insert(players, {0xffffffffu, 0x7fffffffu});
            if (version >= 31) insert(players, {0x87654321u, 1u});
            insert(players, {0u}); insert(12, {1u, 32u, 10u, 20456u});
        }
        versioned.bytes[0] = std::byte(version);
        auto readOnly = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3i", versioned.bytes}});
        auto queried = Json::parse(Take(QueryMapCustomSettings(readOnly, binding, R"({"section":"summary"})")));
        Require(queried["version"] == version && queried["custom_present"] == true && queried["native_v25_writable"] == false, "actual version layout query remains read only");
        if (version == 18) Require(queried["counts"]["random_items"] == 0, "ROC never fabricates an item table body");
    }
}
void CommandAndSave() {
    Fixture fixture; auto source = Take(MapArchiveSession::Open(fixture.source.path));
    ObjectCatalogSources inputs; inputs.archive = &source; inputs.gameRoot = fixture.base.inputs.game;
    auto validation = Take(CaptureMapCustomSettingsValidation(inputs));
    auto document = std::make_shared<const MapDocument>(MapDocument::FromMembers(fixture.members));
    auto request = fixture.AppendUpgrade();
    auto plan = Take(MapCustomSettingsEditing::Prepare(document, request, validation));
    Require(plan.HasChanges() && plan.CanCommit(), "real object validation allows upgrade");
    const auto parsedOverlay = ObjectData::LniObjectCodec(ObjectData::ObjectType::Upgrade).Parse(Bytes("[R001]\nmaxlevel = 1\n"));
    const auto* overlayDocument = parsedOverlay.Succeeded() ? dynamic_cast<ObjectData::LniObjectDocument*>(parsedOverlay.document.get()) : nullptr;
    Require(overlayDocument && overlayDocument->IsStructurallySafe(), "owned unsaved upgrade overlay");
    MapCustomSettingsObjectOverlay overlay;
    overlay[static_cast<std::size_t>(ObjectData::ObjectType::Upgrade)] = std::make_shared<const ObjectData::LniObjectDocument>(*overlayDocument);
    auto overlayValidation = Take(CaptureMapCustomSettingsValidation(inputs, std::move(overlay)));
    Require(Code(MapCustomSettingsEditing::Prepare(document, request, overlayValidation), "SETTINGS_UPGRADE_LEVEL_INVALID"), "actual unsaved object override controls allowed level");
    auto legacySettings = *document->Settings(); legacySettings.custom->upgrades.push_back({0, Raw("R001"), 0, 1});
    const auto legacy = Take(MapData::ProjectMapData(*document, std::vector<MapArchiveMember>{{"war3map.w3i", Take(legacySettings.Serialize())}}));
    MapCustomSettingsCommand preserve; preserve.action = MapCustomSettingsAction::Update; preserve.index = 0;
    preserve.expectedKey = Take(MapCustomSettingsEditing::RecordKey(MapCustomSettingsSection::Upgrades, legacySettings.custom->upgrades.front(), 25));
    preserve.value = MapData::MapUpgradeAvailability{0, Raw("R001"), 1, 1};
    MapCustomSettingsEditRequest legacyRequest{Take(MapCustomSettingsEditing::MemberIdentity(legacy)), {preserve}};
    Require(Take(MapCustomSettingsEditing::Prepare(std::make_shared<const MapDocument>(legacy), legacyRequest, validation)).After().Settings()->custom->upgrades.front().playerMask == 0, "existing empty mask preserved without creating a new empty row");
    MapData::MapDataEditSession edits(*document);
    Require(MapCustomSettingsEditing::Apply(edits, plan, validation).Succeeded(), "one original transaction applies");
    Require(edits.Snapshot().Settings()->custom->upgrades.size() == 1, "upgrade applied");
    Require(!MapCustomSettingsEditing::Apply(edits, plan, validation).Succeeded(), "stale plan refuses overwrite");
    Require(edits.Undo() && edits.Snapshot().Settings()->custom->upgrades.empty(), "undo restores exact table");
    Require(edits.Redo() && edits.Snapshot().Settings()->custom->upgrades.size() == 1, "redo restores one transaction");
    auto updated = request; updated.expectedMemberSha256 = Take(MapCustomSettingsEditing::MemberIdentity(plan.After()));
    updated.operations.front().action = MapCustomSettingsAction::Update; updated.operations.front().expectedCount.reset(); updated.operations.front().index = 0; updated.operations.front().expectedKey = plan.StepKeys().front();
    std::get<MapData::MapUpgradeAvailability>(updated.operations.front().value).level = 2;
    auto changed = Take(MapCustomSettingsEditing::Prepare(std::make_shared<const MapDocument>(plan.After()), updated, validation));
    Require(changed.After().Settings()->custom->upgrades.front().level == 2, "update uses whole row CAS");
    updated.operations.front().expectedKey.assign(64, 'a');
    Require(Code(MapCustomSettingsEditing::Prepare(std::make_shared<const MapDocument>(plan.After()), updated, validation), "SETTINGS_ROW_STALE"), "old row key rejected");
    auto removed = request; removed.expectedMemberSha256 = Take(MapCustomSettingsEditing::MemberIdentity(plan.After()));
    removed.operations.front().action = MapCustomSettingsAction::Delete; removed.operations.front().expectedCount.reset(); removed.operations.front().index = 0;
    removed.operations.front().expectedKey = plan.StepKeys().front(); removed.operations.front().value = std::monostate{};
    Require(Take(MapCustomSettingsEditing::Prepare(std::make_shared<const MapDocument>(plan.After()), removed, validation)).After().Settings()->custom->upgrades.empty(), "delete preserves unrelated tables");
    auto twice = request; auto second = twice.operations.front(); second.expectedCount = 1; second.value = MapData::MapUpgradeAvailability{1, Raw("R002"), 0, 0}; twice.operations.push_back(second);
    Require(Take(MapCustomSettingsEditing::Prepare(document, twice, validation)).After().Settings()->custom->upgrades.size() == 2, "append CAS evaluates actual prior prefix");
    twice.operations.back().expectedCount = 0;
    Require(Code(MapCustomSettingsEditing::Prepare(document, twice, validation), "SETTINGS_COUNT_STALE"), "failed suffix does not produce plan");
    for (unsigned mode = 0; mode < 5; ++mode) {
        auto bad = request; auto& row = std::get<MapData::MapUpgradeAvailability>(bad.operations.front().value);
        if (mode == 0) row.level = 3; if (mode == 1) row.id = Raw("Miss"); if (mode == 2) row.playerMask = 0; if (mode == 3) row.playerMask = 2; if (mode == 4) row.availability = 9;
        Require(!MapCustomSettingsEditing::Prepare(document, bad, validation).Succeeded(), "unsupported level/id/mask/value cannot prepare");
    }
    auto lost = validation; lost.verify = [] { return Status::Failure("Fixture", "SOURCE_DRIFT", "changed"); };
    MapData::MapDataEditSession untouched(*document);
    Require(!MapCustomSettingsEditing::Apply(untouched, plan, lost).Succeeded() && untouched.Snapshot().Settings()->custom->upgrades.empty(), "publish rechecks resource source");
    auto random = request; MapCustomSettingsCommand table; table.section = MapCustomSettingsSection::RandomUnits; table.expectedCount = 0; table.number = 7;
    table.value = MapData::MapRandomUnitTable{7, "Original table", {0}, {{30, {Raw("u001")}}}}; random.operations = {table};
    auto preview = Take(MapCustomSettingsEditing::Prepare(document, random, validation));
    Require(!preview.CanCommit() && !preview.Diagnostics().empty(), "non100 weight is proportional info, preview only");
    Require(Code(MapCustomSettingsEditing::Apply(untouched, preview, validation), "SETTINGS_RANDOM_COMMIT_UNSUPPORTED"), "random commit remains explicit unsupported");
    std::get<MapData::MapRandomUnitTable>(random.operations.front().value).rows.front().chance = 0;
    Require(Code(MapCustomSettingsEditing::Prepare(document, random, validation), "SETTINGS_RANDOM_WEIGHT_EMPTY"), "zero total rejected");
    std::get<MapData::MapRandomUnitTable>(random.operations.front().value).rows.front() = {30, {Raw("YYU0")}};
    Require(Code(MapCustomSettingsEditing::Prepare(document, random, validation), "SETTINGS_PSEUDO_ID_READ_ONLY"), "new category/level pseudo id rejected");
    auto itemRequest = request; MapCustomSettingsCommand itemTable; itemTable.section = MapCustomSettingsSection::RandomItems; itemTable.expectedCount = 0; itemTable.number = 11;
    itemTable.value = MapData::MapRandomItemTable{11, "Original items", {{{40, Raw("i001")}}}}; itemRequest.operations = {itemTable};
    auto itemPreview = Take(MapCustomSettingsEditing::Prepare(document, itemRequest, validation));
    Require(itemPreview.HasChanges() && !itemPreview.CanCommit(), "item table can preview less than100 with no drop remainder");
    std::get<MapData::MapRandomItemTable>(itemRequest.operations.front().value).sets.front().push_back({70, Raw("i001")});
    Require(Code(MapCustomSettingsEditing::Prepare(document, itemRequest, validation), "SETTINGS_RANDOM_CHANCE_SUM"), "item chance sum above100 rejected");
    auto save = fixture.Request("saved-custom"); save.settingsCustomEdits = request;
    auto receipt = Take(SaveMapEditCandidate(save));
    Require(receipt.appliedSettingsCustomEditCount == 1, "normal save records command count");
    auto reopened = Take(MapArchiveSession::Open(save.targetCandidate));
    auto actual = MapDocument::FromArchive(reopened);
    Require(actual.Settings()->custom->upgrades.front().level == 1, "normal save independent readback");
    const auto scriptBytes = Take(reopened.ReadMember("war3map.j")); const std::string script(reinterpret_cast<const char*>(scriptBytes.data()), scriptBytes.size());
    Require(script.find("SetPlayerTechResearched( Player(0), 'R001', 2 )") != script.npos, "actual regenerated script uses one-based research level");
    Require(Take(reopened.ReadMember("unchanged.bin")) == Take(source.ReadMember("unchanged.bin")), "unrelated original member preserved");
    Require(Take(Sha256File(source.SourcePath())) == fixture.source.sha256, "source archive unchanged");
    save.targetCandidate = fixture.base.root / "cancelled-custom.w3x";
    save.beforePublish = [] { return Status::Failure("Fixture", "CANCELLED", "cancel before publish"); };
    Require(!SaveMapEditCandidate(save).Succeeded() && !std::filesystem::exists(save.targetCandidate), "cancelled normal save does not publish");
    auto tech = request; MapCustomSettingsCommand restriction; restriction.section = MapCustomSettingsSection::TechRestrictions; restriction.expectedCount = 0;
    restriction.value = MapData::MapTechRestriction{1, Raw("u001")}; tech.operations = {restriction};
    auto techPlan = Take(MapCustomSettingsEditing::Prepare(document, tech, validation));
    Require(techPlan.CanCommit() && techPlan.After().Settings()->custom->techRestrictions.size() == 1, "tech restriction resolves an actual unit");
    auto techSave = fixture.Request("tech-restriction"); techSave.settingsCustomEdits = tech;
    Take(SaveMapEditCandidate(techSave));
    auto techMap = Take(MapArchiveSession::Open(techSave.targetCandidate));
    const auto techScriptBytes = Take(techMap.ReadMember("war3map.j")); const std::string techScript(reinterpret_cast<const char*>(techScriptBytes.data()), techScriptBytes.size());
    Require(techScript.find("SetPlayerTechMaxAllowed( Player(0), 'u001', 0 )") != techScript.npos, "tech restriction normal initialization emitted");
}
void JsonErrors() {
    const std::string prefix = R"({"expected_member_sha256":")" + std::string(64, 'a') + R"(","operations":)";
    Require(!ParseMapCustomSettingsEdit(prefix + R"([{"section":"upgrades","section":"upgrades","op":"delete"}]})").Succeeded(), "duplicate keys rejected");
    Require(!ParseMapCustomSettingsEdit(prefix + R"([{"section":"upgrades","op":"append","expected_count":0,"value":{"player_mask":1.5,"id_hex":"52303031","level":0,"availability":2}}]})").Succeeded(), "fractional mask rejected");
}
}
int main() {
    try { QueryAndReadOnly(); CommandAndSave(); JsonErrors(); std::cout << "custom settings service passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
