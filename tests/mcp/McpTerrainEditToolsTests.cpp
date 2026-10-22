#include "McpTerrainEditTools.h"
#include "McpTools.h"
#include "McpWorkspace.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/TerrainPatchCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using namespace War3Map::Mcp;

namespace {

void Expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

std::vector<MapArchiveMember> FollowPlacementMembers() {
    UnitPlacements units;
    UnitPlacement unit;
    unit.rawcode = "hfoo"; unit.creationId = 7; unit.position = {0, 0, 10};
    unit.scale = {1, 1, 1}; unit.rotation = 0.75f; unit.flags = 2; unit.hitPoints = 73;
    unit.inventory.push_back({0, "ratf"}); units.units.push_back(unit);
    unit.creationId = 8; unit.position = {512, 512, 20}; units.units.push_back(unit);
    DoodadPlacements doodads;
    DoodadPlacement tree;
    tree.rawcode = "LTlt"; tree.creationId = 9; tree.position = {0, 0, 5};
    tree.scale = {1, 1, 1}; tree.rotation = 1.25f; tree.flags = 2; tree.lifePercent = 61;
    tree.itemSets.push_back({{{"ratf", 100}}}); doodads.doodads.push_back(tree);
    tree.creationId = 10; tree.position = {512, 512, 15}; doodads.doodads.push_back(tree);
    doodads.specialDoodads.push_back({"D001", 3, {1, 2}});
    const auto unitBytes = units.Serialize(), doodadBytes = doodads.Serialize();
    Expect(unitBytes.Succeeded() && unitBytes.value && doodadBytes.Succeeded() && doodadBytes.value,
        "original following-placement fixtures encode");
    return {{"war3mapUnits.doo", *unitBytes.value}, {"war3map.doo", *doodadBytes.value}};
}

struct Fixture {
    QTemporaryDir temporary;
    std::filesystem::path source;
    std::vector<std::byte> terrain, pathing;
    std::vector<MapArchiveMember> placements;
    MapDocument document;
    MapDataEditSession edits;
    explicit Fixture(bool withPlacements = false)
        : placements(withPlacements ? FollowPlacementMembers() : std::vector<MapArchiveMember>{}),
          document(MapDocument::FromMembers(placements)), edits(document) {
        Expect(temporary.isValid(), "temporary directory available");
        source = std::filesystem::path(temporary.path().toStdWString()) / "source";
        TerrainGridSnapshot grid;
        grid.header.width = 17; grid.header.height = 17; grid.header.offset = {-1024, -1024}; grid.header.mainTileset = 'L';
        grid.header.groundTilesets = {"Ldrt", "Lgrs"}; grid.header.cliffTilesets = {"CLdi"};
        grid.vertices.resize(17 * 17);
        for (auto& vertex : grid.vertices) { vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2; }
        RefreshTerrainDerived(grid, 0, 0, 17, 17); terrain = *SerializeMapTerrain(grid).value;
        pathing = {std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        for (const auto value : {0u, 64u, 64u}) for (unsigned shift = 0; shift < 32; shift += 8)
            pathing.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        pathing.resize(16 + 64 * 64, std::byte{0x02});
        Write(source / "war3map.w3e", terrain); Write(source / "war3map.wpm", pathing);
        for (const auto& member : placements) Write(source / member.name, member.data);
        Expect(edits.AttachTerrain(terrain).Succeeded() && edits.AttachPathing(pathing).Succeeded(), "fixture terrain sources attach");
    }
    static void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        Expect(static_cast<bool>(stream), "fixture write succeeds");
    }
    MapDataQueryBinding Binding() const { return {"terrain-fixture", 1, edits.Revision(), std::string(64, 'a')}; }
    TerrainEditToolHost Host() {
        TerrainEditToolHost host;
        host.identity = [this] { return QJsonObject{{"project_identity", "terrain-fixture"}, {"revision", QString::number(edits.Revision())}}; };
        host.context = [this] { return Result<TerrainEditToolContext>::Success({&edits, Binding(), false, edits.IsDirty()}); };
        host.apply = [this](const auto& prepared, const auto& validate) { return ApplyTerrainEdit(edits, Binding(), prepared, validate); };
        return host;
    }
};

QJsonObject State(Fixture& fixture) {
    return InvokeTerrainEditTool("get_terrain_edit_state", {}, fixture.Host());
}
QJsonObject Stroke(const QJsonObject& state, bool dryRun = true) {
    return {{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")},
        {"terrain_binding", state.value("terrain_binding")}, {"dry_run", dryRun},
        {"points", QJsonArray{QJsonObject{{"x", 0}, {"y", 0}}}},
        {"brush", QJsonObject{{"kind", "raise"}, {"size", 1}, {"strength", 16}}}};
}

void FacadeQueryPreviewApplyAndUndo() {
    Fixture fixture;
    const auto definitions = TerrainEditToolDefinitions();
    Expect(definitions.size() == 3 && definitions[0].permission == ToolPermission::Query
        && definitions[1].permission == ToolPermission::Query && definitions[2].permission == ToolPermission::Edit,
        "tool schemas distinguish bounded reads and edits");
    const auto state = State(fixture);
    Expect(state.value("ok").toBool() && state.value("pathing_available").toBool(), "state returns real source binding");
    auto query = InvokeTerrainEditTool("query_terrain_points", {{"terrain_binding", state.value("terrain_binding")},
        {"points", QJsonArray{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 1024}, {"y", 1024}}}}}, fixture.Host());
    Expect(query.value("ok").toBool() && query.value("points").toArray().size() == 2, "bounded point query succeeds");
    const auto center = query.value("points").toArray()[0].toObject();
    const auto outsideWpm = query.value("points").toArray()[1].toObject();
    Expect(center.value("pathing").toObject().value("raw_flags").toInt() == 2
        && outsideWpm.value("pathing").toObject().value("state") == "outside", "WPM boundary and raw flags remain distinct");
    auto request = Stroke(state);
    auto result = InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host());
    Expect(result.value("ok").toBool() && result.value("dry_run").toBool() && !result.value("applied").toBool()
        && fixture.edits.Revision() == 0 && !fixture.edits.CanUndo(), "dry-run leaves history and bytes intact");
    Expect(result.value("terrain_pathing_gate").toObject().value("passed").toBool()
        && !result.value("map_saved").toBool(), "preview gate is not reported as a map save");
    request["dry_run"] = false;
    result = InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host());
    Expect(result.value("ok").toBool() && result.value("applied").toBool() && fixture.edits.Revision() == 1
        && result.value("edit_receipt").isObject(), "submission uses guarded original transaction");
    Expect(!InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host()).value("ok").toBool(), "old envelope and binding reject replay");
    Expect(fixture.edits.Undo() && *SerializeMapTerrain(*fixture.edits.Terrain()).value == fixture.terrain,
        "same undo restores exact original terrain bytes");
    Expect(*IO::ReadFileBytes(fixture.source / "war3map.w3e").value == fixture.terrain, "query and edit never write the source file");
}

void FacadeRejectsUnknownTypesBoundsAndStaleSources() {
    Fixture fixture; auto state = State(fixture); auto request = Stroke(state);
    auto invalid = request; invalid["dry_run"] = "false";
    Expect(!InvokeTerrainEditTool("edit_terrain_stroke", invalid, fixture.Host()).value("ok").toBool(), "string boolean rejects");
    invalid = request; auto brush = invalid.value("brush").toObject(); brush["size"] = 1.5; invalid["brush"] = brush;
    Expect(!InvokeTerrainEditTool("edit_terrain_stroke", invalid, fixture.Host()).value("ok").toBool(), "fractional size rejects");
    invalid = request; brush = invalid.value("brush").toObject(); brush["ground_texture"] = "Lgrs"; invalid["brush"] = brush;
    Expect(!InvokeTerrainEditTool("edit_terrain_stroke", invalid, fixture.Host()).value("ok").toBool(), "irrelevant brush fields reject instead of silently ignoring");
    invalid = request; invalid["points"] = QJsonArray{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 1025}, {"y", 0}}};
    Expect(!InvokeTerrainEditTool("edit_terrain_stroke", invalid, fixture.Host()).value("ok").toBool(), "later outside point rejects whole explicit stroke");
    invalid = request; invalid["brush"] = QJsonObject{{"kind", "shallow_water"}};
    Expect(!InvokeTerrainEditTool("edit_terrain_stroke", invalid, fixture.Host()).value("ok").toBool(), "unimplemented shallow-water tool is explicit failure");
    auto binding = request.value("terrain_binding").toObject(); binding["terrain_revision"] = 0;
    invalid = request; invalid["terrain_binding"] = binding;
    Expect(!InvokeTerrainEditTool("edit_terrain_stroke", invalid, fixture.Host()).value("ok").toBool(), "binding IDs require exact canonical string format");
    QJsonArray many; for (int i = 0; i < 257; ++i) many.append(QJsonObject{{"x", 0}, {"y", 0}});
    Expect(!InvokeTerrainEditTool("query_terrain_points", {{"terrain_binding", state.value("terrain_binding")}, {"points", many}}, fixture.Host()).value("ok").toBool(),
        "point query budget enforced");
    Expect(!fixture.edits.IsDirty() && !fixture.edits.CanUndo(), "all rejected requests leave source and undo untouched");
    fixture.edits.CommitSaved(fixture.edits.Snapshot());
    Expect(!InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host()).value("ok").toBool(), "baseline change invalidates otherwise equal edit revision");
    request = Stroke(State(fixture)); request["brush"] = QJsonObject{{"kind", "texture"}, {"size", 1}, {"ground_texture", "Lgrs"}};
    const auto preview = InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host());
    Expect(preview.value("ok").toBool() && !preview.value("terrain_pathing_gate").toObject().value("passed").toBool(),
        "unverified native-save eligibility remains visible in a valid project edit preview");
}

TerrainStrokeIntent Raise() {
    TerrainStrokeIntent intent; intent.brush.kind = TerrainBrushKind::Raise; intent.brush.size = 1; intent.brush.strength = 16;
    intent.points.push_back({0, 0}); return intent;
}

void CodecProvesSourceIntentAndNetBytes() {
    Fixture fixture; const auto directory = DirectoryWorkspace::Open(fixture.source);
    Expect(directory.Succeeded(), "original directory snapshot opens");
    const auto stroke = Raise();
    Expect(fixture.edits.EditTerrain(stroke.brush, stroke.points, false).Succeeded(), "height command applies");
    const auto seal = [&](const WorkspaceSnapshot& source, std::span<const TerrainStrokeIntent> strokes) {
        return TerrainPatchCodec::Seal(fixture.document, fixture.terrain, fixture.pathing, source, fixture.edits, strokes);
    };
    Expect(seal(directory.value->Snapshot(), std::span(&stroke, 1)).Succeeded(), "shared original command proves final bytes");
    auto wrong = directory.value->Snapshot(); wrong.files[0].sha256 = std::string(64, 'f');
    Expect(!seal(wrong, std::span(&stroke, 1)).Succeeded(), "source SHA substitution rejects");
    wrong = directory.value->Snapshot(); auto duplicate = wrong.files[0]; duplicate.relativePath = std::filesystem::path("map") / duplicate.relativePath;
    wrong.files.push_back(duplicate);
    Expect(!seal(wrong, std::span(&stroke, 1)).Succeeded(), "root/map duplicate source rejects");
    Expect(!seal(directory.value->Snapshot(), {}).Succeeded(), "no arbitrary-byte patch without intent");
    TerrainStrokeIntent overlap; overlap.brush.kind = TerrainBrushKind::PathingRestriction;
    overlap.brush.size = 1; overlap.brush.pathingRestrictions = 2; overlap.points = {{0, 0}};
    Expect(fixture.edits.EditTerrain(overlap.brush, overlap.points, false).Succeeded(), "source-overlap intent changes only ownership");
    Expect(!seal(directory.value->Snapshot(), std::span(&stroke, 1)).Succeeded(),
        "equal WPM bytes cannot hide an omitted overlap-intent command");
    const std::array withOverlap{stroke, overlap};
    Expect(seal(directory.value->Snapshot(), withOverlap).Succeeded(), "complete recipe preserves identical WPM and explicit ownership");
    Expect(fixture.edits.Undo(), "restore ownership before independent blight fixture");
    TerrainStrokeIntent extra; extra.brush.kind = TerrainBrushKind::Blight; extra.brush.size = 1; extra.points = {{128, 0}};
    Expect(fixture.edits.EditTerrain(extra.brush, extra.points, false).Succeeded(), "extra actual command applies");
    Expect(!seal(directory.value->Snapshot(), std::span(&stroke, 1)).Succeeded(), "omitted command cannot seal unrelated final WPM");
    const std::array intents{stroke, extra};
    Expect(seal(directory.value->Snapshot(), intents).Succeeded(), "complete cumulative intent seals both terrain and pathing");
    extra.brush.kind = TerrainBrushKind::Texture; extra.brush.groundTexture = "Lgrs";
    Expect(fixture.edits.EditTerrain(extra.brush, extra.points, false).Succeeded(), "unproven texture remains project-editable");
    const std::array blocked{stroke, intents[1], extra};
    Expect(!seal(directory.value->Snapshot(), blocked).Succeeded(), "matching bytes alone do not bypass native-save gate");
}

void FollowingPlacementsRequireExactSourceAndCompleteRecipe() {
    Fixture fixture(true);
    const auto directory = DirectoryWorkspace::Open(fixture.source);
    Expect(directory.Succeeded() && directory.value, "placement source directory opens");
    auto raise = Raise(); raise.followDoodads = true;
    TerrainStrokeIntent restrictionStroke;
    restrictionStroke.brush.kind = TerrainBrushKind::PathingRestriction;
    restrictionStroke.brush.size = 1; restrictionStroke.brush.pathingRestrictions = 8; restrictionStroke.points = {{0, 0}};
    Expect(fixture.edits.EditTerrain(raise.brush, raise.points, true).Succeeded()
        && fixture.edits.EditTerrain(restrictionStroke.brush, restrictionStroke.points, false).Succeeded(),
        "following height and restriction share one source session");
    const std::array recipe{raise, restrictionStroke};
    const auto seal = [&](const WorkspaceSnapshot& source, std::span<const TerrainStrokeIntent> strokes) {
        return TerrainPatchCodec::Seal(fixture.document, fixture.terrain, fixture.pathing, source, fixture.edits, strokes);
    };
    Expect(fixture.edits.Replacements().size() == 4 && seal(directory.value->Snapshot(), recipe).Succeeded(),
        "complete recipe seals W3E, WPM and both placement members");
    Expect(!seal(directory.value->Snapshot(), std::span(&raise, 1)).Succeeded(),
        "following elevation cannot hide an omitted restriction command");
    auto missingFollow = recipe; missingFollow[0].followDoodads = false;
    Expect(!seal(directory.value->Snapshot(), missingFollow).Succeeded(),
        "same terrain and WPM cannot hide an omitted following-placement instruction");
    for (const auto& member : fixture.placements) {
        auto wrong = directory.value->Snapshot();
        const auto found = std::find_if(wrong.files.begin(), wrong.files.end(), [&](const auto& file) {
            return file.relativePath == std::filesystem::path(member.name);
        });
        Expect(found != wrong.files.end(), "both placement sources are present in the source snapshot");
        found->sha256 = std::string(64, 'f');
        Expect(!seal(wrong, recipe).Succeeded(), "following recipe cannot borrow a different placement source SHA");
    }
    auto units = *fixture.edits.Snapshot().Units();
    units.units[0].position[0] += 1; units.units[0].position[2] += 1;
    Expect(fixture.edits.EditUnits(units).Succeeded(), "unit X and Z change remains a real draft edit");
    Expect(!fixture.edits.VerifyTerrainPathingForSave().Succeeded()
        && !seal(directory.value->Snapshot(), recipe).Succeeded(), "unit XZ change is not a terrain-following elevation");
    Expect(fixture.edits.Undo(), "undo unrelated unit movement");
    auto doodads = *fixture.edits.Snapshot().Doodads();
    doodads.doodads[0].position[0] += 1; doodads.doodads[0].position[2] += 1;
    Expect(fixture.edits.EditDoodads(doodads).Succeeded(), "doodad X and Z change remains a real draft edit");
    Expect(!fixture.edits.VerifyTerrainPathingForSave().Succeeded()
        && !seal(directory.value->Snapshot(), recipe).Succeeded(), "doodad XZ change is not a terrain-following elevation");
    Expect(fixture.edits.Undo() && seal(directory.value->Snapshot(), recipe).Succeeded(),
        "undo restores the exact four-member recipe");
    for (const auto& member : fixture.placements) {
        const auto source = IO::ReadFileBytes(fixture.source / member.name);
        Expect(source.Succeeded() && source.value && *source.value == member.data,
            "positive and rejected seals never modify placement sources");
    }
}

void SealedTerrainUsesExistingDirectoryTransaction() {
    Fixture fixture; const auto directory = DirectoryWorkspace::Open(fixture.source); const auto stroke = Raise();
    Expect(fixture.edits.EditTerrain(stroke.brush, stroke.points, false).Succeeded(), "candidate has a real common command");
    auto patch = TerrainPatchCodec::Seal(fixture.document, fixture.terrain, fixture.pathing,
        directory.value->Snapshot(), fixture.edits, std::span(&stroke, 1));
    Expect(patch.Succeeded(), "candidate sealed for directory transaction");
    auto editor = EditorSession::Open({fixture.source, CompatibilityProfile::Default(), "synthetic-catalog"});
    Expect(editor.Succeeded(), "existing directory editor opens");
    const auto revision = editor.value->Revision(); const auto transaction = editor.value->BeginEdit(revision);
    Expect(transaction.Succeeded() && editor.value->ApplyTerrainPatch(*transaction.value, std::move(*patch.value)).Succeeded(),
        "terrain patch enters existing EditorSession transaction");
    const auto preview = editor.value->PreviewEdit(*transaction.value);
    Expect(preview.Succeeded() && preview.value->changes.size() == 2
        && preview.value->changes.front().kind == "terrain", "normal preview identifies the terrain member domain");
    const auto output = std::filesystem::path(fixture.temporary.path().toStdWString()) / "candidate";
    Expect(editor.value->CommitCandidate(*transaction.value, revision, output).Succeeded(), "normal candidate save publishes sealed overlay");
    const auto writtenTerrain = IO::ReadFileBytes(output / "war3map.w3e");
    const auto writtenPathing = IO::ReadFileBytes(output / "war3map.wpm");
    MapDataEditSession reopened(fixture.document);
    Expect(writtenTerrain.Succeeded() && writtenPathing.Succeeded() && reopened.AttachTerrain(*writtenTerrain.value).Succeeded()
        && reopened.AttachPathing(*writtenPathing.value).Succeeded(), "candidate reopens through same parsers");
    Expect(reopened.InspectTerrainPoint({0, 0}).value->groundHeight == 16
        && *writtenPathing.value == fixture.pathing, "reopened height and preserved pathing match proven command");
    Expect(*IO::ReadFileBytes(fixture.source / "war3map.w3e").value == fixture.terrain, "candidate publication preserves original source");
}

void OpenWorkspace(Fixture& fixture, McpServer& server) {
    const auto ui = std::filesystem::path(fixture.temporary.path().toStdWString()) / "ui";
    const auto text = [&](const auto& path, const std::string& value) { Fixture::Write(path, std::as_bytes(std::span(value))); };
    text(ui / "config", "base\n"); text(ui / "base/action.txt", "[DoNothing]\ntitle=Do nothing\n");
    const auto catalog = Trigger::UiCatalog::Load(ui);
    Expect(catalog.Succeeded(), "original terrain test catalog loads");
    auto document = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
    Expect(document.Succeeded() && Trigger::TriggerDirectoryEditor::AddCategory(*document.value, "合成地形").Succeeded(), "original terrain trigger document");
    const auto pair = Trigger::WtgWctCodec{}.Serialize(*document.value, *catalog.value);
    Expect(pair.Succeeded(), "original trigger pair encodes");
    Fixture::Write(fixture.source / "war3map.wtg", pair.value->wtg);
    Fixture::Write(fixture.source / "war3map.wct", pair.value->wct);
    server.SetWorkspace(fixture.source, ui); RegisterAllTools(server);
    Expect(server.Workspace() != nullptr, "standalone terrain host opens actual workspace");
}

QJsonObject Call(McpServer& server, const char* tool, const QJsonObject& arguments = {}) {
    const auto message = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}}).toJson(QJsonDocument::Compact);
    const auto reply = server.HandleMessage(message);
    Expect(reply && !reply->error, "actual MCP tool accepts schema and dispatches");
    const auto content = reply->result.toObject().value("content").toArray();
    Expect(content.size() == 1, "MCP returns one bounded receipt");
    return QJsonDocument::fromJson(content.first().toObject().value("text").toString().toUtf8()).object();
}

QJsonObject Current(McpServer& server) {
    auto state = Call(server, "get_terrain_edit_state");
    Expect(state.value("ok").toBool(), "actual MCP terrain state available"); return state;
}

void BoundaryDraftSchemaAndNativeGateStaySeparate() {
    Fixture fixture;
    auto request = Stroke(State(fixture));
    request["brush"] = QJsonObject{{"kind", "boundary"}, {"size", 1}, {"value", true}};
    auto response = InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host());
    Expect(response.value("ok").toBool() && !response.value("terrain_pathing_gate").toObject().value("passed").toBool()
        && !fixture.edits.CanUndo(), "boundary schema previews a draft without granting native save");
    request["dry_run"] = false;
    response = InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host());
    Expect(response.value("ok").toBool() && response.value("applied").toBool(), "draft-capable host applies the shared boundary command");
    auto query = InvokeTerrainEditTool("query_terrain_points", {{"terrain_binding", State(fixture).value("terrain_binding")},
        {"points", request.value("points")}}, fixture.Host());
    const auto point = query.value("points").toArray()[0].toObject();
    Expect(point.value("nearest_vertex").toObject().value("boundary").toBool()
        && !point.value("nearest_vertex").toObject().value("map_edge").toBool()
        && point.value("pathing").toObject().value("raw_flags").toInt() == 2, "query separates boundary, outer edge and unchanged WPM");
    Expect(fixture.edits.Undo() && !fixture.edits.IsDirty(), "boundary draft uses original undo");

    McpServer server; OpenWorkspace(fixture, server);
    auto* workspace = server.Workspace();
    const auto transaction = workspace->BeginEdit(workspace->Revision());
    Expect(transaction.Succeeded(), "standalone boundary transaction opens");
    request = Stroke(Current(server));
    request["brush"] = QJsonObject{{"kind", "boundary"}, {"value", true}};
    const auto before = Current(server);
    Expect(Call(server, "edit_terrain_stroke", request).value("ok").toBool(), "standalone boundary preview is supported");
    request["dry_run"] = false;
    Expect(!Call(server, "edit_terrain_stroke", request).value("ok").toBool(), "standalone candidate host refuses unproven boundary before mutation");
    Expect(Current(server) == before, "rejected boundary candidate retains source, generation and original typed session");
}

void PathingRestrictionsKeepSourceBitsThroughNormalDirectorySave() {
    Fixture fixture;
    auto request = Stroke(State(fixture));
    request["brush"] = QJsonObject{{"kind", "pathing_restriction"}, {"blocked_type", "fly"}, {"value", true}};
    auto response = InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host());
    Expect(response.value("ok").toBool() && response.value("terrain_pathing_gate").toObject().value("passed").toBool()
        && !response.value("map_saved").toBool() && !fixture.edits.CanUndo(), "pathing preview shares save eligibility without claiming a write");
    request["dry_run"] = false;
    response = InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host());
    Expect(response.value("ok").toBool() && response.value("applied").toBool(), "draft-capable host applies pathing via shared command");
    const auto query = InvokeTerrainEditTool("query_terrain_points", {{"terrain_binding", State(fixture).value("terrain_binding")},
        {"points", request.value("points")}}, fixture.Host());
    const auto flags = query.value("points").toArray()[0].toObject().value("pathing").toObject();
    Expect(flags.value("raw_flags").toInt() == 6 && flags.value("source_flags").toInt() == 2
        && flags.value("added_restrictions").toInt() == 4, "query identifies original and added restrictions");
    auto remove = Stroke(State(fixture)); remove["dry_run"] = false;
    remove["brush"] = QJsonObject{{"kind", "pathing_restriction"}, {"blocked_type", "walk"}, {"value", false}};
    Expect(InvokeTerrainEditTool("edit_terrain_stroke", remove, fixture.Host()).value("ok").toBool()
        && fixture.edits.InspectTerrainPoint({0, 0}).value->pathingFlags == 6, "removal cannot erase original walk bit");
    Expect(fixture.edits.Undo() && !fixture.edits.IsDirty(), "pathing uses the original undo stack");
    McpServer server; OpenWorkspace(fixture, server);
    auto* workspace = server.Workspace(); const auto revision = workspace->Revision();
    const auto transaction = workspace->BeginEdit(revision);
    Expect(transaction.Succeeded(), "standalone transaction opens");
    request = Stroke(Current(server));
    request["brush"] = QJsonObject{{"kind", "pathing_restriction"}, {"blocked_type", "build"}};
    Expect(Call(server, "edit_terrain_stroke", request).value("ok").toBool(), "standalone preview accepts typed pathing intent");
    request["dry_run"] = false;
    Expect(Call(server, "edit_terrain_stroke", request).value("ok").toBool(), "standalone applies the same fully declared restriction");
    Expect(workspace->PreviewEdit(*transaction.value, revision).Succeeded()
        && workspace->ValidateEdit(*transaction.value, revision).satisfied, "normal directory transaction validates the complete stroke recipe");
    const auto output = std::filesystem::path(fixture.temporary.path().toStdWString()) / "pathing-candidate";
    Expect(workspace->ExportCandidate(*transaction.value, revision, output).Succeeded(), "normal directory exporter writes restriction output");
    const auto terrain = IO::ReadFileBytes(output / "war3map.w3e"), pathing = IO::ReadFileBytes(output / "war3map.wpm");
    Expect(terrain.Succeeded() && pathing.Succeeded() && *terrain.value == fixture.terrain, "pathing output preserves every W3E byte");
    auto expected = fixture.pathing; expected[16 + 32 * 64 + 32] = std::byte{0x0a};
    Expect(*pathing.value == expected, "output changes only the declared cell and retains its original walk restriction");
    Expect(*IO::ReadFileBytes(fixture.source / "war3map.w3e").value == fixture.terrain
        && *IO::ReadFileBytes(fixture.source / "war3map.wpm").value == fixture.pathing, "original directory remains byte-identical");
}

void FollowingPlacementsAndRestrictionsUseNormalDirectorySave() {
    for (const bool restrictionFirst : {false, true}) {
        Fixture fixture(true); McpServer server; OpenWorkspace(fixture, server);
        const auto source = DirectoryWorkspace::Open(fixture.source);
        Expect(source.Succeeded() && source.value, "freeze original directory with both placement members");
        auto* workspace = server.Workspace(); const auto revision = workspace->Revision();
        const auto transaction = workspace->BeginEdit(revision);
        Expect(transaction.Succeeded() && transaction.value, "following placement transaction opens");
        for (const bool restriction : {restrictionFirst, !restrictionFirst}) {
            auto request = Stroke(Current(server));
            if (restriction) request["brush"] = QJsonObject{{"kind", "pathing_restriction"}, {"size", 1}, {"blocked_type", "build"}};
            else request["follow_doodads"] = true;
            const auto before = Current(server);
            const auto preview = Call(server, "edit_terrain_stroke", request);
            Expect(preview.value("ok").toBool() && preview.value("terrain_pathing_gate").toObject().value("passed").toBool()
                && Current(server) == before, "both stroke orders preview without consuming terrain identity");
            request["dry_run"] = false;
            const auto applied = Call(server, "edit_terrain_stroke", request);
            Expect(applied.value("ok").toBool() && applied.value("applied").toBool(),
                "registered MCP applies height following and explicit restrictions through shared session");
            Expect(!Call(server, "edit_terrain_stroke", request).value("ok").toBool(),
                "four-member support preserves stale terrain binding refusal");
        }
        const auto preview = workspace->PreviewEdit(*transaction.value, revision);
        Expect(preview.Succeeded() && preview.value && preview.value->changes.size() == 4
            && workspace->ValidateEdit(*transaction.value, revision).satisfied,
            "normal directory transaction retains all four proven native members");
        const auto output = std::filesystem::path(fixture.temporary.path().toStdWString()) / "following-candidate";
        Expect(workspace->ExportCandidate(*transaction.value, revision, output).Succeeded(),
            "normal directory exporter publishes height, restrictions and both following placements");
        auto units = *fixture.document.Units(); units.units[0].position[2] += 16;
        auto doodads = *fixture.document.Doodads(); doodads.doodads[0].position[2] += 16;
        const auto expectedUnits = units.Serialize(), expectedDoodads = doodads.Serialize();
        Expect(expectedUnits.Succeeded() && expectedUnits.value && expectedDoodads.Succeeded() && expectedDoodads.value,
            "expected placement bytes encode");
        const auto savedUnits = IO::ReadFileBytes(output / "war3mapUnits.doo");
        const auto savedDoodads = IO::ReadFileBytes(output / "war3map.doo");
        const auto savedTerrain = IO::ReadFileBytes(output / "war3map.w3e"), savedPathing = IO::ReadFileBytes(output / "war3map.wpm");
        Expect(savedUnits.Succeeded() && savedUnits.value && savedDoodads.Succeeded() && savedDoodads.value
            && savedTerrain.Succeeded() && savedTerrain.value && savedPathing.Succeeded() && savedPathing.value,
            "all four exported native files independently reopen");
        Expect(*savedUnits.value == *expectedUnits.value && *savedDoodads.value == *expectedDoodads.value,
            "only covered Z values change; other placements, fields and special tail remain exact");
        auto expectedPathing = fixture.pathing; expectedPathing[16 + 32 * 64 + 32] = std::byte{0x0a};
        Expect(*savedPathing.value == expectedPathing, "following placement save preserves protected WPM bits and exact unedited cells");
        const std::array reopenedMembers{MapArchiveMember{"war3mapUnits.doo", *savedUnits.value},
            MapArchiveMember{"war3map.doo", *savedDoodads.value}};
        auto reopened = MapDocument::FromMembers(reopenedMembers);
        MapDataEditSession edits(reopened);
        Expect(reopened.Units() && reopened.Doodads() && edits.AttachTerrain(*savedTerrain.value).Succeeded()
            && edits.AttachPathing(*savedPathing.value).Succeeded(),
            "exported terrain and both native placement parsers reopen the same elevation change");
        const auto center = edits.InspectTerrainPoint({0, 0});
        Expect(center.Succeeded() && center.value && center.value->groundHeight == 16,
            "independent reopened terrain has the saved height");
        const auto sourceAfter = DirectoryWorkspace::Open(fixture.source);
        Expect(sourceAfter.Succeeded() && sourceAfter.value
            && sourceAfter.value->Snapshot().aggregateSha256 == source.value->Snapshot().aggregateSha256,
            "normal MCP export preserves the complete original directory identity");
        for (const auto& file : source.value->Snapshot().files) {
            const auto name = IO::GenericPathUtf8(file.relativePath);
            if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapUnits.doo" || name == "war3map.doo") continue;
            const auto before = IO::ReadFileBytes(fixture.source / file.relativePath);
            const auto after = IO::ReadFileBytes(output / file.relativePath);
            Expect(before.Succeeded() && after.Succeeded() && *before.value == *after.value,
                "normal terrain export keeps trigger pair and every unrelated member exact");
        }
    }
}

void FollowingHeightRestorationRemovesObsoletePlacementOverlay() {
    Fixture fixture(true); McpServer server; OpenWorkspace(fixture, server);
    auto* workspace = server.Workspace();
    const auto revision = workspace->Revision();
    const auto transaction = workspace->BeginEdit(revision);
    Expect(transaction.Succeeded(), "restoration starts a normal directory transaction");
    auto raise = Stroke(Current(server), false); raise["follow_doodads"] = true;
    Expect(Call(server, "edit_terrain_stroke", raise).value("ok").toBool(), "first following stroke applies");
    Expect(workspace->PreviewEdit(*transaction.value, revision).value->changes.size() == 4,
        "first stroke owns two placement outputs");
    auto lower = Stroke(Current(server), false); lower["follow_doodads"] = true;
    lower["brush"] = QJsonObject{{"kind", "lower"}, {"size", 1}, {"strength", 16}};
    Expect(Call(server, "edit_terrain_stroke", lower).value("ok").toBool(), "opposite following stroke restores original elevations");
    const auto preview = workspace->PreviewEdit(*transaction.value, revision);
    Expect(preview.Succeeded() && preview.value->changes.size() == 2
        && std::all_of(preview.value->changes.begin(), preview.value->changes.end(),
            [](const auto& change) { return change.beforeSha256 == change.afterSha256; })
        && workspace->ValidateEdit(*transaction.value, revision).satisfied,
        "replacement recipe drops stale placement overlays and keeps exact source restoration");
    const auto output = std::filesystem::path(fixture.temporary.path().toStdWString()) / "restored-placements";
    Expect(workspace->ExportCandidate(*transaction.value, revision, output).Succeeded(), "restored directory exports normally");
    for (const auto& member : fixture.placements)
        Expect(IO::ReadFileBytes(output / member.name).value == std::optional(member.data), "original placement bytes survive net-zero height edits");
    Expect(IO::ReadFileBytes(output / "war3map.w3e").value == std::optional(fixture.terrain)
        && IO::ReadFileBytes(output / "war3map.wpm").value == std::optional(fixture.pathing),
        "net-zero candidate preserves every terrain and pathing byte");
}

void RegisteredHostRetainsTransactionAndPublishesDirectory() {
    Fixture fixture; McpServer server; OpenWorkspace(fixture, server);
    auto* workspace = server.Workspace();
    const auto initial = Current(server), dry = Call(server, "edit_terrain_stroke", Stroke(initial));
    Expect(dry.value("ok").toBool() && !workspace->HasActiveTransaction(), "outside-transaction preview must not acquire a writer");
    Expect(!Call(server, "edit_terrain_stroke", Stroke(initial, false)).value("ok").toBool(), "actual write needs explicit transaction");
    const auto revision = workspace->Revision(); const auto transaction = workspace->BeginEdit(revision);
    Expect(transaction.Succeeded(), "existing directory transaction opens");
    Expect(!Call(server, "edit_terrain_stroke", Stroke(initial, false)).value("ok").toBool(), "begin invalidates old terrain identity");
    const auto before = workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration());
    const auto request = Stroke(Current(server), false);
    const auto applied = Call(server, "edit_terrain_stroke", request);
    Expect(applied.value("ok").toBool() && applied.value("applied").toBool(), "registered tool writes common typed command");
    const auto after = workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration());
    Expect(before.value->session == after.value->session && after.value->session->Revision() == 1
        && after.value->session->InspectTerrainPoint({0, 0}).value->groundHeight == 16,
        "publication keeps original typed session and undo history");
    Expect(!Call(server, "edit_terrain_stroke", request).value("ok").toBool(), "same request cannot be replayed on new revision");
    auto blight = Stroke(Current(server), false); blight["brush"] = QJsonObject{{"kind", "blight"}, {"size", 1}, {"value", true}};
    blight["points"] = QJsonArray{QJsonObject{{"x", 128}, {"y", 0}}};
    Expect(Call(server, "edit_terrain_stroke", blight).value("ok").toBool(), "second stroke accumulates into same terrain transaction");
    const auto category = workspace->VisibleDocument().categories.front().categoryId;
    Expect(!workspace->CreateTrigger(*transaction.value, revision, category, "不应混入").Succeeded(), "unproven mixed trigger edit rejects before publishing");
    const auto preview = workspace->PreviewEdit(*transaction.value, revision);
    Expect(preview.Succeeded() && preview.value->changes.size() == 2 && workspace->ValidateEdit(*transaction.value, revision).satisfied,
        "normal preview and validation prove exactly two replayed terrain members");
    const auto output = std::filesystem::path(fixture.temporary.path().toStdWString()) / "host-candidate";
    Expect(workspace->ExportCandidate(*transaction.value, revision, output).Succeeded(), "normal directory candidate exporter publishes terrain");
    MapDataEditSession reopened(fixture.document);
    const auto w3e = IO::ReadFileBytes(output / "war3map.w3e"), wpm = IO::ReadFileBytes(output / "war3map.wpm");
    Expect(w3e.Succeeded() && wpm.Succeeded() && reopened.AttachTerrain(*w3e.value).Succeeded()
        && reopened.AttachPathing(*wpm.value).Succeeded() && reopened.InspectTerrainPoint({0, 0}).value->groundHeight == 16
        && reopened.InspectTerrainPoint({128, 0}).value->vertex.IsBlight(), "candidate readback retains both cumulative commands");
    Expect(*IO::ReadFileBytes(fixture.source / "war3map.w3e").value == fixture.terrain
        && *IO::ReadFileBytes(fixture.source / "war3map.wpm").value == fixture.pathing, "host never writes original directory");
    const auto nextRevision = workspace->Revision(); const auto next = workspace->BeginEdit(nextRevision);
    Expect(next.Succeeded(), "new transaction retains prior candidate overlay");
    const auto continued = workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration());
    Expect(continued.value->session != after.value->session && continued.value->session->InspectTerrainPoint({0, 0}).value->groundHeight == 16,
        "new transaction has fresh session identity and full retained stroke recipe");
    auto lower = Stroke(Current(server), false); lower["brush"] = QJsonObject{{"kind", "lower"}, {"size", 1}, {"strength", 16}};
    Expect(Call(server, "edit_terrain_stroke", lower).value("ok").toBool(), "continuation may restore original terrain bytes");
    auto clear = Stroke(Current(server), false); clear["brush"] = QJsonObject{{"kind", "blight"}, {"size", 1}, {"value", false}};
    clear["points"] = QJsonArray{QJsonObject{{"x", 128}, {"y", 0}}};
    Expect(Call(server, "edit_terrain_stroke", clear).value("ok").toBool(), "continuation restores original WPM bits too");
    const auto restored = workspace->PreviewEdit(*next.value, nextRevision);
    Expect(restored.Succeeded() && restored.value->changes.size() == 2
        && std::all_of(restored.value->changes.begin(), restored.value->changes.end(), [](const auto& change) { return change.beforeSha256 == change.afterSha256; })
        && workspace->ValidateEdit(*next.value, nextRevision).satisfied, "explicit restoration replaces prior overlays with exact original bytes");
    Expect(workspace->Rollback(*next.value, nextRevision).Succeeded(), "rollback preserves prior published candidate");
    const auto retained = workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration());
    Expect(retained.value->session->InspectTerrainPoint({0, 0}).value->groundHeight == 16, "rollback discards active edits only");
}

void HostRejectsWithoutPartialOverlayOrSourceWrites() {
    Fixture fixture; McpServer server; OpenWorkspace(fixture, server); auto* workspace = server.Workspace();
    const auto revision = workspace->Revision(); const auto transaction = workspace->BeginEdit(revision);
    Expect(transaction.Succeeded(), "terrain failure fixture transaction starts");
    const auto context = workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration());
    const auto state = CaptureTerrainEditState(*context.value->session, context.value->document);
    const auto stroke = Raise();
    const auto plan = PrepareTerrainEdit(*context.value->session, context.value->document, state.value->binding, stroke.brush, stroke.points, false);
    Expect(plan.Succeeded(), "sealed host plan prepares");
    auto zero = stroke.brush; zero.kind = TerrainBrushKind::Plateau;
    const auto unchanged = PrepareTerrainEdit(*context.value->session, context.value->document, state.value->binding, zero, stroke.points, false);
    const auto unchangedRevision = workspace->ObjectRevision();
    Expect(unchanged.Succeeded() && !unchanged.value->Plan().HasChanges() && workspace->ApplyTerrainPlan(*unchanged.value).Succeeded()
        && workspace->ObjectRevision() == unchangedRevision && workspace->Session()->PreviewEdit(*transaction.value).value->changes.empty(),
        "direct host no-op does not seed terrain overlay or consume generation");
    auto rejected = workspace->ApplyTerrainPlan(*plan.value, [&](const auto&) {
        Expect(!workspace->Rollback(*transaction.value, revision).Succeeded(), "prepublish reentry cannot destroy owning transaction");
        return Status::Failure("Test", "TEST_DENIED", "拒绝地形发布。");
    });
    Expect(!rejected.Succeeded() && context.value->session->Revision() == 0 && !context.value->session->CanUndo()
        && workspace->Session()->PreviewEdit(*transaction.value).value->changes.empty(), "callback rejection leaves both typed session and EditorSession overlay untouched");
    auto texture = Stroke(Current(server), false); texture["brush"] = QJsonObject{{"kind", "texture"}, {"size", 1}, {"ground_texture", "Lgrs"}};
    Expect(!Call(server, "edit_terrain_stroke", texture).value("ok").toBool() && context.value->session->Revision() == 0,
        "unproven texture save gate rejects before actual typed commit");
    Expect(workspace->ApplyTerrainPlan(*plan.value).Succeeded(), "rejected publish does not consume the original plan");
    Expect(workspace->Rollback(*transaction.value, revision).Succeeded(), "real command rolls back through original transaction");
    const auto second = workspace->BeginEdit(revision);
    Expect(second.Succeeded(), "next transaction from original source opens");
    const auto trigger = workspace->CreateTrigger(*second.value, revision, workspace->VisibleDocument().categories.front().categoryId, "原事务触发器");
    Expect(trigger.Succeeded() && !Call(server, "edit_terrain_stroke", Stroke(Current(server), false)).value("ok").toBool(),
        "terrain cannot overwrite an existing trigger transaction");
    Expect(workspace->Session()->PreviewEdit(*second.value).value->changes.size() == 2, "rejected terrain preserves original trigger pair only");
}

void MissingPathingRemainsReadableAndSourceDriftRejects() {
    Fixture fixture;
    Expect(std::filesystem::remove(fixture.source / "war3map.wpm"), "remove own synthetic WPM for missing-source scenario");
    McpServer server; OpenWorkspace(fixture, server); auto* workspace = server.Workspace();
    const auto state = Current(server);
    Expect(!state.value("pathing_available").toBool(), "missing WPM is explicit in query state");
    const auto points = Call(server, "query_terrain_points", {{"terrain_binding", state.value("terrain_binding")},
        {"points", QJsonArray{QJsonObject{{"x", 0}, {"y", 0}}}}});
    Expect(points.value("ok").toBool(), "missing pathing does not prevent reading terrain height");
    const auto transaction = workspace->BeginEdit(workspace->Revision());
    Expect(transaction.Succeeded() && !Call(server, "edit_terrain_stroke", Stroke(Current(server), false)).value("ok").toBool(),
        "missing WPM cannot become a saveable terrain transaction");
    auto changed = fixture.terrain; changed.back() = std::byte{3}; Fixture::Write(fixture.source / "war3map.w3e", changed);
    Expect(!Call(server, "get_terrain_edit_state").value("ok").toBool(), "readonly source drift invalidates cached terrain state");
}

void ArchiveExportBindsPathingAndCompanionMembers() {
    Fixture fixture; McpServer server; OpenWorkspace(fixture, server); auto* workspace = server.Workspace();
    const auto revision = workspace->Revision(); const auto transaction = workspace->BeginEdit(revision);
    Expect(transaction.Succeeded() && Call(server, "edit_terrain_stroke", Stroke(Current(server), false)).value("ok").toBool(), "archive fixture terrain edit applies");
    Expect(workspace->PreviewEdit(*transaction.value, revision).Succeeded() && workspace->ValidateEdit(*transaction.value, revision).satisfied,
        "archive fixture uses normal preview and validation");
    const auto outputRoot = std::filesystem::path(fixture.temporary.path().toStdWString());
    std::vector<MapArchiveMember> members{{"war3map.w3e", fixture.terrain}, {"war3map.wpm", fixture.pathing},
        {"war3map.wtg", *IO::ReadFileBytes(fixture.source / "war3map.wtg").value},
        {"war3map.wct", *IO::ReadFileBytes(fixture.source / "war3map.wct").value}};
    const auto mismatch = [&](const char* name, const std::vector<MapArchiveMember>& inputs) {
        const auto source = outputRoot / (std::string(name) + ".w3x");
        const auto archive = WriteVerifiedArchive(source, inputs);
        Expect(archive.Succeeded(), "original synthetic archive generated");
        MapEditSaveRequest request; request.sourceMap = source; request.expectedSourceSha256 = archive.value->sha256;
        request.targetCandidate = outputRoot / (std::string(name) + "-candidate.w3x");
        request.toolRoot = outputRoot; request.gameRoot = outputRoot;
        const auto result = workspace->ExportArchiveCandidate(*transaction.value, revision, request);
        Expect(!result.Succeeded() && !std::filesystem::exists(request.targetCandidate), "mismatched or script-incomplete synthetic archive never publishes");
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& item) { return item.code == "MCP_TERRAIN_ARCHIVE_SOURCE_MISMATCH"; });
    };
    auto wrong = members; wrong[1].data.back() ^= std::byte{1};
    Expect(mismatch("changed-pathing", wrong), "same W3E with different WPM must not borrow directory terrain proof");
    wrong = members; wrong[2].data.back() ^= std::byte{1};
    Expect(mismatch("changed-trigger", wrong), "different trigger source is not the directory companion");
    wrong = members; wrong.push_back({"war3mapUnits.doo", {std::byte{0}}});
    Expect(mismatch("extra-placement", wrong), "missing source member cannot be replaced with an unbound archive member");
    Expect(!mismatch("matching-source", members), "same raw members pass source gate and continue into unchanged script-save checks");
    Expect(workspace->HasActiveTransaction() && workspace->Session()->PreviewEdit(*transaction.value).value->changes.size() == 2,
        "archive failures retain active directory edits");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        FacadeQueryPreviewApplyAndUndo();
        FacadeRejectsUnknownTypesBoundsAndStaleSources();
        CodecProvesSourceIntentAndNetBytes();
        FollowingPlacementsRequireExactSourceAndCompleteRecipe();
        SealedTerrainUsesExistingDirectoryTransaction();
        RegisteredHostRetainsTransactionAndPublishesDirectory();
        HostRejectsWithoutPartialOverlayOrSourceWrites();
        MissingPathingRemainsReadableAndSourceDriftRejects();
        ArchiveExportBindsPathingAndCompanionMembers();
        BoundaryDraftSchemaAndNativeGateStaySeparate();
        PathingRestrictionsKeepSourceBitsThroughNormalDirectorySave();
        FollowingPlacementsAndRestrictionsUseNormalDirectorySave();
        FollowingHeightRestorationRemovesObsoletePlacementOverlay();
        std::cout << "12 terrain MCP and sealed transaction scenarios passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
