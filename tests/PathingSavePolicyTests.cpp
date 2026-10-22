#include "War3MapCore/MapData/PathingSavePolicy.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "proof fixture failed";
        for (const auto& item : result.diagnostics) message += " | " + item.code + ": " + item.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}

template<class T> void Refused(const Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "rejection must not return partial proof");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& item) { return item.code == code; }),
        "rejection keeps its precise reason");
}

void Refused(const Status& status, std::string_view code) {
    Require(!status.Succeeded(), "rejection must not report success");
    Require(std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [&](const auto& item) { return item.code == code; }),
        "status rejection keeps its precise reason");
}

template<class T> void DraftRefused(const Result<T>& result, std::size_t offset,
    std::initializer_list<std::string_view> details = {}, bool missingByte = false) {
    Refused(result, "PATHING_DRAFT_IDENTITY");
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& item) { return item.code == "PATHING_DRAFT_IDENTITY"; });
    Require(found->origin && found->origin->source.generic_string() == "overlay/editor/pathing-restrictions.bin"
        && found->origin->offset == offset, "APR rejection locates its first differing or missing declaration byte");
    for (const auto detail : details)
        Require(found->message.find(detail) != std::string::npos, "APR rejection retains independently expected mismatch details");
    if (missingByte)
        Require(found->message.find("首个差异位=") == std::string::npos, "a missing declaration byte must not invent a differing bit");
}

void U32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((value >> shift) & 255));
}

struct Fixture {
    Bytes terrain, pathing;
    std::uint32_t width = 0, height = 0;
    PathingPreservationBinding binding;

    explicit Fixture(std::uint32_t verticesX = 5, std::uint32_t verticesY = 5) {
        // 原创手写 W3E/MP3W 小端布局，绝不调用产品 writer 生成来源。
        for (const char c : std::string_view("W3E!")) terrain.push_back(static_cast<std::byte>(c));
        U32(terrain, 11); terrain.push_back(std::byte{'L'}); U32(terrain, 0);
        U32(terrain, 1);
        for (const char c : std::string_view("Tg01")) terrain.push_back(static_cast<std::byte>(c));
        U32(terrain, 1);
        for (const char c : std::string_view("Cl01")) terrain.push_back(static_cast<std::byte>(c));
        U32(terrain, verticesX); U32(terrain, verticesY);
        U32(terrain, std::bit_cast<std::uint32_t>(-512.0f)); U32(terrain, std::bit_cast<std::uint32_t>(-640.0f));
        for (std::size_t index = 0; index < static_cast<std::size_t>(verticesX) * verticesY; ++index)
            for (const unsigned value : {0u, 0x20u, 0u, 0x1fu, 0u, 0u, 2u}) terrain.push_back(static_cast<std::byte>(value));
        width = (verticesX - 1) * 4; height = (verticesY - 1) * 4;
        for (const char c : std::string_view("MP3W")) pathing.push_back(static_cast<std::byte>(c));
        U32(pathing, 0); U32(pathing, width); U32(pathing, height);
        for (std::size_t index = 0; index < static_cast<std::size_t>(width) * height; ++index)
            pathing.push_back(static_cast<std::byte>(index & 255));
        binding = {Take(Sha256Text("owned original archive identity")), Take(Sha256(terrain)), Take(Sha256(pathing)), 11, 0, 2};
    }

    Bytes Draft(std::span<const std::uint8_t> masks) const {
        if (std::all_of(masks.begin(), masks.end(), [](auto value) { return value == 0; })) return {};
        Bytes bytes{std::byte{'A'}, std::byte{'P'}, std::byte{'R'}, std::byte{1}};
        for (const auto* sha : {&binding.terrainSha256, &binding.pathingSha256})
            for (const char value : *sha) bytes.push_back(static_cast<std::byte>(value));
        for (const auto value : masks) bytes.push_back(static_cast<std::byte>(value));
        return bytes;
    }

    Result<PathingPreservationProof> Prepare(const Bytes& current, const Bytes& draft,
        const PathingPreservationLimits& limits = {}) const {
        return PathingPreservationProof::Prepare(binding, terrain, pathing, terrain, current, draft, limits);
    }
};

// 独立集合 oracle：逐个限制元素判定，不复用产品编码器或其整字节公式。
std::uint8_t AddedBySetOracle(unsigned source, unsigned requested, unsigned withdrawn) {
    constexpr std::array<unsigned, 3> flags{2, 4, 8};
    unsigned added = 0;
    for (unsigned index = 0; index < flags.size(); ++index)
        if ((requested & (1u << index)) && !(withdrawn & (1u << index)) && !(source & flags[index])) added += flags[index];
    return static_cast<std::uint8_t>(added);
}

void AllSourceBytesAndRestrictionSubsetsArePreserved() {
    const Fixture fixture; // 一图 256 格，完整覆盖每种原始字节。
    for (unsigned request = 0; request < 8; ++request) for (unsigned withdrawn = 0; withdrawn < 8; ++withdrawn) {
        auto current = fixture.pathing;
        std::vector<std::uint8_t> masks(256);
        std::size_t changed = 0;
        for (unsigned source = 0; source < 256; ++source) {
            masks[source] = AddedBySetOracle(source, request, withdrawn);
            if (masks[source]) ++changed;
            // 逐位形成预期字节，原字中每一个位都保留。
            unsigned expected = 0;
            for (unsigned bit = 0; bit < 8; ++bit)
                if ((source & (1u << bit)) || (masks[source] & (1u << bit))) expected += 1u << bit;
            current[16 + source] = static_cast<std::byte>(expected);
        }
        const auto draft = fixture.Draft(masks);
        const auto proof = Take(fixture.Prepare(current, draft));
        Require(proof.ChangedCells() == changed && proof.ReportedCells().size() == (std::min)(changed, std::size_t{64}),
            "bounded summary never changes the complete semantic count");
        Require((proof.Kind() == PathingPreservationKind::ExplicitRestrictionCandidate) == (changed != 0)
            && proof.RequiresNativeVerification() == (changed != 0), "preserve versus explicit candidate stays separate from native evidence");
        Require(proof.VerifyCurrent(fixture.binding, fixture.terrain, current, draft).Succeeded(), "complete declared byte output verifies");
        Require(proof.Bounds().has_value() == (changed != 0), "exact preservation has no fake dirty region");
        for (const auto& cell : proof.ReportedCells()) {
            const auto source = cell.y * fixture.width + cell.x;
            Require(cell.original == source && cell.added == masks[source]
                && cell.current == std::to_integer<unsigned>(current[16 + source]), "reported cells point to exact row-major bytes");
        }
    }
}

void CompleteIntentCanOverlapSourceWithoutGrantingNewPermission() {
    const Fixture fixture;
    std::vector<std::uint8_t> masks(256);
    masks[2] = 2; masks[6] = 4; masks[8] = 8; // 每项都已存在于相应来源字节。
    auto declaration = fixture.Draft(masks); declaration[3] = std::byte{2};
    const auto proof = Take(fixture.Prepare(fixture.pathing, declaration));
    Require(proof.Kind() == PathingPreservationKind::PreserveExact && proof.ChangedCells() == 0,
        "valid full intent preserves exact native bytes and cannot become a restriction candidate");
    auto missingSource = fixture.pathing; missingSource[16 + 2] &= ~std::byte{2};
    Refused(fixture.Prepare(missingSource, declaration), "PATHING_POLICY_SOURCE_RESTRICTION_REMOVED");
    auto invalid = declaration; invalid[132] = std::byte{2};
    Refused(fixture.Prepare(fixture.pathing, invalid), "PATHING_DRAFT_IDENTITY");
    invalid = declaration; invalid[132 + 2] |= std::byte{0x20};
    Refused(fixture.Prepare(fixture.pathing, invalid), "PATHING_DRAFT_IDENTITY");
    invalid = declaration; invalid[4] ^= std::byte{1};
    Refused(fixture.Prepare(fixture.pathing, invalid), "PATHING_DRAFT_IDENTITY");
    invalid = declaration; invalid.pop_back();
    Refused(fixture.Prepare(fixture.pathing, invalid), "PATHING_DRAFT_IDENTITY");
}

void RectangularGridAndDeclarationMustBeComplete() {
    const Fixture fixture(3, 4); // WPM 8×12，不能把 X/Y 或顶点/tile 数混用。
    auto current = fixture.pathing;
    std::vector<std::uint8_t> masks(96);
    masks[0] = 2; masks[88] = 4; // 88=0x58，不含0x04，x=0/y=11。
    current[16] |= std::byte{2}; current[16 + 88] |= std::byte{4};
    const auto draft = fixture.Draft(masks);
    const auto proof = Take(fixture.Prepare(current, draft));
    Require(proof.ChangedCells() == 2 && proof.Bounds()->x0 == 0 && proof.Bounds()->x1 == 1
        && proof.Bounds()->y0 == 0 && proof.Bounds()->y1 == 12, "non-square grid dirty bounds retain the narrow full-height column");
    Refused(fixture.Prepare(current, {}), "PATHING_POLICY_DECLARATION_MISSING");
    auto bad = draft; bad[4] ^= std::byte{1};
    DraftRefused(fixture.Prepare(current, bad), 4);
    bad = draft; bad[0] = std::byte{'X'}; DraftRefused(fixture.Prepare(current, bad), 0);
    bad = draft; bad.resize(3); DraftRefused(fixture.Prepare(current, bad), 3, {}, true);
    bad = draft; bad.pop_back(); DraftRefused(fixture.Prepare(current, bad), draft.size() - 1, {}, true);
    bad = draft; bad.push_back(std::byte{0}); DraftRefused(fixture.Prepare(current, bad), draft.size());
    bad = draft; bad[132] = std::byte{0x20};
    DraftRefused(fixture.Prepare(current, bad), 132, {"x=0", "y=0", "首个差异位=0x02", "不匹配格数=1"});
    bad = draft; bad[132 + 88] = std::byte{0};
    DraftRefused(fixture.Prepare(current, bad), 132 + 88, {"x=0", "y=11", "首个差异位=0x04", "不匹配格数=1"});
    bad = draft; bad[132 + 1] = std::byte{4};
    DraftRefused(fixture.Prepare(current, bad), 132 + 1, {"x=1", "y=0", "首个差异位=0x04", "不匹配格数=1"});
    Refused(fixture.Prepare(fixture.pathing, draft), "PATHING_POLICY_REDUNDANT_DECLARATION");
    PathingPreservationLimits limits; limits.maximumReportedCells = 0;
    const auto countOnly = Take(fixture.Prepare(current, draft, limits));
    Require(countOnly.ChangedCells() == 2 && countOnly.ReportedCells().empty(), "zero report budget retains complete validation and count");
}

void ReportedCellLimitNeverLimitsValidation() {
    const Fixture fixture;
    auto current = fixture.pathing;
    std::vector<std::uint8_t> masks(256);
    for (unsigned source = 0; source < 256; ++source) {
        masks[source] = AddedBySetOracle(source, 7, 0);
        current[16 + source] |= static_cast<std::byte>(masks[source]);
    }
    const auto draft = fixture.Draft(masks);
    const auto proof = Take(fixture.Prepare(current, draft));
    Require(proof.ChangedCells() > 64 && proof.ReportedCells().size() == 64,
        "fixture exceeds the bounded cell report before injecting a late failure");
    Require(std::none_of(proof.ReportedCells().begin(), proof.ReportedCells().end(), [](const auto& cell) {
        return cell.x == 8 && cell.y == 12;
    }), "the independently chosen late cell is beyond the retained report");

    auto badPathing = current; badPathing[16 + 200] ^= std::byte{0x10};
    const auto invalid = fixture.Prepare(badPathing, draft);
    Refused(invalid, "PATHING_POLICY_UNSUPPORTED_BITS");
    Require(invalid.diagnostics.front().origin && invalid.diagnostics.front().origin->source.generic_string() == "war3map.wpm"
        && invalid.diagnostics.front().origin->offset == 16 + 200, "unsupported WPM bit beyond the report still rejects the whole proof");

    // 源 200/201 的 0x0e 缺位均为 0x06；首次坏格固定为 x=8/y=12。
    auto bad = draft; bad[132 + 200] = std::byte{0}; bad[132 + 201] = std::byte{0};
    DraftRefused(fixture.Prepare(current, bad), 132 + 200,
        {"x=8", "y=12", "首个差异位=0x02", "不匹配格数=2"});
    bad = draft; bad[132 + 200] = std::byte{0x02};
    DraftRefused(fixture.Prepare(current, bad), 132 + 200,
        {"x=8", "y=12", "首个差异位=0x04", "不匹配格数=1"});
}

void SourceBitsTerrainAndShapeRemainStrict() {
    const Fixture fixture;
    for (const auto bit : {1u, 16u, 32u, 64u, 128u}) {
        auto changed = fixture.pathing; changed[16] ^= static_cast<std::byte>(bit);
        const auto result = fixture.Prepare(changed, {});
        Refused(result, "PATHING_POLICY_UNSUPPORTED_BITS");
        Require(result.diagnostics.front().origin && result.diagnostics.front().origin->offset == 16,
            "unsupported bit rejection locates the actual raw byte");
    }
    auto changed = fixture.pathing; changed[16 + 14] &= std::byte{0xfd};
    Refused(fixture.Prepare(changed, {}), "PATHING_POLICY_SOURCE_RESTRICTION_REMOVED");
    changed = fixture.pathing; changed[8] ^= std::byte{4};
    Refused(fixture.Prepare(changed, {}), "PATHING_POLICY_SHAPE");
    changed = fixture.pathing; changed.resize(15); Refused(fixture.Prepare(changed, {}), "PATHING_POLICY_SHAPE");
    changed = fixture.pathing; changed.push_back(std::byte{0}); Refused(fixture.Prepare(changed, {}), "PATHING_POLICY_SHAPE");
    for (const auto offset : {8u, 13u, 25u, 37u, 45u, 47u, 49u, 50u, 51u}) {
        auto terrain = fixture.terrain; terrain[offset] ^= std::byte{1};
        Refused(PathingPreservationProof::Prepare(fixture.binding, fixture.terrain, fixture.pathing, terrain, fixture.pathing, {}),
            "PATHING_POLICY_TERRAIN_CHANGED");
    }
    auto malformed = fixture.terrain; malformed.push_back(std::byte{0x73});
    auto binding = fixture.binding; binding.terrainSha256 = Take(Sha256(malformed));
    Refused(PathingPreservationProof::Prepare(binding, malformed, fixture.pathing, malformed, fixture.pathing, {}), "TERRAIN_TRAILING_DATA");
    auto v12 = fixture.terrain; v12[4] = std::byte{12};
    for (std::size_t record = 25; record-- > 0;)
        v12.insert(v12.begin() + static_cast<std::ptrdiff_t>(45 + record * 7 + 5), std::byte{0});
    binding = fixture.binding; binding.terrainSha256 = Take(Sha256(v12));
    Refused(PathingPreservationProof::Prepare(binding, v12, fixture.pathing, v12, fixture.pathing, {}), "PATHING_POLICY_TERRAIN_VERSION");
    auto badPathing = fixture.pathing; badPathing[0] = std::byte{'X'};
    binding = fixture.binding; binding.pathingSha256 = Take(Sha256(badPathing));
    Refused(PathingPreservationProof::Prepare(binding, fixture.terrain, badPathing, fixture.terrain, badPathing, {}), "PATHING_MAGIC");
    badPathing = fixture.pathing; badPathing[4] = std::byte{1};
    binding.pathingSha256 = Take(Sha256(badPathing));
    Refused(PathingPreservationProof::Prepare(binding, fixture.terrain, badPathing, fixture.terrain, badPathing, {}), "PATHING_VERSION_UNSUPPORTED");
    badPathing = fixture.pathing; badPathing.push_back(std::byte{0});
    binding.pathingSha256 = Take(Sha256(badPathing));
    Refused(PathingPreservationProof::Prepare(binding, fixture.terrain, badPathing, fixture.terrain, badPathing, {}), "PATHING_TRAILING_DATA");
}

void BindingBudgetsAndCurrentBytesNeverReviveAProof() {
    const Fixture fixture;
    const auto exact = Take(fixture.Prepare(fixture.pathing, {}));
    for (const auto field : {0, 1, 2, 3, 4, 5}) {
        auto binding = fixture.binding;
        if (field == 0) binding.archiveSha256[0] = binding.archiveSha256[0] == 'a' ? 'b' : 'a';
        if (field == 1) binding.terrainSha256[0] = 'A';
        if (field == 2) binding.pathingSha256[0] = 'A';
        if (field == 3) ++binding.sessionIdentity;
        if (field == 4) ++binding.revision;
        if (field == 5) ++binding.terrainSourceRevision;
        Refused(exact.VerifyCurrent(binding, fixture.terrain, fixture.pathing, {}), "PATHING_POLICY_STALE_BINDING");
    }
    auto wrong = fixture.binding; wrong.pathingSha256 = Take(Sha256Text("different source"));
    Refused(PathingPreservationProof::Prepare(wrong, fixture.terrain, fixture.pathing, fixture.terrain, fixture.pathing, {}),
        "PATHING_POLICY_SOURCE_MISMATCH");
    wrong = fixture.binding; wrong.archiveSha256.clear();
    Refused(PathingPreservationProof::Prepare(wrong, fixture.terrain, fixture.pathing, fixture.terrain, fixture.pathing, {}), "PATHING_POLICY_BINDING");
    auto changed = fixture.pathing; changed.back() ^= std::byte{1};
    Refused(exact.VerifyCurrent(fixture.binding, fixture.terrain, changed, {}), "PATHING_POLICY_STALE_OUTPUT");
    PathingPreservationLimits limits; limits.maximumInputBytes = 16;
    Refused(fixture.Prepare(fixture.pathing, {}, limits), "PATHING_POLICY_BUDGET");
    limits = {}; limits.maximumDecodedTerrainBytes = 1;
    Refused(fixture.Prepare(fixture.pathing, {}, limits), "TERRAIN_MEMORY_LIMIT");
    for (const auto field : {0, 1, 2}) {
        limits = {};
        if (field == 0) ++limits.maximumInputBytes;
        if (field == 1) ++limits.maximumDecodedTerrainBytes;
        if (field == 2) ++limits.maximumReportedCells;
        Refused(fixture.Prepare(fixture.pathing, {}, limits), "PATHING_POLICY_LIMITS");
    }
}

void RestrictionProofBindsEveryCurrentByteAndLength() {
    const Fixture fixture(3, 4);
    auto current = fixture.pathing;
    std::vector<std::uint8_t> masks(96);
    masks[0] = 2; masks[88] = 4;
    current[16] |= std::byte{2}; current[16 + 88] |= std::byte{4};
    const auto draft = fixture.Draft(masks);
    const auto proof = Take(fixture.Prepare(current, draft));
    Require(proof.Kind() == PathingPreservationKind::ExplicitRestrictionCandidate,
        "stale-output cases start from a complete successful restriction proof");

    for (const auto member : {0, 1, 2}) {
        auto terrain = fixture.terrain, pathing = current, declaration = draft;
        auto& bytes = member == 0 ? terrain : member == 1 ? pathing : declaration;
        bytes.back() ^= std::byte{1};
        Refused(proof.VerifyCurrent(fixture.binding, terrain, pathing, declaration), "PATHING_POLICY_STALE_OUTPUT");
        for (const bool append : {false, true}) {
            terrain = fixture.terrain; pathing = current; declaration = draft;
            auto& resized = member == 0 ? terrain : member == 1 ? pathing : declaration;
            if (append) resized.push_back(std::byte{0});
            else resized.pop_back();
            Refused(proof.VerifyCurrent(fixture.binding, terrain, pathing, declaration), "PATHING_POLICY_STALE_OUTPUT");
        }
    }
    auto binding = fixture.binding; ++binding.revision;
    Refused(proof.VerifyCurrent(binding, fixture.terrain, current, draft), "PATHING_POLICY_STALE_BINDING");
    Require(proof.VerifyCurrent(fixture.binding, fixture.terrain, current, draft).Succeeded(),
        "failed verification cannot alter an immutable proof or its accepted original output");
}

void CumulativeInputAndDecodedTerrainBudgetBoundariesAreExact() {
    const Fixture fixture;
    auto current = fixture.pathing;
    std::vector<std::uint8_t> masks(256);
    masks[0] = 2; current[16] |= std::byte{2};
    const auto draft = fixture.Draft(masks);
    PathingPreservationLimits limits;
    limits.maximumInputBytes = fixture.terrain.size() * 2 + fixture.pathing.size() + current.size() + draft.size();
    // 手写 5×5 来源有两项四字节贴图 ID、25 顶点、16 tile；核载荷合同，不以运行时 RSS 为 oracle。
    limits.maximumDecodedTerrainBytes = fixture.terrain.size() + sizeof(TerrainGridSnapshot)
        + 2 * (sizeof(std::string) + 4) + 25 * sizeof(TerrainVertex) + 16 * sizeof(TerrainTile);
    const auto exact = Take(fixture.Prepare(current, draft, limits));
    Require(exact.ChangedCells() == 1 && exact.Kind() == PathingPreservationKind::ExplicitRestrictionCandidate,
        "the complete declared candidate fits both exact cumulative and decoded payload boundaries");
    auto tooSmall = limits; --tooSmall.maximumInputBytes;
    Refused(fixture.Prepare(current, draft, tooSmall), "PATHING_POLICY_BUDGET");
    tooSmall = limits; --tooSmall.maximumDecodedTerrainBytes;
    Refused(fixture.Prepare(current, draft, tooSmall), "TERRAIN_MEMORY_LIMIT");
}

Bytes CurrentPathing(const MapDataEditSession& edits, const Bytes& original) {
    for (const auto& member : edits.Replacements()) if (member.name == "war3map.wpm") return member.data;
    return original;
}

void RealSessionUndoAndSaveGateStaySeparate() {
    const Fixture fixture;
    MapDataEditSession edits(MapDocument::FromMembers({}));
    Require(edits.AttachTerrain(fixture.terrain).Succeeded() && edits.AttachPathing(fixture.pathing).Succeeded(), "attach hand-authored native layouts");
    TerrainBrush brush; brush.kind = TerrainBrushKind::PathingRestriction; brush.size = 1; brush.pathingRestrictions = 4;
    const TerrainStrokePoint point{-464, -592}; // offset+(48,48) -> WPM(1,1)，源0x11未含0x04。
    Require(edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "existing command edits restriction draft");
    auto binding = fixture.binding;
    binding.sessionIdentity = edits.SessionIdentity(); binding.revision = edits.Revision(); binding.terrainSourceRevision = edits.TerrainSourceRevision();
    const auto current = CurrentPathing(edits, fixture.pathing);
    const auto draft = Take(edits.SerializePathingRestrictionDraft());
    const auto proof = Take(PathingPreservationProof::Prepare(binding, fixture.terrain, fixture.pathing, fixture.terrain, current, draft));
    Require(proof.Kind() == PathingPreservationKind::ExplicitRestrictionCandidate && proof.ChangedCells() == 1,
        "existing APR command is fully explained by the independent policy");
    Refused(edits.VerifyTerrainPathingForSave(), "MAP_TERRAIN_PATHING_UNVERIFIED");
    WorkspaceSnapshot source;
    source.aggregateSha256 = binding.archiveSha256;
    source.files = {{"map/war3map.w3e", fixture.terrain.size(), 0, binding.terrainSha256},
        {"map/war3map.wpm", fixture.pathing.size(), 0, binding.pathingSha256}};
    const auto plan = Take(TerrainSavePlan::Prepare(source, edits));
    Refused(plan.SaveGate(), "MAP_TERRAIN_PATHING_UNVERIFIED");
    Refused(plan.VerifyForSave(edits, source.aggregateSha256, edits.Replacements()), "MAP_TERRAIN_PATHING_UNVERIFIED");
    Require(edits.Undo() && CurrentPathing(edits, fixture.pathing) == fixture.pathing && edits.Redo()
        && CurrentPathing(edits, fixture.pathing) == current, "undo-redo preserves exact source and output bytes");
    binding.revision = edits.Revision();
    Refused(proof.VerifyCurrent(binding, fixture.terrain, current, draft), "PATHING_POLICY_STALE_BINDING");
    Require(edits.Undo(), "restore exact original source for rebase");
    binding.revision = edits.Revision();
    const auto exact = Take(PathingPreservationProof::Prepare(binding, fixture.terrain, fixture.pathing, fixture.terrain, fixture.pathing, {}));
    edits.CommitSaved(MapDocument::FromMembers({}));
    binding.terrainSourceRevision = edits.TerrainSourceRevision();
    Refused(exact.VerifyCurrent(binding, fixture.terrain, fixture.pathing, {}), "PATHING_POLICY_STALE_BINDING");
}

} // namespace

int main() {
    try {
        AllSourceBytesAndRestrictionSubsetsArePreserved();
        CompleteIntentCanOverlapSourceWithoutGrantingNewPermission();
        RectangularGridAndDeclarationMustBeComplete();
        ReportedCellLimitNeverLimitsValidation();
        SourceBitsTerrainAndShapeRemainStrict();
        BindingBudgetsAndCurrentBytesNeverReviveAProof();
        RestrictionProofBindsEveryCurrentByteAndLength();
        CumulativeInputAndDecodedTerrainBudgetBoundariesAreExact();
        RealSessionUndoAndSaveGateStaySeparate();
        std::cout << "Pathing preservation policy: 9 scenario groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
