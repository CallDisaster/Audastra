#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "MapDataFixtures.h"

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

} // namespace

int main() {
    try {
        SerializeRoundTrips();
        TextureAndBlight();
        HeightBrushes();
        CliffLevels();
        AnchoredStrokeKeepsDragStartAndRawHeight();
        InvalidInputChangesNothing();
        SessionUndoAndDoodadsFollow();
        std::cout << "TerrainEditing: 7 scenario groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "TerrainEditing: " << exception.what() << '\n';
        return 1;
    }
}
