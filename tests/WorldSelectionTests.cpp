#include "War3MapCore/Services/WorldSelection.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Services;
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
MapDataQueryBinding Binding() { return {"world-selection", 3, 11, std::string(64, '0')}; }
MapData::UnitPlacements Units() {
    MapData::UnitPlacements units;
    int creation = 1;
    for (const auto* code : {"u001", "u003", "sloc", "u999"}) {
        MapData::UnitPlacement row; row.rawcode = code; row.creationId = creation++;
        row.position = {float(creation * 32), 64, 127}; row.scale = {1, 1, 1}; row.owner = 16;
        units.units.push_back(row);
    }
    return units;
}
MapDocument Document(const MapData::UnitPlacements& units) {
    MapData::DoodadPlacements doodads;
    MapData::DoodadPlacement doodad; doodad.rawcode = "d001"; doodad.creationId = 8; doodad.scale = {1, 1, 1};
    doodads.doodads.push_back(doodad);
    doodads.specialDoodads.push_back({"d001", 0, {2, 3}});
    MapData::RegionsDocument regions{5, {{-10, -20, 30, 40, "TRIGSTR_0042", 17, {}, "", {0, 0, 255, 0}}}, {}};
    MapData::CamerasDocument cameras{0, {{100, 200, 0, 90, 304, 1650, 0, 70, 5000, 100, "Intro"}}, {}};
    const auto u = units.Serialize(), d = doodads.Serialize(), r = regions.Serialize(), c = cameras.Serialize();
    Require(u.Succeeded() && d.Succeeded() && r.Succeeded() && c.Succeeded(), "original synthetic member serialization");
    const std::string wts = "STRING 42\n{\n边界区域\n}\n";
    std::vector<std::byte> strings; for (const auto byte : wts) strings.push_back(static_cast<std::byte>(byte));
    std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *u.value}, {"war3map.doo", *d.value},
        {"war3map.w3r", *r.value}, {"war3map.w3c", *c.value}, {"war3map.wts", std::move(strings)}};
    auto document = MapDocument::FromMembers(members);
    Require(document.Units() && document.Doodads() && document.Regions() && document.Cameras(),
        "selection fixture has four parsed classic tables");
    return document;
}
struct Sources {
    std::array<ObjectData::ObjectRecord, 1> units, items, destructibles;
    std::array<ProjectSearchObjectSource, 4> views;
    Sources() {
        units[0].key = {ObjectData::ObjectType::Unit, "u001"};
        items[0].key = {ObjectData::ObjectType::Item, "u003"}; // u 前缀不能代替实际 Item 类型
        destructibles[0].key = {ObjectData::ObjectType::Destructable, "d001"};
        views = {{{ObjectData::ObjectType::Unit, units, ProjectSearchState::Ready, ProjectSearchObjectOrigin::Native},
            {ObjectData::ObjectType::Item, items, ProjectSearchState::Ready, ProjectSearchObjectOrigin::Lni},
            {ObjectData::ObjectType::Destructable, destructibles, ProjectSearchState::Ready},
            {ObjectData::ObjectType::Doodad, {}, ProjectSearchState::Missing}}};
    }
};
void SelectionAndEditorState() {
    auto units = Units(); auto doc = Document(units); auto query = CaptureMapDataSnapshot(doc, Binding(), false);
    Require(query.Succeeded(), "capture current document"); Sources sources;
    WorldSelectionContext context{&doc, &*query.value, Binding(), 5, sources.views, nullptr, ProjectSearchState::Missing};
    WorldSelection selection; Require(selection.Bind(context).Succeeded(), "bind shared state");
    const std::array<WorldObjectId, 2> ids{MapData::MakePlacementId(MapData::PlacementFile::Units, 0, units.units[0]),
        MapData::MakePlacementId(MapData::PlacementFile::Units, 1, units.units[1])};
    const auto original = *selection.Version();
    Require(selection.Select(context, original, SelectionMode::Replace, std::span(ids).first(1)).Succeeded(), "replace without model availability");
    Require(!selection.Select(context, original, SelectionMode::Add, std::span(ids).last(1)).Succeeded(), "old editor revision rejects concurrent request");
    auto result = selection.Select(context, *selection.Version(), SelectionMode::Add, ids);
    Require(result.Succeeded() && result.value->selected.size() == 2 && result.value->primary == ids[1], "add deduplicates and sets primary");
    Require(selection.EditStatus(context, *selection.Version()).Succeeded(), "complete typed placement group remains editable without models");
    result = selection.Select(context, *selection.Version(), SelectionMode::Subtract, std::span(ids).last(1));
    Require(result.Succeeded() && result.value->selected.size() == 1 && result.value->primary == ids[0], "Ctrl subtraction preserves remaining primary");
    auto staleId = std::get<MapData::PlacementId>(ids[1]); staleId.creationId = 999;
    const std::array<WorldObjectId, 2> invalid{ids[1], staleId}; const auto before = *selection.Version();
    Require(!selection.Select(context, before, SelectionMode::Add, invalid).Succeeded()
        && selection.Snapshot(context, before).value->selected.size() == 1, "invalid batch never partially changes selection");
    const WorldEditorFlagEdit lock{ids[0], {true, true}};
    result = selection.SetEditorFlags(context, before, std::span(&lock, 1));
    Require(result.Succeeded() && result.value->selected.empty() && result.value->version.document == Binding()
        && doc.Units()->Serialize().value == units.Serialize().value, "lock removes editable selection without altering map revision or bytes");
    Require(!selection.Select(context, *selection.Version(), SelectionMode::Add, std::span(ids).first(1)).Succeeded(), "locked object cannot be added via service");
    ++context.catalogRevision; Require(!selection.Snapshot(context, *selection.Version()).Succeeded(), "catalog changed before rebind rejects old state");
    Require(selection.Bind(context).Succeeded() && selection.Flags(ids[0]).locked, "catalog rebind preserves exact existing flags");
    units.units.erase(units.units.begin()); doc = Document(units); ++context.binding.revision;
    query = CaptureMapDataSnapshot(doc, context.binding, false); context.mapData = &*query.value;
    Require(selection.Bind(context).Succeeded() && !selection.Flags(ids[0]).locked, "deleted/shifted row cannot inherit old lock by index");
    ++context.binding.generation; query = CaptureMapDataSnapshot(doc, context.binding, false); context.mapData = &*query.value;
    Require(selection.Bind(context).Succeeded() && selection.Snapshot(context, *selection.Version()).value->selected.empty(), "new document generation clears editor state");
    const auto closed = *selection.Version(); selection.Clear();
    Require(selection.Bind(context).Succeeded() && !selection.Snapshot(context, closed).Succeeded(),
        "clear/rebind of same document cannot reuse retired editor revision");
}
void QueriesAndTypedIdentity() {
    const auto units = Units(); const auto doc = Document(units); auto query = CaptureMapDataSnapshot(doc, Binding(), false); Sources sources;
    Require(query.Succeeded(), "capture typed query fixture");
    WorldSelectionContext context{&doc, &*query.value, Binding(), 8, sources.views, nullptr, ProjectSearchState::Missing};
    context.objectNameReader = [](const ObjectData::ObjectKey& key) {
        return Result<MapDataQueryText>::Success({"TRIGSTR_17", key.type == ObjectData::ObjectType::Unit ? "步兵" : "物品", MapDataQueryTextState::Resolved});
    };
    WorldSelection selection; Require(selection.Bind(context).Succeeded(), "bind query fixture");
    auto page = selection.QueryObjects(context, *selection.Version(), MapDataKind::Units, {}, {}, {1});
    Require(page.Succeeded() && page.value->rows[0].category == WorldCategory::Unit && page.value->next
        && page.value->rows[0].positionWorld == units.units[0].position, "typed positions and bounded first page without scene");
    auto next = selection.QueryObjects(context, *selection.Version(), MapDataKind::Units, {}, page.value->next, {1});
    Require(next.Succeeded() && next.value->rows[0].category == WorldCategory::Item && next.value->rows[0].owner == 16, "classifies native/LNI sources, never rawcode prefix");
    const auto savedCursor = page.value->next;
    page = selection.QueryObjects(context, *selection.Version(), MapDataKind::Units, {"步兵"});
    Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->rows[0].name.displayBytes == "步兵", "shared resolved name participates before filtering");
    page = selection.QueryObjects(context, *selection.Version(), MapDataKind::Units, {}, {}, {1, 1});
    Require(page.Succeeded() && page.value->next && page.value->examined == 1 && page.value->partial, "scan limit retains continuation rather than declaring full result");
    page = selection.QueryObjects(context, *selection.Version(), MapDataKind::Units);
    Require(page.Succeeded() && page.value->rows[2].category == WorldCategory::StartLocation
        && page.value->rows[3].typeState == WorldTypeState::Missing && page.value->partial, "explicit start location and unresolved records retained");
    page = selection.QueryObjects(context, *selection.Version(), MapDataKind::Doodads);
    Require(page.Succeeded() && page.value->rows.size() == 2 && page.value->rows[0].category == WorldCategory::Destructable
        && !page.value->rows[1].id && page.value->partial, "special doodad never maps onto ordinary placement index");
    page = selection.QueryObjects(context, *selection.Version(), MapDataKind::Regions, {"边界"});
    Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->rows[0].name.state == MapDataQueryTextState::Resolved
        && std::holds_alternative<MapData::RegionId>(*page.value->rows[0].id), "region WTS and complete typed identity survive shared query");
    sources.items[0].key.rawcode = "u001"; ++context.catalogRevision; Require(selection.Bind(context).Succeeded(), "bind changed catalog");
    page = selection.QueryObjects(context, *selection.Version(), MapDataKind::Units, {}, {}, {1});
    Require(page.Succeeded() && page.value->rows[0].typeState == WorldTypeState::Ambiguous, "cross-class collision remains unknown");
    Require(!selection.QueryObjects(context, *selection.Version(), MapDataKind::Units, {}, savedCursor).Succeeded()
        && !selection.QueryObjects(context, *selection.Version(), MapDataKind::Units, {}, {}, {1, 1, 1}).Succeeded(), "old catalog cursor and undersized byte budget refused");
}
void PartialSourceRemainsInspectable() {
    auto units = Units(); units.units[1].creationId = units.units[0].creationId;
    const auto doc = Document(units); auto query = CaptureMapDataSnapshot(doc, Binding(), false); Sources sources;
    Require(doc.Member(MapDataKind::Units).state == MapDataState::Partial && query.Succeeded(), "duplicate creation IDs produce real partial source");
    WorldSelectionContext context{&doc, &*query.value, Binding(), 8, sources.views, nullptr, ProjectSearchState::Missing};
    WorldSelection selection; Require(selection.Bind(context).Succeeded(), "bind readable partial source");
    const WorldObjectId id = MapData::MakePlacementId(MapData::PlacementFile::Units, 0, units.units[0]);
    Require(selection.Select(context, *selection.Version(), SelectionMode::Replace, std::span(&id, 1)).Succeeded(), "partial record remains selectable for inspection");
    const auto status = selection.EditStatus(context, *selection.Version());
    Require(!status.Succeeded() && status.diagnostics.front().code == "MAP_SOURCE_UNSAFE"
        && status.diagnostics.front().message.find("war3mapUnits.doo") != std::string::npos, "partial member disables editing with source identity");
    Require(std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "MAP_IDENTITY_AMBIGUOUS"; }), "original duplicate identity diagnosis survives");
}
}
int main() {
    try { SelectionAndEditorState(); QueriesAndTypedIdentity(); PartialSourceRemainsInspectable(); std::cout << "WorldSelection tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
