#pragma once

#include "support/TerrainResizeFixture.h"
#include "War3MapCore/MapData/TerrainResizeEditing.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"

#include <limits>

namespace AudastraTest::TerrainResizeCases {
using namespace TerrainResize;
using War3Map::Services::TerrainResizeEditService;

inline TerrainResizeDimensionRules Rules() { return {32, 480, 32, 256u * 256u}; }
inline constexpr std::array<float, 4> Margins{1, 2, 1, 2};
inline TerrainResizeIntent Intent(int west, int south, int east, int north) {
    return {1, {west, south, east, north}};
}
inline MapSettings Settings(const Fixture& fixture) { return Take(MapSettings::Parse(fixture.members[2].data)); }
inline Bytes Wpm(const MapDataEditSession& edits, const Bytes& original) {
    for (const auto& item : edits.Replacements()) if (item.name == "war3map.wpm") return item.data;
    return original;
}
inline Bytes Grid(const MapDataEditSession& edits) { return Take(SerializeMapTerrain(*edits.Terrain())); }
inline void Resize(Fixture& fixture, const TerrainResizeIntent& intent) {
    const auto owner = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    const auto prepared = Take(fixture.edits->PrepareTerrainResizeEdit(owner, intent));
    Check(fixture.edits->ApplyPreparedTerrainResizeEdit(prepared));
}

inline void GeometryDirectionsAndLiteralBytes() {
    Fixture fixture;
    const auto settings = Settings(fixture);
    const std::array<TerrainCanvasRectangle, 4> directions{{{-32, 0, 32, 32}, {0, -32, 32, 32},
        {0, 0, 64, 32}, {0, 0, 32, 64}}};
    for (const auto& rectangle : directions) {
        const auto projected = Take(ProjectTerrainResizeGeometry(fixture.grid, settings, Margins, Rules(), {1, rectangle}));
        Require(Take(SerializeMapTerrain(projected.terrain)) == TerrainLiteral(rectangle.west, rectangle.south, rectangle.east, rectangle.north),
            "four independent expansions match complete independent W3E literal including edge variation");
        Require(std::bit_cast<std::array<std::uint32_t, 8>>(projected.settings.cameraBounds)
            == std::bit_cast<std::array<std::uint32_t, 8>>(settings.cameraBounds), "camera F32 bits remain exact");
        const auto& h = projected.terrain.header;
        const auto playable = Take(ComputeTerrainNativePlayableBounds(h, projected.settings, Margins));
        Require(playable.left == -1920 && playable.bottom == -1920 && playable.right == 1792 && playable.top == 1792,
            "playable world extent is invariant across four directions");
        Require(projected.settings.playableWidth == 29 && projected.settings.playableHeight == 29,
            "playable dimensions do not grow with canvas");
        const auto crop = Intent(-rectangle.west, -rectangle.south, 32 - rectangle.west, 32 - rectangle.south);
        const auto restored = Take(ProjectTerrainResizeGeometry(projected.terrain, projected.settings, Margins, Rules(), crop));
        Require(Take(SerializeMapTerrain(restored.terrain)) == TerrainLiteral(), "cropping added border restores original full W3E");
        Require(restored.settings.cameraMargins == settings.cameraMargins, "crop restores original relative margins");
        Require(projected.preparationBytes > fixture.grid.vertices.size() * sizeof(TerrainVertex)
            && projected.preparationWork > projected.terrain.vertices.size(), "complete preparation cost includes retained input and work");
    }
    auto unknown = fixture.grid;
    unknown.vertices[0].groundHeightRaw |= 0xc000u;
    unknown.vertices[0].waterHeightAndFlagsRaw |= 0x8000u;
    const auto projected = Take(ProjectTerrainResizeGeometry(unknown, settings, Margins, Rules(), Intent(-32, -32, 32, 32)));
    const auto& copy = projected.terrain.vertices[0];
    Require(copy.groundHeightRaw == unknown.vertices[0].groundHeightRaw
        && (copy.waterHeightAndFlagsRaw & 0x8000u) == 0x8000u && copy.variationRaw == unknown.vertices[0].variationRaw,
        "nearest edge copies unknown raw bits and deterministic variation");
    Require(Take(SerializeMapTerrain(fixture.grid)) == TerrainLiteral(), "pure projector never mutates source");
}

inline void GeometryRejectsInvalidDomainsAndBudgets() {
    Fixture fixture;
    const auto settings = Settings(fixture);
    const auto rejected = [&](const TerrainResizeIntent& intent, std::string_view code, const TerrainResizeLimits& limits = {}) {
        const auto result = ProjectTerrainResizeGeometry(fixture.grid, settings, Margins, Rules(), intent, limits);
        Require(!result.Succeeded() && HasCode({result.diagnostics}, code), "invalid resize is rejected by its exact gate");
    };
    rejected(Intent(0, 0, 31, 32), "TERRAIN_RESIZE_RULES");
    rejected(Intent(0, 0, 512, 32), "TERRAIN_RESIZE_RULES");
    rejected(Intent(0, 0, 288, 256), "TERRAIN_RESIZE_RULES");
    rejected(Intent(64, 0, 96, 32), "TERRAIN_RESIZE_OVERLAP");
    rejected(Intent(2, 0, 34, 32), "TERRAIN_RESIZE_PLAYABLE_CLIPPED");
    rejected(Intent(std::numeric_limits<int>::min(), 0, std::numeric_limits<int>::max(), 32), "TERRAIN_RESIZE_DIMENSION");
    rejected(Intent(0, 0, 0, 32), "TERRAIN_RESIZE_INTENT");
    rejected(Intent(-32, 0, 32, 32), "TERRAIN_RESIZE_BUDGET", {1, 16u * 1024u * 1024u});
    rejected(Intent(-32, 0, 32, 32), "TERRAIN_RESIZE_BUDGET", {256u * 1024u * 1024u, 1});
    auto bad = fixture.grid; bad.header.offset[0] += 1;
    Require(!ProjectTerrainResizeGeometry(bad, settings, Margins, Rules(), Intent(-32, 0, 32, 32)).Succeeded(),
        "unaligned origin cannot be silently rounded");
    bad = fixture.grid; bad.vertices[0].extendedTextureLayout = true;
    Require(!ProjectTerrainResizeGeometry(bad, settings, Margins, Rules(), Intent(-32, 0, 32, 32)).Succeeded(),
        "extended vertex layout cannot enter classic resize");
    bad = fixture.grid; bad.tiles.pop_back();
    Require(!ProjectTerrainResizeGeometry(bad, settings, Margins, Rules(), Intent(-32, 0, 32, 32)).Succeeded(),
        "partial tile topology cannot be resized");
    auto noRules = Rules(); noRules.minimumSide = 0;
    Require(!ProjectTerrainResizeGeometry(fixture.grid, settings, Margins, noRules, Intent(-32, 0, 32, 32)).Succeeded(),
        "missing actual dimension values have no fallback");
    auto settingsWithPayload = settings; settingsWithPayload.description.assign(50000, 'x');
    Require(!ProjectTerrainResizeGeometry(fixture.grid, settingsWithPayload, Margins, Rules(), Intent(-32, 0, 32, 32),
        {100000, 16u * 1024u * 1024u}).Succeeded(), "actual variable settings payload participates in memory budget");
}

inline void AtomicResizeAndHistory() {
    Fixture fixture(true);
    const auto originalWpm = fixture.Member("war3map.wpm");
    const auto originalSettings = fixture.Member("war3map.w3i");
    const auto originalUnits = fixture.Member("war3mapUnits.doo"), originalDoodads = fixture.Member("war3map.doo");
    const auto sourceTerrain = fixture.edits->Terrain();
    const auto owner = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    const auto prepared = Take(fixture.edits->PrepareTerrainResizeEdit(owner, Intent(-32, 0, 32, 32)));
    Require(prepared.RetainedCommandBytes() > 0, "resize retains full reversible material");
    const auto revision = fixture.edits->Revision();
    const auto refused = fixture.edits->ApplyPreparedTerrainResizeEdit(prepared, [](const auto&) {
        return Status::Failure("Test", "TEST_DECLINED", "synthetic prepublish refusal");
    });
    Require(!refused.Succeeded() && fixture.edits->Revision() == revision && fixture.edits->Terrain() == sourceTerrain
        && !fixture.edits->CanUndo() && fixture.edits->Replacements().empty(), "rejected callback publishes no partial member or history");
    Check(fixture.edits->ApplyPreparedTerrainResizeEdit(prepared));
    Require(Grid(*fixture.edits) == TerrainLiteral(-32, 0, 32, 32), "atomic resize publishes full independent geometry");
    const auto shadow = Take(fixture.edits->TerrainResizeShadowDraft());
    Require(shadow.present && shadow.bytes == ShadowLiteral(-32, 0, 32, 32, true),
        "SHD nonzero disk bytes become FF, new outside cells are zero");
    const auto document = fixture.edits->Snapshot();
    Require(document.Member(MapDataKind::Units).sources.front().data == originalUnits
        && document.Member(MapDataKind::Doodads).sources.front().data == originalDoodads,
        "ordinary objects retain all original fields and world positions");
    Require(!fixture.edits->ApplyPreparedTerrainResizeEdit(prepared).Succeeded(), "old revision cannot reapply prepared resize");
    Require(fixture.edits->Undo(), "resize can undo");
    Require(Grid(*fixture.edits) == TerrainLiteral() && Wpm(*fixture.edits, originalWpm) == originalWpm,
        "undo restores complete original W3E and all WPM bits");
    Require(Take(fixture.edits->TerrainResizeShadowDraft()).bytes == ShadowLiteral(), "undo restores exact disk SHD including 02/80");
    Require(fixture.edits->Snapshot().Member(MapDataKind::Settings).sources.front().data == originalSettings,
        "undo restores complete original W3I");
    Require(!fixture.edits->IsDirty() && !fixture.edits->HasTerrainResizeChanges(), "undo returns to genuine original resize identity");
    Require(fixture.edits->Redo() && Grid(*fixture.edits) == TerrainLiteral(-32, 0, 32, 32), "redo restores full resized geometry");
    const auto after = fixture.edits->Terrain();
    const auto owner2 = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    const auto prepared2 = Take(fixture.edits->PrepareTerrainResizeEdit(owner2, Intent(32, 0, 64, 32)));
    const auto innerName = std::string("inner successful edit");
    const auto reentered = fixture.edits->ApplyPreparedTerrainResizeEdit(prepared2, [&](const auto&) {
        auto changed = *fixture.edits->Snapshot().Settings(); changed.mapName = innerName;
        return fixture.edits->EditSettings(changed);
    });
    Require(!reentered.Succeeded() && HasCode(reentered, "MAP_EDIT_PREPUBLISH_STALE") && fixture.edits->Terrain() == after
        && fixture.edits->Snapshot().Settings()->mapName == innerName, "reentrant callback keeps inner edit and rejects stale outer resize");
    auto other = fixture.FreshSession();
    Require(!other->PrepareTerrainResizeEdit(owner, Intent(-32, 0, 32, 32)).Succeeded(), "actual owner cannot authorize another session");
    Check(IO::WriteFileBytes(fixture.gameRoot / "WorldEditData.txt", Text(
        "[WorldEditMisc]\nMinimumMapSize=32\nMaximumMapSize=448\n[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n")));
    Require(!owner2.VerifySources(*fixture.archive, *fixture.edits).Succeeded(), "startup source change invalidates owned resize provenance");
}

inline void AbsentShadowAndBudgetCancellation() {
    Fixture fixture(false, false);
    Resize(fixture, Intent(0, -32, 32, 32));
    const auto shadow = Take(fixture.edits->TerrainResizeShadowDraft());
    Require(!shadow.present && shadow.bytes.empty(), "missing SHD stays explicitly missing");
    const auto replacements = fixture.edits->Replacements();
    Require(std::none_of(replacements.begin(), replacements.end(), [](const auto& item) { return item.name == "war3map.shd"; }),
        "resize never invents a missing SHD member");
    const auto measured = Take(fixture.edits->MeasureTerrainJournalDraft(64u * 1024u * 1024u, 64u * 1024u * 1024u));
    Require(measured.retainedBytes > Grid(*fixture.edits).size() && measured.comparisonWork > 1000,
        "full grids and resize layers are counted before raw comparison");
    Require(!fixture.edits->MeasureTerrainJournalDraft(measured.retainedBytes - 1, 64u * 1024u * 1024u).Succeeded(),
        "retained resize budget can be tightened by caller");
    Require(!fixture.edits->FreezeForSave(1024).Succeeded(), "freeze cannot evade retained resize material budget");
}

inline void ResizeCannotLaunderRawUnknownFields() {
    const auto rejected = [](unsigned mutation, bool afterResize) {
        Fixture fixture;
        if (afterResize) Resize(fixture, Intent(-32, 0, 32, 32));
        auto changed = *fixture.edits->Terrain();
        if (mutation == 0) changed.header.mainTileset = 'A';
        else if (mutation == 1) changed.vertices[changed.header.width + 1].waterHeightAndFlagsRaw ^= 0x8000u;
        else changed.vertices[changed.header.width + 1].groundHeightRaw ^= 0x4000u;
        Check(fixture.edits->SetTerrain(Take(SerializeMapTerrain(changed))));
        const auto revision = fixture.edits->Revision(); const auto before = fixture.edits->Terrain();
        const auto owner = TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot);
        // 主地形改变可能已在实际资源读取时拒绝；能捕获的未知raw位必须在Core准备门拒绝。
        if (owner.Succeeded() && owner.value) {
            const auto prepared = fixture.edits->PrepareTerrainResizeEdit(*owner.value, Intent(0, -32,
                int(changed.header.width - 1), int(changed.header.height - 1)));
            Require(!prepared.Succeeded() && HasCode({prepared.diagnostics}, "TERRAIN_RESIZE_RAW_INPUT"),
                "raw unknown fields never become trusted through a later resize");
        } else Require(!owner.Succeeded(), "unavailable actual source must not create a resize owner");
        Require(fixture.edits->Revision() == revision && fixture.edits->Terrain() == before,
            "failed raw resize leaves current draft and history revision unchanged");
    };
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        rejected(mutation, false); rejected(mutation, true);
    }
    Fixture legitimate;
    TerrainBrush height; height.kind = TerrainBrushKind::Raise; height.radius = 0; height.strength = 16;
    const TerrainStrokePoint center{0, 0};
    Take(legitimate.edits->EditTerrain(height, std::span(&center, 1), false));
    Resize(legitimate, Intent(-32, 0, 32, 32));
    const auto& moved = legitimate.edits->Terrain()->vertices[16 * 65 + 48];
    Require(moved.groundHeightRaw == 8256, "legitimate known-bit height edit survives exact nearest-edge resize");
}

inline void TestTerrainResizeGeometryCases() {
    GeometryDirectionsAndLiteralBytes();
    GeometryRejectsInvalidDomainsAndBudgets();
    AtomicResizeAndHistory();
    AbsentShadowAndBudgetCancellation();
    ResizeCannotLaunderRawUnknownFields();
}
} // namespace AudastraTest::TerrainResizeCases
