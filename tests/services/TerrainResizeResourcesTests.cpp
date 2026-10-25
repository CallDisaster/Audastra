#include "../support/TerrainResizeFixture.h"
#include "TerrainSessionResourcesCases.h"
#include "TerrainHistorySourceShadowCases.h"
#include "TerrainOrdinaryShadowHistoryCases.h"
#include "TerrainHistoryShadowReadBudgetCases.h"

#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <iterator>

namespace {
using namespace AudastraTest::TerrainResize;
using namespace War3Map::Services;

TerrainResizeIntent Expand() { return {1, {-32, -32, 32, 32}}; }
TerrainResizeIntent CropBack() { return {1, {32, 32, 64, 64}}; }

MapDataQueryBinding Document(const Fixture& fixture) {
    return {"synthetic-canvas", 3, 0, fixture.archive->SourceSha256()};
}
TerrainEditBinding Binding(const Fixture& fixture) {
    return {Document(fixture), fixture.edits->SessionIdentity(), fixture.edits->Revision(), fixture.edits->TerrainSourceRevision()};
}
void Apply(Fixture& fixture, const TerrainResizeIntent& intent) {
    const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    const auto prepared = Take(PrepareTerrainResizeEdit(*fixture.edits, Document(fixture), Binding(fixture), resources, intent));
    Check(resources.VerifySources(*fixture.archive, *fixture.edits));
    Check(ApplyTerrainResizeEdit(*fixture.edits, Document(fixture), prepared));
}

void ActualStartupAndAtomicCas() {
    Fixture fixture(true);
    Check(IO::WriteFileBytes(fixture.gameRoot / "WorldEditData.txt", AudastraTest::TerrainBounds::Text(
        "[WorldEditMisc]\nMinimumMapSize=32\nMinimumMapSize=64\nMaximumMapSize=480\n"
        "[CameraMargins]\nLeft=1\nLeft=9\nRight=2\nBottom=1\nTop=2\n")));
    auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    Require(resources.DimensionRules().minimumSide == 32 && resources.DimensionRules().maximumSide == 480
        && resources.DimensionRules().sideStep == 32 && resources.DimensionRules().maximumArea == 256u * 256u
        && resources.CameraMargins() == std::array<float, 4>{1, 2, 1, 2}, "actual append-first startup rules and explicit expansion product area");
    Require(resources.SourceShadow().present && resources.SourceShadow().bytes == ShadowLiteral(), "physical raw SHD is retained, including 02 and 80");
    Require(resources.CaptureWork() >= fixture.edits->Terrain()->vertices.size()
        && resources.CaptureWork() != resources.OwnedCapacityBytes(), "capture reports its actual work independently of owned memory capacity");
    TerrainResizeLimits captureLimits;
    captureLimits.maximumWork = resources.CaptureWork();
    Require(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot, captureLimits).Succeeded(),
        "capture succeeds at its reported complete work budget");
    --captureLimits.maximumWork;
    Require(!TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot, captureLimits).Succeeded(),
        "capture rejects a budget one unit below its reported complete work");
    Check(resources.VerifySourceInputs(*fixture.archive));
    TerrainResizeLimits verifyLimits;
    verifyLimits.maximumCandidateBytes = resources.CaptureBudgetBytes() - 1;
    const auto limitedVerify = resources.VerifySourceInputs(*fixture.archive, verifyLimits);
    Require(!limitedVerify.Succeeded() && HasCode(limitedVerify, "TERRAIN_RESIZE_BUDGET"),
        "64MiB exact-read cap preserves the separate complete owner candidate budget");
    const auto before = fixture.edits->Revision();
    const auto prepared = Take(PrepareTerrainResizeEdit(*fixture.edits, Document(fixture), Binding(fixture), resources, Expand()));
    const auto refused = ApplyTerrainResizeEdit(*fixture.edits, Document(fixture), prepared, [](const auto&) {
        return Status::Failure("Test", "SYNTHETIC_PREPUBLISH_REJECT", "test rejection");
    });
    Require(!refused.Succeeded() && fixture.edits->Revision() == before && !fixture.edits->CanUndo()
        && Take(SerializeMapTerrain(*fixture.edits->Terrain())) == TerrainLiteral(), "prepublish rejection publishes no terrain or partial metadata");
    Check(ApplyTerrainResizeEdit(*fixture.edits, Document(fixture), prepared));
    Require(!ApplyTerrainResizeEdit(*fixture.edits, Document(fixture), prepared).Succeeded()
        && !resources.VerifySources(*fixture.archive, *fixture.edits).Succeeded(), "owner and sealed command keep original session/revision CAS");
    Require(Take(SerializeMapTerrain(*fixture.edits->Terrain())) == TerrainLiteral(-32, -32, 32, 32), "full independent expanded W3E literal");
    Require(Take(fixture.edits->TerrainResizeShadowDraft()).bytes == ShadowLiteral(-32, -32, 32, 32, true), "nonzero disk SHD migrates as native FF with zero expansion");
    Require(fixture.edits->Undo() && Take(SerializeMapTerrain(*fixture.edits->Terrain())) == TerrainLiteral()
        && Take(fixture.edits->TerrainResizeShadowDraft()).bytes == ShadowLiteral(), "one undo restores old complete raw SHD and W3E");
    Require(fixture.edits->Redo(), "one redo restores complete typed resize");
}

void ChangedPhysicalSpatialMembersAndRulesReject() {
    for (const auto name : {std::string_view("war3map.w3r"), std::string_view("war3map.w3c")}) {
        for (const bool originallyMissing : {false, true}) {
            Fixture fixture;
            if (originallyMissing) { fixture.EraseMember(name); fixture.Seal(); }
            auto oldSession = std::move(fixture.edits);
            const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *oldSession, fixture.gameRoot));
            if (name.ends_with("w3r")) {
                RegionRecord region{}; region.left = -64; region.bottom = -32; region.right = 64; region.top = 32;
                region.name = "new region"; region.creationNumber = 91; fixture.regions.entries.push_back(region);
                fixture.PutMember(name, Take(fixture.regions.Serialize()));
            } else {
                CameraRecord camera{}; camera.name = "new camera"; camera.targetX = 32; camera.targetY = 64;
                fixture.cameras.entries.push_back(camera); fixture.PutMember(name, Take(fixture.cameras.Serialize()));
            }
            fixture.Seal();
            const auto changed = resources.VerifySources(*fixture.archive, *oldSession);
            Require(!changed.Succeeded() && HasCode(changed, "TERRAIN_RESIZE_SOURCE_CHANGED"), "actual W3R/W3C content and absence-to-presence are source-bound");
            Require(!TerrainResizeEditService::Capture(*fixture.archive, *oldSession, fixture.gameRoot).Succeeded(), "another map cannot supply same-size spatial sources");
        }
    }
    Fixture fixture;
    const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    Check(IO::WriteFileBytes(fixture.gameRoot / "WorldEditData.txt", AudastraTest::TerrainBounds::Text(
        "[WorldEditMisc]\nMinimumMapSize=32\nMaximumMapSize=448\n"
        "[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n")));
    Require(!resources.VerifySources(*fixture.archive, *fixture.edits).Succeeded(), "changed actual startup winner is not a valid old owner");
    TerrainResizeLimits limits; limits.maximumCandidateBytes = 1;
    Require(!TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot, limits).Succeeded(), "capture cannot widen tiny candidate budget");
    limits = {}; limits.maximumWork = 1;
    Require(!TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot, limits).Succeeded(), "full real sources respect work budget before partial owner");
}

void MakeExpandedPhysicalFixture(Fixture& fixture) {
    fixture.grid = *Take(ParseMapTerrain(TerrainLiteral(-32, -32, 32, 32))).Snapshot();
    fixture.settings.cameraMargins = {33, 2, 33, 2};
    fixture.PutMember("war3map.w3e", TerrainLiteral(-32, -32, 32, 32));
    fixture.PutMember("war3map.wpm", PathingLiteral(-32, -32, 32, 32));
    fixture.PutMember("war3map.shd", ShadowLiteral(-32, -32, 32, 32, true));
    fixture.PutMember("war3map.w3i", Take(fixture.settings.Serialize()));
}

void CropCannotClipAnyActualSpatialClass() {
    for (unsigned kind = 0; kind < 5; ++kind) {
        Fixture fixture(true); MakeExpandedPhysicalFixture(fixture);
        if (kind == 0) { fixture.units.units[0].position[0] = -3000; fixture.PutMember("war3mapUnits.doo", Take(fixture.units.Serialize())); }
        if (kind == 1) { fixture.doodads.doodads[0].position[0] = -3000; fixture.PutMember("war3map.doo", Take(fixture.doodads.Serialize())); }
        if (kind == 2) { fixture.regions.entries[0].left = -3000; fixture.PutMember("war3map.w3r", Take(fixture.regions.Serialize())); }
        if (kind == 3) { fixture.cameras.entries[0].targetX = -3000; fixture.PutMember("war3map.w3c", Take(fixture.cameras.Serialize())); }
        if (kind == 4) {
            MapPlayerSettings player{}; player.number = 0; player.controller = 1; player.race = 1; player.startX = -3000; player.startY = 0;
            fixture.settings.players.push_back(player); fixture.PutMember("war3map.w3i", Take(fixture.settings.Serialize()));
        }
        fixture.Seal();
        const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
        const auto prepared = PrepareTerrainResizeEdit(*fixture.edits, Document(fixture), Binding(fixture), resources, CropBack());
        Require(!prepared.Succeeded() && HasCode({prepared.diagnostics}, "TERRAIN_RESIZE_SPATIAL_CLIPPED")
            && !fixture.edits->CanUndo(), "unit/doodad/region/camera/start world position cannot be silently clipped");
    }
}

void ActualZeroRgbFootprintAndEmptySpecialStillHaveExtent() {
    {
        Fixture fixture(true); MakeExpandedPhysicalFixture(fixture);
        fixture.doodads.doodads[0].rawcode = "Doo0";
        fixture.doodads.doodads[0].position = {-2016, 0, 0};
        fixture.PutMember("war3map.doo", Take(fixture.doodads.Serialize()));
        fixture.PutMember("Doodads/Doodads.slk", Slk({"doodID", "code", "category", "fixedRot", "pathTex"},
            {{"Doo0", "Doo0", "A", "-1", "PathTextures/zero-border.tga"}}));
        Bytes tga(18 + 16 * 16 * 3); tga[2] = std::byte{2}; tga[12] = tga[14] = std::byte{16};
        tga[16] = std::byte{24}; tga[17] = std::byte{32}; fixture.PutMember("PathTextures/zero-border.tga", std::move(tga));
        fixture.Seal();
        const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
        const auto prepared = PrepareTerrainResizeEdit(*fixture.edits, Document(fixture), Binding(fixture), resources, CropBack());
        if (!prepared.Succeeded() && !HasCode({prepared.diagnostics}, "TERRAIN_RESIZE_FOOTPRINT_CLIPPED"))
            Check({prepared.diagnostics});
        Require(!prepared.Succeeded() && HasCode({prepared.diagnostics}, "TERRAIN_RESIZE_FOOTPRINT_CLIPPED"),
            "actual decoded zero-RGB footprint is not discarded by its empty OR mask");
        fixture.EraseMember("PathTextures/zero-border.tga"); fixture.Seal();
        const auto sourceSha = fixture.archive->SourceSha256();
        const auto revision = fixture.edits->Revision();
        const auto terrain = fixture.edits->Terrain();
        const auto missing = TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot);
        // TGA缺席后，BLP为必需回退成员；严格游戏读取器在调用visitor前
        // 原样返回Game.DataSource/file-not-found，不会改写成对象层诊断。
        const bool missingBlp = std::any_of(missing.diagnostics.begin(), missing.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.subsystem == "Game.DataSource" && diagnostic.code == "file-not-found"
                && diagnostic.message.find("pathtextures/zero-border.blp") != std::string::npos;
        });
        if (!missing.Succeeded() && !missingBlp)
            Check({missing.diagnostics});
        Require(!missing.Succeeded() && !missing.value && missingBlp,
            "ordinary doodad zero-RGB extent comes from an actual decoded TGA, not a fabricated rectangle");
        Require(Take(Sha256File(fixture.archive->SourcePath())) == sourceSha
            && fixture.edits->Revision() == revision && fixture.edits->Terrain() == terrain && !fixture.edits->CanUndo(),
            "missing required footprint returns no owner and leaves the physical source and current session unchanged");
    }
    {
        Fixture fixture; MakeExpandedPhysicalFixture(fixture);
        fixture.doodads.specialDoodads.push_back({"Doo0", 0, {24, 40}});
        fixture.PutMember("war3map.doo", Take(fixture.doodads.Serialize()));
        fixture.PutMember("Doodads/Doodads.slk", Slk({"doodID", "code", "pathTex"}, {{"Doo0", "Doo0", "_"}}));
        fixture.Seal();
        const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
        Require(resources.SpecialShapes().size() == 1 && !resources.SpecialShapes()[0].hasFootprint,
            "actual empty special has a proven origin, without invented footprint bounds");
        const auto prepared = PrepareTerrainResizeEdit(*fixture.edits, Document(fixture), Binding(fixture), resources, CropBack());
        Require(!prepared.Succeeded() && HasCode({prepared.diagnostics}, "TERRAIN_RESIZE_SPECIAL_CLIPPED"),
            "empty special origin cannot be silently clipped");
    }
}

void NonWalkableDestructableKeepsProvenZeroContribution() {
    Fixture fixture(true); MakeExpandedPhysicalFixture(fixture);
    fixture.doodads.doodads[0].position = {-2016, 0, 0};
    fixture.PutMember("war3map.doo", Take(fixture.doodads.Serialize()));
    // 现有原版规则中，非walkable可破坏物不注册静态RGB层；pathTex单元格
    // 不授予普通Doodad资格。故意不给这个名字实际贴图，不能靠猜测宽高裁图。
    fixture.PutMember("Units/DestructableData.slk", Slk({"destructableID", "code", "walkable", "pathTex"},
        {{"Dst0", "Dst0", "0", "PathTextures/unread-zero-contribution.tga"}}));
    fixture.Seal();
    const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    const auto retained = PrepareTerrainResizeEdit(*fixture.edits, Document(fixture), Binding(fixture), resources, CropBack());
    if (!retained.Succeeded()) Check({retained.diagnostics});
    Require(retained.Succeeded(),
        "actual nonwalkable destructable remains zero static contribution and keeps its in-bounds origin");
    fixture.doodads.doodads[0].position[0] = -2112;
    fixture.PutMember("war3map.doo", Take(fixture.doodads.Serialize())); fixture.Seal();
    const auto outside = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    const auto clipped = PrepareTerrainResizeEdit(*fixture.edits, Document(fixture), Binding(fixture), outside, CropBack());
    Require(!clipped.Succeeded() && HasCode({clipped.diagnostics}, "TERRAIN_RESIZE_SPATIAL_CLIPPED"),
        "zero static contribution still cannot hide a nonwalkable destructable origin outside the canvas");
}

void FootprintQueryKeepsZeroBorderAndLiteralOrientation() {
    Bytes pixels;
    for (const auto pixel : std::array<std::array<unsigned, 3>, 6>{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1}}})
        for (const auto channel : pixel) pixels.push_back(std::byte(channel));
    TerrainDoodadFootprint footprint{3, 2, pixels, 3, true, false, 0};
    TerrainHeader header; header.width = header.height = 5;
    DoodadPlacement placement; placement.rawcode = "Dst0"; placement.position = {128, 128, 0}; placement.scale = {1, 1, 1};
    constexpr std::array<std::array<std::uint8_t, 6>, 4> expected{{
        {0, 0x31, 4, 6, 2, 0x35}, {2, 4, 0, 0x35, 6, 0x31},
        {0x35, 2, 6, 4, 0x31, 0}, {0x31, 6, 0x35, 0, 4, 2}}};
    constexpr std::array<float, 4> angles{0, 1.57079637f, 3.14159274f, 4.712389f};
    for (unsigned turn = 0; turn < 4; ++turn) {
        placement.rotation = angles[turn];
        const auto rectangle = Take(QueryDoodadFootprintRectangle(header, placement, footprint, 2, -1.0f));
        const auto x0 = turn % 2 ? 3 : 4, y0 = turn % 2 ? 4 : 3, width = turn % 2 ? 3 : 2, height = turn % 2 ? 2 : 3;
        Require(rectangle == std::array<std::int64_t, 4>{x0, y0, x0 + width, y0 + height}, "full projection retains zero RGB leading border");
        std::vector<std::uint8_t> actual(256, 0x40), literal(256, 0x40);
        Check(AddDoodadFootprint(actual, header, placement, footprint, 2, -1.0f));
        for (unsigned i = 0; i < 6; ++i) literal[(y0 + i / width) * 16 + x0 + i % width] |= expected[turn][i];
        Require(actual == literal, "extracted projection preserves every original Add pixel and untouched cell");
    }
    Bytes empty(16 * 16 * 3);
    footprint = {16, 16, empty, 3, true, true, 0}; placement.position = {0, 0, 0}; placement.rotation = 0;
    Require(Take(QueryDoodadFootprintRectangle(header, placement, footprint, 2, {})) == std::array<std::int64_t, 4>{-7, -7, 9, 9},
        "all-zero footprint retains unclipped full authored extent");
}

void SaveAndUndoKeepEveryActualMember(bool shadow) {
    Fixture fixture(true, shadow);
    const auto sourceSha = fixture.archive->SourceSha256(); const auto sourcePath = fixture.archive->SourcePath();
    unsigned saveOrdinal = 0;
    const auto save = [&](const char* name, const Bytes& terrain, const Bytes& pathing, const Bytes& expectedShadow,
        const std::array<std::int32_t, 4>& expectedMargins) {
        ++saveOrdinal;
        auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.archive, *fixture.edits, fixture.gameRoot));
        Check(plan.SaveGate());
        auto replacements = fixture.edits->Replacements(); const auto removals = fixture.edits->Removals();
        Check(plan.InstallPathingOutput(*fixture.edits, replacements));
        Check(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), replacements, removals));
        Check(plan.VerifySourceInputs(*fixture.archive));
        auto actual = Take(MapArchiveSession::Open(Take(fixture.archive->SaveCandidate(fixture.directory / name, replacements, removals)).path));
        Check(plan.VerifyReadback(actual));
        Require(Take(actual.ReadMember("war3map.w3e")) == terrain && Take(actual.ReadMember("war3map.wpm")) == pathing,
            "actual save/reopen matches independent full W3E and WPM literals");
        auto expectedSettings = fixture.settings; expectedSettings.cameraMargins = expectedMargins;
        Require(Take(actual.ReadMember("war3map.w3i")) == Take(expectedSettings.Serialize()),
            "actual W3I changes only the independently expected four relative canvas margins");
        const auto savedDocument = MapDocument::FromArchive(actual);
        const auto savedTerrain = Take(ParseMapTerrain(Take(actual.ReadMember("war3map.w3e")))).Snapshot();
        const auto& savedSettings = savedDocument.Settings();
        Require(savedSettings && savedTerrain
            && double(savedTerrain->header.offset[0]) + savedSettings->cameraMargins[0] * 128.0 == -1920
            && double(savedTerrain->header.offset[1]) + savedSettings->cameraMargins[2] * 128.0 == -1920
            && double(savedTerrain->header.offset[0]) + (savedTerrain->header.width - 1 - savedSettings->cameraMargins[1]) * 128.0 == 1792
            && double(savedTerrain->header.offset[1]) + (savedTerrain->header.height - 1 - savedSettings->cameraMargins[3]) * 128.0 == 1792,
            "actual saved playable world rectangle is unchanged by expand, undo, redo and crop");
        const std::array<std::string, 1> names{"war3map.shd"}; const auto members = Take(actual.CaptureExactMembers(names));
        if (members[0].present != shadow || members[0].bytes != expectedShadow) {
            const auto& actualShadow = members[0].bytes;
            std::cerr << "phase=shadow-readback saveOrdinal=" << saveOrdinal << " file=" << name
                << " shadow=" << shadow << " expectedPresent=" << shadow << " actualPresent=" << members[0].present
                << " expectedBytes=" << expectedShadow.size() << " actualBytes=" << actualShadow.size();
            const auto mismatch = std::mismatch(expectedShadow.begin(), expectedShadow.end(), actualShadow.begin(), actualShadow.end());
            if (mismatch.first != expectedShadow.end() || mismatch.second != actualShadow.end()) {
                std::cerr << " firstDiff=" << std::distance(expectedShadow.begin(), mismatch.first) << " expectedByte=";
                if (mismatch.first != expectedShadow.end()) std::cerr << std::to_integer<unsigned>(*mismatch.first);
                else std::cerr << "<end>";
                std::cerr << " actualByte=";
                if (mismatch.second != actualShadow.end()) std::cerr << std::to_integer<unsigned>(*mismatch.second);
                else std::cerr << "<end>";
            } else std::cerr << " firstDiff=none";
            std::cerr << '\n';
        }
        Require(members[0].present == shadow && members[0].bytes == expectedShadow, "SHD actual presence and complete migration bytes match; absence stays absence");
        for (const auto& member : fixture.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
            && member.name != "war3map.w3i" && member.name != "war3map.shd")
            Require(Take(actual.ReadMember(member.name)) == member.data, "ordinary world objects, W3R/W3C, script, TGA and actual resources remain exact");
        Check(plan.CommitSaved(*fixture.edits, savedDocument));
        Require(!fixture.edits->IsDirty(), "save commits all current layers and actual SHD as one new baseline");
        fixture.archive = std::make_unique<MapArchiveSession>(std::move(actual));
    };
    Apply(fixture, Expand());
    save("expanded.w3x", TerrainLiteral(-32, -32, 32, 32), PathingLiteral(-32, -32, 32, 32),
        shadow ? ShadowLiteral(-32, -32, 32, 32, true) : Bytes{}, {33, 2, 33, 2});
    Require(fixture.edits->Undo(), "cross-save hot undo restores original shape");
    const auto undoneShadow = Take(fixture.edits->TerrainResizeShadowDraft());
    Require(undoneShadow.present == shadow && undoneShadow.bytes == (shadow ? ShadowLiteral() : Bytes{}),
        "cross-save hot undo retains the original raw shadow bytes or physical absence before writing");
    // 热Undo保留原字；跨保存站的实际resize写盘仍遵守原版非零转FF规则。
    save("undone.w3x", TerrainLiteral(), PathingLiteral(), shadow ? ShadowLiteral(0, 0, 32, 32, true) : Bytes{}, {1, 2, 1, 2});
    Require(fixture.edits->Redo(), "cross-save hot redo restores expanded shape");
    save("redone.w3x", TerrainLiteral(-32, -32, 32, 32), PathingLiteral(-32, -32, 32, 32),
        shadow ? ShadowLiteral(-32, -32, 32, 32, true) : Bytes{}, {33, 2, 33, 2});
    Apply(fixture, CropBack());
    save("cropped.w3x", TerrainLiteral(), PathingLiteral(), shadow ? ShadowLiteral(0, 0, 32, 32, true) : Bytes{}, {1, 2, 1, 2});
    Require(Take(Sha256File(sourcePath)) == sourceSha, "all save-as and undo cases leave original synthetic source archive read-only");
}

void FinalSpatialOutputAndManualShadowConflictsReject() {
    Fixture fixture(true); Apply(fixture, Expand());
    auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.archive, *fixture.edits, fixture.gameRoot)); Check(plan.SaveGate());
    auto original = fixture.edits->Replacements();
    auto conflict = original;
    const auto shd = std::find_if(conflict.begin(), conflict.end(), [](const auto& item) { return item.name == "war3map.shd"; });
    Require(shd != conflict.end(), "typed SHD migration participates in ordinary save list");
    shd->data.front() ^= std::byte{1}; const auto before = conflict;
    Require(!plan.InstallPathingOutput(*fixture.edits, conflict).Succeeded()
        && conflict.size() == before.size() && std::equal(conflict.begin(), conflict.end(), before.begin(),
            [](const auto& a, const auto& b) { return a.name == b.name && a.data == b.data; }), "conflicting SHD cannot partially install WPM");
    Check(plan.InstallPathingOutput(*fixture.edits, original));
    for (const auto name : {"war3map.w3r", "war3map.w3c"}) {
        conflict = original; conflict.push_back({name, AudastraTest::TerrainBounds::Text("other bytes")});
        Require(!plan.VerifyFrozenOutput(fixture.archive->SourceSha256(), conflict).Succeeded(), "frozen output rejects swapped actual W3R/W3C");
    }
    conflict = original;
    auto wrongW3i = fixture.Member("war3map.w3i"); wrongW3i.back() ^= std::byte{1}; conflict.push_back({"WAR3MAP.W3I", std::move(wrongW3i)});
    Require(!TerrainPathingSave::PrepareForSave(*fixture.archive, *fixture.edits, fixture.gameRoot, {}, conflict).Succeeded(),
        "final supplied view cannot launder duplicate or incorrect typed W3I");
}
} // namespace

int main() {
    unsigned passed = 0, failed = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            test();
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": non-standard exception\n";
        }
    };
    run("ActualStartupAndAtomicCas", ActualStartupAndAtomicCas);
    run("ChangedPhysicalSpatialMembersAndRulesReject", ChangedPhysicalSpatialMembersAndRulesReject);
    run("CropCannotClipAnyActualSpatialClass", CropCannotClipAnyActualSpatialClass);
    run("ActualZeroRgbFootprintAndEmptySpecialStillHaveExtent", ActualZeroRgbFootprintAndEmptySpecialStillHaveExtent);
    run("NonWalkableDestructableKeepsProvenZeroContribution", NonWalkableDestructableKeepsProvenZeroContribution);
    run("FootprintQueryKeepsZeroBorderAndLiteralOrientation", FootprintQueryKeepsZeroBorderAndLiteralOrientation);
    run("SaveAndUndoKeepEveryActualMember(true)", [] { SaveAndUndoKeepEveryActualMember(true); });
    run("SaveAndUndoKeepEveryActualMember(false)", [] { SaveAndUndoKeepEveryActualMember(false); });
    run("FinalSpatialOutputAndManualShadowConflictsReject", FinalSpatialOutputAndManualShadowConflictsReject);
    run("TerrainSessionResources::ActualCurrentWaterAndBothNativeDoodadStages",
        AudastraTest::TerrainSessionResourcesCases::ActualCurrentWaterAndBothNativeDoodadStages);
    run("TerrainSessionResources::SharedFrozenBudgetsIncludeRetainedRequestAndDirectory",
        AudastraTest::TerrainSessionResourcesCases::SharedFrozenBudgetsIncludeRetainedRequestAndDirectory);
    run("TerrainSessionResources::PhysicalChangesAndBudgetsStayFailClosed",
        AudastraTest::TerrainSessionResourcesCases::PhysicalChangesAndBudgetsStayFailClosed);
    run("TerrainSessionResources::SavedResizeUsesCurrentPaletteResources",
        AudastraTest::TerrainSessionResourcesCases::SavedResizeUsesCurrentPaletteResources);
    run("TerrainSessionResources::ResizeWaterBoundsSaveReadback",
        AudastraTest::TerrainSessionResourcesCases::ResizeWaterBoundsSaveReadback);
    run("TerrainHistorySourceShadow::ActualArchiveAndDirectoryBindingAreImmutableAndBudgeted",
        AudastraTest::TerrainHistorySourceShadowCases::ActualArchiveAndDirectoryBindingAreImmutableAndBudgeted);
    run("TerrainHistorySourceShadow::SourceDriftCorruptionAndIoNeverBecomeMissing",
        AudastraTest::TerrainHistorySourceShadowCases::SourceDriftCorruptionAndIoNeverBecomeMissing);
    run("TerrainHistorySourceShadow::RepeatedCleanSaveInheritsTheOldBindingWithoutReadingNewSource",
        AudastraTest::TerrainHistorySourceShadowCases::RepeatedCleanSaveInheritsTheOldBindingWithoutReadingNewSource);
    run("TerrainHistorySourceShadow::OrdinarySaveThenFirstResizeRestoresAndRecapturesColdHistory",
        AudastraTest::TerrainHistorySourceShadowCases::OrdinarySaveThenFirstResizeRestoresAndRecapturesColdHistory);
    run("TerrainOrdinaryShadowHistory::LegacyEnvelopesAndNewVersionDomainsAreStrict",
        AudastraTest::TerrainOrdinaryShadowHistoryCases::LegacyEnvelopesAndNewVersionDomainsAreStrict);
    run("TerrainOrdinaryShadowHistory::OrdinaryProjectColdOpenBeforeFirstResizeRetainsTheOldShadow",
        AudastraTest::TerrainOrdinaryShadowHistoryCases::OrdinaryProjectColdOpenBeforeFirstResizeRetainsTheOldShadow);
    run("TerrainOrdinaryShadowHistory::LegacyOrdinaryHistoryDoesNotAcquireMissingShadowMaterial",
        AudastraTest::TerrainOrdinaryShadowHistoryCases::LegacyOrdinaryHistoryDoesNotAcquireMissingShadowMaterial);
    run("TerrainHistoryShadowReadBudget::PhysicalShadowIsBoundedBeforeReadingAfterTheTenMembers",
        AudastraTest::TerrainHistoryShadowReadBudgetCases::PhysicalShadowIsBoundedBeforeReadingAfterTheTenMembers);
    std::cout << "TerrainResizeResources cases: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
