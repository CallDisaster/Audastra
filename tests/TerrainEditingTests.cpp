#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "MapDataFixtures.h"
#include "TerrainResizeCases.h"

#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::MapData;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 原创 W3E：size×size 顶点，西南角 (-256,-256)，地面 raw 8192、悬崖层级 2；最外一圈标记为地图边界。
std::vector<std::byte> Terrain(std::uint32_t size = 9, std::size_t groundTextures = 2) {
    std::vector<std::byte> bytes;
    const auto u8 = [&](std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); };
    const auto u16 = [&](std::uint16_t value) { u8(static_cast<std::uint8_t>(value & 0xff)); u8(static_cast<std::uint8_t>(value >> 8)); };
    const auto u32 = [&](std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) u8(static_cast<std::uint8_t>((value >> shift) & 0xff)); };
    const auto raw = [&](std::string_view value) { for (const auto c : value) u8(static_cast<std::uint8_t>(c)); };
    raw("W3E!"); u32(11); u8('L'); u32(0);
    u32(static_cast<std::uint32_t>(groundTextures));
    for (std::size_t i = 0; i < groundTextures; ++i) raw(std::string("Tx") + static_cast<char>('A' + i / 26) + static_cast<char>('a' + i % 26));
    u32(2); raw("CLdi"); raw("CLgr");
    u32(size); u32(size);
    u32(std::bit_cast<std::uint32_t>(-256.0f)); u32(std::bit_cast<std::uint32_t>(-256.0f));
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const bool edge = x == 0 || y == 0 || x + 1 == size || y + 1 == size;
            u16(8192); u16(8192 - 256); u8(static_cast<std::uint8_t>(edge ? 0x80 : 0x00)); u8(0xa3); u8(0x02);
        }
    }
    return bytes;
}

std::shared_ptr<TerrainGridSnapshot> Grid(std::uint32_t size = 9, std::size_t groundTextures = 2) {
    auto parsed = ParseMapTerrain(Terrain(size, groundTextures));
    Require(parsed.Succeeded() && parsed.value, "fixture terrain must parse");
    return std::make_shared<TerrainGridSnapshot>(*parsed.value->Snapshot());
}

const TerrainVertex& At(const TerrainGridSnapshot& grid, std::uint32_t x, std::uint32_t y) {
    return grid.vertices[static_cast<std::size_t>(y) * grid.header.width + x];
}

void SerializeRoundTrips() {
    const auto bytes = Terrain();
    auto parsed = ParseMapTerrain(bytes);
    Require(parsed.Succeeded(), "fixture must parse");
    const auto written = SerializeMapTerrain(*parsed.value->Snapshot());
    Require(written.Succeeded() && *written.value == bytes, "unedited terrain must serialize byte for byte");
    auto broken = *parsed.value->Snapshot();
    broken.vertices.pop_back();
    Require(!SerializeMapTerrain(broken).Succeeded(), "inconsistent vertex count must not be written");
}

void TextureAndBlight() {
    auto grid = Grid();
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "Ngrs"; brush.radius = 1; brush.seed = 7;
    const TerrainStrokePoint center{0, 0};
    auto result = ApplyTerrainStroke(*grid, brush, std::span(&center, 1));
    Require(result.Succeeded() && result.value->headerChanged, "painting a new texture must extend the tileset table");
    Require(grid->header.groundTilesets.size() == 3 && grid->header.groundTilesets[2] == "Ngrs", "new texture is appended");
    Require(At(*grid, 2, 2).GroundTextureIndex() == 2 && At(*grid, 1, 2).GroundTextureIndex() == 2, "brush centre and cross are painted");
    Require(At(*grid, 1, 1).GroundTextureIndex() == 0, "circle of radius 1 leaves the corners");
    Require((At(*grid, 2, 2).variationRaw & 0xe0) == 0xa0, "cliff variation bits are preserved");
    Require(result.value->x0 == 1 && result.value->y0 == 1 && result.value->x1 == 4 && result.value->y1 == 4, "changed range is exact");
    auto again = Grid();
    (void)ApplyTerrainStroke(*again, brush, std::span(&center, 1));
    Require(again->vertices[20].variationRaw == grid->vertices[20].variationRaw, "same seed gives the same variation");
    brush.kind = TerrainBrushKind::Blight; brush.value = true; brush.radius = 0;
    Require(ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded() && At(*grid, 2, 2).IsBlight(), "blight is set");
    brush.value = false;
    Require(ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded() && !At(*grid, 2, 2).IsBlight(), "blight is cleared");
    // 表已满时不能加入新贴图，且不修改地形。
    auto full = Grid(9, 16);
    brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "Zzzz";
    Require(!ApplyTerrainStroke(*full, brush, std::span(&center, 1)).Succeeded() && full->header.groundTilesets.size() == 16,
        "a full tileset table rejects a new texture");
}

void HeightBrushes() {
    auto grid = Grid();
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Raise; brush.radius = 2; brush.strength = 64;
    const TerrainStrokePoint center{256, 256}; // 顶点 (4,4)
    Require(ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded(), "raise must succeed");
    const float middle = At(*grid, 4, 4).position[2], side = At(*grid, 5, 4).position[2];
    Require(middle == 64.0f && side > 0 && side < middle, "raise peaks at the centre and falls off");
    Require(grid->maximum[2] == 64.0f, "bounds follow the raised height");
    // 边界顶点不改高度。
    const TerrainStrokePoint corner{-256, -256};
    Require(ApplyTerrainStroke(*grid, brush, std::span(&corner, 1)).Succeeded() && At(*grid, 0, 0).position[2] == 0.0f,
        "map boundary vertices keep their height");
    // 高原：取第一个点处的原始高度。
    brush.kind = TerrainBrushKind::Plateau; brush.radius = 1;
    const std::array<TerrainStrokePoint, 2> path{TerrainStrokePoint{256, 256}, TerrainStrokePoint{384, 256}};
    Require(ApplyTerrainStroke(*grid, brush, path).Succeeded(), "plateau must succeed");
    Require(At(*grid, 6, 4).position[2] == 64.0f && At(*grid, 5, 5).position[2] == 64.0f, "plateau copies the first point's height");
    // 平滑：尖峰被拉向邻域平均。
    auto spike = Grid();
    brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 256;
    (void)ApplyTerrainStroke(*spike, brush, std::span(&center, 1));
    brush.kind = TerrainBrushKind::Smooth; brush.strength = 1;
    Require(ApplyTerrainStroke(*spike, brush, std::span(&center, 1)).Succeeded(), "smooth must succeed");
    Require(std::abs(At(*spike, 4, 4).position[2] - 256.0f / 9.0f) < 0.3f, "smooth moves to the 3x3 average");
}

void CliffLevels() {
    auto grid = Grid(11);
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::CliffLevel; brush.levelDelta = 1; brush.radius = 0; brush.cliffTexture = 1;
    const TerrainStrokePoint center{384, 384}; // 顶点 (5,5)
    for (int i = 0; i < 4; ++i) Require(ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded(), "cliff raise must succeed");
    Require(At(*grid, 5, 5).CliffLevel() == 6 && At(*grid, 5, 5).CliffTextureIndex() == 1, "four raises reach level 6 with the brush texture");
    Require(At(*grid, 4, 5).CliffLevel() == 4 && At(*grid, 3, 5).CliffLevel() == 2, "neighbours are lifted to keep steps within two levels");
    Require(At(*grid, 5, 5).position[2] == 128.0f * 4, "height follows the cliff level");
    brush.levelDelta = 0; brush.cliffTexture = 5;
    Require(!ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded(), "unknown cliff texture is rejected");
}

void AnchoredStrokeKeepsDragStartAndRawHeight() {
    auto grid = Grid();
    auto& startVertex = grid->vertices[4 * grid->header.width + 4];
    startVertex.groundHeightRaw = 0x4001; // 解析器接受的 bit14 不应被高原笔刷截断。
    startVertex.cliffAndLayerRaw = 3;
    RefreshTerrainDerived(*grid, 4, 4, 5, 5);
    const TerrainStrokePoint start{256, 256};
    const auto anchor = CaptureTerrainStrokeAnchor(*grid, start);
    Require(anchor.Succeeded() && anchor.value->groundHeightRaw == 0x4001 && anchor.value->cliffLevel == 3,
        "drag start captures the original ground raw value and cliff level");
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Plateau; brush.radius = 0; brush.anchor = *anchor.value;
    const std::array<TerrainStrokePoint, 2> path{TerrainStrokePoint{384, 256}, TerrainStrokePoint{512, 256}};
    auto preview = *grid;
    Require(ApplyTerrainStroke(preview, brush, std::span(path.data(), 1)).Succeeded(), "anchored preview succeeds");
    Require(ApplyTerrainStroke(*grid, brush, path).Succeeded(), "anchored commit succeeds");
    Require(At(*grid, 5, 4).groundHeightRaw == 0x4001 && At(*grid, 6, 4).groundHeightRaw == 0x4001,
        "plateau copies the captured raw height without clipping bit14");
    Require(At(preview, 5, 4).groundHeightRaw == At(*grid, 5, 4).groundHeightRaw,
        "preview and full stroke agree on the first changed vertex");
    brush.kind = TerrainBrushKind::CliffLevel; brush.levelDelta = 1; brush.cliffTexture = 1;
    Require(ApplyTerrainStroke(*grid, brush, path).Succeeded(), "anchored cliff stroke succeeds");
    Require(At(*grid, 5, 4).CliffLevel() == 4 && At(*grid, 6, 4).CliffLevel() == 4,
        "cliff target stays tied to the drag start rather than each painted point");
}

void InvalidInputChangesNothing() {
    auto grid = Grid();
    const auto before = SerializeMapTerrain(*grid);
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Raise;
    const TerrainStrokePoint center{0, 0};
    const TerrainStrokePoint nan{std::numeric_limits<double>::quiet_NaN(), 0};
    brush.radius = 17;
    Require(!ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded(), "radius above 16 is rejected");
    brush.radius = 1;
    Require(!ApplyTerrainStroke(*grid, brush, {}).Succeeded(), "empty strokes are rejected");
    Require(!ApplyTerrainStroke(*grid, brush, std::span(&nan, 1)).Succeeded(), "non-finite points are rejected");
    brush.strength = 0;
    Require(!ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded(), "zero strength is rejected");
    brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "abc";
    Require(!ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded(), "texture ids must be four bytes");
    Require(*SerializeMapTerrain(*grid).value == *before.value, "rejected strokes leave the terrain untouched");
    // 地图外的点没有效果但不是错误。
    brush.kind = TerrainBrushKind::Raise; brush.strength = 16;
    const TerrainStrokePoint outside{5000, 5000};
    const auto none = ApplyTerrainStroke(*grid, brush, std::span(&outside, 1));
    Require(none.Succeeded() && none.value->Empty(), "points outside the map change nothing");
    Require(!CaptureTerrainStrokeAnchor(*grid, outside).Succeeded(), "an outside drag start cannot capture a terrain anchor");
    brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "Ngrs";
    const TerrainStrokePoint distant{1e300, 1e300};
    const auto emptyTexture = ApplyTerrainStroke(*grid, brush, std::span(&distant, 1));
    Require(emptyTexture.Succeeded() && emptyTexture.value->Empty() && !emptyTexture.value->headerChanged,
        "a distant finite stroke is a no-op and cannot append a tileset entry");
    Require(*SerializeMapTerrain(*grid).value == *before.value, "an outside texture stroke preserves original bytes");
    brush.kind = TerrainBrushKind::Plateau; brush.anchor = TerrainStrokeAnchor{0x8000, 2};
    Require(!ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded(), "ambiguous anchor height is rejected");
    Require(*SerializeMapTerrain(*grid).value == *before.value, "invalid anchor leaves the terrain untouched");
}

void SessionUndoAndDoodadsFollow() {
    auto doodads = ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    Require(doodads.Succeeded(), "doodad fixture must parse");
    DoodadPlacement tree;
    tree.rawcode = "LTlt"; tree.creationId = 1; tree.flags = 2; tree.position = {0, 0, 10}; tree.scale = {1, 1, 1};
    doodads.value->doodads.push_back(tree);
    tree.creationId = 2; tree.position = {-200, 200, 5}; // 改动范围外
    doodads.value->doodads.push_back(tree);
    auto doodadBytes = doodads.value->Serialize();
    const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3map.doo", std::move(*doodadBytes.value)}};
    const auto source = MapDocument::FromMembers(members);
    MapDataEditSession edits(source);
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Raise; brush.radius = 1; brush.strength = 32;
    const TerrainStrokePoint center{0, 0};
    Require(!edits.EditTerrain(brush, std::span(&center, 1)).Succeeded(), "terrain edits need an attached terrain");
    const auto original = Terrain();
    Require(edits.AttachTerrain(original).Succeeded(), "terrain attaches before edits");
    Require(!edits.AttachTerrain(original).Succeeded(), "terrain attaches only once");
    Require(edits.EditTerrain(brush, std::span(&center, 1)).Succeeded(), "raise through the session");
    Require(edits.IsDirty() && edits.Revision() == 1, "terrain edits make the session dirty");
    const auto replacements = edits.Replacements();
    Require(replacements.size() == 2, "terrain and doodads are both replaced");
    const auto moved = edits.Snapshot().Doodads()->doodads;
    Require(moved[0].position[2] == 42.0f && moved[1].position[2] == 5.0f, "only doodads in the changed area follow the ground");
    const auto receipt = edits.LastEdit();
    Require(edits.Undo() && !edits.IsDirty() && edits.Replacements().empty(), "undo restores terrain and doodads");
    Require(*SerializeMapTerrain(*edits.Terrain()).value == original, "undo restores the exact original bytes");
    Require(edits.Redo() && edits.Terrain()->vertices[20].position[2] == 32.0f, "redo reapplies the stroke");
    Require(edits.Replay(receipt, false).Succeeded() && !edits.IsDirty(), "receipt replay undoes the stroke");
    Require(!edits.Replay(receipt, false).Succeeded(), "stale receipts are rejected");
    Require(edits.Replay(receipt, true).Succeeded() && edits.IsDirty(), "receipt replay redoes the stroke");
    // 保存后成为新基线；工程恢复用 SetTerrain 以完整覆盖为一次修改。
    edits.CommitSaved(edits.Snapshot());
    Require(!edits.IsDirty(), "saved terrain becomes the baseline");
    MapDataEditSession restored(source);
    Require(restored.AttachTerrain(original).Succeeded(), "attach source terrain");
    Require(restored.SetTerrain(replacements[1].name == "war3map.w3e" ? replacements[1].data : replacements[0].data).Succeeded(),
        "saved terrain overlay restores");
    Require(restored.Terrain()->vertices[20].position[2] == 32.0f && restored.Undo() && !restored.IsDirty(), "restored overlay is one undoable edit");
    Require(!restored.SetTerrain(Terrain(11)).Succeeded(), "a terrain overlay of a different size is rejected");
}

void ExplicitSizesAndVariation() {
    const TerrainStrokePoint point{768, 768};
    for (std::uint32_t size = 1; size <= 8; ++size) {
        auto grid = Grid(17);
        TerrainBrush brush;
        brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "TxAa";
        brush.shape = TerrainBrushShape::Square; brush.size = size; brush.variation = std::uint8_t{17};
        const auto footprint = GetTerrainBrushFootprint(*grid, brush, point);
        const std::uint32_t side = 2 * size - 1;
        Require(footprint && footprint->x1 - footprint->x0 == side && footprint->y1 - footprint->y0 == side,
            "classic size 1..8 uses vertex radius size minus one");
        Require(footprint->centerX == 768.0, "all brush sizes preview the same snapped vertex center");
        Require(ApplyTerrainStroke(*grid, brush, std::span(&point, 1)).Succeeded(), "explicit size stroke succeeds");
        std::size_t changed = 0;
        for (const auto& vertex : grid->vertices) {
            Require((vertex.variationRaw & 0xe0) == 0xa0, "texture variation preserves cliff variation bits");
            changed += (vertex.variationRaw & 0x1f) == 17;
        }
        Require(changed == side * side, "core changes exactly the previewed square footprint");
    }
    {
        auto circle = Grid();
        TerrainBrush brush; brush.groundTexture = "TxAa"; brush.size = 3; brush.variation = std::uint8_t{17};
        const TerrainStrokePoint center{256, 256};
        Require(ApplyTerrainStroke(*circle, brush, std::span(&center, 1)).Succeeded(), "explicit circular size succeeds");
        Require(At(*circle, 6, 4).GroundVariation() == 17 && At(*circle, 6, 5).GroundVariation() != 17,
            "classic circular brush includes the radius edge but clips the outside diagonal");
    }
    auto grid = Grid();
    const auto before = *SerializeMapTerrain(*grid).value;
    TerrainBrush brush; brush.groundTexture = "TxAa"; brush.size = 9;
    Require(!ApplyTerrainStroke(*grid, brush, std::span(&point, 1)).Succeeded(), "size above eight rejects");
    brush.size = 2; brush.variation = std::uint8_t{18};
    Require(!ApplyTerrainStroke(*grid, brush, std::span(&point, 1)).Succeeded(), "unknown variation rejects");
    brush.variation.reset(); brush.shape = static_cast<TerrainBrushShape>(44);
    Require(!ApplyTerrainStroke(*grid, brush, std::span(&point, 1)).Succeeded(), "unknown shape rejects");
    Require(*SerializeMapTerrain(*grid).value == before, "invalid new controls do not mutate terrain");
}

void NoiseIsBoundedPerStroke() {
    const TerrainStrokePoint point{256, 256};
    const std::array<TerrainStrokePoint, 3> repeated{point, point, point};
    TerrainBrush brush; brush.kind = TerrainBrushKind::Noise; brush.size = 2; brush.strength = 39; brush.seed = 17;
    auto once = Grid(), multiple = Grid();
    Require(ApplyTerrainStroke(*once, brush, std::span(&point, 1)).Succeeded(), "noise stroke succeeds");
    Require(ApplyTerrainStroke(*multiple, brush, repeated).Succeeded(), "overlapping noise stroke succeeds");
    Require(*SerializeMapTerrain(*once).value == *SerializeMapTerrain(*multiple).value,
        "repeated stamps in one noise stroke do not compound the amplitude");
    bool changed = false;
    for (const auto& vertex : once->vertices) {
        const auto delta = static_cast<int>(vertex.groundHeightRaw) - 8192;
        Require(std::abs(delta) <= 156, "noise raw height remains within the requested world amplitude");
        changed |= delta != 0;
    }
    Require(changed, "noise makes actual height changes");
    std::vector<std::byte> pathing(16 + 32 * 32, std::byte{0});
    pathing[0] = std::byte{'M'}; pathing[1] = std::byte{'P'}; pathing[2] = std::byte{'3'}; pathing[3] = std::byte{'W'};
    pathing[4] = std::byte{0}; pathing[8] = std::byte{32}; pathing[12] = std::byte{32};
    const auto document = MapDocument::FromMembers({});
    MapDataEditSession edits(document);
    Require(edits.AttachTerrain(Terrain()).Succeeded() && edits.AttachPathing(pathing).Succeeded(), "attach clean dry terrain and pathing");
    Require(edits.EditTerrain(brush, repeated, false).Succeeded(), "noise uses the shared transaction");
    Require(edits.VerifyTerrainPathingForSave().Succeeded(), "bounded dry noise retains the existing height save gate");
    const auto output = *SerializeMapTerrain(*edits.Terrain()).value;
    Require(edits.Undo() && !edits.IsDirty() && edits.Redo(), "noise undo and redo are one stroke");
    Require(*SerializeMapTerrain(*edits.Terrain()).value == output, "noise redo restores exact bytes");
}

void WaterAndCliffPreserveOtherFields() {
    auto grid = Grid();
    const TerrainStrokePoint point{256, 256};
    auto& center = grid->vertices[4 * 9 + 4];
    center.textureAndFlagsRaw |= 0x40; center.waterHeightAndFlagsRaw |= 0xc000;
    center.cliffAndLayerRaw = 0x04;
    RefreshTerrainDerived(*grid, 4, 4, 5, 5);
    TerrainBrush brush; brush.kind = TerrainBrushKind::WaterHeight; brush.radius = 1; brush.waterHeight = 17.25f;
    Require(ApplyTerrainStroke(*grid, brush, std::span(&point, 1)).Succeeded(), "existing water height edit succeeds");
    Require(center.waterBaseHeight == 17.25f && (center.waterHeightAndFlagsRaw & 0xc000u) == 0xc000u,
        "water height updates derived height and preserves both high flags");
    Require(!At(*grid, 3, 4).HasWater() && At(*grid, 3, 4).waterHeightAndFlagsRaw == 7936,
        "water height tool does not turn dry vertices into water");
    brush.kind = TerrainBrushKind::CliffTexture; brush.cliffTexture = 1; brush.levelDelta = -1;
    const auto height = center.groundHeightRaw;
    Require(ApplyTerrainStroke(*grid, brush, std::span(&point, 1)).Succeeded(), "cliff-only texture stroke succeeds");
    Require(center.CliffLevel() == 4 && center.CliffTextureIndex() == 1 && center.groundHeightRaw == height,
        "cliff texture edit preserves per-vertex level and height");
    Require(At(*grid, 3, 4).CliffLevel() == 2, "cliff texture tool never normalizes neighboring levels");
    const auto bytes = *SerializeMapTerrain(*grid).value;
    const auto read = ParseMapTerrain(bytes);
    Require(read.Succeeded() && *SerializeMapTerrain(*read.value->Snapshot()).value == bytes,
        "water and cliff fields round trip byte for byte");
    MapDataEditSession edits(MapDocument::FromMembers({}));
    Require(edits.AttachTerrain(bytes).Succeeded(), "water session attaches");
    brush.kind = TerrainBrushKind::WaterHeight; brush.waterHeight = 32;
    Require(edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "water uses the shared transaction");
    const auto revision = edits.Revision();
    brush.waterHeight = std::numeric_limits<float>::infinity();
    Require(!edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded() && edits.Revision() == revision,
        "invalid water height cannot partially commit");
    Require(!edits.VerifyTerrainPathingForSave().Succeeded(), "water edits retain the derived-data save gate");
    Require(edits.Undo() && *SerializeMapTerrain(*edits.Terrain()).value == bytes && edits.Redo(),
        "water undo restores exact bytes and redo restores the stroke");
}

void RemoveWaterPreservesRawFieldsAndPlacements() {
    static_assert(static_cast<unsigned>(TerrainBrushKind::CornerRamp) == 13);
    static_assert(static_cast<unsigned>(TerrainBrushKind::RemoveWater) == 14);
    auto grid = Grid();
    for (std::size_t i = 0; i < grid->vertices.size(); ++i) {
        auto& vertex = grid->vertices[i];
        vertex.textureAndFlagsRaw |= 0x70; // 湿点、荒芜与坡道标志同时存在。
        vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(0xc000u | (7900u + i));
        vertex.groundHeightRaw += static_cast<std::uint16_t>(i);
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>(0x10 | (2 + i % 3));
    }
    RefreshTerrainDerived(*grid, 0, 0, 9, 9);
    const auto original = *SerializeMapTerrain(*grid).value;
    TerrainBrush brush; brush.kind = TerrainBrushKind::RemoveWater; brush.radius = 1;
    const TerrainStrokePoint center{0, 0}; // (2,2)，圆形半径1只清十字五点。
    const auto removed = ApplyTerrainStroke(*grid, brush, std::span(&center, 1));
    Require(removed.Succeeded() && !removed.value->Empty() && !removed.value->headerChanged,
        "remove-water has an ordinary bounded footprint and never changes the palette");
    const auto output = *SerializeMapTerrain(*grid).value;
    const auto vertexStart = original.size() - grid->vertices.size() * 7;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (output[i] == original[i]) continue;
        Require(i >= vertexStart && (i - vertexStart) % 7 == 4
            && (original[i] ^ output[i]) == std::byte{0x40}, "only HasWater in each selected raw vertex may change");
        ++changed;
    }
    Require(changed == 5 && At(*grid, 1, 1).HasWater() && !At(*grid, 2, 2).HasWater(),
        "circle clears the five selected vertices without touching diagonal neighbors");
    Require(ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).value->Empty(), "removing already dry water is a no-op");
    brush.shape = TerrainBrushShape::Square; brush.size = 2;
    Require(ApplyTerrainStroke(*grid, brush, std::span(&center, 1)).Succeeded() && !At(*grid, 1, 1).HasWater(),
        "classic square size shares ordinary footprint semantics");
    brush.radius = 0; brush.size.reset();
    const TerrainStrokePoint edge{-256, -256};
    Require(ApplyTerrainStroke(*grid, brush, std::span(&edge, 1)).Succeeded()
        && !At(*grid, 0, 0).HasWater() && At(*grid, 0, 0).IsBoundary(),
        "edge water can be removed while preserving the boundary and ramp flags");

    DoodadPlacements doodads;
    DoodadPlacement tree; tree.rawcode = "LTlt"; tree.position = {0, 0, 37}; tree.scale = {1, 1, 1};
    tree.flags = 2; tree.creationId = 17; doodads.doodads.push_back(tree);
    doodads.specialDoodads.push_back({"D001", 3, {2, 2}});
    UnitPlacements units;
    UnitPlacement unit; unit.rawcode = "hfoo"; unit.position = {0, 0, 51}; unit.scale = {1, 1, 1};
    unit.flags = 2; unit.creationId = 18; units.units.push_back(unit);
    const auto doo = *doodads.Serialize().value, unitWire = *units.Serialize().value;
    const std::vector<MapArchiveMember> members{{"war3map.doo", doo}, {"war3mapUnits.doo", unitWire}};
    MapDataEditSession edits(MapDocument::FromMembers(members));
    Require(edits.AttachTerrain(original).Succeeded(), "wet source attaches before any edit");
    Require(edits.EditTerrain(brush, std::span(&center, 1), true).Succeeded(), "remove-water uses the shared transaction");
    Require(*edits.Snapshot().Doodads()->Serialize().value == doo && *edits.Snapshot().Units()->Serialize().value == unitWire
        && !edits.LastEdit().ChangesPlacements(), "following is inert: ordinary/special doodads and units remain byte-identical");
    const auto dry = *SerializeMapTerrain(*edits.Terrain()).value;
    Require(!edits.VerifyTerrainPathingForSave().Succeeded(), "remove-water cannot bypass complete native pathing reconstruction");
    const auto revision = edits.Revision();
    Require(edits.EditTerrain(brush, std::span(&center, 1), true).Succeeded() && edits.Revision() == revision,
        "dry no-op does not add undo history or a revision");
    Require(edits.Undo() && !edits.IsDirty() && *SerializeMapTerrain(*edits.Terrain()).value == original,
        "undo restores all original raw fields including water");
    Require(edits.Redo() && *SerializeMapTerrain(*edits.Terrain()).value == dry,
        "redo restores the exact removal without moving or deleting placements");
}

void TextureSlotsAreUndoable() {
    const auto document = MapDocument::FromMembers({});
    MapDataEditSession edits(document);
    Require(edits.AttachTerrain(Terrain()).Succeeded(), "slot session source attaches");
    const auto original = *SerializeMapTerrain(*edits.Terrain()).value;
    Require(edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 0, "Lgrs").Succeeded(), "replace existing slot");
    Require(edits.Terrain()->header.groundTilesets[0] == "Lgrs" && edits.Terrain()->header.usesCustomTileset,
        "slot replacement changes header and marks custom tileset");
    Require(edits.Terrain()->vertices[40].GroundTextureIndex() == 0, "slot replacement retains raw vertex indices");
    Require(!edits.VerifyTerrainPathingForSave().Succeeded(), "header edits do not bypass derived-data save gate");
    Require(edits.Undo() && *SerializeMapTerrain(*edits.Terrain()).value == original && edits.Redo(), "slot undo restores exact original header");
    const auto revision = edits.Revision();
    Require(!edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 1, "Lgrs").Succeeded(), "duplicate slots reject");
    Require(edits.Revision() == revision, "rejected slot edit leaves revision and history unchanged");
    Require(edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 2, "Ldrt").Succeeded(), "append next ground slot");
    Require(!edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 4, "Lrok").Succeeded(), "sparse slot append rejects");
    auto full = Grid(9, 16);
    Require(!ReplaceTerrainTextureSlot(*full, TerrainTextureTable::Ground, 16, "Lrok").Succeeded(), "slot encoding remains capped at sixteen");
}

void BoundaryDraftPreservesRawFieldsAndRestores() {
    auto grid = Grid();
    auto& center = grid->vertices[40];
    center.textureAndFlagsRaw = 0x71; // 地面1、坡道、荒芜、水面全部保留。
    center.waterHeightAndFlagsRaw |= 0xc000; // 外缘与未知水位位不是边界笔刷的目标。
    const auto original = *SerializeMapTerrain(*grid).value;
    std::vector<std::byte> pathing(16 + 32 * 32, std::byte{0});
    pathing[0] = std::byte{'M'}; pathing[1] = std::byte{'P'}; pathing[2] = std::byte{'3'}; pathing[3] = std::byte{'W'};
    pathing[8] = std::byte{32}; pathing[12] = std::byte{32};
    MapDataEditSession edits(MapDocument::FromMembers({}));
    Require(edits.AttachTerrain(original).Succeeded() && edits.AttachPathing(pathing).Succeeded(), "boundary sources attach");
    TerrainBrush brush; brush.kind = TerrainBrushKind::Boundary; brush.size = 1;
    const TerrainStrokePoint point{256, 256};
    const auto prepared = edits.PrepareTerrainEdit(brush, std::span(&point, 1), false);
    Require(prepared.Succeeded() && prepared.value->HasChanges() && !prepared.value->TerrainPathingGate().Succeeded(),
        "boundary preview is supported but cannot claim verified native pathing");
    Require(edits.Revision() == 0 && !edits.CanUndo() && *SerializeMapTerrain(*edits.Terrain()).value == original,
        "boundary preview is read only");
    Require(edits.ApplyPreparedTerrainEdit(*prepared.value).Succeeded(), "boundary uses the sealed shared command");
    const auto changed = *SerializeMapTerrain(*edits.Terrain()).value;
    std::size_t differences = 0;
    for (std::size_t index = 0; index < original.size(); ++index) if (original[index] != changed[index]) {
        ++differences;
        Require((original[index] ^ changed[index]) == std::byte{0x80}, "boundary only changes its own W3E bit");
    }
    Require(differences == 1 && edits.Terrain()->vertices[40].IsBoundary(), "one-point boundary affects exactly one vertex");
    const auto replacements = edits.Replacements();
    Require(replacements.size() == 1 && replacements.front().name == "war3map.w3e",
        "boundary draft only exposes W3E as an overlay, not WPM");
    Require(!edits.VerifyTerrainPathingForSave().Succeeded(), "boundary native save remains rejected");
    Require(edits.Undo() && *SerializeMapTerrain(*edits.Terrain()).value == original && edits.Redo(),
        "boundary undo and redo restore exact bytes");
    MapDataEditSession restored(MapDocument::FromMembers({}));
    Require(restored.AttachTerrain(original).Succeeded() && restored.AttachPathing(pathing).Succeeded(), "restoration source attaches");
    Require(restored.SetTerrainAndPathing(changed, pathing).Succeeded(), "boundary draft restores through project joint overlay path");
    Require(*SerializeMapTerrain(*restored.Terrain()).value == changed && !restored.VerifyTerrainPathingForSave().Succeeded(),
        "restored boundary remains exact and retains its save refusal");
    brush.value = false;
    Require(restored.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "clear also accepts marked vertices");
    Require(*SerializeMapTerrain(*restored.Terrain()).value == original && restored.VerifyTerrainPathingForSave().Succeeded(),
        "clearing the boundary returns to the original net bytes and eligibility");
}

} // namespace

void SessionUnitsFollowAndStandability() {
    // 单位与物品同在 war3mapUnits.doo；落笔范围内的随地面升降，范围外不动，撤销一并恢复。
    auto units = ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    Require(units.Succeeded(), "unit fixture must parse");
    UnitPlacement footman;
    footman.rawcode = "hfoo"; footman.creationId = 1; footman.flags = 2; footman.position = {0, 0, 10}; footman.scale = {1, 1, 1};
    units.value->units.push_back(footman);
    footman.creationId = 2; footman.position = {-200, 200, 5}; // 改动范围外
    units.value->units.push_back(footman);
    auto unitBytes = units.value->Serialize();
    Require(unitBytes.Succeeded(), "unit fixture serializes");
    const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", std::move(*unitBytes.value)}};
    MapDataEditSession edits(MapDocument::FromMembers(members));
    Require(edits.AttachTerrain(Terrain()).Succeeded(), "terrain attaches before edits");
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Raise; brush.radius = 1; brush.strength = 32;
    const TerrainStrokePoint center{0, 0};
    Require(edits.EditTerrain(brush, std::span(&center, 1)).Succeeded(), "raise through the session");
    const auto moved = edits.Snapshot().Units()->units;
    Require(moved[0].position[2] == 42.0f && moved[1].position[2] == 5.0f, "only units in the changed area follow the ground");
    const auto receipt = edits.LastEdit();
    Require(receipt.ChangesPlacements() && receipt.HasPlacementLineage() && receipt.PlacementLineage().size() == 1
        && receipt.PlacementLineage()[0].kind == MapDataKind::Units && receipt.PlacementLineage()[0].beforeToAfter[1] == 1u,
        "followed rows keep their identities so the world view can update incrementally");
    Require(edits.Undo() && edits.Snapshot().Units()->units[0].position[2] == 10.0f, "undo restores the unit height");
    MapDataEditSession still(MapDocument::FromMembers(members));
    Require(still.AttachTerrain(Terrain()).Succeeded(), "terrain attaches");
    Require(still.EditTerrain(brush, std::span(&center, 1), false).Succeeded()
        && still.Snapshot().Units()->units[0].position[2] == 10.0f, "follow disabled keeps unit heights");

    // 悬崖面不可站立；最近可站立点在相邻的平地格。
    auto grid = Grid(11);
    const TerrainStrokePoint onTile{448, 448}; // 格 (5,5)
    Require(TerrainGroundStandable(*grid, onTile.x, onTile.y), "flat tile is standable");
    Require(!TerrainGroundStandable(*grid, 1e6, 1e6), "outside the map is not standable");
    TerrainBrush cliff;
    cliff.kind = TerrainBrushKind::CliffLevel; cliff.levelDelta = 1; cliff.radius = 0; cliff.cliffTexture = 1;
    const TerrainStrokePoint vertex{384, 384}; // 顶点 (5,5)
    Require(ApplyTerrainStroke(*grid, cliff, std::span(&vertex, 1)).Succeeded(), "raise one cliff level");
    Require(!TerrainGroundStandable(*grid, onTile.x, onTile.y), "a tile with mixed cliff levels is a cliff face");
    const auto nearest = NearestTerrainGroundStandable(*grid, onTile.x, onTile.y);
    Require(nearest && TerrainGroundStandable(*grid, nearest->x, nearest->y)
        && std::hypot(nearest->x - onTile.x, nearest->y - onTile.y) <= 128.0 * 1.5, "nearest standable tile is adjacent");
    Require(NearestTerrainGroundStandable(*grid, 1e6, 1e6, 2) == std::nullopt, "no standable tile far outside the map");
}

int main() {
    try {
        SerializeRoundTrips();
        TextureAndBlight();
        HeightBrushes();
        CliffLevels();
        AnchoredStrokeKeepsDragStartAndRawHeight();
        InvalidInputChangesNothing();
        SessionUndoAndDoodadsFollow();
        ExplicitSizesAndVariation();
        NoiseIsBoundedPerStroke();
        WaterAndCliffPreserveOtherFields();
        RemoveWaterPreservesRawFieldsAndPlacements();
        TextureSlotsAreUndoable();
        BoundaryDraftPreservesRawFieldsAndRestores();
        SessionUnitsFollowAndStandability();
        AudastraTest::TerrainResizeCases::TestTerrainResizeGeometryCases();
        std::cout << "TerrainEditing: brush and canvas resize scenario groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "TerrainEditing: " << exception.what() << '\n';
        return 1;
    }
}
