#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kResourceBudget = 8u * 1024u * 1024u;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message;
        for (const auto& diagnostic : result.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
        throw std::runtime_error(message.empty() ? "missing result" : message);
    }
    return std::move(*result.value);
}
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
Bytes Text(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())};
}
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream << text;
    Require(stream.good(), "write private input");
}

struct Fixture {
    std::filesystem::path root, game;
    TerrainGridSnapshot grid;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> source;
    std::unique_ptr<MapDataEditSession> edits;
    TerrainStrokePoint point{2048, 2048};

    explicit Fixture(bool variedProtection = false) {
        static std::atomic<unsigned> next{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-water-service-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(root), "private fixture directory");
        game = root / "game"; Require(std::filesystem::create_directory(game), "private game directory");
        const std::vector<MapArchiveMember> gameMembers{{"fixture.txt", Text("private fixture")}};
        Take(WriteVerifiedArchive(game / "war3.mpq", gameMembers));
        grid.header.width = grid.header.height = 33; grid.header.mainTileset = 'L';
        grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01", "Cl02"};
        grid.vertices.resize(33 * 33);
        for (auto& vertex : grid.vertices) {
            vertex.groundHeightRaw = 8320; vertex.waterHeightAndFlagsRaw = 7936;
            vertex.variationRaw = 3; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(grid, 0, 0, 33, 33);
        Bytes pathing{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        for (const auto value : {0u, 128u, 128u}) for (unsigned shift = 0; shift < 32; shift += 8)
            pathing.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        // 原创平干地的独立六mode基线40，加作者保护11；不调用待测生成器制造G0。
        pathing.resize(16 + 128 * 128, std::byte{0x51});
        if (variedProtection) {
            constexpr std::array protection{std::byte{0x00}, std::byte{0x01}, std::byte{0x10}, std::byte{0x11}};
            for (std::size_t index = 16; index < pathing.size(); ++index)
                pathing[index] = std::byte{0x40} | protection[(index - 16) % protection.size()];
        }
        members = {{"war3map.w3e", Take(SerializeMapTerrain(grid))}, {"war3map.wpm", std::move(pathing)},
            {"TerrainArt/Terrain.slk", Text("ID;PWXL;N;E\nB;X4;Y2;D0\n"
                "C;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
                "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\nE\n")},
            {"TerrainArt/Water.slk", Text("ID;PWXL;N;E\nB;X3;Y2;D0\n"
                "C;X1;Y1;K\"waterID\"\nC;X2;K\"height\"\nC;X3;K\"impassable\"\n"
                "C;X1;Y2;K\"LSha\"\nC;X2;K0\nC;X3;K0\nE\n")},
            {"TerrainArt/CliffTypes.slk", Text("ID;PWXL;N;E\nB;X1;Y3;D0\n"
                "C;X1;Y1;K\"cliffID\"\nC;X1;Y2;K\"Cl01\"\nC;X1;Y3;K\"Cl02\"\nE\n")},
            {"UI/MiscData.txt", Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=128\n"
                "[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")},
            {"UI/WorldEditData.txt", Text("[Other]\nValue=1\n")},
            {"war3map.j", Text("// original script\n")}};
        MapSettings settings; settings.formatVersion = 25; settings.custom.emplace();
        settings.tileset = 'L'; settings.playableWidth = settings.playableHeight = 32; settings.flags |= 0x400;
        members.push_back({"war3map.w3i", Take(settings.Serialize())});
        members.push_back({"war3map.doo", Take(DoodadPlacements{}.Serialize())});
        members.push_back({"war3mapUnits.doo", Take(UnitPlacements{}.Serialize())});
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
        const auto catalog = Take(Trigger::UiCatalog::Load(root / "ui"));
        const auto triggers = Take(Trigger::WtgWctCodec{}.CreateEmpty(catalog));
        const auto pair = Take(Trigger::WtgWctCodec{}.Serialize(triggers, catalog));
        members.push_back({"war3map.wtg", pair.wtg}); members.push_back({"war3map.wct", pair.wct});
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / "source.w3x", members)).path)));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Require(edits->AttachTerrain(members[0].data).Succeeded() && edits->AttachPathing(members[1].data).Succeeded(), "attach source");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    TerrainPathingResources Rules(bool enforce = true) const {
        const auto current = Take(edits->Snapshot(MapDataKind::Doodads, kResourceBudget));
        Require(current.Doodads().has_value(), "service rule capture reads a bounded current DOO snapshot");
        return Take(TerrainPathingSave::CaptureWaterBrushRules(*source, grid, *edits->Terrain(),
            *current.Doodads(), game, enforce, kResourceBudget));
    }
    MapDataQueryBinding Document() const { return {"water-service", 1, 0, source->SourceSha256()}; }
    TerrainEditBinding Binding() const { return {Document(), edits->SessionIdentity(), edits->Revision(), edits->TerrainSourceRevision()}; }
    MapEditSaveRequest Request() const {
        MapEditSaveRequest request;
        request.sourceMap = source->SourcePath(); request.targetCandidate = root / "candidate.w3x";
        request.expectedSourceSha256 = source->SourceSha256(); request.expectedTerrainSha256 = Take(Sha256(members[0].data));
        request.gameRoot = game; request.toolRoot = root / "missing-toolchain"; request.uiRoot = root / "ui";
        request.minimapChoice = TerrainMinimapSaveChoice::Preserve;
        MapEditTerrainStroke stroke; stroke.pointSpace = MapEditPointSpace::TerrainVertex; stroke.points = {{16, 16}};
        stroke.waterBrush = TerrainWaterStrokeOptions{}; request.terrainStrokes = {stroke};
        return request;
    }
};

void OneAnchorAndWholePointSequenceShareCoreCommand() {
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        Fixture f; const auto rules = f.Rules(); const auto binding = f.Binding();
        const auto before = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const std::array points{f.point, TerrainStrokePoint{2176, 2048}, f.point};
        TerrainWaterStrokeOptions options; options.kind = kind; options.shape = TerrainBrushShape::Square;
        options.cliffTexture = 1; options.randomSeed = (std::numeric_limits<std::uint32_t>::max)();
        const auto expectedAnchor = Take(CaptureTerrainWaterStrokeAnchor(*f.edits->Terrain(), points.front()));
        const TerrainWaterBrush brush{kind, options.shape, options.size, options.cliffTexture, options.randomSeed, expectedAnchor};
        const auto expected = Take(PrepareTerrainWaterStroke(*f.edits->Terrain(), *rules.WaterBrushRules(), brush, points, {}));
        const auto plan = Take(PrepareTerrainWaterEdit(*f.edits, f.Document(), binding, rules, options, points));
        const auto* captured = plan.Plan().WaterBrush();
        Require(captured && captured->randomSeed == options.randomSeed && captured->kind == kind
            && captured->anchor.groundHeightRaw == expectedAnchor.groundHeightRaw
            && captured->anchor.waterHeightRaw == expectedAnchor.waterHeightRaw
            && captured->anchor.cliffLevel == expectedAnchor.cliffLevel && captured->anchor.hasWater == expectedAnchor.hasWater
            && plan.Plan().Points().size() == points.size(), "service captures one anchor and preserves seed and every sample");
        Require(f.edits->Revision() == 0 && !f.edits->CanUndo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == before,
            "water preparation is pure");
        Require(HasCode(plan.Plan().TerrainPathingGate(), "TERRAIN_SAVE_GATE_NOT_INSPECTED"), "draft has no native save qualification");
        const auto rejected = ApplyTerrainEdit(*f.edits, f.Document(), plan, [](const auto&) {
            return Status::Failure("test", "REJECT", "private callback rejection");
        });
        Require(!rejected.Succeeded() && f.edits->Revision() == 0 && !f.edits->CanUndo(), "callback rejection publishes no state");
        Take(ApplyTerrainEdit(*f.edits, f.Document(), plan));
        Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == Take(SerializeMapTerrain(expected.terrain))
            && f.edits->Revision() == 1 && f.edits->CanUndo(), "service commits the complete pure algorithm result once");
        Require(!ApplyTerrainEdit(*f.edits, f.Document(), plan).Succeeded(), "stale water candidate cannot be applied twice");
        Require(f.edits->Undo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == before, "one Undo restores the whole stroke");
    }
}

void OrderedSourceAndOptionsCannotBeSubstituted() {
    Fixture f; const auto staleRules = f.Rules();
    TerrainBrush raise; raise.kind = TerrainBrushKind::Raise; raise.radius = 0; raise.strength = 16;
    Take(f.edits->EditTerrain(raise, std::span(&f.point, 1), false));
    const auto before = Take(SerializeMapTerrain(*f.edits->Terrain()));
    TerrainWaterStrokeOptions options;
    Require(HasCode(PrepareTerrainWaterEdit(*f.edits, f.Document(), f.Binding(), staleRules, options, std::span(&f.point, 1)),
        "TERRAIN_WATER_RULE_INPUT_CHANGED"), "rules bound before an earlier stroke cannot prepare the later current draft");
    const auto rules = f.Rules();
    options.enforceWaterLimits = false;
    Require(HasCode(PrepareTerrainWaterEdit(*f.edits, f.Document(), f.Binding(), rules, options, std::span(&f.point, 1)),
        "TERRAIN_WATER_RULE_OPTIONS"), "a different enforce option cannot reuse the captured rules");
    options.enforceWaterLimits = true;
    const std::array points{f.point, TerrainStrokePoint{5000, 0}};
    Require(!PrepareTerrainWaterEdit(*f.edits, f.Document(), f.Binding(), rules, options, points).Succeeded(), "a later outside point rejects the whole stroke");
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == before && f.edits->Revision() == 1, "failed water preparation preserves the earlier command");
    const auto plan = Take(PrepareTerrainWaterEdit(*f.edits, f.Document(), f.Binding(), rules, options, std::span(&f.point, 1)));
    Require(plan.Plan().WaterBrush()->anchor.groundHeightRaw == Take(CaptureTerrainWaterStrokeAnchor(*f.edits->Terrain(), f.point)).groundHeightRaw,
        "new stroke anchor comes from the current ordered terrain");
    Write(f.game / "UI/MiscData.txt", "[Terrain]\nMaxSlope=1\n");
    const auto rejected = ApplyTerrainEdit(*f.edits, f.Document(), plan, [&](const auto&) { return rules.VerifyReadback(*f.source, kResourceBudget); });
    Require(HasCode(rejected, "TERRAIN_WATER_RULE_SOURCE") && f.edits->Revision() == 1
        && Take(SerializeMapTerrain(*f.edits->Terrain())) == before, "actual new loose winner fails the shared prepublish callback without publishing");
}

void WaterServiceCandidatesPassNativePlanAndIndependentArchiveReadback() {
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        Fixture f; const auto rules = f.Rules(); TerrainWaterStrokeOptions options;
        options.kind = kind; options.shape = TerrainBrushShape::Square; options.cliffTexture = 1; options.randomSeed = 17;
        const std::array points{f.point, TerrainStrokePoint{2176, 2048}};
        const auto prepared = Take(PrepareTerrainWaterEdit(*f.edits, f.Document(), f.Binding(), rules, options, points));
        Take(ApplyTerrainEdit(*f.edits, f.Document(), prepared));
        const auto plan = Take(TerrainSavePlan::PrepareForSave(*f.source, *f.edits, f.game));
        Require(plan.SaveGate().Succeeded() && plan.RebuiltPathing(), "shallow/deep service candidate passes actual native G0/G1 save plan");
        auto replacements = f.edits->Replacements();
        Require(plan.InstallPathingOutput(*f.edits, replacements).Succeeded()
            && plan.VerifyForSave(*f.edits, f.source->SourceSha256(), replacements).Succeeded(), "native output is bound to the same Core command");
        Require(rules.VerifyReadback(*f.source, kResourceBudget).Succeeded(), "water editing rules are independently rechecked before archive publication");
        const auto written = Take(f.source->SaveCandidate(f.root / "native-water.w3x", replacements));
        const auto saved = Take(MapArchiveSession::Open(written.path));
        Require(plan.VerifyReadback(saved).Succeeded(), "independently reopened candidate has the proven W3E/WPM identities");
        Require(Take(saved.ReadMember("war3map.w3e")) == Take(SerializeMapTerrain(*f.edits->Terrain())), "saved W3E equals the complete service result");
        const auto wpm = Take(saved.ReadMember("war3map.wpm"));
        Require(wpm != f.members[1].data, "water save changes actual native pathing bits");
        for (std::size_t index = 16; index < wpm.size(); ++index)
            Require((wpm[index] & std::byte{0x11}) == (f.members[1].data[index] & std::byte{0x11}), "water output preserves author protection bits");
        Require(Take(Sha256File(f.source->SourcePath())) == f.source->SourceSha256(), "native water candidate preserves the source archive");
    }
}

void SavedWaterStrokeUndoAndRedoRebaseAgainstEachSavedSource() {
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        Fixture f(true);
        const auto originalTerrain = f.members[0].data, originalPathing = f.members[1].data;
        const auto sessionIdentity = f.edits->SessionIdentity();
        const auto rules = f.Rules();
        TerrainWaterStrokeOptions options;
        options.kind = kind; options.shape = TerrainBrushShape::Square; options.size = 2;
        options.cliffTexture = 1; options.randomSeed = 17;
        const std::array points{f.point, TerrainStrokePoint{2176, 2048}};
        const auto prepared = Take(PrepareTerrainWaterEdit(*f.edits, f.Document(), f.Binding(), rules, options, points));
        Take(ApplyTerrainEdit(*f.edits, f.Document(), prepared));
        const auto wetTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
        Require(wetTerrain != originalTerrain && std::any_of(f.edits->Terrain()->vertices.begin(),
            f.edits->Terrain()->vertices.end(), [](const auto& vertex) { return vertex.HasWater(); }),
            "the saved history regression creates new water on originally dry ground");
        Require(std::any_of(f.edits->Terrain()->vertices.begin(), f.edits->Terrain()->vertices.end(),
            [](const auto& vertex) { return vertex.CliffTextureIndex() == 1 && vertex.CliffVariation() != 0; }),
            "the saved water command also changes cliff palette and variation");
        Require(f.edits->CanUndo() && !f.edits->CanRedo(), "one water command precedes the first save");
        Require(rules.VerifyReadback(*f.source, kResourceBudget).Succeeded(), "the original water rule owner is still current");

        std::vector<std::pair<std::filesystem::path, std::string>> sources{
            {f.source->SourcePath(), f.source->SourceSha256()}};
        const auto saveAndCommit = [&](const char* name, const Bytes& expectedTerrain, const Bytes* expectedPathing) {
            // 每次从上次实际保存并回读的归档重新捕获Native规则，不能复用第一份保存计划。
            const auto plan = Take(TerrainSavePlan::PrepareForSave(*f.source, *f.edits, f.game));
            Require(plan.SaveGate().Succeeded() && plan.RebuiltPathing()
                && plan.SourceSha256() == f.source->SourceSha256(), "each history state prepares native pathing from its current saved source");
            auto replacements = f.edits->Replacements();
            Require(plan.InstallPathingOutput(*f.edits, replacements).Succeeded()
                && plan.VerifyForSave(*f.edits, f.source->SourceSha256(), replacements).Succeeded(),
                "the history state installs and verifies its complete native output");
            const auto written = Take(f.source->SaveCandidate(f.root / name, replacements));
            auto saved = Take(MapArchiveSession::Open(written.path));
            Require(plan.VerifyReadback(saved).Succeeded(), "independent archive readback verifies this history state");
            const auto actualTerrain = Take(saved.ReadMember("war3map.w3e"));
            auto actualPathing = Take(saved.ReadMember("war3map.wpm"));
            Require(actualTerrain == expectedTerrain, "saved history restores the entire expected W3E byte stream");
            Require(actualPathing.size() == originalPathing.size()
                && std::equal(actualPathing.begin(), actualPathing.begin() + 16, originalPathing.begin()),
                "saved history preserves the complete WPM header and dimensions");
            if (expectedPathing)
                Require(actualPathing == *expectedPathing, "saved history restores the entire expected WPM byte stream");
            for (std::size_t index = 16; index < actualPathing.size(); ++index)
                Require((actualPathing[index] & std::byte{0x11}) == (originalPathing[index] & std::byte{0x11}),
                    "every save preserves each original author protection bit, including absent bits");
            for (const auto& [path, sha] : sources)
                Require(Take(Sha256File(path)) == sha, "saving a later history state never modifies an earlier source archive");
            const auto revision = f.edits->Revision(), sourceRevision = f.edits->TerrainSourceRevision();
            Require(plan.CommitSaved(*f.edits, MapDocument::FromArchive(saved)).Succeeded(),
                "the real saved document becomes the next session baseline");
            Require(!f.edits->IsDirty() && f.edits->Replacements().empty()
                && f.edits->SessionIdentity() == sessionIdentity && f.edits->Revision() == revision
                && f.edits->TerrainSourceRevision() == sourceRevision + 1,
                "saving rebases the same session without adding an edit or discarding its history");
            sources.emplace_back(saved.SourcePath(), saved.SourceSha256());
            f.source = std::make_unique<MapArchiveSession>(std::move(saved));
            return actualPathing;
        };

        const auto wetPathing = saveAndCommit("water-first-save.w3x", wetTerrain, nullptr);
        Require(wetPathing != originalPathing, "new water changes the native WPM contribution");
        Require(f.edits->Undo() && !f.edits->CanUndo() && f.edits->CanRedo() && f.edits->IsDirty()
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == originalTerrain,
            "one Undo after saving restores all original W3E bytes, including water and cliff variation");
        (void)saveAndCommit("water-undo-save.w3x", originalTerrain, &originalPathing);
        Require(f.edits->Redo() && f.edits->CanUndo() && !f.edits->CanRedo() && f.edits->IsDirty()
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == wetTerrain,
            "one Redo after saving the Undo restores the exact water command without rerunning its random sequence");
        (void)saveAndCommit("water-redo-save.w3x", wetTerrain, &wetPathing);
    }
}

void DirectSaveRejectsEveryConflictingOrdinaryField() {
    Fixture f;
    const std::vector<std::function<void(TerrainBrush&)>> conflicts{
        [](auto& b) { b.kind = TerrainBrushKind::WaterHeight; }, [](auto& b) { b.shape = TerrainBrushShape::Square; },
        [](auto& b) { b.radius = 0; }, [](auto& b) { b.groundTexture = "Tg01"; }, [](auto& b) { b.cliffTexture = 1; },
        [](auto& b) { b.levelDelta = 1; }, [](auto& b) { b.value = false; }, [](auto& b) { b.strength = 1; },
        [](auto& b) { b.seed = 1; }, [](auto& b) { b.anchor = TerrainStrokeAnchor{}; }, [](auto& b) { b.size = 1; },
        [](auto& b) { b.variation = 0; }, [](auto& b) { b.randomizeVariation = true; }, [](auto& b) { b.waterHeight = 1; },
        [](auto& b) { b.pathingRestrictions = 0x04; }};
    for (const auto& conflict : conflicts) {
        auto request = f.Request(); conflict(request.terrainStrokes[0].brush);
        Require(HasCode(SaveMapEditCandidate(request), "SAVE_STROKE_BRUSH_CONFLICT"), "water cannot silently ignore an ordinary brush field");
    }
    auto request = f.Request(); request.terrainStrokes[0].waterBrush->size = 9;
    Require(HasCode(SaveMapEditCandidate(request), "SAVE_STROKE_WATER_BOUNDS"), "water classic size is bounded before source reads");
    request = f.Request(); request.terrainStrokes[0].waterBrush->cliffTexture = 15;
    Require(HasCode(SaveMapEditCandidate(request), "SAVE_STROKE_WATER_BOUNDS"), "water cliff sentinel is not a resource slot");
    request = f.Request(); request.terrainStrokes[0].points.push_back({33, 16});
    Require(HasCode(SaveMapEditCandidate(request), "SAVE_VERTEX_BOUNDS"), "direct water save rejects a later outside vertex");
    Require(Take(Sha256File(f.source->SourcePath())) == f.source->SourceSha256() && !std::filesystem::exists(request.targetCandidate),
        "all rejected direct requests preserve source and publish no candidate");
}

void DirectSaveUsesOrderedWaterAndOriginalScriptGate() {
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        Fixture f; auto request = f.Request();
        request.terrainStrokes[0].waterBrush->kind = kind;
        request.terrainStrokes[0].waterBrush->randomSeed = (std::numeric_limits<std::uint32_t>::max)();
        auto raise = request.terrainStrokes[0]; raise.waterBrush.reset();
        raise.brush.kind = TerrainBrushKind::Raise; raise.brush.radius = 0; raise.brush.strength = 16;
        request.terrainStrokes.insert(request.terrainStrokes.begin(), raise);
        const auto prepared = SaveMapEditCandidate(request);
        Require(HasCode(prepared, "SCRIPT_TOOLCHAIN_MISSING"), "ordinary plus water strokes reach the unchanged supervised script gate");
        Require(Take(Sha256File(f.source->SourcePath())) == f.source->SourceSha256() && !std::filesystem::exists(request.targetCandidate),
            "missing compiler cannot publish the internally prepared water candidate");
        request = f.Request(); request.terrainStrokes[0].waterBrush.reset();
        request.terrainStrokes[0].brush.kind = TerrainBrushKind::WaterHeight;
        const auto heightOnly = SaveMapEditCandidate(request);
        Require(HasCode(heightOnly, "SAVE_NO_TERRAIN_CHANGE"), "water-height on dry ground remains distinct from creating shallow or deep water");
    }
}
} // namespace

int main() {
    try {
        OneAnchorAndWholePointSequenceShareCoreCommand();
        OrderedSourceAndOptionsCannotBeSubstituted();
        WaterServiceCandidatesPassNativePlanAndIndependentArchiveReadback();
        SavedWaterStrokeUndoAndRedoRebaseAgainstEachSavedSource();
        DirectSaveRejectsEveryConflictingOrdinaryField();
        DirectSaveUsesOrderedWaterAndOriginalScriptGate();
        std::cout << "TerrainWaterEditServiceTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
