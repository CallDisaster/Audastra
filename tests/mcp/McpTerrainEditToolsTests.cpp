#include "McpTerrainEditTools.h"
#include "McpTools.h"
#include "McpWorkspace.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapDataFieldPatchCodec.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/TerrainPatchCodec.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "../support/TerrainBoundsFixture.h"
#include "../TerrainBlockFixtures.h"
#include "../TerrainConnectedRampFixtures.h"
#include "../support/TerrainTextureSlotSaveFixture.h"

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
template<class T> T TerrainTake(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "terrain integration failed";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}

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
    explicit Fixture(bool withPlacements = false, bool withWater = false)
        : placements(withPlacements ? FollowPlacementMembers() : std::vector<MapArchiveMember>{}),
          document(MapDocument::FromMembers(placements)), edits(document) {
        Expect(temporary.isValid(), "temporary directory available");
        source = std::filesystem::path(temporary.path().toStdWString()) / "source";
        TerrainGridSnapshot grid;
        grid.header.width = 17; grid.header.height = 17; grid.header.offset = {-1024, -1024}; grid.header.mainTileset = 'L';
        grid.header.groundTilesets = {"Ldrt", "Lgrs"}; grid.header.cliffTilesets = {"CLdi"};
        grid.vertices.resize(17 * 17);
        for (auto& vertex : grid.vertices) { vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2; }
        if (withWater) for (auto& vertex : grid.vertices) {
            vertex.textureAndFlagsRaw = 0x40; vertex.waterHeightAndFlagsRaw = 8384;
        }
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
    std::filesystem::path InstallWaterSources() {
        const auto text = [](std::string_view value) {
            return std::vector<std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                reinterpret_cast<const std::byte*>(value.data() + value.size()));
        };
        std::fill(pathing.begin() + 16, pathing.end(), std::byte{0x51});
        Write(source / "war3map.wpm", pathing);
        const std::vector<MapArchiveMember> resources{
            {"TerrainArt/Terrain.slk", text("ID;PWXL;N;E\nC;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\n"
                "C;X3;K\"flyable\"\nC;X4;K\"buildable\"\nC;X1;Y2;K\"Ldrt\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\n"
                "C;X1;Y3;K\"Lgrs\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\nE\n")},
            {"TerrainArt/Water.slk", text("ID;PWXL;N;E\nC;X1;Y1;K\"waterID\"\nC;X2;K\"height\"\n"
                "C;X3;K\"impassable\"\nC;X1;Y2;K\"LSha\"\nC;X2;K0\nC;X3;K0\nE\n")},
            {"TerrainArt/CliffTypes.slk", text("ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nC;X1;Y2;K\"CLdi\"\nE\n")},
            {"UI/MiscData.txt", text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n"
                "[Water]\nDeepLevelPathing=128\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")},
            {"UI/WorldEditData.txt", text("[Other]\nValue=1\n")},
            {"war3map.doo", TerrainTake(DoodadPlacements{}.Serialize())},
            {"war3mapUnits.doo", TerrainTake(UnitPlacements{}.Serialize())},
            {"war3map.j", text("// original owned source\n")}};
        for (const auto& member : resources) Write(source / member.name, member.data);
        MapSettings settings; settings.formatVersion = 25; settings.custom.emplace(); settings.tileset = 'L';
        settings.playableWidth = settings.playableHeight = 16; settings.flags |= 0x400;
        const auto settingsBytes = settings.Serialize(); Expect(settingsBytes.Succeeded(), "owned native water bounds encode");
        Write(source / "war3map.w3i", *settingsBytes.value);
        const auto game = std::filesystem::path(temporary.path().toStdWString()) / "game";
        Expect(std::filesystem::create_directory(game), "private water game directory");
        const auto archive = WriteVerifiedArchive(game / "war3.mpq",
            std::vector<MapArchiveMember>{{"fixture.txt", text("owned water fallback archive")}});
        Expect(archive.Succeeded(), "owned fallback archive fixes optional resource absence");
        return game;
    }
};

struct ScopedWaterGameRoot {
    bool present = qEnvironmentVariableIsSet("FORGE_MCP_WAR3_ROOT");
    QByteArray previous = qgetenv("FORGE_MCP_WAR3_ROOT");
    explicit ScopedWaterGameRoot(const std::filesystem::path& path) {
        Expect(qputenv("FORGE_MCP_WAR3_ROOT", QString::fromStdWString(path.wstring()).toUtf8()), "set owned MCP water source");
    }
    ~ScopedWaterGameRoot() {
        if (present) qputenv("FORGE_MCP_WAR3_ROOT", previous);
        else qunsetenv("FORGE_MCP_WAR3_ROOT");
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

QJsonObject WaterStroke(const QJsonObject& state, bool deep, bool dryRun = true) {
    auto result = Stroke(state, dryRun);
    result["brush"] = QJsonObject{{"kind", deep ? "deep_water" : "shallow_water"}, {"shape", "circle"},
        {"size", 1}, {"cliff_texture", 0}, {"seed", 4294967295.0}, {"enforce_water_limits", true}};
    return result;
}

void FacadeQueryPreviewApplyAndUndo() {
    Fixture fixture;
    const auto definitions = TerrainEditToolDefinitions();
    Expect(definitions.size() == 7 && std::all_of(definitions.begin(), definitions.end(), [](const auto& definition) {
        return definition.permission == (definition.name == "get_terrain_edit_state" || definition.name == "query_terrain_points"
            ? ToolPermission::Query : ToolPermission::Edit);
    }),
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

void FacadePointQueryPreservesApproximateHeightWarning() {
    Fixture fixture;
    const auto parsed = TerrainTake(ParseMapTerrain(fixture.terrain));
    auto grid = *parsed.Snapshot();
    grid.vertices[8 * 17 + 8].textureAndFlagsRaw |= 0x10;
    const auto wire = TerrainTake(SerializeMapTerrain(grid));
    Expect(fixture.edits.SetTerrain(wire).Succeeded(), "install explicit packed ramp fixture");
    const auto revision = fixture.edits.Revision();
    const auto query = InvokeTerrainEditTool("query_terrain_points",
        {{"terrain_binding", State(fixture).value("terrain_binding")},
         {"points", QJsonArray{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 0}, {"y", 0}}}}}, fixture.Host());
    const auto points = query.value("points").toArray();
    const auto diagnostics = query.value("diagnostics").toArray();
    Expect(query.value("ok").toBool() && points.size() == 2
        && points[0].toObject().value("ground_height_approximate").toBool()
        && points[1].toObject().value("ground_height_approximate").toBool()
        && diagnostics.size() == 1
        && diagnostics[0].toObject().value("code") == "TERRAIN_HEIGHT_APPROXIMATE",
        "shared MCP facade preserves per-point approximation and deduplicated source warning");
    Expect(fixture.edits.Revision() == revision
        && TerrainTake(SerializeMapTerrain(*fixture.edits.Terrain())) == wire,
        "approximate point query cannot change raw terrain or history");
}

void FacadeRemoveWaterUsesTheOrdinaryTransaction() {
    Fixture fixture(true, true);
    const auto state = State(fixture);
    auto request = Stroke(state);
    request["brush"] = QJsonObject{{"kind", "remove_water"}, {"shape", "square"}, {"size", 1}};
    request["follow_doodads"] = true;
    const auto preview = InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host());
    Expect(preview.value("ok").toBool() && !preview.value("applied").toBool()
        && !preview.value("terrain_pathing_gate").toObject().value("passed").toBool()
        && !fixture.edits.CanUndo(), "removal preview is an ordinary draft and preserves the full native save gate");
    for (const auto* field : {"water_height", "cliff_texture", "seed", "enforce_water_limits", "value", "anchor", "strength"}) {
        auto invalid = request; auto brush = request.value("brush").toObject(); brush[field] = 1; invalid["brush"] = brush;
        Expect(!InvokeTerrainEditTool("edit_terrain_stroke", invalid, fixture.Host()).value("ok").toBool(),
            "remove-water accepts only its ordinary footprint, never native-water or height options");
    }
    request["dry_run"] = false;
    const auto applied = InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host());
    Expect(applied.value("ok").toBool() && applied.value("applied").toBool()
        && !fixture.edits.Terrain()->vertices[8 * 17 + 8].HasWater(),
        "removal applies without a native-water resource host");
    const auto changed = TerrainTake(SerializeMapTerrain(*fixture.edits.Terrain()));
    const auto doo = fixture.edits.Snapshot().Doodads()->Serialize(), units = fixture.edits.Snapshot().Units()->Serialize();
    Expect(doo.Succeeded() && units.Succeeded() && *units.value == fixture.placements[0].data
        && *doo.value == fixture.placements[1].data && !fixture.edits.LastEdit().ChangesPlacements(),
        "MCP following keeps ordinary/special doodads and units byte-identical");
    Expect(!InvokeTerrainEditTool("edit_terrain_stroke", request, fixture.Host()).value("ok").toBool(),
        "the same removal envelope cannot replay across its revision change");
    Expect(fixture.edits.Undo() && TerrainTake(SerializeMapTerrain(*fixture.edits.Terrain())) == fixture.terrain
        && fixture.edits.Redo() && TerrainTake(SerializeMapTerrain(*fixture.edits.Terrain())) == changed,
        "MCP removal owns the shared byte-exact undo and redo command");
    Expect(TerrainTake(IO::ReadFileBytes(fixture.source / "war3map.w3e")) == fixture.terrain,
        "MCP edit does not write its source map");
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
    invalid = WaterStroke(state, false);
    const auto unavailable = InvokeTerrainEditTool("edit_terrain_stroke", invalid, fixture.Host());
    Expect(!unavailable.value("ok").toBool() && unavailable.value("diagnostics").toArray().size() == 1
        && unavailable.value("diagnostics").toArray()[0].toObject().value("code") == "TERRAIN_WATER_HOST_UNAVAILABLE",
        "water without an actual resource-capture host refuses explicitly");
    auto corner = request; corner["brush"] = QJsonObject{{"kind", "corner_ramp"}, {"value", true}};
    corner["points"] = QJsonArray{QJsonObject{{"x", 1056}, {"y", 1056}}};
    const auto unboundCorner = InvokeTerrainEditTool("edit_terrain_stroke", corner, fixture.Host());
    Expect(!unboundCorner.value("ok").toBool()
        && unboundCorner.value("diagnostics").toArray()[0].toObject().value("code") == "TERRAIN_CORNER_RAMP_HOST_UNAVAILABLE",
        "corner ramp must require actual rules even in a dry run");
    for (const char* field : {"size", "shape", "anchor", "strength", "enforce_water_limits"}) {
        auto conflict = corner; auto fields = conflict.value("brush").toObject(); fields[field] = 1; conflict["brush"] = fields;
        const auto result = InvokeTerrainEditTool("edit_terrain_stroke", conflict, fixture.Host());
        Expect(!result.value("ok").toBool()
            && result.value("diagnostics").toArray()[0].toObject().value("code") == "TERRAIN_ARGUMENT_UNKNOWN",
            "corner ramp rejects hidden ordinary brush options before capture");
    }
    auto twoCorners = corner; auto points = corner.value("points").toArray(); points.append(points.at(0)); twoCorners["points"] = points;
    const auto tooManyCorners = InvokeTerrainEditTool("edit_terrain_stroke", twoCorners, fixture.Host());
    Expect(!tooManyCorners.value("ok").toBool()
        && tooManyCorners.value("diagnostics").toArray()[0].toObject().value("code") == "TERRAIN_CORNER_RAMP_POINT_COUNT",
        "corner ramp rejects multiple points without mutating session or invoking an owner");
    for (const char* field : {"shape", "size", "cliff_texture", "seed", "enforce_water_limits"}) {
        auto incomplete = invalid; auto water = incomplete.value("brush").toObject(); water.remove(field); incomplete["brush"] = water;
        const auto missing = InvokeTerrainEditTool("edit_terrain_stroke", incomplete, fixture.Host());
        Expect(!missing.value("ok").toBool() && missing.value("diagnostics").toArray().size() == 1
            && missing.value("diagnostics").toArray()[0].toObject().value("code") == "TERRAIN_ARGUMENT_REQUIRED",
            "water requires every explicit replay option before host dispatch");
    }
    for (const char* field : {"strength", "water_height", "anchor"}) {
        auto conflict = invalid; auto water = conflict.value("brush").toObject(); water[field] = 1; conflict["brush"] = water;
        const auto rejected = InvokeTerrainEditTool("edit_terrain_stroke", conflict, fixture.Host());
        Expect(!rejected.value("ok").toBool() && rejected.value("diagnostics").toArray().size() == 1
            && rejected.value("diagnostics").toArray()[0].toObject().value("code") == "TERRAIN_ARGUMENT_UNKNOWN",
            "water cannot accept an ordinary field or a caller-supplied anchor");
    }
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

void BoundsFacadeUsesActualResourcesCasAndSharedUndo() {
    AudastraTest::TerrainBounds::Fixture fixture;
    std::optional<TerrainPathingResources> owner;
    const auto document = [&] { return MapDataQueryBinding{"bounds-facade", 1, fixture.edits->Revision(), fixture.archive->SourceSha256()}; };
    TerrainEditToolHost host;
    host.identity = [&] { return QJsonObject{{"project_identity", "bounds-facade"}, {"revision", QString::number(fixture.edits->Revision())}}; };
    host.context = [&] { return Result<TerrainEditToolContext>::Success({fixture.edits.get(), document(), false, fixture.edits->IsDirty()}); };
    host.prepareBounds = [&](const auto& context, const auto& binding, const auto& intent) -> Result<PreparedTerrainBoundsEdit> {
        const auto source = TerrainTake(ParseMapTerrain(fixture.members[0].data));
        auto captured = TerrainPathingSave::CaptureNative(*fixture.archive, *source.Snapshot(), *fixture.edits->Terrain(), {}, 64u * 1024u * 1024u);
        if (!captured.Succeeded() || !captured.value) return {{}, captured.diagnostics};
        owner = std::move(*captured.value);
        return PrepareTerrainBoundsEdit(*context.session, context.document, binding, *owner, intent);
    };
    host.verifyBounds = [&] { return owner ? owner->VerifySourceInputs(*fixture.archive, 64u * 1024u * 1024u)
        : Status::Failure("Test", "NO_ACTUAL_OWNER", "missing native owner"); };
    host.applyBounds = [&](const auto& prepared, const auto& validate) { return ApplyTerrainBoundsEdit(*fixture.edits, document(), prepared, validate); };
    const auto state = InvokeTerrainEditTool("get_terrain_edit_state", {}, host);
    Expect(state.value("ok").toBool(), "actual bounds host can report its exact terrain binding");
    const QJsonObject request{{"project_identity", "bounds-facade"}, {"expected_revision", "0"},
        {"terrain_binding", state.value("terrain_binding")}, {"west", 1}, {"south", 1}, {"east", 9}, {"north", 7}, {"dry_run", true}};
    const auto preview = InvokeTerrainEditTool("edit_terrain_bounds", request, host);
    Expect(preview.value("ok").toBool() && !preview.value("applied").toBool()
        && preview.value("native_save_required").toBool() && !preview.value("map_saved").toBool()
        && preview.value("changes").toObject().value("map_edge_vertices").toInt() == 32
        && !fixture.edits->CanUndo() && fixture.edits->Revision() == 0, "bounds dry-run is actual and does not mutate or claim a save");
    for (const auto field : {"points", "follow_doodads", "actual_margins", "rules_sha256", "brush"}) {
        auto invalid = request; invalid[field] = 1;
        Expect(!InvokeTerrainEditTool("edit_terrain_bounds", invalid, host).value("ok").toBool(), "bounds reject irrelevant or self-attested fields");
    }
    auto invalid = request; invalid["west"] = 0.5;
    Expect(!InvokeTerrainEditTool("edit_terrain_bounds", invalid, host).value("ok").toBool(), "bounds reject fractional cells");
    auto write = request; write["dry_run"] = false;
    const auto applied = InvokeTerrainEditTool("edit_terrain_bounds", write, host);
    Expect(applied.value("ok").toBool() && applied.value("applied").toBool()
        && applied.value("edit_receipt").toObject().value("before_revision") == "0"
        && fixture.edits->Revision() == 1 && fixture.edits->HasTerrainBoundsChanges(), "same sealed bounds command publishes one shared Undo transaction");
    Expect(!InvokeTerrainEditTool("edit_terrain_bounds", write, host).value("ok").toBool(), "old bounds binding cannot replay on a new revision");
    const auto after = TerrainTake(SerializeMapTerrain(*fixture.edits->Terrain()));
    Expect(fixture.edits->Undo() && !fixture.edits->IsDirty(), "shared bounds Undo restores complete source");
    Expect(fixture.edits->Redo() && TerrainTake(SerializeMapTerrain(*fixture.edits->Terrain())) == after,
        "shared bounds Redo restores exact output bytes");
    const auto current = InvokeTerrainEditTool("get_terrain_edit_state", {}, host);
    auto second = write; second["expected_revision"] = QString::number(fixture.edits->Revision());
    second["terrain_binding"] = current.value("terrain_binding");
    second["west"] = 2; second["south"] = 2; second["east"] = 8; second["north"] = 6;
    const auto revision = fixture.edits->Revision();
    const auto appliedAgain = InvokeTerrainEditTool("edit_terrain_bounds", second, host);
    Expect(appliedAgain.value("ok").toBool() && appliedAgain.value("applied").toBool()
        && fixture.edits->Revision() == revision + 1,
        "a second real bounds command verifies original sources instead of requiring the unsaved first output");
    const auto secondTerrain = TerrainTake(SerializeMapTerrain(*fixture.edits->Terrain()));
    Expect(fixture.edits->Undo() && TerrainTake(SerializeMapTerrain(*fixture.edits->Terrain())) == after,
        "second bounds Undo restores the first owned draft");
    Expect(fixture.edits->Redo() && TerrainTake(SerializeMapTerrain(*fixture.edits->Terrain())) == secondTerrain,
        "second bounds Redo restores the complete second mask");
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

void TerrainAndFieldOverlaysCannotOverwriteEachOther() {
    Fixture fixture(true);
    const auto directory = DirectoryWorkspace::Open(fixture.source);
    Expect(directory.Succeeded(), "cross-domain fixture opens");
    auto raise = Raise(); raise.followDoodads = true;
    Expect(fixture.edits.EditTerrain(raise.brush, raise.points, true).Succeeded(), "height candidate follows both placements");
    auto fields = *fixture.document.Units(); fields.units[0].hitPoints = 50;
    MapDataEditSession fieldEdits(fixture.document);
    Expect(fieldEdits.EditUnits(fields).Succeeded(), "independent field candidate changes unit health");
    const std::array kinds{MapDataKind::Units};
    for (const bool terrainFirst : {false, true}) {
        auto editor = EditorSession::Open({fixture.source, CompatibilityProfile::Default(), "synthetic-catalog"});
        Expect(editor.Succeeded(), "cross-domain editor opens");
        const auto transaction = editor.value->BeginEdit(editor.value->Revision());
        Expect(transaction.Succeeded(), "cross-domain transaction opens");
        auto terrain = TerrainPatchCodec::Seal(fixture.document, fixture.terrain, fixture.pathing,
            directory.value->Snapshot(), fixture.edits, std::span(&raise, 1));
        auto field = MapDataFieldPatchCodec::Seal(fixture.document, fieldEdits.Snapshot(), directory.value->Snapshot(), kinds);
        Expect(terrain.Succeeded() && field.Succeeded(), "both individual domains have valid sealed source bindings");
        const auto applyTerrain = [&] { return editor.value->ApplyTerrainPatch(*transaction.value, std::move(*terrain.value)); };
        const auto applyField = [&] { return editor.value->ApplyMapDataPatch(*transaction.value, std::move(*field.value)); };
        Expect((terrainFirst ? applyTerrain() : applyField()).Succeeded(), "first domain applies");
        const auto before = editor.value->PreviewEdit(*transaction.value);
        const auto rejected = terrainFirst ? applyField() : applyTerrain();
        Expect(!rejected.Succeeded() && std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
            [](const auto& item) { return item.code == "TERRAIN_PATCH_OVERLAY_CONFLICT"; }),
            "second domain cannot silently overwrite an existing placement edit");
        const auto after = editor.value->PreviewEdit(*transaction.value);
        Expect(before.Succeeded() && after.Succeeded() && before.value->changes.size() == after.value->changes.size(),
            "rejected cross-domain patch does not publish a partial overlay");
        for (std::size_t index = 0; index < before.value->changes.size(); ++index) {
            const auto& a = before.value->changes[index]; const auto& b = after.value->changes[index];
            Expect(a.path == b.path && a.kind == b.kind && a.beforeSha256 == b.beforeSha256 && a.afterSha256 == b.afterSha256,
                "every first-domain output stays exact after rejection");
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

void RegisteredWaterHostRetainsOrderedReplayAndCompleteDirectoryOutput() {
    std::vector<std::byte> shallowTerrain;
    for (const bool deep : {false, true}) {
        Fixture fixture; const auto game = fixture.InstallWaterSources(); ScopedWaterGameRoot environment(game);
        McpServer server; OpenWorkspace(fixture, server); auto* workspace = server.Workspace();
        const auto original = TerrainTake(DirectoryWorkspace::Open(fixture.source));
        const auto initial = Current(server);
        const auto dry = Call(server, "edit_terrain_stroke", WaterStroke(initial, deep));
        Expect(dry.value("ok").toBool() && !dry.value("applied").toBool()
            && !workspace->HasActiveTransaction() && Current(server) == initial,
            "registered shallow/deep preview captures actual resources without acquiring a transaction");
        const auto revision = workspace->Revision(); const auto transaction = TerrainTake(workspace->BeginEdit(revision));
        const auto before = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
        const auto request = WaterStroke(Current(server), deep, false);
        const auto applied = Call(server, "edit_terrain_stroke", request);
        Expect(applied.value("ok").toBool() && applied.value("applied").toBool() && !applied.value("map_saved").toBool(),
            "registered directory host publishes a real shallow/deep command without reporting a map save");
        const auto after = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
        Expect(after.session == before.session && after.session->Revision() == 1 && after.session->CanUndo()
            && TerrainTake(after.session->InspectTerrainPoint({0, 0})).vertex.HasWater(),
            "water publication keeps the original typed session and its undo command");
        const auto firstTerrain = TerrainTake(SerializeMapTerrain(*after.session->Terrain()));
        if (deep) Expect(firstTerrain != shallowTerrain, "deep options survive protocol parsing and differ from shallow output");
        else shallowTerrain = firstTerrain;
        Expect(!Call(server, "edit_terrain_stroke", request).value("ok").toBool(), "water request cannot replay on a newer revision");
        auto raise = Stroke(Current(server), false);
        raise["points"] = QJsonArray{QJsonObject{{"x", 512}, {"y", 512}}};
        Expect(Call(server, "edit_terrain_stroke", raise).value("ok").toBool(),
            "ordinary command replays prior water intent before sealing the cumulative candidate");
        const auto current = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
        const auto expectedTerrain = TerrainTake(SerializeMapTerrain(*current.session->Terrain()));
        Expect(TerrainTake(current.session->InspectTerrainPoint({0, 0})).vertex.HasWater()
            && TerrainTake(current.session->InspectTerrainPoint({512, 512})).groundHeight == 16,
            "ordered water and ordinary commands both survive workspace replay");
        const auto plan = TerrainTake(TerrainPathingSave::PrepareForSave(original.Snapshot(), *current.session, game));
        Expect(plan.SaveGate().Succeeded() && plan.RebuiltPathing(), "same actual source passes complete native G0/G1 proof");
        const auto pathing = std::find_if(plan.Replacements().begin(), plan.Replacements().end(),
            [](const auto& member) { return member.name == "war3map.wpm"; });
        Expect(pathing != plan.Replacements().end(), "water save has a complete regenerated WPM member");
        const auto expectedPathing = pathing->data;
        Expect(workspace->PreviewEdit(transaction, revision).Succeeded() && workspace->ValidateEdit(transaction, revision).satisfied,
            "normal directory preview and validation accept the same water recipe");
        const auto root = std::filesystem::path(fixture.temporary.path().toStdWString());
        std::vector<MapArchiveMember> archiveMembers;
        for (const auto& file : original.Snapshot().files)
            archiveMembers.push_back({IO::GenericPathUtf8(file.relativePath), TerrainTake(IO::ReadFileBytes(fixture.source / file.relativePath))});
        const auto archive = TerrainTake(WriteVerifiedArchive(root / "water-source.w3x", archiveMembers));
        MapEditSaveRequest archiveRequest; archiveRequest.sourceMap = archive.path; archiveRequest.expectedSourceSha256 = archive.sha256;
        archiveRequest.targetCandidate = root / "water-archive-candidate.w3x"; archiveRequest.gameRoot = game;
        archiveRequest.uiRoot = root / "ui"; archiveRequest.toolRoot = root / "missing-toolchain";
        archiveRequest.minimapChoice = TerrainMinimapSaveChoice::Preserve;
        const auto pending = workspace->ExportArchiveCandidate(transaction, revision, archiveRequest);
        if (!pending.Succeeded())
            for (const auto& diagnostic : pending.diagnostics)
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        Expect(!pending.Succeeded() && std::any_of(pending.diagnostics.begin(), pending.diagnostics.end(),
            [](const auto& item) { return item.code == "SCRIPT_TOOLCHAIN_MISSING"; })
            && !std::filesystem::exists(archiveRequest.targetCandidate) && workspace->HasActiveTransaction(),
            "archive export retains water through actual save preparation and stops at the unchanged missing script toolchain");
        const auto output = root / "water-directory-candidate";
        Expect(workspace->ExportCandidate(transaction, revision, output).Succeeded(), "ordinary directory exporter publishes water recipe");
        Expect(TerrainTake(IO::ReadFileBytes(output / "war3map.w3e")) == expectedTerrain
            && TerrainTake(IO::ReadFileBytes(output / "war3map.wpm")) == expectedPathing,
            "directory exporter preserves every W3E byte and complete native WPM output");
        const auto nextRevision = workspace->Revision(); const auto next = TerrainTake(workspace->BeginEdit(nextRevision));
        const auto seeded = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
        Expect(seeded.session != current.session && TerrainTake(SerializeMapTerrain(*seeded.session->Terrain())) == expectedTerrain,
            "continuation seeds the complete ordered water recipe into a fresh typed session");
        auto extra = WaterStroke(Current(server), !deep, false);
        extra["points"] = QJsonArray{QJsonObject{{"x", -512}, {"y", 0}}};
        Expect(Call(server, "edit_terrain_stroke", extra).value("ok").toBool()
            && TerrainTake(SerializeMapTerrain(*seeded.session->Terrain())) != expectedTerrain,
            "continuation can add another explicitly configured water command");
        Expect(workspace->Rollback(next, nextRevision).Succeeded(), "water continuation rolls back through the existing transaction");
        const auto retained = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
        Expect(TerrainTake(SerializeMapTerrain(*retained.session->Terrain())) == expectedTerrain,
            "rollback preserves the previously exported cumulative water state");
        Expect(TerrainTake(IO::ReadFileBytes(fixture.source / "war3map.w3e")) == fixture.terrain
            && TerrainTake(IO::ReadFileBytes(fixture.source / "war3map.wpm")) == fixture.pathing,
            "preview, archive rejection, directory publication and continuation never modify original terrain members");
    }
}

void WaterResourceDriftAndPrepublishReentryLeaveTheOriginalTransactionIntact() {
    Fixture fixture; const auto game = fixture.InstallWaterSources(); ScopedWaterGameRoot environment(game);
    McpServer server; OpenWorkspace(fixture, server); auto* workspace = server.Workspace();
    const auto revision = workspace->Revision(); const auto transaction = TerrainTake(workspace->BeginEdit(revision));
    const auto context = TerrainTake(workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration()));
    const auto state = TerrainTake(CaptureTerrainEditState(*context.session, context.document));
    const auto resources = TerrainTake(workspace->CaptureTerrainWaterResources(true));
    const TerrainStrokePoint point{0, 0}; TerrainWaterStrokeOptions options; options.randomSeed = 991;
    const auto prepared = TerrainTake(PrepareTerrainWaterEdit(*context.session, context.document,
        state.binding, resources, options, std::span(&point, 1), false));
    bool callbackReached = false;
    const auto rejected = workspace->ApplyTerrainPlan(prepared, [&](const auto&) {
        callbackReached = true;
        Expect(!workspace->Rollback(transaction, revision).Succeeded(), "water prepublish reentry cannot discard its active transaction");
        const std::string profile = "[Unused]\nValue=1\n";
        TerrainTake(WriteVerifiedArchive(game / "war3patch.mpq", std::vector<MapArchiveMember>{{"Units/MiscData.txt",
            {reinterpret_cast<const std::byte*>(profile.data()), reinterpret_cast<const std::byte*>(profile.data() + profile.size())}}}));
        return Status::Ok();
    });
    Expect(callbackReached && !rejected.Succeeded() && context.session->Revision() == 0 && !context.session->CanUndo()
        && TerrainTake(workspace->Session()->PreviewEdit(transaction)).changes.empty(),
        "changed optional game profile fails final water resource verification before either session publishes");
    Expect(std::filesystem::remove(game / "war3patch.mpq"), "remove only the owned injected profile archive");
    Expect(workspace->ApplyTerrainPlan(prepared).Succeeded(), "resource rejection leaves the original prepared command reusable");
    Expect(workspace->Rollback(transaction, revision).Succeeded(), "accepted water command retains normal rollback");
    auto changed = fixture.terrain; changed.back() ^= std::byte{1}; Fixture::Write(fixture.source / "war3map.w3e", changed);
    Expect(!Call(server, "get_terrain_edit_state").value("ok").toBool(), "water source drift invalidates actual registered host state");
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

#include "McpStraightRampEditCases.h"
#include "McpTerrainBlockTransportCases.h"
#include "McpTerrainWorkspaceBlockCases.h"
#include "TerrainResizeToolCases.h"
#include "TerrainTextureSlotEditCases.h"

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        FacadeQueryPreviewApplyAndUndo();
        FacadePointQueryPreservesApproximateHeightWarning();
        FacadeRemoveWaterUsesTheOrdinaryTransaction();
        FacadeRejectsUnknownTypesBoundsAndStaleSources();
        StraightRampFacadeRequiresItsOwnCapturedOwner();
        TerrainBlockFacadeCapturesItsOwnSourceDigest();
        RegisteredBlockHostPublishesCompleteTypedDirectoryAndColdReadback();
        BlockDirectorySealRejectsUntypedOrConflictingEmptyCommands();
        RegisteredConnectedHostKeepsItsAlgorithmThroughDirectoryReplay();
        BoundsFacadeUsesActualResourcesCasAndSharedUndo();
        ResizeToolRequiresBoundSourcesAndStrictIntent();
        TextureSlotFacadeUsesRealOwnersAndAtomicPublication();
        RegisteredTextureSlotHostPreviewsAndRoutesSavesExplicitly();
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
        TerrainAndFieldOverlaysCannotOverwriteEachOther();
        FollowingHeightRestorationRemovesObsoletePlacementOverlay();
        RegisteredWaterHostRetainsOrderedReplayAndCompleteDirectoryOutput();
        WaterResourceDriftAndPrepublishReentryLeaveTheOriginalTransactionIntact();
        std::cout << "terrain MCP and sealed transaction scenarios passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
