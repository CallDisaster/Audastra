#include "War3MapCore/Render/TerrainCliffs.h"
#include "War3MapCore/Render/TerrainGroundMesh.h"
#include "War3MapCore/Render/MdxModel.h"
#include "MdxTestModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::Render;
using MapData::TerrainGridSnapshot;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void RequireOriginalFailure(const Result<TerrainCliffs>& result, const std::vector<Diagnostic>& expected) {
    Require(!result.Succeeded() && !result.value && result.diagnostics.size() == expected.size(),
        "失败必须拒绝整批value/coveredTiles并完整保留原诊断数量");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& actual = result.diagnostics[i]; const auto& original = expected[i];
        Require(actual.severity == original.severity && actual.subsystem == original.subsystem
            && actual.code == original.code && actual.message == original.message
            && actual.origin.has_value() == original.origin.has_value(), "读取/解析失败不得重写code、severity或消息");
        if (original.origin)
            Require(actual.origin->source == original.origin->source && actual.origin->offset == original.origin->offset
                && actual.origin->length == original.origin->length && actual.origin->fingerprint == original.origin->fingerprint,
                "读取/解析失败必须保留原来源位置和fingerprint");
    }
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
            if (found == files.end()) return Result<std::vector<std::byte>>::Failure("Test", "RESOURCE_NOT_FOUND", "缺少文件");
            return Result<std::vector<std::byte>>::Success(found->second);
        };
    }
};

const std::vector<std::optional<CliffTypeSource>> kTypes{
    CliffTypeSource{"Cliffs", "ReplaceableTextures\\Cliff\\Cliff0.blp", "CliffTrans"}};

using Vec3 = std::array<float, 3>;
struct NormalGeoset {
    std::vector<Vec3> positions, normals;
    std::uint32_t samplerFlags = 0;
};

// 原创平面/切向采样 MDX，不沿用旧四点非平面且全 up 法线的几何 oracle。
std::vector<std::byte> NormalModel(const std::vector<NormalGeoset>& geosets) {
    MdxTest::Writer w;
    w.Tag("MDLX"); w.Tag("VERS"); w.U32(4); w.U32(800);
    // 每个 geoset 独立引用 TEXS/MTLS；即使路径相同，也不能先抹掉作者 sampler 身份。
    w.Tag("TEXS"); w.U32(static_cast<std::uint32_t>(268 * geosets.size()));
    for (const auto& input : geosets) {
        w.U32(0); w.Fixed("Original\\CliffNormals.blp", 260); w.U32(input.samplerFlags);
    }
    w.Tag("MTLS"); {
        const auto chunk = w.Begin();
        for (std::uint32_t i = 0; i < geosets.size(); ++i) {
            const auto material = w.Begin();
            w.U32(0); w.U32(0); w.Tag("LAYS"); w.U32(1);
            const auto layer = w.Begin();
            w.U32(0); w.U32(0); w.U32(i); w.U32(UINT32_MAX); w.U32(0); w.F32(1);
            w.EndInclusive(layer); w.EndInclusive(material);
        }
        w.EndChunk(chunk);
    }
    w.Tag("GEOS"); {
        const auto chunk = w.Begin();
        std::uint32_t material = 0;
        for (const auto& input : geosets) {
            const auto item = w.Begin();
            const auto count = static_cast<std::uint32_t>(input.positions.size());
            w.Tag("VRTX"); w.U32(count);
            for (const auto& p : input.positions) for (const auto axis : p) w.F32(axis);
            w.Tag("NRMS"); w.U32(static_cast<std::uint32_t>(input.normals.size()));
            for (const auto& n : input.normals) for (const auto axis : n) w.F32(axis);
            w.Tag("PTYP"); w.U32(1); w.U32(4); w.Tag("PCNT"); w.U32(1); w.U32(3);
            w.Tag("PVTX"); w.U32(3); w.U16(0); w.U16(1); w.U16(2);
            w.Tag("GNDX"); w.U32(count); for (std::uint32_t i = 0; i < count; ++i) w.Raw("\0", 1);
            w.Tag("MTGC"); w.U32(1); w.U32(1); w.Tag("MATS"); w.U32(1); w.U32(0);
            w.U32(material++); w.U32(0); w.U32(0);
            for (int i = 0; i < 7; ++i) w.F32(0);
            w.U32(0);
            w.Tag("UVAS"); w.U32(1); w.Tag("UVBS"); w.U32(count);
            for (std::uint32_t i = 0; i < count; ++i) { w.F32(static_cast<float>(i) / 8); w.F32(0.25f); }
            w.EndInclusive(item);
        }
        w.EndChunk(chunk);
    }
    w.Tag("BONE"); {
        const auto chunk = w.Begin(), node = w.Begin();
        w.Fixed("Original", 80); w.U32(0); w.U32(UINT32_MAX); w.U32(0);
        w.EndInclusive(node); w.U32(UINT32_MAX); w.U32(UINT32_MAX); w.EndChunk(chunk);
    }
    w.Tag("PIVT"); w.U32(12); w.F32(0); w.F32(0); w.F32(0);
    return std::move(w.bytes);
}

TerrainCliffs NormalCliffs(const TerrainGridSnapshot& grid, const std::vector<NormalGeoset>& geosets) {
    Files files;
    const auto bytes = NormalModel(geosets);
    const auto parsed = ParseMdx(bytes);
    Require(parsed.Succeeded() && parsed.value->geosets.size() == geosets.size(), "原创法线 MDX 必须通过真实解析");
    for (const auto& geoset : parsed.value->geosets) Require(geoset.visibleInStand, "两个原创 geoset 都必须实际可见");
    files.files["Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx"] = bytes;
    files.files["Doodads\\Terrain\\CliffTrans\\CliffTransABHL0.mdx"] = bytes;
    auto result = BuildTerrainCliffs(grid, kTypes, files.Reader());
    Require(result.Succeeded() && result.value->meshes.size() == 1, "原创法线悬崖烘焙失败");
    return std::move(*result.value);
}

TerrainGridSnapshot NormalGrid(const std::vector<float>& relief) {
    return GridOf(2, 2, {2, 3, 2, 2}, {}, relief);
}

std::array<double, 3> Difference(const War3RenderModelVertexV2& a, const War3RenderModelVertexV2& b) {
    return {static_cast<double>(a.x) - b.x, static_cast<double>(a.y) - b.y, static_cast<double>(a.z) - b.z};
}

void CheckPerpendicular(const War3RenderModelVertexV2& vertex,
    const std::array<double, 3>& tangentX, const std::array<double, 3>& tangentY, bool requireWinding = true) {
    const std::array<double, 3> n{vertex.nx, vertex.ny, vertex.nz};
    const auto dot = [](const auto& a, const auto& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto length = [&](const auto& a) { return std::sqrt(dot(a, a)); };
    Require(std::abs(length(n) - 1.0) < 1e-6, "修正后有效法线必须是有限单位方向");
    Require(std::abs(dot(n, tangentX)) < 1e-5 * length(tangentX)
        && std::abs(dot(n, tangentY)) < 1e-5 * length(tangentY), "法线必须与独立输出几何切向正交");
    if (requireWinding) {
        const std::array<double, 3> cross{tangentX[1] * tangentY[2] - tangentX[2] * tangentY[1],
            tangentX[2] * tangentY[0] - tangentX[0] * tangentY[2], tangentX[0] * tangentY[1] - tangentX[1] * tangentY[0]};
        Require(dot(n, cross) > 0, "法线朝向必须遵守原始绕序，不能只比较绝对正交");
    }
}

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

void TestReliefNormalsFromGeometry() {
    const NormalGeoset plane{{{-96, 24, 0}, {-32, 24, 0}, {-96, 88, 0}}, std::vector<Vec3>(3, {0, 0, 1})};
    for (const auto& relief : std::vector<std::vector<float>>{{0, 32, 0, 32}, {0, 0, 16, 16}, {0, 32, 16, 48}}) {
        const auto output = NormalCliffs(NormalGrid(relief), {plane});
        const auto unlitNormal = NormalCliffs(NormalGrid(relief), {{plane.positions, std::vector<Vec3>(3, {0, 0, 0})}});
        const auto& v = output.meshes[0].vertices;
        for (const auto& vertex : v) CheckPerpendicular(vertex, Difference(v[1], v[0]), Difference(v[2], v[0]));
        Require(output.coveredTiles == std::vector<std::uint8_t>{1}
            && output.meshes[0].indices == std::vector<std::uint16_t>{0, 1, 2}, "法线修正不应改变覆盖或三角形绕序");
        Require(output.meshes[0].texturePath == "Original\\CliffNormals.blp"
            && output.meshes[0].indices == unlitNormal.meshes[0].indices
            && output.coveredTiles == unlitNormal.coveredTiles, "法线变化不能改变材质来源、拓扑和覆盖集合");
        for (std::size_t i = 0; i < v.size(); ++i) {
            Require(v[i].x == plane.positions[i][0] && v[i].y == plane.positions[i][1]
                && v[i].u == static_cast<float>(i) / 8 && v[i].v == 0.25f, "位置 XY 和 UV 不得随法线修正改变");
            const auto& unchanged = unlitNormal.meshes[0].vertices[i];
            Require(std::array<float, 5>{v[i].x, v[i].y, v[i].z, v[i].u, v[i].v}
                == std::array<float, 5>{unchanged.x, unchanged.y, unchanged.z, unchanged.u, unchanged.v},
                "有效法线和近似法线必须产生完全相同的位置及 UV");
        }
    }
    const NormalGeoset tilted{{{-96, 24, 0}, {-32, 24, -32}, {-96, 88, -16}}, std::vector<Vec3>(3, {2, 1, 4})};
    const auto changedTilt = NormalCliffs(NormalGrid({0, 32, 16, 48}), {tilted});
    const auto& t = changedTilt.meshes[0].vertices;
    for (const auto& vertex : t) CheckPerpendicular(vertex, Difference(t[1], t[0]), Difference(t[2], t[0]));
    // 鞍面：用 Bake 输出位置的中心差分作为切向 oracle，不复制生产梯度公式。
    const NormalGeoset probes{{{-64, 64, 0}, {-63, 64, 0}, {-65, 64, 0}, {-64, 65, 0}, {-64, 63, 0}},
        std::vector<Vec3>(5, {0, 0, 1})};
    const auto saddle = NormalCliffs(NormalGrid({0, 0, 0, 64}), {probes});
    const auto& s = saddle.meshes[0].vertices;
    CheckPerpendicular(s[0], Difference(s[1], s[2]), Difference(s[3], s[4]));
    // 坡道 footprint 深256：实际输出边的高度差必须决定法线，不能仍除128。
    const NormalGeoset rampPlane{{{10, 24, 0}, {20, 24, 0}, {10, 88, 0}}, std::vector<Vec3>(3, {0, 0, 1})};
    const auto ramp = NormalCliffs(RampColumn(), {rampPlane});
    const auto& r = ramp.meshes[0].vertices;
    Require(ramp.rampPieces == 1, "两格坡道夹具必须真正使用 CliffTrans");
    for (const auto& vertex : r) CheckPerpendicular(vertex, Difference(r[1], r[0]), Difference(r[2], r[0]));
}

void TestReliefNormalBoundaryConvention() {
    const auto grid = NormalGrid({0, 32, 16, 48});
    // 每个中心点配一个沿内侧/外侧 x 与 y 的位置样本；严格外侧仍允许另一轴有斜率。
    const std::array<Vec3, 8> centers{{{-128, 64, 0}, {0, 64, 0}, {-64, 0, 0}, {-64, 128, 0},
        {-129, 64, 0}, {1, 64, 0}, {-64, -1, 0}, {-64, 129, 0}}};
    for (std::size_t i = 0; i < centers.size(); ++i) {
        auto alongX = centers[i], alongY = centers[i];
        const float dx = i == 1 || i == 4 ? -1.0f : 1.0f;
        const float dy = i == 3 || i == 6 ? -1.0f : 1.0f;
        alongX[0] += dx; alongY[1] += dy;
        const auto output = NormalCliffs(grid, {{{centers[i], alongX, alongY}, std::vector<Vec3>(3, {0, 0, 1})}});
        const auto& v = output.meshes[0].vertices;
        auto tx = Difference(v[1], v[0]), ty = Difference(v[2], v[0]);
        for (auto& axis : tx) axis /= dx;
        for (auto& axis : ty) axis /= dy;
        CheckPerpendicular(v[0], tx, ty);
    }
}

void TestReliefNormalsPreserveHardEdgesAndApproximationGates() {
    const std::vector<Vec3> positions{{-96, 24, 0}, {-32, 24, 0}, {-96, 88, 0}};
    const auto grid = NormalGrid({0, 32, 16, 48});
    const std::vector<Vec3> vertical{{-96, 24, 0}, {-32, 24, 0}, {-96, 24, 64}};
    const auto hard = NormalCliffs(grid, {{positions, std::vector<Vec3>(3, {0, 0, 2})},
        {vertical, std::vector<Vec3>(3, {0, -1, 0})}});
    const auto& v = hard.meshes[0].vertices;
    Require(v.size() == 6 && hard.meshes[0].indices == std::vector<std::uint16_t>{0, 1, 2, 3, 4, 5},
        "两个同纹理可见 geoset 必须保持独立顶点和法线，不能焊接硬边");
    CheckPerpendicular(v[0], Difference(v[1], v[0]), Difference(v[2], v[0]));
    CheckPerpendicular(v[3], Difference(v[4], v[3]), Difference(v[5], v[3]));
    Require(v[3].nx == 0 && v[3].ny == -1 && v[3].nz == 0 && v[0].nz > 0,
        "共位硬边的不同作者法线不能被跨 geoset 平均");
    const auto flat = NormalCliffs(NormalGrid({7, 7, 7, 7}), {{positions, std::vector<Vec3>(3, {0, 0, 2})}});
    Require(flat.meshes[0].vertices[0].nz == 2, "平坦 relief 必须保留原法线字节，不做双重归一化");
    for (const auto normal : {Vec3{-0.0f, 0, 0}, Vec3{0, 0, 1e-8f}, Vec3{0, 0, 1e7f + 1}}) {
        const auto output = NormalCliffs(grid, {{positions, std::vector<Vec3>(3, normal)}});
        const auto& actual = output.meshes[0].vertices[0];
        Require(actual.nx == normal[0] && actual.ny == normal[1] && actual.nz == normal[2]
            && std::signbit(actual.nx) == std::signbit(normal[0]), "零/小向量及原提交超幅拒绝门必须保留");
        if (normal[2] > 1e7f) {
            Files files;
            const auto rejected = BuildWorldModelScene({}, files.Reader(), {}, output.meshes);
            Require(!rejected.Succeeded() && !rejected.diagnostics.empty()
                && rejected.diagnostics.front().code == "WORLD_BAKED_MESH_INVALID" && files.reads.empty(),
                "超幅法线必须仍被实际烘焙提交校验拒绝，不能借归一化变成新接受来源");
        }
    }
    const auto missing = NormalCliffs(grid, {{positions, {}}});
    Require(missing.meshes[0].vertices[0].nx == 0 && missing.meshes[0].vertices[0].ny == 0
        && missing.meshes[0].vertices[0].nz == 1, "空 NRMS 保留已有 up 近似，不伪造恢复法线");
    const auto invalid = ParseMdx(NormalModel({{positions, std::vector<Vec3>(3, {0, 0, std::numeric_limits<float>::infinity()})}}));
    Require(!invalid.Succeeded(), "非有限源法线仍须由共享解析器拒绝");
    const auto bounded = NormalCliffs(grid, {{positions, std::vector<Vec3>(3, {0, 0, 1e7f})}});
    CheckPerpendicular(bounded.meshes[0].vertices[0], Difference(bounded.meshes[0].vertices[1], bounded.meshes[0].vertices[0]),
        Difference(bounded.meshes[0].vertices[2], bounded.meshes[0].vertices[0]));
}

void TestSamplerIdentitySurvivesCliffBaking() {
    const std::vector<Vec3> positions{{-96, 24, 0}, {-32, 24, 0}, {-96, 88, 0}};
    const std::vector<Vec3> normals(3, {0, 0, 1});
    const std::array<std::uint32_t, 6> authoredFlags{0, 1, 2, 3, 0, 3};
    std::vector<NormalGeoset> geosets;
    for (const auto flags : authoredFlags) geosets.push_back({positions, normals, flags});
    const auto bytes = NormalModel(geosets);
    const auto parsed = ParseMdx(bytes);
    Require(parsed.Succeeded() && parsed.value->geosets.size() == authoredFlags.size()
        && parsed.value->textures.size() == authoredFlags.size(), "原创 sampler MDX 必须经过真实解析");
    for (std::size_t i = 0; i < authoredFlags.size(); ++i) {
        const auto& geoset = parsed.value->geosets[i];
        Require(geoset.visibleInStand && geoset.indices.size() == 3 && geoset.materialId == i,
            "各 sampler geoset 都必须实际可见，不能靠隐藏组绕过合并检查");
        const auto texture = parsed.value->materials[geoset.materialId].layers.front().textureId;
        Require(parsed.value->textures[texture].path == "Original\\CliffNormals.blp"
            && parsed.value->textures[texture].flags == authoredFlags[i], "每组独立保留同路径 TEXS 标志");
    }
    Files files;
    const std::string path = "Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx";
    files.files[path] = bytes;
    const auto built = BuildTerrainCliffs(NormalGrid({0, 0, 0, 0}), kTypes, files.Reader());
    Require(built.Succeeded() && built.value->placedTiles == 1 && built.value->coveredTiles == std::vector<std::uint8_t>{1}
        && built.value->meshes.size() == 4 && files.reads[path] == 1, "单格六个可见组应按四种 sampler 分桶");
    std::array<std::size_t, 4> vertexCounts{}, indexCounts{}, meshCounts{};
    for (const auto& mesh : built.value->meshes) {
        Require(mesh.texturePath == "Original\\CliffNormals.blp" && mesh.samplerFlags < meshCounts.size(),
            "烘焙不应改写贴图路径或接受未知 sampler");
        const auto flags = mesh.samplerFlags;
        ++meshCounts[flags]; vertexCounts[flags] += mesh.vertices.size(); indexCounts[flags] += mesh.indices.size();
        Require(std::all_of(mesh.indices.begin(), mesh.indices.end(), [&](std::uint16_t index) {
            return index < mesh.vertices.size();
        }), "合并后的索引仍只引用本 sampler 网格顶点");
    }
    Require(meshCounts == std::array<std::size_t, 4>{1, 1, 1, 1}
        && vertexCounts == std::array<std::size_t, 4>{6, 3, 3, 6}
        && indexCounts == vertexCounts, "同路径同 flags 可合并，同路径不同 flags 不得误合并");

    // 有效组排在未知组之前，拒绝也不得泄漏半个候选或假的 coveredTiles。
    files.files[path] = NormalModel({{positions, normals, 0}, {positions, normals, 4}});
    const auto unknown = ParseMdx(files.files[path]);
    Require(unknown.Succeeded() && unknown.value->geosets.size() == 2
        && unknown.value->geosets[0].visibleInStand && unknown.value->geosets[1].visibleInStand,
        "flags=4 夹具必须实际到达悬崖 sampler 门禁");
    const auto rejected = BuildTerrainCliffs(NormalGrid({0, 0, 0, 0}), kTypes, files.Reader());
    Require(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty()
        && rejected.diagnostics.front().code == "TERRAIN_CLIFF_SAMPLER_UNSUPPORTED",
        "未知 sampler 必须明确拒绝整次烘焙，不可用部分几何伪造完整覆盖");
}

void TestOnlyExplicitMissingAllowsVariantFallback() {
    Files files;
    const auto grid = GridOf(2, 2, {2, 3, 2, 2}, {}, {}, {}, 1);
    const std::string requested = "Doodads\\Terrain\\Cliffs\\CliffsAAAB1.mdx";
    const std::string lower = "Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx";
    files.files[lower] = MdxTest::BuildTestMdx();
    const auto fallback = BuildTerrainCliffs(grid, kTypes, files.Reader());
    Require(fallback.Succeeded() && fallback.value->placedTiles == 1
        && fallback.value->coveredTiles == std::vector<std::uint8_t>{1}
        && files.reads[requested] == 1 && files.reads[lower] == 1, "明确Missing回执仍允许既有1到0变体回退");
    files.files.clear(); files.reads.clear();
    const auto missing = BuildTerrainCliffs(grid, kTypes, files.Reader());
    Require(missing.Succeeded() && missing.value->meshes.empty() && missing.value->placedTiles == 0
        && missing.value->missingModelTiles == 1 && missing.value->coveredTiles == std::vector<std::uint8_t>{0}
        && missing.value->warnings.size() == 1 && missing.value->warnings[0].code == "TERRAIN_CLIFF_MODEL",
        "所有变体明确Missing仍返回未覆盖格及原缺模型warning");

    for (const auto* code : {"RESOURCE_READ_FAILED", "RESOURCE_CONFLICT", "WORLD_ASSET_READ", "MISSING"}) {
        files.reads.clear(); files.files[lower] = MdxTest::BuildTestMdx();
        const std::vector<Diagnostic> diagnostics{{DiagnosticSeverity::Warning, "OwnedWinner", code, "原选中模型读取失败",
            OriginSpan{requested, 17, 23, "session-winner-1"}}};
        auto originalReader = files.Reader();
        const auto rejected = BuildTerrainCliffs(grid, kTypes, [&](std::string_view path) {
            if (path != requested) return originalReader(path);
            ++files.reads[std::string(path)]; return Result<std::vector<std::byte>>{std::nullopt, diagnostics};
        });
        RequireOriginalFailure(rejected, diagnostics);
        Require(files.reads[requested] == 1 && files.reads[lower] == 0, "非明确Missing不得尝试有效低变体");
    }
    files.reads.clear();
    const std::vector<Diagnostic> compound{
        {DiagnosticSeverity::Error, "OwnedWinner", "RESOURCE_NOT_FOUND", "不能用其中一条Missing掩盖其它失败", std::nullopt},
        {DiagnosticSeverity::Error, "OwnedWinner", "RESOURCE_READ_FAILED", "实际来源失败", OriginSpan{requested, 4, 8, "winner-2"}}};
    auto originalReader = files.Reader();
    const auto compoundFailure = BuildTerrainCliffs(grid, kTypes, [&](std::string_view path) {
        if (path != requested) return originalReader(path);
        ++files.reads[std::string(path)]; return Result<std::vector<std::byte>>{std::nullopt, compound};
    });
    RequireOriginalFailure(compoundFailure, compound);
    Require(files.reads[lower] == 0, "复合失败包含Missing也不能回退");
}

void TestParseFailureAndValidRequestedVariantKeepTheirMeaning() {
    Files files;
    const std::string requested = "Doodads\\Terrain\\Cliffs\\CliffsAAAB1.mdx";
    const std::string lower = "Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx";
    const auto grid = GridOf(2, 2, {2, 3, 2, 2}, {}, {}, {}, 1);
    MdxTest::Options broken; broken.truncate = true;
    files.files[requested] = MdxTest::BuildTestMdx(broken); files.files[lower] = MdxTest::BuildTestMdx();
    const auto parsed = ParseMdx(files.files[requested]); Require(!parsed.Succeeded(), "命中模型确实被唯一MDX解析器拒绝");
    RequireOriginalFailure(BuildTerrainCliffs(grid, kTypes, files.Reader()), parsed.diagnostics);
    Require(files.reads[requested] == 1 && files.reads[lower] == 0, "命中损坏MDX不得当缺失改画有效0变体");

    const std::vector<Vec3> lowPositions{{-96, 24, 0}, {-32, 24, 0}, {-96, 88, 0}};
    const std::vector<Vec3> highPositions{{-96, 24, 37}, {-32, 24, 37}, {-96, 88, 37}}, normals(3, {0, 0, 1});
    files.files[requested] = NormalModel({{highPositions, normals}}); files.files[lower] = NormalModel({{lowPositions, normals}});
    files.reads.clear();
    const auto selected = BuildTerrainCliffs(grid, kTypes, files.Reader());
    Require(selected.Succeeded() && selected.value->placedTiles == 1 && selected.value->meshes.size() == 1
        && selected.value->meshes.front().vertices.front().z == 37 && files.reads[lower] == 0,
        "有效原请求变体须保留实际不同几何，不被低变体替代");
}

void TestRampFailureAndLateFailureRejectTheEntireBatch() {
    const std::string rampPath = "Doodads\\Terrain\\CliffTrans\\CliffTransABHL1.mdx";
    auto ramp = RampColumn(); ramp.vertices[0].variationRaw = 1 << 5;
    for (const bool readFailure : {false, true}) {
        Files files;
        files.files["Doodads\\Terrain\\CliffTrans\\CliffTransABHL0.mdx"] = MdxTest::BuildTestMdx();
        files.files["Doodads\\Terrain\\Cliffs\\CliffsABBA0.mdx"] = MdxTest::BuildTestMdx();
        MdxTest::Options broken; broken.truncate = true; files.files[rampPath] = MdxTest::BuildTestMdx(broken);
        const auto parsed = ParseMdx(files.files[rampPath]); Require(!parsed.Succeeded(), "原创坡道坏模型须解析失败");
        const std::vector<Diagnostic> diagnostics{{DiagnosticSeverity::Error, "SessionCliff", "RESOURCE_READ_FAILED",
            "原坡道winner不可读", OriginSpan{rampPath, 9, 3, "ramp-winner"}}};
        auto originalReader = files.Reader();
        const auto rejected = BuildTerrainCliffs(ramp, kTypes, [&](std::string_view path) {
            if (!readFailure || path != rampPath) return originalReader(path);
            ++files.reads[std::string(path)]; return Result<std::vector<std::byte>>{std::nullopt, diagnostics};
        });
        RequireOriginalFailure(rejected, readFailure ? diagnostics : parsed.diagnostics);
        Require(files.reads.size() == 1 && files.reads[rampPath] == 1,
            "坡道读取/解析失败不得转低变体、其它方向或普通悬崖");
    }
    Files files;
    files.files["Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx"] = MdxTest::BuildTestMdx();
    const std::string second = "Doodads\\Terrain\\Cliffs\\CliffsBAAA0.mdx";
    MdxTest::Options broken; broken.truncate = true; files.files[second] = MdxTest::BuildTestMdx(broken);
    const auto parsed = ParseMdx(files.files[second]); Require(!parsed.Succeeded(), "第二格模型须确实损坏");
    const auto rejected = BuildTerrainCliffs(Grid({2, 3, 2, 2, 2, 2}), kTypes, files.Reader());
    RequireOriginalFailure(rejected, parsed.diagnostics);
    Require(files.reads["Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx"] == 1 && files.reads[second] == 1,
        "先烘焙有效格再遇失败仍不得发布部分meshes或coveredTiles");
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
        TestReliefNormalsFromGeometry();
        TestReliefNormalBoundaryConvention();
        TestReliefNormalsPreserveHardEdgesAndApproximationGates();
        TestSamplerIdentitySurvivesCliffBaking();
        TestOnlyExplicitMissingAllowsVariantFallback();
        TestParseFailureAndValidRequestedVariantKeepTheirMeaning();
        TestRampFailureAndLateFailureRejectTheEntireBatch();
        std::cout << "terrain cliffs: shape names, variation fallback, relief lift, skips, ramp heights, ramp pieces and ground hiding passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
