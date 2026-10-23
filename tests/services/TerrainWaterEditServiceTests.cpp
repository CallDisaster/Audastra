#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
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
Bytes Slk(const std::vector<std::string>& columns, std::initializer_list<std::vector<std::string>> rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto append = [&](const auto& values, std::size_t y) {
        for (std::size_t x = 0; x < values.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + values[x] + "\"\n";
    };
    append(columns, 1); std::size_t y = 2;
    for (const auto& row : rows) append(row, y++);
    return Text(text + "E\n");
}

struct Fixture {
    std::filesystem::path root, game;
    TerrainGridSnapshot grid;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> source;
    std::unique_ptr<MapDataEditSession> edits;
    TerrainStrokePoint point{2048, 2048};

    explicit Fixture(bool variedProtection = false, bool withSpecials = false) {
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
        if (withSpecials) {
            DoodadPlacements doodads;
            doodads.specialDoodads = {{"Doo0", 0, {15, 15}}, {"Doo0", 0, {24, 24}},
                {"Doo0", 0, {15, 15}}, {"Doo0", 0, {28, 28}}};
            for (auto& member : members) {
                if (member.name == "war3map.doo") member.data = Take(doodads.Serialize());
                if (member.name == "war3map.wpm") for (const auto& special : doodads.specialDoodads)
                    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                        const auto cell = 16 + std::size_t(special.position[1] * 4 + y) * 128 + special.position[0] * 4 + x;
                        // 原创black RGB/alpha0 special独立预期：去掉地面0x40，保留作者0x11。
                        member.data[cell] &= std::byte{0x11};
                    }
            }
            Bytes texture(18 + 4 * 4 * 4);
            texture[2] = std::byte{2}; texture[12] = texture[14] = std::byte{4}; texture[16] = std::byte{32};
            texture[17] = std::byte{8};
            members.push_back({"PathTextures/replay-special.tga", std::move(texture)});
            members.push_back({"Units/DestructableData.slk", Slk({"destructableID", "code", "walkable"}, {{"Dst0", "Dst0", "0"}})});
            members.push_back({"Doodads/Doodads.slk", Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
                {{"Doo0", "Doo0", "PathTextures/replay-special.tga", "C", "-1"}})});
            members.push_back({"Units/DestructableMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
                {{"dwlk", "walkable", "DestructableData", "bool", "0", "0", "-1"}})});
        }
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

std::vector<TerrainStrokeIntent> OrderedIntents(const Fixture& f) {
    TerrainStrokeIntent raise;
    raise.brush.kind = TerrainBrushKind::Raise; raise.brush.radius = 0; raise.brush.strength = 16;
    raise.points = {{256, 256}};
    TerrainStrokeIntent shallow; shallow.waterBrush = TerrainWaterStrokeOptions{};
    shallow.waterBrush->randomSeed = 107; shallow.points = {f.point, f.point};
    TerrainStrokeIntent deep; deep.waterBrush = TerrainWaterStrokeOptions{};
    deep.waterBrush->kind = TerrainWaterBrushKind::Deep; deep.waterBrush->randomSeed = 991;
    deep.waterBrush->enforceWaterLimits = false; deep.points = {{2048, 2176}};
    return {raise, shallow, deep};
}

DirectoryWorkspace Directory(const Fixture& f) {
    const auto root = f.root / "directory";
    for (const auto& member : f.members) {
        const auto path = root / member.name; std::filesystem::create_directories(path.parent_path());
        Require(IO::WriteFileBytes(path, member.data).Succeeded(), "write independent directory member");
    }
    return Take(DirectoryWorkspace::Open(root));
}

std::unique_ptr<MapDataEditSession> Session(const Fixture& f) {
    auto session = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(f.members));
    Require(session->AttachTerrain(f.members[0].data).Succeeded() && session->AttachPathing(f.members[1].data).Succeeded(), "attach independent replay");
    return session;
}

template<class Source> Result<TerrainReplayCandidate> ReplayFixture(const Fixture& f, const Source& source,
    std::span<const TerrainStrokeIntent> strokes, const TerrainReplayLimits& limits = {}) {
    return TerrainPathingSave::ReplayStrokes(source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, strokes, f.game, limits);
}

void SharedReplayPreservesOrderSpecialDeletionAndCompleteDirectoryOutput() {
    Fixture f(true, true); const auto directory = Directory(f); const auto intents = OrderedIntents(f);
    auto archiveResult = Take(ReplayFixture(f, *f.source, intents));
    auto directoryResult = Take(ReplayFixture(f, directory.Snapshot(), intents));
    const auto* archive = archiveResult.session.get(); const auto* workspace = directoryResult.session.get();
    Require(archiveResult.waterOwners.size() == 2 && directoryResult.waterOwners.size() == 2, "each water stroke captures a separate actual resource owner");
    Require(Take(SerializeMapTerrain(*archive->Terrain())) == Take(SerializeMapTerrain(*workspace->Terrain()))
        && archive->SerializePathingRestrictionDraft().value == workspace->SerializePathingRestrictionDraft().value,
        "ordered archive and directory replay match full W3E and restriction provenance");
    const auto specials = Take(workspace->Snapshot(MapDataKind::Doodads, kResourceBudget)).Doodads()->specialDoodads;
    Require(specials.size() == 2 && specials[0].position == (std::array<std::int32_t, 2>{28, 28})
        && specials[1].position == (std::array<std::int32_t, 2>{24, 24}), "duplicate XY removals retain native current-first swap-last ordering");
    const auto sealed = Take(TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, directory.Snapshot(), *workspace, intents, f.game));
    const auto archivePlan = Take(TerrainPathingSave::PrepareForSave(*f.source, *archive, f.game));
    auto replacements = archive->Replacements();
    Require(archivePlan.InstallPathingOutput(*archive, replacements).Succeeded(), "install independent archive native pathing");
    for (const auto& name : {"war3map.w3e", "war3map.wpm", "war3map.doo", "war3mapUnits.doo"}) {
        const auto source = Take(f.source->ReadMember(name));
        const auto found = std::find_if(replacements.begin(), replacements.end(), [&](const auto& member) { return member.name == name; });
        const auto& expected = found == replacements.end() ? source : found->data;
        const auto actual = std::find_if(sealed.Files().begin(), sealed.Files().end(), [&](const auto& file) { return file.relativePath == name; });
        Require((actual == sealed.Files().end() ? source : actual->replacement) == expected,
            "complete W3E/DOO/Units/WPM bytes agree between archive and sealed directory");
    }
    Require(workspace->Undo() && workspace->Redo(), "directory replay owns real undo and redo commands");
    Require(TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.members[0].data, f.members[1].data,
        directory.Snapshot(), *workspace, intents, f.game).Succeeded(), "redo restores the original water command and private provenance");
    Require(!TerrainPatchCodec::Seal(MapDocument::FromMembers(f.members), f.members[0].data, f.members[1].data,
        directory.Snapshot(), *workspace, intents, f.game).Succeeded(), "public Core entry cannot authorize water using final bytes");
    auto untrusted = Session(f);
    Require(untrusted->SetTerrainAndPathing(Take(SerializeMapTerrain(*workspace->Terrain())), f.members[1].data).Succeeded(),
        "untrusted draft can carry the matching final W3E and static WPM");
    const auto finalDoodads = Take(Take(workspace->Snapshot(MapDataKind::Doodads, kResourceBudget)).Doodads()->Serialize());
    Require(untrusted->SetSerialized(MapDataKind::Doodads, finalDoodads).Succeeded(),
        "final DOO remains an editable draft");
    Require(!TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.members[0].data, f.members[1].data,
        directory.Snapshot(), *untrusted, intents, f.game).Succeeded(), "matching final DOO cannot substitute for replayed private water command");
}

void ReplayBudgetsAndTamperedIntentDoNotPublish() {
    Fixture f; const auto directory = Directory(f); const auto intents = OrderedIntents(f);
    const auto replayed = Take(ReplayFixture(f, directory.Snapshot(), intents));
    const auto* session = replayed.session.get();
    auto changed = intents; changed[1].points = {{256, 512}};
    Require(!TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.members[0].data, f.members[1].data,
        directory.Snapshot(), *session, changed, f.game).Succeeded(), "changed ordered points cannot seal another complete terrain result");
    std::swap(changed[0], changed[1]);
    Require(!TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.members[0].data, f.members[1].data,
        directory.Snapshot(), *session, changed, f.game).Succeeded(), "changed ordering cannot seal another result");
    for (const auto mode : {0, 1, 2}) {
        TerrainReplayLimits limits;
        if (mode == 0) limits.maximumWork = 1;
        if (mode == 1) limits.maximumCandidateBytes = 1;
        if (mode == 2) limits.maximumResourceBytes = 1;
        const auto rejected = ReplayFixture(f, directory.Snapshot(), std::span(intents).subspan(1), limits);
        Require(!rejected.Succeeded() && !rejected.value, "tight aggregate budget returns no partially published session");
    }
    auto tooMany = std::vector<TerrainStrokeIntent>(129, intents[1]);
    Require(HasCode(ReplayFixture(f, *f.source, tooMany), "TERRAIN_PATCH_STROKE_BUDGET"), "129 strokes rejected before mutation");
    auto tooManyPoints = intents; tooManyPoints[1].points.resize(4096, f.point);
    Require(HasCode(ReplayFixture(f, *f.source, tooManyPoints), "TERRAIN_PATCH_POINT_BUDGET"), "4096 is a whole-sequence point budget");
    auto conflict = intents; conflict[1].brush.size = 1;
    Require(HasCode(ReplayFixture(f, *f.source, conflict), "TERRAIN_REPLAY_BRUSH_CONFLICT"), "water intent cannot carry neutral-looking ordinary fields");
    auto lateFailure = intents; lateFailure[1].points = {{90000, 90000}};
    const auto rejectedPrefix = ReplayFixture(f, *f.source, lateFailure);
    Require(!rejectedPrefix.Succeeded() && !rejectedPrefix.value
        && session->Revision() == 3, "valid first stroke followed by an invalid water stroke returns no prefix session and leaves prior candidate intact");
    auto changedSource = f.members; changedSource[0].data.back() ^= std::byte{1};
    const auto wrongArchive = Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(f.root / "different.w3x", changedSource)).path));
    Require(!ReplayFixture(f, wrongArchive, std::span(intents).first(1)).Succeeded(), "ordinary-only replay also binds actual W3E before any edit");
    changedSource = f.members;
    for (auto& member : changedSource) if (member.name == "war3map.doo") {
        DoodadPlacements added; added.specialDoodads.push_back({"Doo0", 0, {2, 2}}); member.data = Take(added.Serialize());
    }
    const auto changedDoo = Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(f.root / "different-doo.w3x", changedSource)).path));
    Require(!ReplayFixture(f, changedDoo, std::span(intents).first(1)).Succeeded(), "ordinary-only replay binds complete DOO and exact absent records too");
    changedSource = f.members;
    changedSource.push_back({"table/w3i.ini", Text("[w3i]\nmap_name=\"other settings\"\n")});
    const auto ambiguous = Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(f.root / "ambiguous.w3x", changedSource)).path));
    std::erase_if(changedSource, [](const auto& member) { return member.name == "war3map.w3i"; });
    const auto lniOnly = MapDocument::FromMembers(changedSource);
    Require(!TerrainPathingSave::ReplayStrokes(ambiguous, lniOnly, f.members[0].data, f.members[1].data,
        std::span(intents).first(1), f.game).Succeeded(), "a LNI-only document cannot hide the actual source native settings member");
    Write(directory.Snapshot().root / "UI/MiscData.txt", "[Terrain]\nMaxSlope=1\n");
    Require(!TerrainPathingSave::Seal(MapDocument::FromMembers(f.members), f.members[0].data, f.members[1].data,
        directory.Snapshot(), *session, intents, f.game).Succeeded(), "late map resource modification refuses the exact captured directory source");
}

void ReplayAggregateResourceLimitRejectsOnlyTheSequence() {
    Fixture f(false, true); const auto directory = Directory(f);
    auto intents = OrderedIntents(f); intents.erase(intents.begin());
    // 用实际owner容量选择可容纳一笔的预算；同一预算不能在第二笔重新开始记账。
    const auto measured = Take(ReplayFixture(f, directory.Snapshot(), std::span(intents).first(1)));
    const auto& owner = measured.waterOwners.front();
    std::size_t sourceBytes = f.members[0].data.size() + f.members[1].data.size();
    const auto document = MapDocument::FromMembers(f.members);
    for (const auto& member : document.Members()) for (const auto& input : member.sources) sourceBytes += input.data.size();
    TerrainReplayLimits limits;
    const auto capture = owner.CaptureBudgetBytes(), retained = owner.OwnedCapacityBytes();
    limits.maximumResourceBytes = sourceBytes * 2 + std::max(capture, retained) + std::min(capture, retained) / 2;
    Require(ReplayFixture(f, directory.Snapshot(), std::span(intents).first(1), limits).Succeeded(),
        "nonempty DOO and actual special shapes fit the first stroke under the same aggregate resource limit");
    const auto rejected = ReplayFixture(f, directory.Snapshot(), intents, limits);
    Require(!rejected.Succeeded() && !rejected.value && measured.session->Revision() == 1,
        "the second actual resource capture cannot reset the aggregate ledger or return a prefix session");
}

void LargeOrdinaryReplayFitsTheUnchangedDefaultBudgets() {
    Fixture f;
    const auto vertex = f.grid.vertices.front();
    f.grid.header.width = f.grid.header.height = 513;
    f.grid.vertices.assign(513 * 513, vertex);
    RefreshTerrainDerived(f.grid, 0, 0, 513, 513);
    f.members[0].data = Take(SerializeMapTerrain(f.grid));
    Bytes pathing{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
    for (const auto value : {0u, 2048u, 2048u}) for (unsigned shift = 0; shift < 32; shift += 8)
        pathing.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    pathing.resize(16 + 2048 * 2048, std::byte{0x51}); f.members[1].data = std::move(pathing);
    // 大而未跟随的普通DOO只能作为原始字节复制/身份核对，不能按解析32次计费。
    DoodadPlacements doodads; doodads.doodads.resize(8192);
    for (auto& doodad : doodads.doodads) { doodad.rawcode = "Doo0"; doodad.scale = {1, 1, 1}; }
    for (auto& member : f.members) {
        if (member.name == "war3map.doo") member.data = Take(doodads.Serialize());
        if (member.name == "war3map.w3i") {
            auto settings = Take(MapSettings::Parse(member.data)); settings.playableWidth = settings.playableHeight = 512;
            member.data = Take(settings.Serialize());
        }
    }
    const auto directory = Directory(f);
    TerrainStrokeIntent stroke; stroke.brush.kind = TerrainBrushKind::Raise;
    stroke.brush.radius = 0; stroke.brush.strength = 16; stroke.points = {{32768, 32768}};
    const auto result = Take(ReplayFixture(f, directory.Snapshot(), std::span(&stroke, 1)));
    Require(result.session->Revision() == 1 && Take(result.session->InspectTerrainPoint(stroke.points.front())).groundHeight == 48,
        "a 513-square one-point ordinary edit and large untouched DOO fit the unchanged default work and capacity budgets");
    Require(Take(result.session->Snapshot(MapDataKind::Doodads, kResourceBudget)).Doodads()->doodads.size() == 8192,
        "large untouched DOO remains complete after replay");
}

void AggregateWorkAndHistoryCannotRestartAfterTheFirstCommand() {
    Fixture f(false, true); const auto directory = Directory(f);
    TerrainStrokeIntent stroke; stroke.brush.kind = TerrainBrushKind::Raise;
    stroke.brush.radius = 0; stroke.brush.strength = 16; stroke.points = {{256, 256}};
    const std::array pair{stroke, stroke};
    for (const bool candidateBytes : {false, true}) {
        TerrainReplayLimits limits;
        // 从公开工厂搜索可容纳一笔的实际边界，不复制内部扫描/副本常数来生成预期。
        std::size_t lower = 1, upper = candidateBytes ? limits.maximumCandidateBytes : limits.maximumWork;
        while (upper - lower > 1) {
            const auto middle = lower + (upper - lower) / 2;
            if (candidateBytes) limits.maximumCandidateBytes = middle; else limits.maximumWork = middle;
            if (ReplayFixture(f, directory.Snapshot(), std::span(&stroke, 1), limits).Succeeded()) upper = middle;
            else lower = middle;
        }
        if (candidateBytes) limits.maximumCandidateBytes = upper; else limits.maximumWork = upper;
        const auto first = Take(ReplayFixture(f, directory.Snapshot(), std::span(&stroke, 1), limits));
        Require(first.session->Revision() == 1 && first.session->CanUndo(), "the measured boundary permits one complete command");
        const auto sequence = ReplayFixture(f, directory.Snapshot(), pair, limits);
        Require(!sequence.Succeeded() && !sequence.value && first.session->Revision() == 1,
            "work and retained history share one aggregate limit and never return a valid prefix session");
    }
}

void AnomalousCliffPropagationHasAnExactWholeStrokeBound() {
    Fixture f;
    for (const auto x : {16u, 20u}) {
        f.grid.vertices[16 * 33 + x - 1].cliffAndLayerRaw = 0;
        f.grid.vertices[16 * 33 + x + 1].cliffAndLayerRaw = 14;
    }
    RefreshTerrainDerived(f.grid, 0, 0, 33, 33);
    const auto before = Take(SerializeMapTerrain(f.grid));
    TerrainBrush brush; brush.kind = TerrainBrushKind::CliffLevel; brush.radius = 0; brush.levelDelta = 1;
    const std::array points{TerrainStrokePoint{2048, 2048}, TerrainStrokePoint{2560, 2048}};
    const auto first = Take(PrepareTerrainStroke(f.grid, brush, std::span(points).first(1)));
    const auto whole = Take(PrepareTerrainStroke(f.grid, brush, points));
    Require(first.result.levelPropagationWork > 0 && whole.result.levelPropagationWork > first.result.levelPropagationWork,
        "abnormal high and low neighbors terminate and both samples consume one propagation ledger");
    const auto exact = Take(PrepareTerrainStroke(f.grid, brush, points, whole.result.levelPropagationWork));
    Require(Take(SerializeMapTerrain(exact.terrain)) == Take(SerializeMapTerrain(whole.terrain)),
        "the exact measured whole-stroke limit returns the complete deterministic terrain");
    const auto shortLimit = PrepareTerrainStroke(f.grid, brush, points, whole.result.levelPropagationWork - 1);
    Require(HasCode(shortLimit, "TERRAIN_LEVEL_PROPAGATION_BUDGET") && !shortLimit.value
        && Take(SerializeMapTerrain(f.grid)) == before, "one less visit fails without exposing a partly propagated source");
    f.members[0].data = before;
    f.source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
        Take(WriteVerifiedArchive(f.root / "cliff-source.w3x", f.members)).path)));
    auto session = Session(f);
    const auto document = f.Document(); const TerrainEditBinding binding{document, session->SessionIdentity(), session->Revision(), session->TerrainSourceRevision()};
    const auto rejected = PrepareTerrainEdit(*session, document, binding, brush, points, false, whole.result.levelPropagationWork - 1);
    Require(HasCode(rejected, "TERRAIN_LEVEL_PROPAGATION_BUDGET") && session->Revision() == 0 && !session->CanUndo(),
        "shared Services preparation forwards the same hard counter and preserves the original session");
    const auto prepared = Take(PrepareTerrainEdit(*session, document, binding, brush, points, false, whole.result.levelPropagationWork));
    Require(ApplyTerrainEdit(*session, document, prepared).Succeeded() && session->Undo()
        && Take(SerializeMapTerrain(*session->Terrain())) == before && session->Redo(),
        "a complete bounded cliff command keeps the original undo and redo path");
}

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
        if (!HasCode(prepared, "SCRIPT_TOOLCHAIN_MISSING"))
            for (const auto& diagnostic : prepared.diagnostics)
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        Require(HasCode(prepared, "SCRIPT_TOOLCHAIN_MISSING"), "ordinary plus water strokes reach the unchanged supervised script gate");
        Require(Take(Sha256File(f.source->SourcePath())) == f.source->SourceSha256() && !std::filesystem::exists(request.targetCandidate),
            "missing compiler cannot publish the internally prepared water candidate");
        request = f.Request(); request.terrainStrokes[0].waterBrush.reset();
        request.terrainStrokes[0].brush.kind = TerrainBrushKind::WaterHeight;
        const auto heightOnly = SaveMapEditCandidate(request);
        Require(HasCode(heightOnly, "SAVE_NO_TERRAIN_CHANGE"), "water-height on dry ground remains distinct from creating shallow or deep water");
    }
}

void DirectSaveAllowsSpecialWaterDeletionWithoutPlacementFollow() {
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        Fixture f(true, true); auto request = f.Request();
        auto& stroke = request.terrainStrokes.front();
        stroke.waterBrush->kind = kind; stroke.followDoodads = false;
        TerrainStrokeIntent intent;
        intent.waterBrush = stroke.waterBrush; intent.points = {f.point}; intent.followDoodads = false;
        const auto replayed = Take(ReplayFixture(f, *f.source, std::span(&intent, 1)));
        const auto replacements = replayed.session->Replacements();
        Require(std::any_of(replacements.begin(), replacements.end(), [](const auto& member) {
                return member.name == "war3map.doo";
            }) && std::none_of(replacements.begin(), replacements.end(), [](const auto& member) {
                return member.name == "war3mapUnits.doo";
            }), "water without placement follow replaces special DOO but never Units");
        const auto specials = Take(replayed.session->Snapshot(MapDataKind::Doodads, kResourceBudget)).Doodads()->specialDoodads;
        Require(specials.size() == 2 && specials[0].position == (std::array<std::int32_t, 2>{28, 28})
            && specials[1].position == (std::array<std::int32_t, 2>{24, 24}),
            "the direct-save fixture actually deletes duplicate special records with native swap-last ordering");
        const auto prepared = SaveMapEditCandidate(request);
        Require(!prepared.Succeeded() && !prepared.value && HasCode(prepared, "SCRIPT_TOOLCHAIN_MISSING")
            && !HasCode(prepared, "SAVE_EDIT_SCOPE_INVALID"),
            "public save permits proven special-water deletion without placement follow and reaches the original script gate");
        Require(Take(Sha256File(f.source->SourcePath())) == f.source->SourceSha256()
            && !std::filesystem::exists(request.targetCandidate),
            "the missing compiler leaves the original archive intact and publishes no special-water candidate");
    }
}

void ForwardJournalRestoresOwnedSpecialDeletionAndRejectsDrift() {
    Fixture f(true, true); const auto directory = Directory(f); const auto intents = OrderedIntents(f);
    auto replayed = Take(ReplayFixture(f, *f.source, intents));
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *replayed.session));
    const auto decoded = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(journal))));
    Require(decoded.strokes.size() == intents.size() && decoded.members.size() == 10
        && decoded.members[0].present && !decoded.members[6].present,
        "journal captures the complete ordered prefix and exact source absence");
    const auto before = Take(SerializeMapTerrain(*replayed.session->Terrain()));
    auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, decoded, *replayed.session, f.game));
    Require(restored.receipts.size() == intents.size() && restored.waterOwners.size() == 2
        && restored.session->RequiresTerrainStrokeJournal(), "only actual replay restores special deletion lineage");
    Require(TerrainPathingSave::PrepareForSave(*f.source, *restored.session, f.game).Succeeded(),
        "restored private special lineage reaches the unchanged native save plan");
    Require(restored.session->Replay(restored.receipts.back(), false).Succeeded()
        && Take(restored.session->ExportTerrainStrokeJournal()).strokes.size() == 2,
        "restored actual receipts support Qt-style one-stroke undo");
    const auto directoryJournal = Take(TerrainPathingSave::CaptureJournal(directory.Snapshot(), *replayed.session));
    Require(TerrainPathingSave::RestoreJournal(directory.Snapshot(), MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, directoryJournal, *replayed.session, f.game).Succeeded(),
        "directory and archive use the same owned restore factory");

    auto changed = decoded; changed.strokes[1].rulesSha256.assign(64, 'f');
    const auto wrongRules = TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, changed, *replayed.session, f.game);
    Require(!wrongRules.Succeeded() && !wrongRules.value && HasCode(wrongRules, "TERRAIN_JOURNAL_RESOURCE_DRIFT"),
        "final output equality cannot replace actual dependency fingerprints");
    changed = decoded; changed.members[6].present = true; changed.members[6].sha256.assign(64, 'e');
    const auto wrongAbsence = TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, changed, *replayed.session, f.game);
    Require(HasCode(wrongAbsence, "TERRAIN_JOURNAL_BASELINE_MISMATCH") && !wrongAbsence.value,
        "an absent native member is part of the exact baseline identity");
    auto raw = Session(f);
    Require(raw->SetTerrain(before).Succeeded(), "old terrain draft remains readable");
    for (const auto& member : replayed.session->Replacements()) if (member.name == "war3map.doo")
        Require(raw->SetSerialized(MapDataKind::Doodads, member.data).Succeeded(), "complete final raw DOO remains readable");
    Require(!raw->RequiresTerrainStrokeJournal()
        && !TerrainPathingSave::PrepareForSave(*f.source, *raw, f.game).Succeeded(),
        "raw overlay and matching bytes cannot install private special provenance");
    Require(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, decoded, *raw, f.game).Succeeded(), "raw draft can only gain provenance through complete owned replay");
    TerrainReplayLimits tight; tight.maximumWork = 1;
    const auto overBudget = TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, decoded, *replayed.session, f.game, tight);
    Require(!overBudget.Succeeded() && !overBudget.value && Take(SerializeMapTerrain(*replayed.session->Terrain())) == before,
        "budget failure never mutates or publishes a partial live prefix");

    const auto equivalentRoot = f.root / "equivalent-game"; std::filesystem::create_directory(equivalentRoot);
    const std::vector<MapArchiveMember> identicalGame{{"fixture.txt", Text("private fixture")}};
    Take(WriteVerifiedArchive(equivalentRoot / "war3.mpq", identicalGame));
    Require(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, decoded, *replayed.session, equivalentRoot).Succeeded(), "content-identical game root relocation keeps dependency identity");
    const auto driftRoot = f.root / "drift-game"; std::filesystem::create_directory(driftRoot);
    const std::vector<MapArchiveMember> changedGame{{"fixture.txt", Text("private fixture")},
        {"Units/MiscData.txt", Text("[Unrelated]\nValue=1\n")}};
    Take(WriteVerifiedArchive(driftRoot / "war3.mpq", changedGame));
    const auto drifted = TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, decoded, *replayed.session, driftRoot);
    Require(!drifted.Succeeded() && !drifted.value && HasCode(drifted, "TERRAIN_JOURNAL_RESOURCE_DRIFT"),
        "a formerly absent optional dependency rejects restore even when parsed terrain settings are unchanged");
}

void ForwardJournalSeparatesBaselineReadFailureFromResourceFallback() {
    Fixture f(true, true); const auto directory = Directory(f); const auto intents = OrderedIntents(f);
    auto replayed = Take(ReplayFixture(f, *f.source, intents));
    const auto archiveJournal = Take(TerrainPathingSave::CaptureJournal(*f.source, *replayed.session));
    const auto directoryJournal = Take(TerrainPathingSave::CaptureJournal(directory.Snapshot(), *replayed.session));
    const auto before = Take(SerializeMapTerrain(*replayed.session->Terrain()));
    // 完整真实基线失效不能与游戏资源缺失混为一谈，原始底层诊断也必须保留。
    Require(IO::WriteFileBytes(directory.Snapshot().root / "war3map.w3e", Text("changed directory baseline")).Succeeded(),
        "change only the private directory baseline after journal capture");
    const auto changedDirectory = TerrainPathingSave::RestoreJournal(directory.Snapshot(), MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, directoryJournal, *replayed.session, f.game);
    Require(!changedDirectory.Succeeded() && !changedDirectory.value
        && HasCode(changedDirectory, "TERRAIN_REPLAY_SOURCE_READ_FAILED")
        && HasCode(changedDirectory, "TERRAIN_PATHING_RESOURCE_CHANGED"),
        "directory source identity failure is explicitly fatal and retains its original diagnostic");
    Require(IO::WriteFileBytes(f.source->SourcePath(), Text("changed archive baseline")).Succeeded(),
        "change only the private archive baseline after journal capture");
    const auto changedArchive = TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, archiveJournal, *replayed.session, f.game);
    Require(!changedArchive.Succeeded() && !changedArchive.value
        && HasCode(changedArchive, "TERRAIN_REPLAY_SOURCE_READ_FAILED") && HasCode(changedArchive, "SOURCE_CHANGED"),
        "archive source identity failure is explicitly fatal and retains its original diagnostic");
    Require(Take(SerializeMapTerrain(*replayed.session->Terrain())) == before,
        "failed baseline reads do not mutate or publish a partial replay");
}

void ForwardJournalPreservesSavedRestrictionOverlapAndUndo() {
    Fixture f(true, true);
    // 原创来源先明确含不可飞P=4，随后声明S=6，刻意制造P/S重叠。
    for (std::size_t i = 16; i < f.members[1].data.size(); ++i) f.members[1].data[i] |= std::byte{4};
    f.source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
        Take(WriteVerifiedArchive(f.root / "protected-source.w3x", f.members)).path)));
    f.edits = Session(f);
    TerrainBrush restriction; restriction.kind = TerrainBrushKind::PathingRestriction;
    restriction.radius = 0; restriction.pathingRestrictions = 6; restriction.value = true;
    const TerrainStrokePoint point{256, 256};
    Require(f.edits->EditTerrain(restriction, std::span(&point, 1), false).Succeeded(), "overlapping saved intent is a real command");
    const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
    auto replacements = f.edits->Replacements();
    Require(plan.InstallPathingOutput(*f.edits, replacements).Succeeded(), "freeze the actual native saved restriction output");
    const auto written = Take(f.source->SaveCandidate(f.root / "saved-restriction.w3x", replacements));
    auto saved = Take(MapArchiveSession::Open(written.path));
    Require(plan.VerifyReadback(saved).Succeeded() && plan.CommitSaved(*f.edits, MapDocument::FromArchive(saved)).Succeeded(),
        "nonzero savedS comes only from the real save/readback baseline");
    f.source = std::make_unique<MapArchiveSession>(std::move(saved));
    const auto terrain = Take(f.source->ReadMember("war3map.w3e"));
    const auto pathing = Take(f.source->ReadMember("war3map.wpm"));
    const auto original = MapDocument::FromArchive(*f.source);
    const auto savedDraft = Take(f.edits->SerializePathingRestrictionDraft());
    const auto layers = Take(DecodePathingRestrictionLayers(*f.edits->Terrain(), pathing, pathing, savedDraft));
    Require(std::any_of(layers.savedIntent.begin(), layers.savedIntent.end(), [](auto value) { return value == std::byte{6}; }),
        "the tested journal baseline has nonzero savedS");
    bool overlap = false;
    for (std::size_t i = 0; i < layers.savedIntent.size(); ++i)
        overlap = overlap || (layers.savedIntent[i] & layers.protectedSource[i]) != std::byte{0};
    Require(overlap, "protected P and saved S overlap in the tested baseline");
    restriction.value = false;
    Require(f.edits->EditTerrain(restriction, std::span(&point, 1), false).Succeeded(), "new prefix erases only saved project intent");
    const auto erasedDraft = Take(f.edits->SerializePathingRestrictionDraft());
    const auto rules = f.Rules();
    TerrainWaterBrush water; water.size = 1; water.randomSeed = 881;
    auto prepared = Take(PrepareTerrainWaterEdit(*f.edits, f.Document(), f.Binding(), rules,
        TerrainWaterStrokeOptions{water.kind, water.shape, water.size, water.cliffTexture, water.randomSeed, true},
        std::span(&f.point, 1), false));
    Require(ApplyTerrainEdit(*f.edits, f.Document(), prepared).Succeeded(), "water follows the restriction erase in the same typed prefix");
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
    Require(journal.strokes.size() == 2 && journal.baselinePathingLayers == savedDraft,
        "journal starts at actual savedS, not the final erased currentS");
    auto raw = std::make_unique<MapDataEditSession>(original);
    Require(raw->AttachTerrain(terrain).Succeeded() && raw->AttachPathing(pathing).Succeeded(), "attach fresh raw baseline");
    auto finalTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    auto finalPathing = pathing;
    for (const auto& member : f.edits->Replacements()) {
        if (member.name == "war3map.wpm") finalPathing = member.data;
        else if (member.name == "war3map.doo") Require(raw->SetSerialized(MapDataKind::Doodads, member.data).Succeeded(), "restore full raw DOO");
    }
    Require(raw->SetTerrainAndPathingDraft(finalTerrain, finalPathing, Take(f.edits->SerializePathingRestrictionDraft())).Succeeded(),
        "restore full raw output with its actual P/savedS/currentS");
    auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, original, terrain, pathing, journal, *raw, f.game));
    Require(restored.receipts.size() == 2 && restored.session->MatchesTerrainJournalDraft(*raw), "nonzero savedS fully matches the owned replay result");
    Require(restored.session->Replay(restored.receipts.back(), false).Succeeded()
        && Take(restored.session->SerializePathingRestrictionDraft()) == erasedDraft, "water Undo retains erased S and protected P");
    Require(restored.session->Replay(restored.receipts.front(), false).Succeeded()
        && Take(restored.session->SerializePathingRestrictionDraft()) == savedDraft, "erase Undo restores savedS including its overlap with P");
    Require(!restored.session->IsDirty(), "undoing the complete restored prefix returns to the real saved baseline");
    TerrainReplayLimits tight; tight.maximumCandidateBytes = 1;
    const auto denied = TerrainPathingSave::RestoreJournal(*f.source, original, terrain, pathing, journal, *raw, f.game, tight);
    Require(!denied.Succeeded() && !denied.value && HasCode(denied, "TERRAIN_REPLAY_BUDGET"), "raw resident memory is precharged before replay or final comparison");
}
} // namespace

int main() {
    try {
        SharedReplayPreservesOrderSpecialDeletionAndCompleteDirectoryOutput();
        ReplayBudgetsAndTamperedIntentDoNotPublish();
        ReplayAggregateResourceLimitRejectsOnlyTheSequence();
        LargeOrdinaryReplayFitsTheUnchangedDefaultBudgets();
        AggregateWorkAndHistoryCannotRestartAfterTheFirstCommand();
        AnomalousCliffPropagationHasAnExactWholeStrokeBound();
        OneAnchorAndWholePointSequenceShareCoreCommand();
        OrderedSourceAndOptionsCannotBeSubstituted();
        WaterServiceCandidatesPassNativePlanAndIndependentArchiveReadback();
        SavedWaterStrokeUndoAndRedoRebaseAgainstEachSavedSource();
        DirectSaveRejectsEveryConflictingOrdinaryField();
        DirectSaveUsesOrderedWaterAndOriginalScriptGate();
        DirectSaveAllowsSpecialWaterDeletionWithoutPlacementFollow();
        ForwardJournalRestoresOwnedSpecialDeletionAndRejectsDrift();
        ForwardJournalSeparatesBaselineReadFailureFromResourceFallback();
        ForwardJournalPreservesSavedRestrictionOverlapAndUndo();
        std::cout << "TerrainWaterEditServiceTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
