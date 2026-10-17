#include "War3MapCore/Render/TerrainCliffs.h"
#include "War3MapCore/Render/TerrainGroundMesh.h"
#include "MdxTestModel.h"

#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::Render;
using MapData::TerrainGridSnapshot;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// 原创 width×height 顶点网格；各数组按 index = y * width + x 给出，ramps 为坡道标记。
TerrainGridSnapshot GridOf(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> levels,
    std::vector<std::uint8_t> ramps = {}, std::vector<float> relief = {}, std::vector<std::uint8_t> types = {},
    std::uint8_t variation = 0) {
    TerrainGridSnapshot grid;
    grid.header.width = width; grid.header.height = height;
    grid.header.cliffTilesets = {"CLdi"};
    grid.header.offset = {-128.0f, 0.0f};
    for (std::uint32_t i = 0; i < width * height; ++i) {
        MapData::TerrainVertex vertex;
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>(((types.empty() ? 0 : types[i]) << 4) | levels[i]);
        vertex.variationRaw = static_cast<std::uint8_t>(variation << 5);
        vertex.textureAndFlagsRaw = static_cast<std::uint8_t>(!ramps.empty() && ramps[i] ? 0x10 : 0);
        vertex.position = {static_cast<float>(i % width) * 128.0f - 128.0f, static_cast<float>(i / width) * 128.0f,
            (relief.empty() ? 0.0f : relief[i]) + 128.0f * (static_cast<float>(levels[i]) - 2.0f)};
        grid.vertices.push_back(vertex);
    }
    for (std::uint32_t y = 0; y + 1 < height; ++y) for (std::uint32_t x = 0; x + 1 < width; ++x) {
        const auto sw = y * width + x;
        grid.tiles.push_back({{sw, sw + 1, sw + width, sw + width + 1}});
    }
    grid.minimum = grid.maximum = grid.vertices.front().position;
    for (const auto& vertex : grid.vertices) for (std::size_t axis = 0; axis < 3; ++axis) {
        grid.minimum[axis] = std::min(grid.minimum[axis], vertex.position[axis]);
        grid.maximum[axis] = std::max(grid.maximum[axis], vertex.position[axis]);
    }
    return grid;
}

// 3×2 顶点（两格）网格。
TerrainGridSnapshot Grid(std::array<std::uint8_t, 6> levels, std::array<std::uint8_t, 6> types = {},
    std::array<float, 6> relief = {}, std::uint8_t variation = 0) {
    return GridOf(3, 2, {levels.begin(), levels.end()}, {}, {relief.begin(), relief.end()}, {types.begin(), types.end()}, variation);
}

struct Files {
    std::map<std::string, std::vector<std::byte>> files;
    std::map<std::string, int> reads;
    ModelFileReader Reader() {
        return [this](std::string_view path) {
            ++reads[std::string(path)];
            const auto found = files.find(std::string(path));
            if (found == files.end()) return Result<std::vector<std::byte>>::Failure("Test", "MISSING", "缺少文件");
            return Result<std::vector<std::byte>>::Success(found->second);
        };
    }
};

const std::vector<std::optional<CliffTypeSource>> kTypes{
    CliffTypeSource{"Cliffs", "ReplaceableTextures\\Cliff\\Cliff0.blp", "CliffTrans"}};

void TestShapeNames() {
    Require(CliffShapeName(2, 2, 2, 3) == "AAAB", "东南高");
    Require(CliffShapeName(2, 4, 3, 2) == "ACBA", "西北 C、东北 B");
    Require(!CliffShapeName(2, 5, 2, 2), "层级差超过 2 没有原版模型");
}

void TestPlacement() {
    Files files;
    files.files["Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx"] = MdxTest::BuildTestMdx();
    // 第 0 格东南角（顶点 1）高一级，第 1 格西南角（顶点 1）同样高一级 → 第 1 格为 BAAA（缺模型）。
    const auto grid = Grid({2, 3, 2, 2, 2, 2}, {}, {0, 16, 0, 0, 8, 0}, 3);
    const auto result = BuildTerrainCliffs(grid, kTypes, files.Reader());
    Require(result.Succeeded(), "悬崖构建失败");
    const auto& cliffs = *result.value;
    Require(cliffs.cliffTiles == 2 && cliffs.placedTiles == 1 && cliffs.missingModelTiles == 1, "放置与缺失计数");
    Require(cliffs.coveredTiles == std::vector<std::uint8_t>{1, 0}, "只标记实际放置的格");
    // 变体 3、2、1 不存在时退到 0，且每个路径只读一次。
    Require(files.reads["Doodads\\Terrain\\Cliffs\\CliffsAAAB3.mdx"] == 1 && files.reads["Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx"] == 1,
        "变体逐级回退");
    Require(!cliffs.warnings.empty() && cliffs.warnings.front().message.find("CliffsBAAA") != std::string::npos, "缺模型警告");
    Require(cliffs.meshes.size() == 1 && cliffs.meshes[0].texturePath == kTypes[0]->texturePath, "替换贴图 11 用悬崖类型贴图");
    const auto& mesh = cliffs.meshes[0];
    Require(mesh.vertices.size() == 4 && mesh.indices.size() == 6, "只取 Stand 可见 geoset");
    // 合成模型顶点 (10,0,0)：放在格子东侧边 x = −128 + 128 + 10；u 夹到 1、v = 0 → 抬升取东南角起伏 16。
    const auto& east = mesh.vertices[1];
    Require(east.x == 10.0f && east.y == 0.0f && std::abs(east.z - 16.0f) < 1e-4f, "位置与双线性起伏");
    // 顶点 (10,10,5)：v = 10/128，东南 16 与东北 8 之间插值。
    const float expected = 5.0f + 16.0f * (1 - 10.0f / 128.0f) + 8.0f * (10.0f / 128.0f);
    Require(std::abs(mesh.vertices[3].z - expected) < 1e-3f, "起伏插值");
}

void TestSkippedTiles() {
    Files files;
    files.files["Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx"] = MdxTest::BuildTestMdx();
    auto ramp = Grid({2, 3, 2, 2, 2, 2});
    ramp.vertices[2].textureAndFlagsRaw = 0x10; // 只属于第 1 格（东南角），放不下两格坡道边
    auto result = BuildTerrainCliffs(ramp, kTypes, files.Reader());
    Require(result.Succeeded() && result.value->rampPieces == 0 && result.value->placedTiles == 1
        && result.value->missingModelTiles == 1, "放不下坡道边时按普通悬崖处理");
    result = BuildTerrainCliffs(Grid({2, 5, 2, 2, 2, 2}), kTypes, files.Reader());
    Require(result.Succeeded() && result.value->tooHighTiles == 2 && result.value->placedTiles == 0, "层级差过大");
    result = BuildTerrainCliffs(Grid({2, 3, 2, 2, 2, 2}, {15, 15, 15, 15, 15, 15}), kTypes, files.Reader());
    Require(result.Succeeded() && result.value->unknownTypeTiles == 0 && result.value->placedTiles == 1,
        "普通悬崖四角/邻域全15时按1.27a退slot0，不把保留值当材质");
    Require(!BuildTerrainCliffs(Grid({2, 3, 2, 2, 2, 2}), kTypes, {}).Succeeded(), "缺读取器拒绝");
}

// 2×3 顶点、一列两格：东列是坡道（2、2、3 层），西列不是（2、2、3 层）。
TerrainGridSnapshot RampColumn() {
    return GridOf(2, 3, {2, 2, 2, 2, 3, 3}, {0, 1, 0, 1, 0, 1}, {0, 16, 0, 0, 0, 8});
}

void TestRampHeights() {
    auto grid = RampColumn();
    const auto top = grid.maximum[2];
    Require(ApplyRampHeights(grid) == 1, "只有坡道中点抬升");
    Require(grid.vertices[3].position[2] == 64.0f && grid.vertices[1].position[2] == 16.0f
        && grid.vertices[2].position[2] == 0.0f, "中点抬到两端层级正中，端点与非坡道不变");
    Require(grid.maximum[2] == top && grid.minimum[2] == 0.0f, "高度边界随抬升更新");
    auto flat = GridOf(2, 3, {2, 2, 2, 2, 2, 2}, {1, 1, 1, 1, 1, 1});
    Require(ApplyRampHeights(flat) == 0, "同层坡道标记不抬升");
}

void TestRampPiece() {
    Files files;
    files.files["Doodads\\Terrain\\CliffTrans\\CliffTransABHL0.mdx"] = MdxTest::BuildTestMdx();
    auto grid = RampColumn();
    ApplyRampHeights(grid);
    const auto result = BuildTerrainCliffs(grid, kTypes, files.Reader());
    Require(result.Succeeded(), "坡道边构建失败");
    const auto& cliffs = *result.value;
    // 下格四角同层（不算悬崖格），上格有层级差；坡道边一次覆盖两格。
    Require(cliffs.rampPieces == 1 && cliffs.cliffTiles == 1 && cliffs.placedTiles == 1
        && cliffs.coveredTiles == std::vector<std::uint8_t>{1, 1}, "坡道边覆盖沿坡道方向两格");
    Require(files.reads.count("Doodads\\Terrain\\Cliffs\\CliffsABBA0.mdx") == 0, "坡道边成功后不再按普通悬崖读取");
    const auto& mesh = cliffs.meshes.at(0);
    // 顶点 (10,0,0)：东侧外沿、v = 0 → 东南角起伏 16；顶点 (10,10,5)：v = 10/256，东南 16 与东北 8 之间。
    Require(mesh.vertices[1].x == 10.0f && std::abs(mesh.vertices[1].z - 16.0f) < 1e-4f, "坡道边位置");
    const float expected = 5.0f + 16.0f * (1 - 10.0f / 256.0f) + 8.0f * (10.0f / 256.0f);
    Require(std::abs(mesh.vertices[3].z - expected) < 1e-3f, "坡道边沿两格长度插值");
    // 四角全是坡道的层级过渡格是坡道面，交给地面绘制。
    auto surface = GridOf(2, 3, {2, 2, 2, 2, 3, 3}, {1, 1, 1, 1, 1, 1});
    const auto open = BuildTerrainCliffs(surface, kTypes, files.Reader());
    Require(open.Succeeded() && open.value->rampSurfaceTiles == 1 && open.value->cliffTiles == 0
        && open.value->coveredTiles == std::vector<std::uint8_t>{0, 0}, "坡道面不放模型");
}

void TestGroundHiding() {
    auto grid = Grid({2, 3, 2, 2, 2, 2});
    const std::vector<std::shared_ptr<const TerrainAtlasSnapshot>> atlases;
    grid.header.groundTilesets.clear();
    std::vector<std::uint8_t> covered{1, 0};
    auto mesh = BuildTerrainGroundMesh(grid, atlases, {}, {}, nullptr, covered);
    if (!mesh.Succeeded()) throw std::runtime_error("覆盖标记下地面构建失败：" + mesh.diagnostics.front().message);
    Require(mesh.value->hiddenTileCount == 1 && mesh.value->hiddenTiles == covered, "隐藏标记");
    Require(mesh.value->vertices.size() == 4 && mesh.value->indices.size() == 6
        && mesh.value->baseTiles == std::vector<std::uint32_t>{1}, "隐藏格不再分配顶点，可见基础层保留原格映射");
    for (const auto index : mesh.value->indices) Require(index < 4, "索引必须指向紧缩后的可见基础层");
    const auto& expected = grid.vertices[grid.tiles[1].vertices[0]].position;
    Require(mesh.value->vertices[0].x == expected[0] && mesh.value->vertices[0].y == expected[1]
        && mesh.value->vertices[0].z == expected[2], "紧缩不能把后一格挪到隐藏格位置");
    covered.pop_back();
    Require(!BuildTerrainGroundMesh(grid, atlases, {}, {}, nullptr, covered).Succeeded(), "覆盖标记数量不符拒绝");
}

void TestOrdinaryGroundSelection() {
    auto grid = GridOf(3, 3, {2, 3, 3, 2, 3, 3, 2, 3, 3}, {}, {}, {15, 1, 0, 0, 0, 0, 0, 0, 0});
    grid.header.version = 11;
    grid.header.groundTilesets = {"Fgrd", "Ysqd", "Ybtl"};
    grid.header.cliffTilesets = {"CYdi", "CYsq"};
    std::vector<std::optional<CliffTypeSource>> types{
        CliffTypeSource{"", "", "", "Fgrd", "Fgrd"}, CliffTypeSource{"", "", "", "Ysqd", "Ybtl"}};
    auto result = ResolveTerrainGroundSelection(grid, types);
    Require(result.Succeeded() && result.value->indices[0] == 1 && result.value->indices[1] == 2,
        "四角先SE后NW，普通下层ground、上层upper");
    Require(result.value->indices[3] == 0 && result.value->derivedVertices == 6,
        "相邻异层格先(x,y)，不能沿用前一个格的材质");
    for (const auto& vertex : grid.vertices) Require(vertex.GroundTextureIndex() == 0, "派生不得修改原顶点");
    Require(grid.header.groundTilesets == std::vector<std::string>{"Fgrd", "Ysqd", "Ybtl"}, "不得扩充原palette");
    types[1]->upperTile = "_";
    result = ResolveTerrainGroundSelection(grid, types);
    Require(result.Succeeded() && result.value->indices[1] == 1, "upper未提供退ground");
    grid.vertices[8].textureAndFlagsRaw |= 0x10;
    result = ResolveTerrainGroundSelection(grid, types);
    Require(result.Succeeded() && result.value->rampNeighborhoodVertices == 4 && result.value->indices[4] == 0
        && !result.diagnostics.empty(), "坡道3x3邻域保留raw并报告子集");
    grid.vertices[8].textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x10);
    types[1]->groundTile = "NOTH";
    result = ResolveTerrainGroundSelection(grid, types);
    Require(result.Succeeded() && result.value->missingPairedGroundVertices != 0 && result.value->indices[0] == 0
        && result.diagnostics.back().code == "TERRAIN_GROUND_SELECTION_UNRESOLVED", "NOTH未证语义保留并诊断");
    types[1]->groundTile = "Ysqd";
    grid.vertices[0].cliffAndLayerRaw = 0x72;
    result = ResolveTerrainGroundSelection(grid, types);
    Require(result.Succeeded() && result.value->unknownCliffVertices != 0 && result.value->indices[0] == 0,
        "首个非15的非法cliff不能被后面的合法值替代");
    for (auto& vertex : grid.vertices) vertex.cliffAndLayerRaw |= 0xf0;
    grid.vertices[8].cliffAndLayerRaw = 0x13;
    result = ResolveTerrainGroundSelection(grid, types);
    Require(result.Succeeded() && result.value->indices[0] == 1, "四角全15时使用有界5x5邻域");
    grid.vertices[8].cliffAndLayerRaw = 0xf3;
    result = ResolveTerrainGroundSelection(grid, types);
    Require(result.Succeeded() && result.value->indices[0] == 0 && result.value->derivedVertices == 6,
        "全邻域15按slot0配对，平层无邻格保持raw");
}

} // namespace

int main() {
    try {
        TestShapeNames();
        TestPlacement();
        TestSkippedTiles();
        TestRampHeights();
        TestRampPiece();
        TestGroundHiding();
        TestOrdinaryGroundSelection();
        std::cout << "terrain cliffs: shape names, variation fallback, relief lift, skips, ramp heights, ramp pieces and ground hiding passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
