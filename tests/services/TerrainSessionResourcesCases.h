#pragma once

#include "../support/TerrainResizeFixture.h"
#include "War3MapCore/MapData/NativeEditorProfileInputs.h"
#include "War3MapCore/Services/TerrainBoundsEditService.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"
#include "War3MapCore/Services/TerrainSessionResources.h"

#include <limits>

namespace AudastraTest::TerrainSessionResourcesCases {
using namespace AudastraTest::TerrainResize;
using namespace War3Map::Services;

inline MapDataQueryBinding Document(const Fixture& fixture) {
    return {"synthetic-current-structure", 7, 0, fixture.archive->SourceSha256()};
}
inline TerrainEditBinding Binding(const Fixture& fixture) {
    return {Document(fixture), fixture.edits->SessionIdentity(), fixture.edits->Revision(), fixture.edits->TerrainSourceRevision()};
}

// 所有数据均为原创规则。显式列出每个实际启动输入，既不借本机游戏资源，
// 也不让缺项通过fallback或测试桩变成“已核对”。
inline void AddActualWaterInputs(Fixture& fixture) {
    for (const auto& profile : kClassicProfiles) {
        if (std::none_of(fixture.members.begin(), fixture.members.end(), [&](const auto& member) { return member.name == profile.path; }))
            fixture.PutMember(profile.path, Text("[Synthetic]\nPresent=1\n"));
    }
    fixture.PutMember("UI/MiscData.txt", Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n"
        "[Water]\nDeepLevelPathing=192\n[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n"));
    fixture.PutMember("UI/WorldEditData.txt", Text("[Synthetic]\nPresent=1\n"));
    fixture.Seal();
}

inline std::shared_ptr<const MapArchiveSession> Archive(const Fixture& fixture) {
    return std::make_shared<MapArchiveSession>(Take(MapArchiveSession::Open(fixture.archive->SourcePath())));
}
inline TerrainSessionResources Current(const Fixture& fixture) {
    return Take(TerrainSessionResources::Capture(Archive(fixture), *fixture.edits, fixture.gameRoot));
}
inline void Expand(Fixture& fixture) {
    const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    const auto prepared = Take(PrepareTerrainResizeEdit(*fixture.edits, Document(fixture), Binding(fixture), resources,
        TerrainResizeIntent{1, {-32, -32, 32, 32}}));
    Check(resources.VerifySources(*fixture.archive, *fixture.edits));
    Check(ApplyTerrainResizeEdit(*fixture.edits, Document(fixture), prepared));
}

inline void ActualCurrentWaterAndBothNativeDoodadStages() {
    Fixture fixture(true); AddActualWaterInputs(fixture); Expand(fixture);
    const auto owner = Current(fixture);
    const auto water = Take(owner.CaptureWater(true));
    Require(water.WaterBrushRules() && water.WaterBrushRules()->maximumCliffLevel == 14
        && water.WaterBrushRules()->maxHeight == 1536 && water.WaterBrushRules()->deepWaterThreshold == 192,
        "water owner reads actual current palette and complete original startup fields after resize");
    Check(owner.Verify(water));
    const auto native = Take(owner.CaptureNative(*fixture.edits->Terrain()));
    Check(owner.Verify(native));
    // 独立平地六mode字面：世界可玩区仍对应扩展canvas的[33,62)²。
    Bytes expected(256 * 256);
    for (std::size_t y = 0; y < 256; ++y) for (std::size_t x = 0; x < 256; ++x)
        expected[y * 256 + x] = std::byte(x < 132 || x >= 248 || y < 132 || y >= 248 ? 0xce : 0x40);
    Require(Take(native.GenerateNative(*fixture.edits->Terrain(), TerrainNativeStage::Before, 64u * 1024u * 1024u)) == expected
        && Take(native.GenerateNative(*fixture.edits->Terrain(), TerrainNativeStage::After, 64u * 1024u * 1024u)) == expected,
        "both native stages bind the actual expanded current shape, never the old physical dimensions");

    auto candidateDoodads = *Take(fixture.edits->Snapshot(MapDataKind::Doodads)).Doodads();
    candidateDoodads.doodads[0].position[0] = 256;
    const auto candidateWire = Take(candidateDoodads.Serialize());
    const auto dual = Take(owner.CaptureNative(*fixture.edits->Terrain(), &candidateWire));
    Check(owner.Verify(dual)); // sourceInputs核当前源DOO，不能拿目标DOO替代真实源。
    const std::array<MemberReplacement, 1> after{{{"war3map.doo", candidateWire}}};
    Check(dual.VerifyFrozenOutput(after, {}));
    const std::array<MemberReplacement, 1> wrong{{{"war3map.doo", fixture.Member("war3map.doo")}}};
    Require(!dual.VerifyFrozenOutput(wrong, {}).Succeeded(), "native output independently rejects a non-target DOO even while current source verification succeeds");
    Require(Take(SerializeMapTerrain(*fixture.edits->Terrain())) == TerrainLiteral(-32, -32, 32, 32),
        "resource capture and both DOO stages never mutate the active terrain");
}

inline std::filesystem::path WriteWorkspace(Fixture& fixture) {
    const auto root = fixture.directory / "actual-directory-source";
    Require(std::filesystem::create_directory(root), "create original private directory source");
    for (const auto& member : fixture.members) {
        const auto path = root / std::filesystem::path(member.name);
        std::filesystem::create_directories(path.parent_path());
        Check(IO::WriteFileBytes(path, member.data));
    }
    return root;
}

inline void SharedFrozenBudgetsIncludeRetainedRequestAndDirectory() {
    constexpr std::size_t maximum = 64u * 1024u * 1024u;
    constexpr std::size_t tight = 1024u * 1024u;
    Fixture fixture; AddActualWaterInputs(fixture); Expand(fixture);
    const auto revision = fixture.edits->Revision();
    const auto terrain = fixture.edits->Terrain();
    std::size_t copiedBytes = 0;
    const auto frozen = Take(fixture.edits->FreezeForSave(maximum, &copiedBytes));
    Require(copiedBytes > 0 && frozen->Terrain() == terrain && frozen->Revision() == revision,
        "shared worker uses one genuine bounded frozen session and shared immutable current terrain");
    const auto archive = Archive(fixture);
    const auto shared = Take(TerrainSessionResources::Capture(archive, frozen, fixture.gameRoot, maximum));
    const auto retained = Take(TerrainSessionResources::Capture(archive, frozen, fixture.gameRoot, maximum, copiedBytes));
    Require(retained.OwnedCapacityBytes() >= shared.OwnedCapacityBytes() + copiedBytes,
        "shared archive owner charges the actual frozen request deep copies in the same budget");
    const auto water = Take(retained.CaptureWater(true)); Check(retained.Verify(water));
    const auto tightArchive = Take(TerrainSessionResources::Capture(archive, frozen, fixture.gameRoot, tight, copiedBytes));
    Require(tightArchive.OwnedCapacityBytes() < tight,
        "valid shared archive capture works under a genuinely smaller explicit budget");
    Require(!TerrainSessionResources::Capture(archive, frozen, fixture.gameRoot, 1, copiedBytes).Succeeded()
        && !TerrainSessionResources::Capture(archive, frozen, fixture.gameRoot, retained.OwnedCapacityBytes(), copiedBytes).Succeeded(),
        "shared archive entry rejects tiny budget and a cap consumed entirely by its retained owner");
    const auto rejectBudget = [](const Result<TerrainSessionResources>& result, const char* message) {
        Require(!result.Succeeded() && HasCode({result.diagnostics}, "TERRAIN_SESSION_RESOURCE_BUDGET"), message);
    };
    for (const auto extra : {maximum - 1, maximum, (std::numeric_limits<std::size_t>::max)()})
        rejectBudget(TerrainSessionResources::Capture(archive, frozen, fixture.gameRoot, maximum, extra),
            "archive request retention cannot consume the cap or wrap additionalOwnedBytes");

    const auto root = WriteWorkspace(fixture);
    const auto workspace = Take(DirectoryWorkspace::Open(root));
    const auto directory = Take(TerrainSessionResources::Capture(workspace.Snapshot(), frozen, fixture.gameRoot, maximum, copiedBytes));
    Require(directory.OwnedCapacityBytes() > retained.OwnedCapacityBytes(),
        "shared directory closure retains and charges its real file listing in addition to frozen request bytes");
    const auto tightDirectory = Take(TerrainSessionResources::Capture(workspace.Snapshot(), frozen, fixture.gameRoot, tight, copiedBytes));
    Require(tightDirectory.OwnedCapacityBytes() < tight,
        "shared directory entry reuses the genuine frozen session within a smaller cap");
    Require(!TerrainSessionResources::Capture(workspace.Snapshot(), frozen, fixture.gameRoot, 1, copiedBytes).Succeeded()
        && !TerrainSessionResources::Capture(workspace.Snapshot(), frozen, fixture.gameRoot, directory.OwnedCapacityBytes(), copiedBytes).Succeeded(),
        "shared directory budget includes closure and request before current structures are materialized");
    for (const auto extra : {maximum - 1, maximum, (std::numeric_limits<std::size_t>::max)()})
        rejectBudget(TerrainSessionResources::Capture(workspace.Snapshot(), frozen, fixture.gameRoot, maximum, extra),
            "directory and request retention cannot consume the cap or overflow their combined charge");
    Require(fixture.edits->Revision() == revision && fixture.edits->Terrain() == terrain
        && Take(SerializeMapTerrain(*terrain)) == TerrainLiteral(-32, -32, 32, 32),
        "all shared capture budget rejections preserve the active session and exact expanded terrain");
}

inline void PhysicalChangesAndBudgetsStayFailClosed() {
    for (const auto name : {"war3map.w3r", "war3map.w3c", "war3map.w3s", "war3map.wts"}) {
        Fixture fixture; AddActualWaterInputs(fixture); Expand(fixture);
        const auto root = WriteWorkspace(fixture);
        const auto workspace = Take(DirectoryWorkspace::Open(root));
        const auto owner = Take(TerrainSessionResources::Capture(workspace.Snapshot(), *fixture.edits, fixture.gameRoot));
        const auto water = Take(owner.CaptureWater(true)); Check(owner.Verify(water));
        Check(IO::WriteFileBytes(root / name, Text("changed physical source, not current overlay")));
        Require(!owner.Verify(water).Succeeded() && !owner.CaptureNative(*fixture.edits->Terrain()).Succeeded(),
            "physical spatial members and absent-to-present fixed inputs are rechecked despite current structural overlay");
    }
    Fixture fixture; AddActualWaterInputs(fixture); Expand(fixture);
    const auto owner = Current(fixture); const auto water = Take(owner.CaptureWater(true));
    Require(!TerrainSessionResources::Capture(Archive(fixture), *fixture.edits, fixture.gameRoot, 1).Succeeded(),
        "tiny public capture budget cannot allocate an unbounded current snapshot or writer");
    Require(!owner.CaptureWater(true, owner.OwnedCapacityBytes()).Succeeded()
        && !owner.CaptureNative(*fixture.edits->Terrain(), nullptr, owner.OwnedCapacityBytes()).Succeeded()
        && !owner.Verify(water, owner.OwnedCapacityBytes()).Succeeded(),
        "retained current owner leaves no borrowed budget for water, native or physical verification");
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        Fixture raw; AddActualWaterInputs(raw); Expand(raw);
        auto grid = *raw.edits->Terrain();
        if (mutation == 0) grid.header.mainTileset = 'A';
        else if (mutation == 1) grid.vertices[66].waterHeightAndFlagsRaw ^= 0x8000;
        else grid.vertices[66].groundHeightRaw ^= 0x4000;
        Check(raw.edits->SetTerrain(Take(SerializeMapTerrain(grid))));
        const auto revision = raw.edits->Revision(); const auto before = raw.edits->Terrain();
        Require(!TerrainSessionResources::Capture(Archive(raw), *raw.edits, raw.gameRoot).Succeeded()
            && raw.edits->Revision() == revision && raw.edits->Terrain() == before,
            "typed resize cannot launder a later raw tileset, water or height-layout mutation into current resource qualification");
    }
}

inline void SavedResizeUsesCurrentPaletteResources() {
    for (const bool directorySource : {false, true}) {
        Fixture fixture; AddActualWaterInputs(fixture);
        fixture.PutMember("TerrainArt/Terrain.slk", Slk({"tileID", "walkable", "flyable", "buildable"},
            {{"Tg01", "1", "1", "1"}, {"Tg02", "1", "1", "1"}}));
        fixture.Seal(); Expand(fixture);
        const auto originalPath = fixture.archive->SourcePath();
        const auto originalSha = fixture.archive->SourceSha256();
        {
            const auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.archive, *fixture.edits, fixture.gameRoot));
            Check(plan.SaveGate()); auto output = fixture.edits->Replacements(); const auto removals = fixture.edits->Removals();
            Check(plan.InstallPathingOutput(*fixture.edits, output));
            Check(plan.VerifyForSave(*fixture.edits, originalSha, output, removals));
            Check(plan.VerifySourceInputs(*fixture.archive));
            auto saved = Take(MapArchiveSession::Open(Take(fixture.archive->SaveCandidate(
                fixture.directory / "expanded-current-source.w3x", output, removals)).path));
            Check(plan.VerifyReadback(saved)); Check(plan.CommitSaved(*fixture.edits, MapDocument::FromArchive(saved)));
            fixture.archive = std::make_unique<MapArchiveSession>(std::move(saved));
        }
        Require(!fixture.edits->IsDirty() && !fixture.edits->HasTerrainResizeChanges()
            && fixture.edits->HasTerrainTypedResizeState(),
            "actual saved resize keeps trusted current structure while its net resize difference is clean");
        const auto savedSha = fixture.archive->SourceSha256();
        {
            const auto owner = Current(fixture); const auto water = Take(owner.CaptureWater(true));
            TerrainWaterStrokeOptions options; options.kind = TerrainWaterBrushKind::Shallow;
            options.shape = TerrainBrushShape::Square; options.size = 3; options.randomSeed = 17;
            const TerrainStrokePoint point{-512, -512};
            const auto prepared = Take(PrepareTerrainWaterEdit(*fixture.edits, Document(fixture), Binding(fixture), water,
                options, std::span(&point, 1), false));
            Check(owner.Verify(water)); Take(ApplyTerrainEdit(*fixture.edits, Document(fixture), prepared));
        }
        if (directorySource) {
            const auto root = WriteWorkspace(fixture);
            for (const auto name : {"war3map.w3e", "war3map.wpm", "war3map.w3i", "war3map.shd"})
                Check(IO::WriteFileBytes(root / name, Take(fixture.archive->ReadMember(name))));
            const auto workspace = Take(DirectoryWorkspace::Open(root));
            Take(TerrainPathingSave::EditTextureSlot(workspace.Snapshot(), *fixture.edits,
                TerrainTextureTable::Ground, 1, "Tg02", fixture.gameRoot));
        } else {
            Take(TerrainPathingSave::EditTextureSlot(*fixture.archive, *fixture.edits,
                TerrainTextureTable::Ground, 1, "Tg02", fixture.gameRoot));
        }
        Require(fixture.edits->Terrain()->header.groundTilesets == std::vector<std::string>{"Tg01", "Tg02"}
            && fixture.edits->Terrain()->vertices[44 * 65 + 44].HasWater()
            && fixture.edits->HasTerrainTypedResizeState() && !fixture.edits->HasTerrainResizeChanges(),
            "archive and directory palette services after save bind full current water edits, not clean physical terrain");
        Require(Take(Sha256File(originalPath)) == originalSha
            && Take(Sha256File(fixture.archive->SourcePath())) == savedSha,
            "post-save water and palette edits leave both original and actual saved physical sources read-only");
    }
}

inline void ResizeWaterBoundsSaveReadback() {
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        Fixture fixture(true); AddActualWaterInputs(fixture); const auto sourceSha = fixture.archive->SourceSha256();
        Expand(fixture);
        {
            const auto owner = Current(fixture); const auto water = Take(owner.CaptureWater(true));
            TerrainWaterStrokeOptions options; options.kind = kind; options.shape = TerrainBrushShape::Square;
            options.size = 3; options.randomSeed = 17;
            const TerrainStrokePoint point{-512, -512};
            const auto prepared = Take(PrepareTerrainWaterEdit(*fixture.edits, Document(fixture), Binding(fixture), water,
                options, std::span(&point, 1), false));
            Check(owner.Verify(water)); Take(ApplyTerrainEdit(*fixture.edits, Document(fixture), prepared));
        }
        const auto& center = fixture.edits->Terrain()->vertices[44 * 65 + 44];
        Require(center.HasWater() && center.groundHeightRaw == 8192 && (center.waterHeightAndFlagsRaw & 0x3fff) == 8192
            && center.CliffLevel() == (kind == TerrainWaterBrushKind::Shallow ? 1 : 0),
            "literal central water fields survive actual water command after resized shape");
        {
            const auto owner = Current(fixture); const auto native = Take(owner.CaptureNative(*fixture.edits->Terrain()));
            const TerrainBoundsEditIntent intent{{34, 34, 62, 62}, 1};
            const auto prepared = Take(PrepareTerrainBoundsEdit(*fixture.edits, Document(fixture), Binding(fixture), native, intent));
            Check(owner.Verify(native)); Check(ApplyTerrainBoundsEdit(*fixture.edits, Document(fixture), prepared));
        }
        const auto beforeSave = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
        auto expectedSettings = fixture.settings; expectedSettings.cameraMargins = {34, 2, 34, 2};
        expectedSettings.playableWidth = expectedSettings.playableHeight = 28;
        expectedSettings.cameraBounds = {-1664, -1664, 1536, 1536, -1664, 1536, 1536, -1664};
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.archive, *fixture.edits, fixture.gameRoot));
        Check(plan.SaveGate()); auto output = fixture.edits->Replacements(); const auto removals = fixture.edits->Removals();
        Check(plan.InstallPathingOutput(*fixture.edits, output));
        Check(plan.VerifyForSave(*fixture.edits, sourceSha, output, removals)); Check(plan.VerifySourceInputs(*fixture.archive));
        auto saved = Take(MapArchiveSession::Open(Take(fixture.archive->SaveCandidate(fixture.directory
            / (kind == TerrainWaterBrushKind::Shallow ? "current-shallow.w3x" : "current-deep.w3x"), output, removals)).path));
        Check(plan.VerifyReadback(saved));
        Require(Take(saved.ReadMember("war3map.w3e")) == beforeSave
            && Take(saved.ReadMember("war3map.w3i")) == Take(expectedSettings.Serialize()),
            "actual reopened combined resize-water-bounds output keeps full terrain and literal independent W3I fields");
        const auto wpm = Take(saved.ReadMember("war3map.wpm"));
        Require(wpm.size() == 16 + 256 * 256 && wpm[16 + 134 * 256 + 134] == std::byte{0xdf}
            && wpm[16 + 224 * 256 + 224] == std::byte{0x51}
            && wpm[16 + 178 * 256 + 178] == std::byte(kind == TerrainWaterBrushKind::Shallow ? 0x19 : 0x1b),
            "saved WPM has literal new boundary, retained dry ground and shallow/deep center plus original author bits");
        Require(Take(saved.ReadMember("war3map.shd")) == ShadowLiteral(-32, -32, 32, 32, true),
            "water and bounds preserve the exact typed resized SHD without pretending to bake it");
        for (const auto name : {"war3map.doo", "war3mapUnits.doo", "war3map.w3r", "war3map.w3c", "war3mapMap.tga", "war3map.j"})
            Require(Take(saved.ReadMember(name)) == fixture.Member(name), "combined save preserves complete ordinary objects, regions, cameras, custom minimap and script");
        Check(plan.CommitSaved(*fixture.edits, MapDocument::FromArchive(saved)));
        Require(!fixture.edits->IsDirty() && Take(Sha256File(fixture.archive->SourcePath())) == sourceSha,
            "actual combined save commits clean baseline while original synthetic physical source stays read-only");
    }
}

inline void RunTerrainSessionResourcesCases() {
    ActualCurrentWaterAndBothNativeDoodadStages();
    SharedFrozenBudgetsIncludeRetainedRequestAndDirectory();
    PhysicalChangesAndBudgetsStayFailClosed();
    SavedResizeUsesCurrentPaletteResources();
    ResizeWaterBoundsSaveReadback();
}
} // namespace AudastraTest::TerrainSessionResourcesCases
