#include "War3MapCore/Services/ProjectChanges.h"
#include "MapDataFixtures.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Services;
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
MapDataQueryBinding Binding() { return {"changes-project", 2, 7, std::string(64, '0')}; }
MapData::UnitPlacements Units() {
    MapData::UnitPlacements table;
    for (int i = 1; i <= 3; ++i) {
        MapData::UnitPlacement row; row.rawcode = "u001"; row.creationId = i;
        row.position = {float(i * 32), -64, float(100 + i)}; row.scale = {1, 1, 1}; row.rotation = .25f;
        row.inventory = {{2, "i001"}}; row.abilities = {{"A001", true, 3}};
        row.itemSets = {{{{"i002", 75}}}}; table.units.push_back(row);
    }
    return table;
}
MapDocument Document(const MapData::UnitPlacements& units) {
    auto bytes = units.Serialize(); Require(bytes.Succeeded(), "synthetic units serialize");
    std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *bytes.value}};
    return MapDocument::FromMembers(members);
}
const ProjectChangeRow& Field(const ProjectChangesPage& page, std::string_view field, std::string_view caseName) {
    const auto found = std::find_if(page.rows.begin(), page.rows.end(), [&](const auto& row) { return row.fieldId == field; });
    if (found == page.rows.end()) throw std::runtime_error(std::string(caseName) + ": expected field '" + std::string(field)
        + "' in " + std::string(MapData::MapDataKindId(page.member.kind)) + "; rows=" + std::to_string(page.rows.size())
        + "; changed=" + std::to_string(page.member.changed) + "; " + page.member.reason);
    return *found;
}
const ProjectChangeRow& Record(const ProjectChangesPage& page, ProjectChangeKind kind) {
    const auto found = std::find_if(page.rows.begin(), page.rows.end(), [&](const auto& row) { return row.kind == kind; });
    Require(found != page.rows.end(), "expected record difference"); return *found;
}
void FieldsPaginationAndStale() {
    auto units = Units(); auto baseline = Document(units);
    units.units[0].rotation = 1; units.units[1].rotation = 2; auto current = Document(units);
    ProjectChangesContext context{&baseline, &current, Binding()};
    auto page = QueryProjectChanges(context, context.binding, MapDataKind::Units, std::nullopt, {1});
    Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->next && !page.value->member.partial,
        "known fields paginate with exact byte accounting proof");
    auto next = QueryProjectChanges(context, context.binding, MapDataKind::Units, page.value->next, {1});
    Require(next.Succeeded() && next.value->rows.size() == 1 && !next.value->next
        && next.value->rows[0].currentIndex == 1, "deterministic cursor advances without duplicates");
    auto row = page.value->rows[0]; auto plan = BuildProjectChangeReversePlan(context, row);
    Require(plan.Succeeded() && plan.value->fieldTables && plan.value->fieldTables->units->units[0].rotation == .25f
        && plan.value->fieldTables->units->units[1].rotation == 2, "field inverse preserves another modified row");
    row.before->floatBits = 0; row.revertible = false;
    Require(BuildProjectChangeReversePlan(context, row).value->fieldTables->units->units[0].rotation == .25f,
        "untrusted adapter values cannot choose restoration payload");
    ++context.binding.revision;
    Require(!BuildProjectChangeReversePlan(context, row).Succeeded()
        && !QueryProjectChanges(context, row.binding, MapDataKind::Units).Succeeded(), "revision change refuses old selection/query");
    context.binding = row.binding; units.units[0].rotation = 3; auto changed = Document(units); context.current = &changed;
    Require(!BuildProjectChangeReversePlan(context, row).Succeeded(), "member SHA prevents unchanged revision aliasing");
    auto cursor = *page.value->next; cursor.afterSha256 = std::string(64, 'f');
    Require(!QueryProjectChanges(context, context.binding, MapDataKind::Units, cursor).Succeeded(), "cursor binds both member SHAs");
    auto limited = QueryProjectChanges(context, context.binding, MapDataKind::Units, std::nullopt, {64, 1});
    Require(limited.Succeeded() && limited.value->member.partial && limited.value->rows.size() == 1
        && limited.value->examinedAfter == 0 && !limited.value->rows[0].revertible, "scan cap cannot claim unique unscanned identities");
    Require(!QueryProjectChanges(context, context.binding, MapDataKind::Units, std::nullopt, {64, 4096, 1}).Succeeded(),
        "metadata cannot spill tiny byte budget");
}
void CompleteRecordsAndIdentity() {
    auto units = Units(); auto baseline = Document(units);
    units.units.erase(units.units.begin() + 1); auto current = Document(units);
    ProjectChangesContext context{&baseline, &current, Binding()};
    auto page = QueryProjectChanges(context, context.binding, MapDataKind::Units);
    const auto& removed = Record(*page.value, ProjectChangeKind::Removed);
    auto plan = BuildProjectChangeReversePlan(context, removed);
    Require(plan.Succeeded() && plan.value->units && plan.value->units->Serialize().value == baseline.Units()->Serialize().value,
        "removed full record returns original order, nested inventory, creation ID and exact height bytes");
    units = Units(); auto added = units.units.front(); added.creationId = 88; added.position[2] = 999; units.units.push_back(added);
    current = Document(units); page = QueryProjectChanges(context, context.binding, MapDataKind::Units);
    plan = BuildProjectChangeReversePlan(context, Record(*page.value, ProjectChangeKind::Added));
    Require(plan.Succeeded() && plan.value->units->Serialize().value == baseline.Units()->Serialize().value,
        "reverting addition preserves all retained record bytes");
    units = Units(); units.units[1].rawcode = "u999"; current = Document(units);
    page = QueryProjectChanges(context, context.binding, MapDataKind::Units);
    Require(page.Succeeded() && page.value->member.partial && std::none_of(page.value->rows.begin(), page.value->rows.end(),
        [](const auto& row) { return row.revertible; }), "creation ID type swap is unresolved identity, not an executable add/delete");
    units = Units(); units.units[2].creationId = 1; current = Document(units);
    page = QueryProjectChanges(context, context.binding, MapDataKind::Units);
    Require(page.Succeeded() && page.value->member.partial && page.value->rows.size() == 1
        && page.value->rows[0].kind == ProjectChangeKind::Member, "duplicate identity cannot align unrelated rows");
    units = Units(); units.units[0].rotation = 1; units.units[0].inventory[0].slot = 5; current = Document(units);
    page = QueryProjectChanges(context, context.binding, MapDataKind::Units);
    Require(page.Succeeded() && page.value->member.partial && Record(*page.value, ProjectChangeKind::Member).revertible == false,
        "unlisted nested byte change survives as explicit member remainder");
    plan = BuildProjectChangeReversePlan(context, Field(*page.value, "facing", "CompleteRecordsAndIdentity"));
    Require(plan.Succeeded() && plan.value->fieldTables->units->units[0].inventory[0].slot == 5,
        "known field inverse preserves uncovered current data");
    units.units.clear(); current = Document(units); page = QueryProjectChanges(context, context.binding, MapDataKind::Units);
    Require(current.Member(MapDataKind::Units).state == MapDataState::Ready
        && current.Units()->recordLayout == MapData::PlacementRecordLayout::Classic
        && current.Units()->Serialize().value == std::optional(current.Units()->OriginalBytes()),
        "MapDocument classic entry keeps its existing exact empty-table roundtrip");
    Require(page.Succeeded() && !page.value->member.partial && page.value->rows.size() == 3,
        "classic empty overlay reports all removed records rather than inventing a read-only layout");
    plan = BuildProjectChangeReversePlan(context, Record(*page.value, ProjectChangeKind::Removed));
    Require(plan.Succeeded() && plan.value->units->units.size() == 1
        && plan.value->units->units[0].creationId == 1 && plan.value->units->units[0].position[2] == 101,
        "classic empty table restores the selected original ID and height without new allocation");
}
void DoodadsAndMapFields() {
    MapData::DoodadPlacements table;
    for (int i = 20; i < 22; ++i) {
        MapData::DoodadPlacement row; row.rawcode = "D001"; row.creationId = i;
        row.scale = {1, 1, 1}; row.position[2] = 321; row.itemSets = {{{{"i001", 100}}}}; table.doodads.push_back(row);
    }
    const auto document = [](const auto& value) {
        return MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.doo", *value.Serialize().value}});
    };
    auto baseline = document(table); table.doodads.erase(table.doodads.begin()); auto current = document(table);
    ProjectChangesContext context{&baseline, &current, Binding()};
    auto page = QueryProjectChanges(context, context.binding, MapDataKind::Doodads);
    auto plan = BuildProjectChangeReversePlan(context, Record(*page.value, ProjectChangeKind::Removed));
    Require(plan.Succeeded() && plan.value->doodads->Serialize().value == baseline.Doodads()->Serialize().value,
        "doodad restoration retains native drop data, ID and elevation");
    const auto fixture = MapDataFixtures::FullMapSettingsV25();
    baseline = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3i", fixture.bytes}});
    auto settings = *baseline.Settings(); settings.gameDataSet = 2;
    current = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3i", *settings.Serialize().value}});
    context.baseline = &baseline; context.current = &current;
    page = QueryProjectChanges(context, context.binding, MapDataKind::Settings);
    plan = BuildProjectChangeReversePlan(context, Field(*page.value, "game_data_set", "DoodadsAndMapFields/settings"));
    Require(plan.Succeeded() && plan.value->fieldTables->settings->Serialize().value == std::optional(fixture.bytes),
        "shared settings field inverse preserves original unknown flags and non-UTF8 bytes");
    MapData::SoundsDocument sounds; MapData::SoundRecord sound{}; sound.name = "Beep"; sound.filePath = "Sound.wav"; sound.pitch = 1;
    sounds.entries = {sound};
    baseline = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3s", *sounds.Serialize().value}});
    sounds.entries[0].filePath = std::string(500, 'x');
    current = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3s", *sounds.Serialize().value}});
    page = QueryProjectChanges(context, context.binding, MapDataKind::Sounds, std::nullopt, {64, 4096, 65536, 256});
    const auto& path = Field(*page.value, "file_path", "DoodadsAndMapFields/sounds");
    Require(path.textTruncated && !path.after && !path.revertible && page.value->member.partial,
        "long original field bytes are omitted, never an actionable truncated value");
    auto missing = QueryProjectChanges(context, context.binding, MapDataKind::Cameras);
    Require(missing.Succeeded() && !missing.value->member.changed && !missing.value->member.partial,
        "known missing on both sides is distinct from unsupported or invalid input");
}
void UnitPositionRevertUsesTheSharedRowGuard() {
    auto units = Units(); auto baseline = Document(units);
    units.units[0].position[0] = 96; auto current = Document(units);
    ProjectChangesContext context{&baseline, &current, Binding()};
    auto page = QueryProjectChanges(context, context.binding, MapDataKind::Units);
    Require(page.Succeeded(), "ordinary position changes remain queryable");
    const auto& x = Field(*page.value, "x", "ordinary coordinate inverse");
    Require(x.revertible && !page.value->member.partial, "ordinary XYZ exposes a proven field inverse");
    auto plan = BuildProjectChangeReversePlan(context, x);
    Require(plan.Succeeded() && plan.value->fieldTables && plan.value->fieldTables->units->Serialize().value == baseline.Units()->Serialize().value,
        "XYZ inverse preserves the entire original nested native unit table");
    units = Units(); units.units[0].rawcode = "sloc"; baseline = Document(units);
    units.units[0].position[0] = 96; current = Document(units);
    page = QueryProjectChanges(context, context.binding, MapDataKind::Units);
    Require(page.Succeeded(), "start-location position change stays visible");
    const auto& readonly = Field(*page.value, "x", "start location guard");
    Require(!readonly.revertible && page.value->member.partial && !BuildProjectChangeReversePlan(context, readonly).Succeeded(),
        "start-location position is never advertised as an independent field restoration");
}
ObjectData::LniObjectDocument ObjectDocument(ObjectData::ObjectType type) {
    const std::string text = "[X001]\nfield = 7\nother = 11\n";
    auto parsed = ObjectData::LniObjectCodec(type).Parse(std::span(
        reinterpret_cast<const std::byte*>(text.data()), text.size()));
    Require(parsed.Succeeded(), "object content fixture parses");
    return *dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.get());
}
void ContentFieldsAndReverse() {
    for (unsigned i = 0; i < 7; ++i) {
        const auto type = static_cast<ObjectData::ObjectType>(i);
        auto before = ObjectDocument(type), after = before;
        const auto key = before.Objects().front().key;
        const auto field = before.Objects().front().modifications.front().key;
        Require(after.SetModification(key, field, std::int32_t(99)).Succeeded(), "set content field");
        ProjectContentChangesContext context; context.binding = Binding(); context.editorRevision = 5;
        context.objects[i] = {&before, &after, true, true, before.DocumentFingerprint(), after.DocumentFingerprint()};
        const auto page = QueryProjectContentChanges(context, context.binding, ProjectChangeDomain::Objects, type);
        Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->totalRows == 1
            && page.value->rows.front().object == key && page.value->rows.front().revertible,
            "all seven object categories expose typed single field and exact row count");
        auto row = page.value->rows.front(); row.objectBefore = std::int32_t(-999); // Adapter cannot choose inverse payload.
        const auto plan = BuildProjectContentReversePlan(context, row);
        Require(plan.Succeeded() && plan.value->objectDocument->DocumentFingerprint() == before.DocumentFingerprint(),
            "inverse re-queries typed baseline and keeps unrelated LNI content");
        ++context.editorRevision;
        Require(!BuildProjectContentReversePlan(context, row).Succeeded(), "content edit revision rejects old row");
        --context.editorRevision; context.objects[i].currentKnown = false;
        const auto unknown = QueryProjectContentChanges(context, context.binding, ProjectChangeDomain::Objects, type);
        Require(unknown.Succeeded() && unknown.value->member.partial && unknown.value->rows.size() == 1
            && !unknown.value->rows.front().revertible, "unparsed content is never reported as empty");
    }
}
void TriggerContentBudgetAndInverse() {
    auto empty = Trigger::WtgWctCodec{}.CreateEmpty(Trigger::UiCatalog{});
    Require(empty.Succeeded(), "empty trigger source seal"); auto before = *empty.value;
    // More than the former 32768 record budget, but no duplicate identities or shared owners.
    constexpr std::size_t count = 40000;
    for (std::size_t i = 0; i < count; ++i) {
        Trigger::ArgumentNode argument; argument.id = 100000 + i; argument.type = 3; argument.value.text = "true";
        before.argumentNodes.push_back(argument);
        Trigger::EcaNode eca; eca.id = 200000 + i; eca.arguments = {static_cast<std::uint32_t>(i)};
        before.ecaNodes.push_back(eca);
    }
    Trigger::TriggerRecord record; record.id = 1; record.name.text = "Large trigger";
    for (std::size_t i = 0; i < count; ++i) record.ecas.push_back(static_cast<std::uint32_t>(i));
    before.triggers = {record};
    Trigger::VariableRecord variable; variable.id = 2; variable.name.text = "Before"; variable.typeName.text = "integer";
    before.variables = {variable};
    auto after = before; after.argumentNodes[7].value.text = "false"; after.variables[0].name.text = "After";
    ProjectContentChangesContext context; context.binding = Binding(); context.editorRevision = 8;
    context.baselineTriggers = &before; context.currentTriggers = &after;
    context.baselineTriggersKnown = context.currentTriggersKnown = true;
    context.baselineTriggerFingerprint = "old"; context.currentTriggerFingerprint = "new";
    const auto page = QueryProjectContentChanges(context, context.binding, ProjectChangeDomain::Triggers);
    Require(page.Succeeded() && !page.value->member.partial && page.value->totalRows == 1
        && page.value->rows.front().changedCount == 1 && page.value->rows.front().revertible
        && page.value->rows.front().beforeText.find("true") != std::string::npos,
        "large valid trigger lists one changed parameter with original value and stable owner");
    const auto plan = BuildProjectContentReversePlan(context, page.value->rows.front());
    Require(plan.Succeeded() && plan.value->arguments.size() == 1 && plan.value->arguments.front().after.value.text == "true",
        "argument inverse keeps unrelated variable changes");
    const auto variables = QueryProjectContentChanges(context, context.binding, ProjectChangeDomain::Variables);
    Require(variables.Succeeded() && variables.value->rows.size() == 1 && variables.value->rows.front().entityId == 2,
        "variable rename preserves stable row identity");
    ProjectChangesOptions limited; limited.maxContentNodes = 100;
    const auto capped = QueryProjectContentChanges(context, context.binding, ProjectChangeDomain::Triggers, ObjectData::ObjectType::Unit,
        std::nullopt, limited);
    Require(capped.Succeeded() && capped.value->member.partial && !capped.value->rows.front().revertible,
        "query work cap leaves explicit uncovered member");
    after.triggers[0].ecas.push_back(0);
    const auto shared = QueryProjectContentChanges(context, context.binding, ProjectChangeDomain::Triggers);
    Require(shared.Succeeded() && shared.value->member.partial && !shared.value->rows.front().revertible,
        "duplicate graph ownership cannot expose an unsafe single-record inverse");
}
} // namespace
int main() {
    try { FieldsPaginationAndStale(); CompleteRecordsAndIdentity(); DoodadsAndMapFields(); UnitPositionRevertUsesTheSharedRowGuard(); ContentFieldsAndReverse(); TriggerContentBudgetAndInverse();
        std::cout << "project changes bounded reverse tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
