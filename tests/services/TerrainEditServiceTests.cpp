#include "War3MapCore/Services/TerrainEditService.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;

namespace {

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

TerrainGridSnapshot Flat() {
    TerrainGridSnapshot terrain;
    terrain.header.mainTileset = 'L'; terrain.header.width = 17; terrain.header.height = 17;
    terrain.header.offset = {-1024, -1024};
    terrain.header.groundTilesets = {"Ldrt", "Lgrs"}; terrain.header.cliffTilesets = {"CLdi"};
    terrain.vertices.resize(17 * 17);
    for (auto& vertex : terrain.vertices) {
        vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936;
        vertex.cliffAndLayerRaw = 2;
    }
    RefreshTerrainDerived(terrain, 0, 0, 17, 17);
    return terrain;
}

std::vector<std::byte> Pathing() {
    std::vector<std::byte> bytes{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
    for (const std::uint32_t value : {0u, 64u, 64u})
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    bytes.resize(16 + 64 * 64, std::byte{0x02});
    return bytes;
}

MapDataQueryBinding Document() { return {"synthetic-terrain", 1, 0, std::string(64, 'a')}; }

void Attach(MapDataEditSession& session, bool pathing = true) {
    const auto bytes = SerializeMapTerrain(Flat());
    Require(bytes.Succeeded() && session.AttachTerrain(*bytes.value).Succeeded(), "attach synthetic terrain");
    if (pathing) Require(session.AttachPathing(Pathing()).Succeeded(), "attach synthetic pathing");
}

TerrainBrush HeightBrush() {
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.size = 1; brush.strength = 16;
    return brush;
}

void QueryHasExplicitUnknownsAndBounds() {
    MapDataEditSession session(MapDocument::FromMembers({})); Attach(session, false);
    const auto document = Document();
    auto state = CaptureTerrainEditState(session, document);
    Require(state.Succeeded() && state.value && !state.value->pathingAvailable, "absent pathing is unavailable");
    const std::array points{TerrainStrokePoint{0, 0}, TerrainStrokePoint{1024, 1024}};
    auto query = QueryTerrainPoints(session, document, state.value->binding, points);
    Require(query.Succeeded() && query.value->size() == 2, "query includes outermost terrain vertex");
    const auto& center = query.value->front();
    Require(center.groundHeight == 0 && center.vertexX == 8 && center.vertexY == 8
        && center.groundTexture == "Ldrt" && center.cliffTexture == "CLdi" && center.vertex.CliffLevel() == 2,
        "point query reports interpolated height and nearest original vertex identity");
    Require(center.pathingState == TerrainPathingState::Unavailable && !center.pathingFlags,
        "missing WPM is never reported as unblocked");
    Require(session.AttachPathing(Pathing()).Succeeded(), "attach pathing before any edit");
    Require(!QueryTerrainPoints(session, document, state.value->binding, points).Succeeded(),
        "source attachment invalidates earlier query identity even at unchanged edit revision");
    state = CaptureTerrainEditState(session, document);
    query = QueryTerrainPoints(session, document, state.value->binding, points);
    Require(query.Succeeded() && query.value->front().pathingState == TerrainPathingState::Available
        && query.value->front().pathingX == 32 && query.value->front().pathingY == 32
        && query.value->front().pathingFlags == 0x02, "WPM is sampled at 32-world-unit cells without changing raw bits");
    Require(query.value->back().pathingState == TerrainPathingState::Outside && !query.value->back().pathingFlags,
        "outer terrain vertex is beyond the half-open WPM cells");
    const TerrainStrokePoint outside{1025, 0}, invalid{std::numeric_limits<double>::quiet_NaN(), 0};
    Require(!QueryTerrainPoints(session, document, state.value->binding, std::span(&outside, 1)).Succeeded(), "outside terrain rejects");
    Require(!QueryTerrainPoints(session, document, state.value->binding, std::span(&invalid, 1)).Succeeded(), "non-finite query rejects");
    Require(!QueryTerrainPoints(session, document, state.value->binding, {}).Succeeded(), "empty query rejects");
    const std::vector<TerrainStrokePoint> tooMany(TerrainQueryMaximumPoints + 1);
    Require(!QueryTerrainPoints(session, document, state.value->binding, tooMany).Succeeded(), "query budget enforced");
    auto wrong = document; wrong.sourceSha256[0] = 'A';
    Require(!CaptureTerrainEditState(session, wrong).Succeeded(), "noncanonical hash rejects");
    wrong = document; ++wrong.revision;
    Require(!QueryTerrainPoints(session, wrong, state.value->binding, points).Succeeded(), "document edit revision is part of identity");
}

void PreviewIsPureAndCommitUsesSamePlan() {
    MapDataEditSession session(MapDocument::FromMembers({})); Attach(session);
    const auto document = Document(); const TerrainStrokePoint point{0, 0};
    const auto source = *SerializeMapTerrain(*session.Terrain()).value;
    const auto state = CaptureTerrainEditState(session, document);
    const auto plan = PrepareTerrainEdit(session, document, state.value->binding, HeightBrush(), std::span(&point, 1));
    Require(plan.Succeeded() && plan.value && !plan.value->Plan().Changed().Empty(), "height preview is prepared");
    Require(plan.value->Plan().TerrainPathingGate().Succeeded(), "verified small dry-ground height preview passes terrain-only gate");
    Require(session.Revision() == 0 && !session.CanUndo() && !session.IsDirty()
        && *SerializeMapTerrain(*session.Terrain()).value == source, "preview does not publish bytes or consume history");
    const auto applied = ApplyTerrainEdit(session, document, *plan.value);
    Require(applied.Succeeded() && session.Revision() == 1 && session.CanUndo(), "same sealed plan commits once");
    Require(session.InspectTerrainPoint(point).value->groundHeight == 16 && session.VerifyTerrainPathingForSave().Succeeded(),
        "actual bytes and gate agree with preview");
    Require(!ApplyTerrainEdit(session, document, *plan.value).Succeeded(), "reapplying stale plan rejects");
    Require(session.Undo() && *SerializeMapTerrain(*session.Terrain()).value == source, "undo restores exact bytes");
    Require(!ApplyTerrainEdit(session, document, *plan.value).Succeeded(), "undo never makes old plan current again");
    Require(session.Redo() && session.InspectTerrainPoint(point).value->groundHeight == 16, "redo reapplies original command");
    MapDataEditSession other(MapDocument::FromMembers({})); Attach(other);
    Require(!other.ApplyPreparedTerrainEdit(plan.value->Plan()).Succeeded(), "Core rejects a plan from another session");
}

void PreviewKeepsSaveRestrictionsAndRejectsWholeStroke() {
    MapDataEditSession session(MapDocument::FromMembers({})); Attach(session);
    const auto document = Document(); const TerrainStrokePoint point{0, 0};
    auto state = CaptureTerrainEditState(session, document);
    TerrainBrush brush; brush.kind = TerrainBrushKind::Texture; brush.size = 1; brush.groundTexture = "Lgrs";
    auto plan = PrepareTerrainEdit(session, document, state.value->binding, brush, std::span(&point, 1));
    Require(plan.Succeeded() && !plan.value->Plan().TerrainPathingGate().Succeeded(),
        "texture preview reports unverified derived-data gate, not map-save success");
    Require(ApplyTerrainEdit(session, document, *plan.value).Succeeded() && !session.VerifyTerrainPathingForSave().Succeeded(),
        "project edit may apply while native map-save gate stays closed");
    const auto bytes = *SerializeMapTerrain(*session.Terrain()).value;
    state = CaptureTerrainEditState(session, document);
    const std::array points{point, TerrainStrokePoint{std::numeric_limits<double>::infinity(), 0}};
    Require(!PrepareTerrainEdit(session, document, state.value->binding, HeightBrush(), points).Succeeded(), "invalid later sample rejects full preview");
    Require(*SerializeMapTerrain(*session.Terrain()).value == bytes && session.Revision() == 1, "failed full preview has no partial writes");
    const std::vector<TerrainStrokePoint> tooMany(TerrainEditMaximumPoints + 1);
    Require(!PrepareTerrainEdit(session, document, state.value->binding, HeightBrush(), tooMany).Succeeded(), "edit budget enforced before copying terrain");
}

void RejectionPreservesRedoAndCallbackCannotOverwrite() {
    MapDataEditSession session(MapDocument::FromMembers({})); Attach(session);
    const auto document = Document(); const TerrainStrokePoint point{0, 0};
    const auto source = *SerializeMapTerrain(*session.Terrain()).value;
    Require(session.EditTerrain(HeightBrush(), std::span(&point, 1), false).Succeeded() && session.Undo(), "seed redo history");
    auto state = CaptureTerrainEditState(session, document);
    auto plan = PrepareTerrainEdit(session, document, state.value->binding, HeightBrush(), std::span(&point, 1));
    Require(plan.Succeeded() && session.CanRedo(), "preview does not erase redo");
    std::uint64_t rejectedIdentity = 0;
    auto rejected = ApplyTerrainEdit(session, document, *plan.value, [&](const auto& receipt) {
        rejectedIdentity = receipt.CommandIdentity();
        return Status::Failure("test", "REJECT", "Synthetic rejection");
    });
    Require(!rejected.Succeeded() && session.CanRedo() && session.Revision() == 2
        && *SerializeMapTerrain(*session.Terrain()).value == source, "callback rejection preserves source, revision, and redo");
    Require(ApplyTerrainEdit(session, document, *plan.value).Succeeded(), "unmodified rejected plan can be submitted again");
    Require(session.LastEdit().CommandIdentity() == rejectedIdentity, "rejection does not consume command identity");
    state = CaptureTerrainEditState(session, document);
    plan = PrepareTerrainEdit(session, document, state.value->binding, HeightBrush(), std::span(&point, 1));
    const auto reentrant = ApplyTerrainEdit(session, document, *plan.value, [&](const auto&) {
        return session.Undo() ? Status::Ok() : Status::Failure("test", "UNDO", "Synthetic undo failure");
    });
    Require(!reentrant.Succeeded() && *SerializeMapTerrain(*session.Terrain()).value == source && session.CanRedo(),
        "reentrant undo is preserved and stale outer candidate is rejected");
}

void SourceChangesInvalidatePreparedPlans() {
    MapDataEditSession session(MapDocument::FromMembers({})); Attach(session, false);
    const auto document = Document(); const TerrainStrokePoint point{0, 0};
    auto state = CaptureTerrainEditState(session, document);
    auto plan = PrepareTerrainEdit(session, document, state.value->binding, HeightBrush(), std::span(&point, 1));
    Require(plan.Succeeded() && !plan.value->Plan().TerrainPathingGate().Succeeded(), "missing WPM preview reports closed gate");
    Require(session.AttachPathing(Pathing()).Succeeded(), "pathing source may attach before editing");
    Require(!session.ApplyPreparedTerrainEdit(plan.value->Plan()).Succeeded(), "Core refuses preview from previous source attachment");
    state = CaptureTerrainEditState(session, document);
    plan = PrepareTerrainEdit(session, document, state.value->binding, HeightBrush(), std::span(&point, 1));
    Require(ApplyTerrainEdit(session, document, *plan.value).Succeeded(), "fresh plan applies");
    state = CaptureTerrainEditState(session, document);
    plan = PrepareTerrainEdit(session, document, state.value->binding, HeightBrush(), std::span(&point, 1));
    session.CommitSaved(session.Snapshot());
    Require(!session.ApplyPreparedTerrainEdit(plan.value->Plan()).Succeeded(), "saving a new baseline invalidates prepared plan");
    Require(!QueryTerrainPoints(session, document, state.value->binding, std::span(&point, 1)).Succeeded(),
        "saved baseline also invalidates query binding without guessing document revision");
    state = CaptureTerrainEditState(session, document);
    plan = PrepareTerrainEdit(session, document, state.value->binding, HeightBrush(), std::span(&point, 1));
    const auto beforeRevision = session.Revision();
    const auto rejected = ApplyTerrainEdit(session, document, *plan.value, [&](const auto&) {
        session.CommitSaved(session.Snapshot());
        return Status::Ok();
    });
    Require(!rejected.Succeeded() && session.Revision() == beforeRevision,
        "reentrant baseline change is checked in the shared prepublish gate");
}

void BlightAndNoOpKeepOneTransaction() {
    MapDataEditSession session(MapDocument::FromMembers({})); Attach(session);
    const auto document = Document(); const TerrainStrokePoint point{0, 0};
    auto state = CaptureTerrainEditState(session, document);
    TerrainBrush brush; brush.kind = TerrainBrushKind::Blight; brush.size = 1;
    auto plan = PrepareTerrainEdit(session, document, state.value->binding, brush, std::span(&point, 1));
    Require(plan.Succeeded() && plan.value->Plan().HasChanges() && plan.value->Plan().TerrainPathingGate().Succeeded(),
        "blight preview includes the proven WPM linkage");
    Require(ApplyTerrainEdit(session, document, *plan.value).Succeeded(), "blight publishes one transaction");
    const auto current = session.InspectTerrainPoint(point);
    Require(current.value->vertex.IsBlight() && current.value->pathingFlags == 0x22, "terrain and WPM bit5 publish together");
    Require(session.Undo(), "one undo restores both members");
    const auto restored = session.InspectTerrainPoint(point);
    Require(!restored.value->vertex.IsBlight() && restored.value->pathingFlags == 0x02, "one undo preserves all unrelated WPM bits");
    brush.value = false;
    state = CaptureTerrainEditState(session, document);
    plan = PrepareTerrainEdit(session, document, state.value->binding, brush, std::span(&point, 1));
    Require(plan.Succeeded() && !plan.value->Plan().HasChanges(), "clearing original nonblight is a no-op preview");
    const auto revision = session.Revision();
    Require(ApplyTerrainEdit(session, document, *plan.value).Succeeded() && session.Revision() == revision && session.CanRedo(),
        "no-op does not add an undo entry or erase redo");
}

} // namespace

int main() {
    try {
        QueryHasExplicitUnknownsAndBounds();
        PreviewIsPureAndCommitUsesSamePlan();
        PreviewKeepsSaveRestrictionsAndRejectsWholeStroke();
        RejectionPreservesRedoAndCallbackCannotOverwrite();
        SourceChangesInvalidatePreparedPlans();
        BlightAndNoOpKeepOneTransaction();
        std::cout << "6 terrain edit service scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
