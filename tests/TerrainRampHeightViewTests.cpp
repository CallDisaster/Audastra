#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/NativeEditorProfileInputs.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/Render/TerrainCliffs.h"
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
constexpr std::size_t kBudget = 8u * 1024u * 1024u;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message;
        for (const auto& diagnostic : result.diagnostics) message += diagnostic.code + ": " + diagnostic.message + " | ";
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
template<class T> void Refused(const Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value && HasCode(result.diagnostics, code), "expected refusal with original diagnostic");
}
std::vector<std::byte> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

TerrainGridSnapshot Flat() {
    TerrainGridSnapshot grid;
    grid.header.mainTileset = 'L'; grid.header.width = grid.header.height = 5;
    grid.header.offset = {-256, -256}; grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01"};
    grid.vertices.resize(25);
    for (auto& vertex : grid.vertices) {
        vertex.groundHeightRaw = vertex.waterHeightAndFlagsRaw = 8192;
        vertex.cliffAndLayerRaw = 2;
    }
    RefreshTerrainDerived(grid, 0, 0, 5, 5);
    return grid;
}

TerrainGridSnapshot Special(int offset) {
    auto grid = Flat();
    for (unsigned y = 1; y <= 3; ++y) for (unsigned x = 1; x <= 3; ++x) {
        auto& vertex = grid.vertices[y * 5 + x]; vertex.textureAndFlagsRaw = 0x10;
        if ((offset == 32 && x == 3 && y == 3)
            || (offset == 64 && ((x == 1 && y == 1) || (x == 3 && y == 3)))
            || (offset == 96 && (x == 3 || y == 3))) vertex.cliffAndLayerRaw = 3;
    }
    RefreshTerrainDerived(grid, 0, 0, 5, 5);
    return grid;
}

void FullHeightsMatchPickingAndSessionQueries() {
    for (const int offset : {32, 64, 96}) {
        const auto raw = Special(offset);
        const auto wire = Take(SerializeMapTerrain(raw));
        auto shown = raw;
        Require(Take(Render::ApplyRampHeights(shown, 12)) != 0 && shown.vertices[12].position[2] == offset,
            "independent S cases display literal quarter, half and three-quarter layer heights");
        Require(Take(SerializeMapTerrain(shown)) == wire, "display heights never change raw W3E serialization");
        const auto center = SampleGroundHeight(shown, 0, 0);
        Require(center && *center == offset, "picking reads the exact displayed center without a second ramp lift");
        MapDataEditSession session(MapDocument::FromMembers({}));
        Require(session.AttachTerrain(wire).Succeeded(), "attach original S terrain");
        const auto exact = Take(session.InspectTerrainPoint({0, 0}, 12));
        Require(exact.groundHeight == offset && !exact.groundHeightApproximate,
            "same actual rule gives point query the independent S height");
        const auto approximate = session.InspectTerrainPoint({0, 0});
        Require(approximate.Succeeded() && approximate.value->groundHeightApproximate
            && HasCode(approximate.diagnostics, "TERRAIN_HEIGHT_APPROXIMATE"), "missing rules are explicitly approximate");
        for (const auto& point : std::array{TerrainStrokePoint{-32, -16}, TerrainStrokePoint{32, 16}, TerrainStrokePoint{0, 0}}) {
            const auto picked = SampleGroundHeight(shown, point.x, point.y);
            const auto queried = Take(session.InspectTerrainPoint(point, 12));
            Require(picked && *picked == queried.groundHeight, "interpolated picking and querying use the same full ramp surface");
        }
        const Services::MapDataQueryBinding binding{"synthetic-height-view", 1, 0, std::string(64, 'a')};
        const auto state = Take(Services::CaptureTerrainEditState(session, binding));
        const std::array points{TerrainStrokePoint{0, 0}, TerrainStrokePoint{-256, -256}};
        const auto queried = Services::QueryTerrainPoints(session, binding, state.binding, points);
        Require(queried.Succeeded() && queried.value->front().groundHeightApproximate
            && HasCode(queried.diagnostics, "TERRAIN_HEIGHT_APPROXIMATE"), "public point queries preserve approximation diagnostics");
    }
    auto straight = Flat();
    for (unsigned y : {1u, 2u}) for (unsigned x = 0; x < 5; ++x) {
        auto& vertex = straight.vertices[y * 5 + x];
        vertex.textureAndFlagsRaw = 0x10; vertex.cliffAndLayerRaw = x < 2 ? 2 : 3;
    }
    RefreshTerrainDerived(straight, 0, 0, 5, 5);
    Require(Take(Render::ApplyRampHeights(straight, 12)) != 0 && straight.vertices[6].position[2] == 64,
        "straight R midpoint remains the literal half layer");
}

void DisplayFailuresAreAtomicAndFlatNeedsNoRules() {
    auto flat = Flat();
    Require(Render::ApplyRampHeights(flat) == 0, "flat compatibility display requires no profile owner");
    MapDataEditSession session(MapDocument::FromMembers({}));
    Require(session.AttachTerrain(Take(SerializeMapTerrain(flat))).Succeeded(), "attach flat terrain");
    const auto flatQuery = session.InspectTerrainPoint({0, 0});
    Require(flatQuery.Succeeded() && flatQuery.diagnostics.empty() && !flatQuery.value->groundHeightApproximate,
        "flat query does not invent a height-rule dependency");
    Refused(session.InspectTerrainPoint({0, 0}, 15), "TERRAIN_HEIGHT_RULE_UNAVAILABLE");
    Refused(session.InspectTerrainPoint({std::numeric_limits<double>::infinity(), 0}, 12), "TERRAIN_QUERY_COORDINATE");
    Refused(session.InspectTerrainPoint({257, 0}, 12), "TERRAIN_QUERY_COORDINATE");

    auto candidate = Special(96);
    const auto before = candidate;
    Refused(Render::ApplyRampHeights(candidate, 12, candidate.vertices.size() * sizeof(float) - 1), "TERRAIN_RAMP_HEIGHT_BUDGET");
    Require(candidate.vertices[12].position == before.vertices[12].position && candidate.minimum == before.minimum,
        "budget failure leaves display unchanged");
    candidate.vertices[24].waterHeightAndFlagsRaw |= 0x8000;
    const auto invalid = candidate;
    Refused(Render::ApplyRampHeights(candidate, 12), "TERRAIN_NATIVE_VERTEX_FLAGS");
    Require(candidate.minimum == invalid.minimum && candidate.maximum == invalid.maximum, "late query failure keeps old bounds");
    for (std::size_t i = 0; i < candidate.vertices.size(); ++i)
        Require(candidate.vertices[i].position == invalid.vertices[i].position, "late query failure cannot partially lift vertices");
    MapDataEditSession invalidSession(MapDocument::FromMembers({}));
    Require(invalidSession.AttachTerrain(Take(SerializeMapTerrain(candidate))).Succeeded(), "attach unknown-neighbor terrain without deleting raw flags");
    Refused(invalidSession.InspectTerrainPoint({0, 0}, 12), "TERRAIN_NATIVE_VERTEX_FLAGS");
}

struct Profiles {
    std::map<std::string, std::vector<std::byte>> files;
    std::size_t reads = 0;
    Profiles() {
        for (const auto& profile : kClassicProfiles) files.emplace(profile.path, std::vector<std::byte>{});
        files["UI/MiscData.txt"] = Bytes("[Terrain]\nMaxHeight=1280\n");
    }
    TerrainPathingResources::HeightSourceReader Reader() {
        return [this](std::span<const std::string> names, const ArchiveExactMemberLimits& limits) {
            ++reads;
            Require(names.size() == kClassicProfiles.size() && names.size() <= limits.memberCount,
                "height capture requests the complete fixed profile set");
            std::vector<ArchiveExactMember> output;
            for (const auto& name : names) {
                const auto found = std::find_if(files.begin(), files.end(), [&](const auto& file) {
                    return Text::LowerAscii(file.first) == Text::LowerAscii(name);
                });
                output.push_back({name, found != files.end(), found == files.end() ? std::vector<std::byte>{} : found->second});
            }
            return Result<std::vector<ArchiveExactMember>>::Success(std::move(output));
        };
    }
    Result<TerrainPathingResources> Capture() { return TerrainPathingResources::CaptureHeightRules(Reader(), 'L', {}, kBudget); }
};

void HeightOwnerKeepsActualRulesAndHasNoSaveAuthority() {
    Profiles source;
    const auto owner = Take(source.Capture());
    Require(owner.HeightMaximumCliffLevel() == 12 && !owner.WaterBrushRules(), "height-only capture uses actual nondefault M and no water rules");
    const auto fingerprint = Take(owner.HeightRuleDependencyFingerprint());
    Require(fingerprint.size() == 64 && owner.VerifyHeightRuleSources(source.Reader(), kBudget).Succeeded(), "owner verifies exact original profile sources");
    const auto reads = source.reads;
    Require(Take(owner.HeightRuleDependencyFingerprint()) == fingerprint && source.reads == reads,
        "reusing fingerprint never recaptures or rereads profile content");
    Require(!owner.GenerateNative(Special(32), kBudget).Succeeded(), "height-only owner cannot authorize native pathing output");
    source.files["UI/MiscData.txt"] = Bytes("[Terrain]\nMaxHeight=1024\n");
    const auto changed = owner.VerifyHeightRuleSources(source.Reader(), kBudget);
    Require(!changed.Succeeded() && HasCode(changed.diagnostics, "TERRAIN_PATHING_RESOURCE_CHANGED"), "rule change invalidates old source identity");
    Require(Take(source.Capture()).HeightMaximumCliffLevel() == 10, "fresh capture reads changed actual maximum rather than default M");
    source.files["UI/WorldEditData.txt"] = Bytes("[Terrain]\nMaxHeight=1280\n");
    Refused(source.Capture(), "TERRAIN_PATHING_PROPERTY_AMBIGUOUS");
    source.files["UI/WorldEditData.txt"].clear(); source.files["UI/MiscData.txt"].clear();
    Refused(source.Capture(), "TERRAIN_SPECIAL_RULE_SOURCE");
    source.files["Units/MiscData.txt"] = Bytes("[Terrain]\nMaxHeight=1280\n");
    Refused(source.Capture(), "TERRAIN_SPECIAL_RULE_SOURCE");
}

void HeightReaderRejectsIncompleteOrMislabelledSets() {
    Profiles profiles;
    const auto wrapped = [&](int mode) {
        const auto reader = profiles.Reader();
        return [reader, mode](auto names, const auto& limits) {
            auto output = reader(names, limits);
            if (mode == 0) output.value->pop_back();
            if (mode == 1) std::swap((*output.value)[0], (*output.value)[1]);
            if (mode == 2) (*output.value)[0].present = false, (*output.value)[0].bytes = Bytes("not absent");
            return output;
        };
    };
    Refused(TerrainPathingResources::CaptureHeightRules(wrapped(0), 'L', {}, kBudget), "TERRAIN_PATHING_RESOURCE_MISSING");
    for (const int mode : {1, 2})
        Refused(TerrainPathingResources::CaptureHeightRules(wrapped(mode), 'L', {}, kBudget), "TERRAIN_PATHING_RESOURCE_CHANGED");
    const auto owner = Take(profiles.Capture());
    for (const int mode : {0, 1, 2}) {
        const auto status = owner.VerifyHeightRuleSources(wrapped(mode), kBudget);
        Require(!status.Succeeded() && HasCode(status.diagnostics, "TERRAIN_PATHING_RESOURCE_CHANGED"),
            "source verification checks complete, named and consistent exact-member result");
    }
}
}

int main() {
    try {
        FullHeightsMatchPickingAndSessionQueries();
        DisplayFailuresAreAtomicAndFlatNeedsNoRules();
        HeightOwnerKeepsActualRulesAndHasNoSaveAuthority();
        HeightReaderRejectsIncompleteOrMislabelledSets();
        std::cout << "terrain-ramp-height-view: full ramp heights and source identity passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
