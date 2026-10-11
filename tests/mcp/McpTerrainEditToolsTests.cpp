#include "McpTerrainEditTools.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/TerrainPatchCodec.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>

#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using namespace War3Map::Mcp;

namespace {

void Expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    QTemporaryDir temporary;
    std::filesystem::path source;
    std::vector<std::byte> terrain, pathing;
    MapDocument document = MapDocument::FromMembers({});
    MapDataEditSession edits{document};
    Fixture() {
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

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        FacadeQueryPreviewApplyAndUndo();
        FacadeRejectsUnknownTypesBoundsAndStaleSources();
        CodecProvesSourceIntentAndNetBytes();
        SealedTerrainUsesExistingDirectoryTransaction();
        std::cout << "4 terrain MCP and sealed transaction scenarios passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
