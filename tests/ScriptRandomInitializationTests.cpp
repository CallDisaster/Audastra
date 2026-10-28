#include "War3MapCore/Script/ScriptQueries.h"
#include "../src/ScriptBuild/MapRandomInitialization.h"
#include "../src/ScriptBuild/MapSections.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Script;
using namespace War3Map::ObjectData;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::byte> Slk(const std::vector<std::vector<std::string>>& rows) {
    std::string text = "ID;PWXL\n";
    for (std::size_t y = 0; y < rows.size(); ++y) for (std::size_t x = 0; x < rows[y].size(); ++x)
        text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y + 1) + ";K\"" + rows[y][x] + "\"\n";
    text += "E\n"; return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
StandardObjectCatalog Catalog() {
    const std::map<std::string, std::vector<std::byte>> files{
        {"units.slk", Slk({{"ID", "isbldg", "teamcolor"}, {"u001", "0", "5"}, {"b001", "1", "-1"}})},
        {"items.slk", Slk({{"ID", "Name"}, {"i001", "Original item"}})},
        {"abilities.slk", Slk({{"ID", "hero", "orderon", "orderoff"}, {"a001", "0", "heal", "unheal"}})},
        {"destructables.slk", Slk({{"ID", "Name"}, {"d001", "Original destructable"}})}};
    CatalogLoadOptions options; options.sources = {{ObjectType::Unit, {"units.slk"}, {}}, {ObjectType::Item, {"items.slk"}, {}},
        {ObjectType::Ability, {"abilities.slk"}, {}}, {ObjectType::Destructable, {"destructables.slk"}, {}}}; options.stringFiles.clear();
    const auto loaded = StandardObjectCatalog::Load([&](const std::string& name) {
        const auto found = files.find(name); return found == files.end() ? Result<std::vector<std::byte>>::Failure("Fixture", "MISSING", "missing") : Result<std::vector<std::byte>>::Success(found->second);
    }, options);
    Require(loaded.Succeeded(), "owned catalog parse"); return *loaded.value;
}
ObjectRecord Child(std::string id, std::string parent, ObjectType type = ObjectType::Unit) {
    ObjectRecord result; result.key = {type, std::move(id)}; result.parentRawcode = std::move(parent); return result;
}
void Override(ObjectRecord& record, std::string field, ObjectData::Value value) {
    ObjectModification modification; modification.key.fieldId = std::move(field); modification.value = std::move(value); record.modifications.push_back(std::move(modification));
}
void FactsArePositiveAndOwnTheirRecords() {
    const auto catalog = Catalog();
    auto parent = Child("U100", "u001"); Override(parent, "ubdg", std::int32_t{1});
    auto child = Child("U101", "U100"); Override(child, "utco", std::int32_t{12});
    ObjectQuerySources source; source.standard = &catalog; source.units = {parent, child};
    const auto queries = BuildObjectQueries(source);
    Require(static_cast<bool>(queries.placement), "positive facts callback installed");
    const auto unit = queries.placement("u001"), building = queries.placement("b001"), item = queries.placement("i001");
    Require(unit && unit->kind == 0 && unit->teamColor == 5, "standard unit facts");
    Require(building && building->kind == 1 && item && item->kind == 2, "known building and item classes");
    const auto inherited = queries.placement("U101"); Require(inherited && inherited->kind == 1 && inherited->teamColor == 12, "full owned parent chain and nearest overrides");
    Require(!queries.placement("Miss"), "missing ID is unknown, not a non-building unit");
    const auto ability = queries.abilityInitialization("a001");
    Require(ability && ability->known && !ability->hero && ability->autocast && ability->orderOn == "heal", "ability initialization has positive source and real order");
    Require(queries.knownDestructable("d001") && !queries.knownDestructable("u001"), "ordinary unit cannot masquerade as destructable");
    source.units = {Child("C100", "C101"), Child("C101", "C100")};
    Require(!BuildObjectQueries(source).placement("C100"), "cycle does not grant classification");
    source.units = {child, child, parent};
    Require(!BuildObjectQueries(source).placement("U101"), "duplicate source identity stays ambiguous");
    source.units = {Child("X100", "Miss")}; source.items = {Child("X100", "i001", ObjectType::Item)};
    Require(!BuildObjectQueries(source).placement("X100"), "invalid unit parent cannot hide a cross-class collision");
    source.items.clear(); source.units = {Child("U102", "u001")}; Override(source.units.front(), "utco", std::string("unknown"));
    Require(!BuildObjectQueries(source).placement("U102"), "unknown color does not silently become zero");
    source.standard = nullptr; Require(!BuildObjectQueries(source).placement("U102"), "no standard source is not positive evidence");
}
MapData::SettingsRawcode Raw(std::string_view value) {
    MapData::SettingsRawcode result{}; Require(value.size() == 4, "raw fixture size");
    for (std::size_t i = 0; i < 4; ++i) result[i] = std::byte(static_cast<unsigned char>(value[i])); return result;
}
struct TriggerFixture {
    std::filesystem::path parent = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
    std::filesystem::path root = parent / ("audastra-script-random-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Trigger::UiCatalog catalog;
    Trigger::TriggerDocument document;
    TriggerFixture() {
        Require(std::filesystem::create_directory(root), "exclusive owned trigger fixture");
        const auto ui = root / "ui"; std::filesystem::create_directories(ui / "base");
        std::ofstream(ui / "config") << "base\n";
        std::ofstream(ui / "base/define.txt") << "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n";
        for (const auto* name : {"action.txt", "call.txt", "condition.txt", "event.txt"}) std::ofstream(ui / "base" / name).close();
        auto loaded = Trigger::UiCatalog::Load(ui); Require(loaded.Succeeded(), "owned trigger catalog"); catalog = std::move(*loaded.value);
        auto created = Trigger::WtgWctCodec{}.CreateEmpty(catalog); Require(created.Succeeded(), "original empty editable T"); document = std::move(*created.value);
    }
    ~TriggerFixture() { std::error_code error; if (root.parent_path() == parent && root.filename().string().starts_with("audastra-script-random-")) std::filesystem::remove_all(root, error); }
};
template<class T> bool HasCode(const Result<T>& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
void RandomPlanKeepsCorrelatedColumnsAndReferences() {
    using namespace War3Map::Script::detail;
    const auto catalog = Catalog(); ObjectQuerySources source; source.standard = &catalog;
    MapData::MapSettings settings; settings.formatVersion = 25; settings.custom.emplace();
    settings.custom->randomUnits.push_back({7, "Original correlated group", {0, 1, 2}, {{30, {Raw("u001"), Raw("b001"), Raw("i001")}}, {0, {{}, {}, {}}}}});
    settings.custom->randomItems.push_back({11, "Original drop", {{{40, Raw("i001")}}}});
    MapData::UnitPlacements units;
    for (std::int32_t column = 0; column < 3; ++column) {
        MapData::UnitPlacement unit; unit.rawcode = column == 2 ? "iDNR" : column == 1 ? "bDNR" : "uDNR"; unit.creationId = column;
        unit.random = MapData::RandomGlobalTable{7, column}; units.units.push_back(std::move(unit));
    }
    units.units.front().droppedItemTable = 11;
    units.units.front().heroLevel = 5; units.units.front().heroStrength = 10;
    MapScriptInputs inputs; inputs.settings = &settings; inputs.units = &units; inputs.queries = BuildObjectQueries(source);
    auto captured = CaptureMapRandomInitialization(inputs); Require(captured.Succeeded(), "well sourced random plan captured");
    Require(captured.value->hasRandomInitialization && captured.value->placements.size() == 3, "random initialization is explicitly captured");
    Require(captured.value->placements.at(0).typeExpression == "gg_rg_007[0]" && captured.value->placements.at(0).typeColors.at("u001") == 5, "native group name and sourced type color retained");
    ScriptText out; std::vector<Diagnostic> diagnostics; MapContext context{inputs, out, diagnostics, {}, {}, *captured.value};
    WriteRandomGroupGlobals(context); WriteRandomGroups(context); WriteItemTables(context); WriteItems(context); WriteUnits(context); const auto text = out.Text();
    Require(text.find("integer array gg_rg_007") != text.npos && text.find("RandomDistAddItem( 0, 30 )") != text.npos, "group declaration and literal proportional weight emitted");
    Require(text.find("set gg_rg_007[0] = 'u001'") != text.npos && text.find("set gg_rg_007[1] = 'b001'") != text.npos
        && text.find("set gg_rg_007[2] = 'i001'") != text.npos, "all correlated positions come from the chosen row");
    Require(text.find("RandomDistAddItem( -1, 70 )") == text.npos && text.find("set gg_rg_007[0] = -1") != text.npos, "unit weights not normalized to100 and empty slots initialized");
    Require(text.find("CreateUnit( p, unitID,") != text.npos && text.find("CreateUnit( p, 'uDNR'") == text.npos && text.find("CreateItem( itemID,") != text.npos, "placeholders create selected concrete IDs");
    Require(text.find("if (unitID != -1 and unitID != 0) then") != text.npos && text.find("IsUnitType( u, UNIT_TYPE_HERO )") != text.npos, "none creates no object and hero attributes are runtime guarded");
    Require(text.find("RandomDistAddItem( -1, 60 )") != text.npos && text.find("function ItemTable000011_DropItems") != text.npos, "global item group retains no-drop remainder");
    auto manyZeroSettings = settings;
    auto& zeroChoices = manyZeroSettings.custom->randomItems.front().sets.front();
    zeroChoices.assign(12000, MapData::MapRandomItem{0, Raw("i001")}); zeroChoices.front().chance = 40;
    auto manyZeroInputs = inputs; manyZeroInputs.settings = &manyZeroSettings;
    auto zeroPlan = CaptureMapRandomInitialization(manyZeroInputs); Require(zeroPlan.Succeeded(), "zero-weight entries remain valid native data within total budget");
    ScriptText zeroOutput; std::vector<Diagnostic> zeroDiagnostics; MapContext zeroContext{manyZeroInputs, zeroOutput, zeroDiagnostics, {}, {}, *zeroPlan.value};
    WriteItemTables(zeroContext);
    Require(zeroOutput.Text().find("'i001', 0") == zeroOutput.Text().npos && zeroOutput.Text().find("'i001', 40") != zeroOutput.Text().npos,
        "zero choices do not overflow classic BJ distribution array and positive weights remain literal");
    auto badUnits = units; std::get<MapData::RandomGlobalTable>(badUnits.units.front().random).column = 3; inputs.units = &badUnits;
    Require(HasCode(CaptureMapRandomInitialization(inputs), "MAP_RANDOM_PLACEMENT_REFERENCE_INVALID"), "dangling column refused");
    badUnits = units; badUnits.units.front().rawcode = "bDNR";
    Require(HasCode(CaptureMapRandomInitialization(inputs), "MAP_RANDOM_PLACEMENT_CLASS_MISMATCH"), "marker and column classes must agree");
    badUnits = units; badUnits.units.front().droppedItemTable = 12;
    Require(HasCode(CaptureMapRandomInitialization(inputs), "MAP_RANDOM_DROP_TABLE_MISSING"), "unbound global drop refused");
    badUnits = units; badUnits.units[2].itemSets.emplace_back();
    Require(HasCode(CaptureMapRandomInitialization(inputs), "MAP_RANDOM_ITEM_UNIT_FIELDS"), "random item cannot silently discard inline unit drop fields");
    inputs.units = &units; settings.custom->randomUnits.front().rows.front().chance = 0;
    Require(HasCode(CaptureMapRandomInitialization(inputs), "MAP_RANDOM_WEIGHT_EMPTY"), "zero random weight refused");
    settings.custom->randomUnits.front().rows.front().chance = 30;
    settings.custom->randomItems.front().sets.front().push_back({70, Raw("i001")});
    Require(HasCode(CaptureMapRandomInitialization(inputs), "MAP_RANDOM_ITEM_CHANCE_SUM"), "item probability over100 refused");
    settings.custom->randomItems.front().sets.front().pop_back(); inputs.queries.placement = {};
    Require(HasCode(CaptureMapRandomInitialization(inputs), "MAP_RANDOM_OBJECT_SOURCE_REQUIRED"), "default false class callbacks never substitute source proof");
    Require(RandomChoiceExpression(std::string(4, '\0'), 0) == "-1", "none is typed no-create sentinel");
    Require(RandomChoiceExpression("YYU3", 0) == "ChooseRandomCreep( 3 )" && RandomChoiceExpression("YiI/", 2) == "ChooseRandomItemEx( ITEM_TYPE_PERMANENT, -1 )", "known legacy pseudo choices preserve engine semantics");
    Require(!RandomChoiceExpression("YYB3", 1), "unverified building pseudo stays unsupported");
    Require(Rawcode("a'b!") == "1629970977" && LegacyItemChoiceExpression("a'b!") == "1629970977", "nonliteral raw4 has one numeric encoding for choices and drops");
    inputs.queries = BuildObjectQueries(source);
    settings.tileset = 'L'; TriggerFixture triggers; inputs.catalog = &triggers.catalog; inputs.triggers = &triggers.document;
    const auto generated = GenerateMapScript(inputs);
    Require(generated.Succeeded(), "actual JASS random path generates after source checks");
    Require(generated.text.find("call InitRandomGroups(") < generated.text.find("call CreateAllUnits("), "actual main initializes group before creating placements");
    Require(generated.text.find("CreateUnit( p, 'uDNR'") == generated.text.npos, "actual generator never creates placeholder rawcode");
    MapData::DoodadPlacements destructables; MapData::DoodadPlacement tree;
    tree.rawcode = "d001"; tree.scale = {1, 1, 1}; tree.droppedItemTable = 11; destructables.doodads.push_back(tree);
    inputs.doodads = &destructables;
    auto withTree = GenerateMapScript(inputs);
    Require(withTree.Succeeded() && withTree.text.find("TriggerAddAction( t, function ItemTable000011_DropItems )") != withTree.text.npos,
        "script-created destructable uses the actual global drop callback");
    destructables.doodads.front().flags = 2;
    const auto autoLoaded = GenerateMapScript(inputs);
    Require(!autoLoaded.Succeeded() && std::any_of(autoLoaded.diagnostics.begin(), autoLoaded.diagnostics.end(), [](const auto& d) { return d.code == "DOODAD_DROP_UNSUPPORTED"; }),
        "archive-auto-loaded doodad does not silently gain a scripted drop hook");
    destructables.doodads.front().flags = 0; destructables.doodads.front().rawcode = "u001";
    Require(HasCode(CaptureMapRandomInitialization(inputs), "MAP_RANDOM_DESTRUCTABLE_SOURCE_REQUIRED"), "global drop cannot classify ordinary units as destructables");
}
}
int main() {
    try { FactsArePositiveAndOwnTheirRecords(); RandomPlanKeepsCorrelatedColumnsAndReferences(); std::cout << "Random initialization source facts passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
