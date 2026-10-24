#include "TerrainCategoryDirectoryRealTests.h"

#include "War3MapCore/Game/NativeDoodadCategoryDirectory.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace MapData;
using namespace Services;
using namespace ObjectData;
constexpr std::size_t kBytes = 64u * 1024u * 1024u;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string message;
    for (const auto& item : result.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message.empty() ? "missing category source result" : message);
}

void Check(const Status& status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& item : status.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message);
}

void CheckCategoryOverrides(const MapArchiveSession& original, const MapEditSaveRequest& prototype,
    const DoodadPlacements& placements, const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    const TerrainDoodadFootprint& footprint, std::uint8_t categorySnap,
    const ObjectFieldSchema& schema, const StandardObjectCatalog& catalog) {
    const auto parent = placements.doodads.back().rawcode;
    const auto* standard = catalog.Find(ObjectType::Doodad, parent);
    Require(standard && standard->Field("category") == "C", "real category override parent is an actual C doodad");
    ObjectFieldContext context; context.rawcode = parent; context.baseCode = standard->Field("code");
    const auto field = schema.MatchSourceField(ObjectType::Doodad, "category", context);
    Require(field.definition && field.safeToEdit && field.sourceKey == "category" && !field.sourceIndex
        && field.definition->nativeValueType == NativeValueType::String && !field.definition->repeat && !field.definition->data
        && Text::LowerAscii(field.definition->sourceTable) == "doodaddata"
        && Text::LowerAscii(field.definition->valueType) == "doodadcategory",
        "actual category metadata has a unique editable String address without a test-only type substitution");
    const auto root = prototype.targetCandidate.parent_path();
    auto background = placements; background.doodads.pop_back();
    const std::array<MemberReplacement, 1> backgroundMember{{{"war3map.doo", Take(background.Serialize())}}};
    const auto backgroundArchive = Take(MapArchiveSession::Open(Take(original.SaveCandidate(
        root / "category-background.w3x", backgroundMember)).path));
    const auto backgroundRules = Take(TerrainPathingSave::CaptureNative(backgroundArchive, before, after, prototype.gameRoot, kBytes));
    const auto backgroundOld = Take(backgroundRules.GenerateNative(before, kBytes));
    const auto backgroundNew = Take(backgroundRules.GenerateNative(after, kBytes));
    struct Case { const char* name; const char* id; const char* value; bool custom; bool overrideCategory; std::uint8_t snap; };
    const std::array<Case, 5> cases{{
        {"original-category", "", "Eextra", false, true, 2},
        {"original-category-fallback", "", "_extra", false, true, parent[1] == 'C' ? categorySnap : std::uint8_t{2}},
        {"custom-category-inherited", "ZE01", "", true, false, categorySnap},
        {"custom-category-empty", "ZE01", "", true, true, 2},
        {"custom-category-prefix", "ZC01", "Eextra", true, true, 2}
    }};
    for (const auto& item : cases) {
        const auto directory = root / item.name;
        Require(std::filesystem::create_directory(directory), "exclusive actual category override directory");
        auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
        auto placed = placements;
        const std::string id = item.custom ? item.id : parent;
        if (item.custom) {
            Require(!catalog.Find(ObjectType::Doodad, id) && !catalog.Find(ObjectType::Destructable, id),
                "category custom fixture never shadows a standard object");
            Check(document.AddCustomObject(id, parent));
            placed.doodads.back().rawcode = id;
        } else Check(document.AddOriginalObject(parent));
        if (item.overrideCategory) Check(document.AddModification({ObjectType::Doodad, id}, field.modification,
            NativeValueType::String, std::string(item.value)));
        const std::array<MemberReplacement, 2> changes{{{"war3map.doo", Take(placed.Serialize())},
            {"war3map.w3d", Take(NativeObjectCodec(ObjectType::Doodad).Serialize(document))}}};
        const auto seed = Take(MapArchiveSession::Open(Take(original.SaveCandidate(directory / "seeded.w3x", changes)).path));
        const auto rules = Take(TerrainPathingSave::CaptureNative(seed, before, after, prototype.gameRoot, kBytes));
        const auto generatedOld = Take(rules.GenerateNative(before, kBytes));
        const auto generatedNew = Take(rules.GenerateNative(after, kBytes));
        auto expectedOld = backgroundOld, expectedNew = backgroundNew;
        std::vector<std::uint8_t> contribution(backgroundOld.size());
        Check(AddDoodadFootprint(contribution, before.header, placed.doodads.back(), footprint, item.snap, std::nullopt));
        Require(generatedOld.size() == contribution.size() && generatedNew.size() == contribution.size(),
            "actual category source keeps pathing dimensions");
        for (std::size_t i = 0; i < contribution.size(); ++i) {
            const auto encoded = static_cast<std::byte>(((contribution[i] & 7) << 1) | ((contribution[i] & 0x38) << 2));
            expectedOld[i] |= encoded; expectedNew[i] |= encoded;
        }
        Require(generatedOld == expectedOld && generatedNew == expectedNew,
            "actual metadata category and String inheritance select the expected footprint grid");
        auto oldWpm = Take(seed.ReadMember("war3map.wpm")), expectedWpm = oldWpm;
        Require(oldWpm.size() == contribution.size() + 16, "category override WPM has exact dimensions");
        for (std::size_t i = 0; i < contribution.size(); ++i) {
            oldWpm[i + 16] = (oldWpm[i + 16] & std::byte{0x11}) | expectedOld[i];
            expectedWpm[i + 16] = (expectedWpm[i + 16] & std::byte{0x11}) | expectedNew[i];
        }
        Require(oldWpm != expectedWpm, "each actual category override case changes water pathing");
        const std::array<MemberReplacement, 1> pathing{{{"war3map.wpm", oldWpm}}};
        const auto source = Take(MapArchiveSession::Open(Take(seed.SaveCandidate(directory / "source.w3x", pathing)).path));
        auto request = prototype; request.sourceMap = source.SourcePath(); request.expectedSourceSha256 = source.SourceSha256();
        request.targetCandidate = directory / "saved.w3x";
        const auto saved = Take(SaveMapEditCandidate(request));
        const auto readback = Take(MapArchiveSession::Open(saved.candidate));
        Require(saved.scriptPreserved && Take(readback.ReadMember("war3map.wpm")) == expectedWpm
            && Take(readback.ReadMember("war3map.w3e")) == Take(SerializeMapTerrain(after)),
            "actual category overrides survive public save with complete terrain and pathing readback");
        for (const auto& member : source.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp") continue;
            Require(Take(readback.ReadMember(member.name)) == member.data, "category override save preserves every unrelated member");
        }
        Check(rules.VerifyReadback(readback, kBytes));
        Require(Take(Sha256File(seed.SourcePath())) == seed.SourceSha256()
            && Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "category override save never mutates its sources");
        std::cout << "category_override_save_passed kind=" << item.name << " snap_cells=" << unsigned(item.snap)
            << " metadata_field=" << field.modification.fieldId << " source_sha256=" << source.SourceSha256()
            << " candidate_sha256=" << saved.candidateSha256 << " native_editor_verified=false game_verified=false\n";
    }
    Require(Take(Sha256File(backgroundArchive.SourcePath())) == backgroundArchive.SourceSha256()
        && Take(Sha256File(original.SourcePath())) == original.SourceSha256(), "category override background sources remain untouched");
}
} // namespace

void CheckRealCategoryDirectorySave(const MapArchiveSession& source,
    const MapEditSaveRequest& request, const DoodadPlacements& placements,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    std::span<const std::byte> footprintBytes, const ObjectFieldSchema& schema, const StandardObjectCatalog& catalog) {
    const auto directory = request.targetCandidate.parent_path();
    const auto categories = Take(Game::NativeDoodadCategoryDirectory::Capture(request.gameRoot,
        Game::NativeEditorStartupRules::Classic127aExpansion, kBytes));
    const auto snap = categories.CategoryCSnapCells();
    Require(snap == 2 || snap == 4, "actual startup source determines a supported C grid");
    auto shifted = placements;
    Require(shifted.doodads.size() >= 2, "category fixture has a final ordinary C placement");
    shifted.doodads.back().position[0] += 64;
    const auto footprint = Take(DecodeDoodadFootprintTga(footprintBytes));
    Require(footprint.uniform && footprint.width == footprint.height,
        "actual category fixture uses an unambiguous uniform square");
    const auto cells = std::size_t{before.header.width - 1} * (before.header.height - 1) * 16;
    std::vector<std::uint8_t> two(cells), four(cells), unshifted(cells);
    Check(AddDoodadFootprint(two, before.header, shifted.doodads.back(), footprint, 2, std::nullopt));
    Check(AddDoodadFootprint(four, before.header, shifted.doodads.back(), footprint, 4, std::nullopt));
    Check(AddDoodadFootprint(unshifted, before.header, placements.doodads.back(), footprint, snap, std::nullopt));
    Require(two != four && unshifted != (snap == 2 ? two : four),
        "shifted actual C placement distinguishes both grids and changes its previous footprint");
    // 原场景为整图深水，可能遮住对象的同类OR位。C区改为干地，只留
    // 本次水位笔刷的中心邻域有水，确保旧占地位移确实造成可测的WPM差异。
    auto dryBefore = before, dryAfter = after;
    for (std::size_t y = 0; y < before.header.height; ++y) for (std::size_t x = 0; x < before.header.width; ++x) {
        if (x >= 31 && x <= 32 && y >= 31 && y <= 32) continue;
        const auto i = y * before.header.width + x;
        dryBefore.vertices[i].textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x40u);
        dryAfter.vertices[i].textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x40u);
    }
    RefreshTerrainDerived(dryBefore, 0, 0, before.header.width, before.header.height);
    RefreshTerrainDerived(dryAfter, 0, 0, after.header.width, after.header.height);
    const auto dryBeforeBytes = Take(SerializeMapTerrain(dryBefore));
    const std::array<MemberReplacement, 1> dryTerrain{{{"war3map.w3e", dryBeforeBytes}}};
    const auto drySeed = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
        directory / "grid-dry-seeded.w3x", dryTerrain)).path));
    const auto dryRules = Take(TerrainPathingSave::CaptureNative(drySeed, dryBefore, dryAfter, request.gameRoot, kBytes));
    const auto baselineNative = Take(dryRules.GenerateNative(dryBefore, kBytes));
    auto originalWpm = Take(source.ReadMember("war3map.wpm"));
    Require(baselineNative.size() == cells && originalWpm.size() == cells + 16, "dry category fixture pathing dimensions");
    for (std::size_t i = 0; i < cells; ++i)
        originalWpm[i + 16] = (originalWpm[i + 16] & std::byte{0x11}) | baselineNative[i];
    const std::array<MemberReplacement, 1> dryPathing{{{"war3map.wpm", originalWpm}}};
    const auto baseline = Take(MapArchiveSession::Open(Take(drySeed.SaveCandidate(
        directory / "grid-dry-source.w3x", dryPathing)).path));
    const auto shiftedDoo = Take(shifted.Serialize());
    const std::array<MemberReplacement, 1> changed{{{"war3map.doo", shiftedDoo}}};
    const auto mismatched = Take(MapArchiveSession::Open(Take(baseline.SaveCandidate(
        directory / "shifted-grid-source.w3x", changed)).path));
    Require(Take(mismatched.ReadMember("war3map.wpm")) == originalWpm,
        "shifted source intentionally retains all previous WPM bytes");
    const auto rules = Take(TerrainPathingSave::CaptureNative(mismatched, dryBefore, dryAfter, request.gameRoot, kBytes));
    const auto oldNative = Take(rules.GenerateNative(dryBefore, kBytes));
    const auto newNative = Take(rules.GenerateNative(dryAfter, kBytes));
    Require(oldNative.size() == cells && newNative.size() == cells,
        "category save keeps complete native pathing dimensions");
    auto correctOldWpm = originalWpm, expectedWpm = originalWpm;
    for (std::size_t i = 0; i < cells; ++i) {
        correctOldWpm[i + 16] = (originalWpm[i + 16] & std::byte{0x11}) | oldNative[i];
        expectedWpm[i + 16] = (originalWpm[i + 16] & std::byte{0x11}) | newNative[i];
    }
    Require(correctOldWpm != originalWpm && expectedWpm != correctOldWpm,
        "real source C relocation and subsequent water edit both change pathing");
    auto blocked = request;
    blocked.sourceMap = mismatched.SourcePath(); blocked.expectedSourceSha256 = mismatched.SourceSha256();
    blocked.expectedTerrainSha256 = Take(Sha256(dryBeforeBytes));
    blocked.targetCandidate = directory / "grid-must-not-exist.w3x";
    const auto rejected = SaveMapEditCandidate(blocked);
    Require(!rejected.Succeeded() && !rejected.value
        && std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_PATHING_SOURCE_UNEXPLAINED";
        }), "known C grid must still reject a source with unexplained old WPM");
    Require(!std::filesystem::exists(blocked.targetCandidate)
        && Take(Sha256File(mismatched.SourcePath())) == mismatched.SourceSha256()
        && Take(Sha256File(source.SourcePath())) == source.SourceSha256(),
        "refused C save preserves both sources and publishes nothing");

    // 与既有实际资源专项一致，完整WPM预期复用生成器；字面网格预期另在Core/Qt覆盖。
    const std::array<MemberReplacement, 1> repaired{{{"war3map.wpm", correctOldWpm}}};
    const auto consistent = Take(MapArchiveSession::Open(Take(mismatched.SaveCandidate(
        directory / "shifted-grid-consistent-source.w3x", repaired)).path));
    auto accepted = request;
    accepted.sourceMap = consistent.SourcePath(); accepted.expectedSourceSha256 = consistent.SourceSha256();
    accepted.expectedTerrainSha256 = blocked.expectedTerrainSha256;
    accepted.targetCandidate = directory / "shifted-grid-saved.w3x";
    const auto saved = Take(SaveMapEditCandidate(accepted));
    const auto readback = Take(MapArchiveSession::Open(saved.candidate));
    Require(saved.scriptPreserved && Take(readback.ReadMember("war3map.wpm")) == expectedWpm
        && Take(readback.ReadMember("war3map.w3e")) == Take(SerializeMapTerrain(dryAfter)),
        "known C grid permits public water save with complete W3E and WPM readback");
    for (const auto& member : consistent.Members()) {
        const auto name = Text::LowerAscii(member.name);
        if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp") continue;
        Require(Take(readback.ReadMember(member.name)) == member.data,
            "category save preserves every unrelated member, including shifted DOO and script");
    }
    Check(categories.VerifySources(kBytes));
    Check(rules.VerifyReadback(readback, kBytes));
    Require(Take(Sha256File(consistent.SourcePath())) == consistent.SourceSha256()
        && Take(Sha256File(mismatched.SourcePath())) == mismatched.SourceSha256()
        && Take(Sha256File(baseline.SourcePath())) == baseline.SourceSha256()
        && Take(Sha256File(drySeed.SourcePath())) == drySeed.SourceSha256()
        && Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "all actual C sources remain untouched");
    std::cout << "category_c_grid_source_mismatch_refused source_sha256=" << mismatched.SourceSha256() << '\n';
    std::cout << "category_c_grid_save_passed snap_cells=" << unsigned(snap)
        << " source_sha256=" << consistent.SourceSha256() << " candidate_sha256=" << saved.candidateSha256
        << " native_editor_verified=false game_verified=false\n";
    CheckCategoryOverrides(consistent, accepted, shifted, dryBefore, dryAfter, footprint, snap, schema, catalog);
}
} // namespace War3Map::TerrainSaveTests
