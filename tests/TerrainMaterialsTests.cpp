#include "War3MapCore/Render/TerrainMaterials.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Game/GameDataSource.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::Render;
using namespace War3Map::Resources;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
void Rejected(const Result<T>& result, std::string_view code) {
    if (result.Succeeded() || result.value || !std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const Diagnostic& diagnostic) { return diagnostic.code == code; })) {
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        throw std::runtime_error("未按预期拒绝：" + std::string(code));
    }
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const auto value : text) result.push_back(static_cast<std::byte>(value));
    return result;
}

std::string Quoted(std::string_view value) {
    std::string result = "\"";
    for (const auto character : value) {
        result += character;
        if (character == '"') result += '"';
    }
    return result + '"';
}

std::string Slk(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows) {
    std::string result = "ID;PWXL;N;E\n";
    const auto add = [&](const auto& row, std::size_t y) {
        for (std::size_t x = 0; x < row.size(); ++x)
            result += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y)
                + ";K" + Quoted(row[x]) + '\n';
    };
    add(headers, 1);
    for (std::size_t y = 0; y < rows.size(); ++y) add(rows[y], y + 2);
    return result + "E\n";
}

void Put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 255);
}

std::vector<std::byte> Blp(std::uint32_t width, std::uint32_t height) {
    // 原创全零调色板与索引；只做目录元数据测试，不嵌入游戏贴图。
    constexpr std::uint32_t payload = 156 + 1024;
    std::vector<std::byte> bytes(payload + static_cast<std::size_t>(width) * height);
    const auto magic = Bytes("BLP1");
    std::copy(magic.begin(), magic.end(), bytes.begin());
    Put32(bytes, 4, 1); Put32(bytes, 12, width); Put32(bytes, 16, height);
    Put32(bytes, 28, payload); Put32(bytes, 92, width * height);
    return bytes;
}

struct Backing {
    std::map<std::string, std::vector<std::byte>, std::less<>> files;
    std::map<std::string, int, std::less<>> reads;
    bool failRead = false;
};

ResourceSource Source(std::string id, int priority, const std::shared_ptr<Backing>& backing) {
    ResourceSource source;
    source.id = std::move(id);
    source.layer = priority >= ResourcePriority::MapArchive ? ResourceLayer::MapImport : ResourceLayer::Game;
    source.priority = priority;
    source.probe = [backing](std::string_view path) {
        return Result<ResourceProbe>::Success(backing->files.contains(path)
            ? ResourceProbe{ResourceProbeState::Found, std::string(path)} : ResourceProbe{});
    };
    source.read = [backing](std::string_view path) {
        ++backing->reads[std::string(path)];
        if (backing->failRead) return Result<std::vector<std::byte>>::Failure("Fixture", "FIXTURE_READ_FAILED", "合成读取故障");
        return Result<std::vector<std::byte>>::Success(backing->files.at(std::string(path)));
    };
    return source;
}

struct Fixture {
    std::shared_ptr<Backing> backing = std::make_shared<Backing>();
    ResourceResolver resolver;
    MapData::TerrainHeader terrain;
    std::vector<std::string> waterHeaders{
        "waterID", "height", "texFile", "numTex", "texRate", "texOffset", "cells", "alphaMode",
        "lighting", "impassable", "mmRed", "mmGreen", "mmBlue", "mmAlpha",
        "Smin_R", "Smin_G", "Smin_B", "Smin_A", "Smax_R", "Smax_G", "Smax_B", "Smax_A",
        "Dmin_R", "Dmin_G", "Dmin_B", "Dmin_A", "Dmax_R", "Dmax_G", "Dmax_B", "Dmax_A", "futureFlag"};
    std::vector<std::string> waterRow{
        "TSha", "-0.375", "Water\\Flow", "2", "7.5", "1.25", "3", "0", "1", "0",
        "11", "12", "13", "14", "21", "22", "23", "24", "31", "32", "33", "34",
        "41", "42", "43", "44", "51", "52", "53", "54", "unchanged"};

    Fixture() {
        backing->files["TERRAINART\\TERRAIN.SLK"] = Bytes(Slk({"tileID", "dir", "file", "name"},
            {{"Q001", "Terrain\\Origin", "Stone", "stone;\"quoted\""},
             {"Q002", "Terrain\\Origin", "Moss.blp", "moss"}}));
        backing->files["TERRAINART\\CLIFFTYPES.SLK"] = Bytes(Slk(
            {"cliffID", "texDir", "texFile", "cliffModelDir", "rampModelDir", "groundTile", "upperTile", "cliffClass"},
            {{"CQ01", "Cliff", "Stone", "RockModels", "RockRamps", "Q001", "_", "c7"}}));
        Water();
        backing->files["TERRAIN\\ORIGIN\\STONE.BLP"] = Blp(32, 16);
        backing->files["TERRAIN\\ORIGIN\\MOSS.BLP"] = Blp(16, 16);
        backing->files["CLIFF\\STONE.BLP"] = Blp(8, 16);
        backing->files["WATER\\FLOW00.BLP"] = Blp(8, 8);
        backing->files["WATER\\FLOW01.BLP"] = Blp(8, 8);
        Require(resolver.AddSource(Source("fixture-game", ResourcePriority::GameBase, backing)).Succeeded(), "合成来源注册失败");
        terrain.mainTileset = 'T';
        terrain.groundTilesets = {"Q002", "Q001", "Q001"};
        terrain.cliffTilesets = {"CQ01"};
    }

    void Water() { backing->files["TERRAINART\\WATER.SLK"] = Bytes(Slk(waterHeaders, {waterRow})); }
    TerrainMaterialCatalog Catalog() {
        auto result = LoadTerrainMaterialCatalog(resolver);
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        Require(result.Succeeded(), "原创材质表必须可解析");
        return std::move(*result.value);
    }
};

void TestCatalogAndBindings() {
    Fixture fixture;
    const auto catalog = fixture.Catalog();
    Require(catalog.ground.size() == 2 && catalog.cliffs.size() == 1 && catalog.water.size() == 1, "目录 rawcode 数量错误");
    Require(catalog.ground.at("Q001").source.fields.at("name") == "stone;\"quoted\"", "严格 SlkCells 的转义未保留");
    Require(catalog.ground.at("Q001").source.row == 2 && catalog.tables[0].sourceId == "fixture-game", "行号或表来源丢失");
    const auto& water = catalog.water.at("TSha");
    Require(water.heightOffsetTiles == -0.375 && water.heightOffsetWorld == -48.0, "水高必须来自表并乘 128");
    Require(water.framesPerSecond == 7.5 && water.textureOffset == 1.25 && water.cells == 3, "水参数被写死");
    Require(water.shallowMinimum == std::array<std::uint8_t, 4>{21, 22, 23, 24}
        && water.deepMaximum == std::array<std::uint8_t, 4>{51, 52, 53, 54}, "RGBA 顺序错误");
    Require(water.source.fields.at("futureflag") == "unchanged", "未知列未保留");
    const auto result = ResolveTerrainMaterialSet(catalog, fixture.terrain, fixture.resolver);
    Require(result.Succeeded(), "合法材质查询失败");
    Require(result.value->ground.size() == 3 && result.value->ground[0].definition.source.id == "Q002"
        && result.value->ground[1].atlasColumns == 8 && result.value->ground[0].atlasColumns == 4
        && result.value->ground[1].atlasCellPixels == 4, "W3E 顺序或实际 atlas 尺寸错误");
    Require(fixture.backing->reads.at("TERRAIN\\ORIGIN\\STONE.BLP") == 1, "本次查询重复资源没有缓存");
    Require(result.value->water.frames.size() == 2
        && result.value->water.frames[1].resource.canonicalPath == "WATER\\FLOW01.BLP", "水序列帧路径错误");
    Require(result.value->cliffs.front().tilesetSubstitutionUnverified
        && !result.diagnostics.empty() && result.diagnostics.front().severity == DiagnosticSeverity::Warning,
        "声明悬崖纹理不能伪装成原版替换纹理身份");
}

void TestTablesAndLimits() {
    Fixture fixture;
    fixture.waterRow[1] = "nan"; fixture.Water();
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_NUMBER");
    fixture.waterRow[1] = "1e999"; fixture.Water();
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_NUMBER");
    fixture.waterRow[1] = "513"; fixture.Water();
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_NUMBER");
    fixture.waterRow[1] = "0"; fixture.waterRow[3] = "0"; fixture.Water();
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_NUMBER");
    fixture.waterRow[3] = "1.5"; fixture.Water();
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_NUMBER");
    fixture.waterRow[3] = "2"; fixture.waterRow[14] = "256"; fixture.Water();
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_NUMBER");
    fixture.waterRow[14] = "21"; fixture.Water();
    fixture.backing->files["TERRAINART\\TERRAIN.SLK"] = Bytes(Slk({"tileID", "dir", "file"},
        {{"Q001", "Terrain", "One"}, {"Q001", "Terrain", "Two"}}));
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_DUPLICATE");
    fixture.backing->files["TERRAINART\\TERRAIN.SLK"] = Bytes(Slk({"tileID", "dir", "file"}, {{"Q001", "..\\Escape", "One"}}));
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_PATH");
    fixture.backing->files["TERRAINART\\TERRAIN.SLK"] = Bytes(Slk({"tileID", "dir", "file"}, {{"Q001", "Terrain"}}));
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_FIELD");
    fixture.backing->files["TERRAINART\\TERRAIN.SLK"] = Bytes(Slk({"tileID", "dir", "DIR"}, {{"Q001", "Terrain", "One"}}));
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_HEADER");
    fixture = Fixture{};
    TerrainMaterialLimits limits;
    limits.maxTableBytes = 1; Rejected(LoadTerrainMaterialCatalog(fixture.resolver, limits), "TERRAIN_MATERIAL_TABLE_BUDGET");
    limits = {}; limits.maxRowsPerTable = 1; Rejected(LoadTerrainMaterialCatalog(fixture.resolver, limits), "TERRAIN_MATERIAL_TABLE_BUDGET");
    limits = {}; limits.maxCellsPerTable = 2; Rejected(LoadTerrainMaterialCatalog(fixture.resolver, limits), "TERRAIN_MATERIAL_TABLE_BUDGET");
    limits = {}; limits.maxCellBytes = 2; Rejected(LoadTerrainMaterialCatalog(fixture.resolver, limits), "TERRAIN_MATERIAL_TABLE_BUDGET");
    limits = {}; limits.maxWaterFrames = 1; Rejected(LoadTerrainMaterialCatalog(fixture.resolver, limits), "TERRAIN_MATERIAL_NUMBER");
    limits = {}; ++limits.maxTableBytes; Rejected(LoadTerrainMaterialCatalog(fixture.resolver, limits), "TERRAIN_MATERIAL_LIMITS");
}

void TestMetadataAndFailures() {
    Fixture fixture;
    const auto catalog = fixture.Catalog();
    const auto resolve = [&](const TerrainMaterialLimits& limits = {}) {
        return ResolveTerrainMaterialSet(catalog, fixture.terrain, fixture.resolver, limits);
    };
    const auto original = fixture.backing->files.at("TERRAIN\\ORIGIN\\STONE.BLP");
    // 每个截断位置均不足 mip0 的声明范围，失败不能回传部分材质。
    for (std::size_t size = 0; size < original.size(); ++size) {
        fixture.backing->files["TERRAIN\\ORIGIN\\STONE.BLP"] = {original.begin(), original.begin() + static_cast<std::ptrdiff_t>(size)};
        Rejected(resolve(), "TERRAIN_MATERIAL_BLP");
    }
    fixture.backing->files["TERRAIN\\ORIGIN\\STONE.BLP"] = original;
    Put32(fixture.backing->files["TERRAIN\\ORIGIN\\STONE.BLP"], 28, 0xfffffff0);
    Rejected(resolve(), "TERRAIN_MATERIAL_BLP");
    // 合成 JPEG 容器仅声明合法范围，故意没有可解码像素，目录必须只承诺 metadata-only。
    auto jpegContainer = std::vector<std::byte>(164);
    const auto magic = Bytes("BLP1");
    std::copy(magic.begin(), magic.end(), jpegContainer.begin());
    Put32(jpegContainer, 8, 8); Put32(jpegContainer, 12, 32); Put32(jpegContainer, 16, 16);
    Put32(jpegContainer, 156, 2); Put32(jpegContainer, 28, 162); Put32(jpegContainer, 92, 2);
    fixture.backing->files["TERRAIN\\ORIGIN\\STONE.BLP"] = jpegContainer;
    Require(resolve().Succeeded(), "目录不应伪装成 JPEG 像素解码器");
    Put32(fixture.backing->files["TERRAIN\\ORIGIN\\STONE.BLP"], 156, 0xffffffff);
    Rejected(resolve(), "TERRAIN_MATERIAL_BLP");
    fixture.backing->files["TERRAIN\\ORIGIN\\STONE.BLP"] = Blp(24, 16);
    Rejected(resolve(), "TERRAIN_MATERIAL_ATLAS");
    fixture.backing->files["TERRAIN\\ORIGIN\\STONE.BLP"] = original;
    fixture.backing->files["WATER\\FLOW01.BLP"] = Blp(4, 8);
    Rejected(resolve(), "TERRAIN_MATERIAL_WATER_DIMENSIONS");
    fixture.backing->files.erase("WATER\\FLOW01.BLP");
    Rejected(resolve(), "RESOURCE_NOT_FOUND");
    fixture.backing->files["WATER\\FLOW01.BLP"] = Blp(8, 8);
    auto savedHeader = fixture.terrain;
    fixture.terrain.groundTilesets = {"MISS"}; Rejected(resolve(), "TERRAIN_MATERIAL_UNKNOWN_GROUND");
    fixture.terrain = savedHeader; fixture.terrain.cliffTilesets = {"MISS"}; Rejected(resolve(), "TERRAIN_MATERIAL_UNKNOWN_CLIFF");
    fixture.terrain = savedHeader; fixture.terrain.mainTileset = 'U'; Rejected(resolve(), "TERRAIN_MATERIAL_UNKNOWN_WATER");
    fixture.terrain = savedHeader; fixture.terrain.version = 12; Rejected(resolve(), "TERRAIN_MATERIAL_VERSION");
    fixture.terrain = savedHeader;
    TerrainMaterialLimits limits;
    limits.maxTextureBytes = 1; Rejected(resolve(limits), "TERRAIN_MATERIAL_TEXTURE_BUDGET");
    limits = {}; limits.maxTotalTextureBytes = 2000; Rejected(resolve(limits), "TERRAIN_MATERIAL_TEXTURE_BUDGET");
    limits = {}; limits.maxTotalTexturePixels = 300; Rejected(resolve(limits), "TERRAIN_MATERIAL_TEXTURE_BUDGET");
    limits = {}; limits.maxTextureDimension = 8; Rejected(resolve(limits), "TERRAIN_MATERIAL_BLP");
    limits = {}; limits.maxTilesetEntries = 2; Rejected(resolve(limits), "TERRAIN_MATERIAL_TILESET_BUDGET");
    auto modifiedCatalog = catalog;
    modifiedCatalog.water.at("TSha").frameCount = 0xffffffff;
    Rejected(ResolveTerrainMaterialSet(modifiedCatalog, fixture.terrain, fixture.resolver), "TERRAIN_MATERIAL_WATER_FRAMES");
    modifiedCatalog = catalog;
    modifiedCatalog.water.at("TSha").heightOffsetWorld = std::numeric_limits<double>::quiet_NaN();
    Rejected(ResolveTerrainMaterialSet(modifiedCatalog, fixture.terrain, fixture.resolver), "TERRAIN_MATERIAL_NUMBER");
}

void TestExplicitWaterFrame() {
    Fixture fixture;
    const auto definition = fixture.Catalog().water.at("TSha");
    fixture.backing->files["WATER\\FLOW01.BLP"][158] = std::byte{217}; // 调色板第 0 色的 R
    const auto first = LoadTerrainWaterFrame(definition, 0, fixture.resolver);
    const auto selected = LoadTerrainWaterFrame(definition, 1, fixture.resolver);
    Require(first.Succeeded() && selected.Succeeded(), "Water.slk 的合法显式帧应解码成功");
    Require(first.value->index == 0 && selected.value->index == 1
        && selected.value->resource.canonicalPath == "WATER\\FLOW01.BLP"
        && selected.value->resource.sourceId == "fixture-game"
        && first.value->image.Valid() && selected.value->image.Valid()
        && first.value->image.rgba[0] == std::byte{0} && selected.value->image.rgba[0] == std::byte{217}
        && first.value->sourceSha256 != selected.value->sourceSha256,
        "选择帧必须返回对应的真实像素和来源身份，不能仍显示第 0 帧");
    Rejected(LoadTerrainWaterFrame(definition, 2, fixture.resolver), "TERRAIN_MATERIAL_WATER_FRAME_INDEX");
    fixture.backing->files["WATER\\FLOW01.BLP"] = Blp(4, 8);
    Rejected(LoadTerrainWaterFrame(definition, 1, fixture.resolver), "TERRAIN_MATERIAL_WATER_DIMENSIONS");
    fixture.backing->files.erase("WATER\\FLOW01.BLP");
    Rejected(LoadTerrainWaterFrame(definition, 1, fixture.resolver), "RESOURCE_NOT_FOUND");
}

void TestResolverLayers() {
    Fixture fixture;
    auto overlay = std::make_shared<Backing>();
    overlay->files["TERRAIN\\ORIGIN\\STONE.BLP"] = Blp(16, 16);
    Require(fixture.resolver.AddSource(Source("fixture-map", ResourcePriority::MapArchive, overlay)).Succeeded(), "覆盖来源注册失败");
    const auto catalog = fixture.Catalog();
    auto result = ResolveTerrainMaterialSet(catalog, fixture.terrain, fixture.resolver);
    Require(result.Succeeded() && result.value->ground[1].atlasColumns == 4
        && result.value->ground[1].texture.resource.sourceId == "fixture-map", "地图贴图覆盖未按 resolver 层级生效");
    Require(!fixture.backing->reads.contains("TERRAIN\\ORIGIN\\STONE.BLP"), "命中高层后仍读取低层贴图");
    overlay->failRead = true;
    Rejected(ResolveTerrainMaterialSet(catalog, fixture.terrain, fixture.resolver), "FIXTURE_READ_FAILED");
    overlay->failRead = false;
    overlay->files["TERRAINART\\TERRAIN.SLK"] = Bytes("broken overlay");
    Rejected(LoadTerrainMaterialCatalog(fixture.resolver), "slk-header");
    Require(fixture.backing->reads.at("TERRAINART\\TERRAIN.SLK") == 1, "损坏高层表不得回退低层表");
}

void InspectGameMap(const std::filesystem::path& gameRoot, const std::filesystem::path& mapPath) {
    auto game = Game::GameDataSource::Mount(gameRoot);
    Require(game.Succeeded(), "真实游戏 MPQ 挂载失败");
    ResourceResolver resolver;
    ResourceSource source;
    source.id = "game-mount-patch-local-expansion-base";
    source.layer = ResourceLayer::Game;
    source.priority = ResourcePriority::GameBase;
    source.probe = [&](std::string_view path) {
        return Result<ResourceProbe>::Success(game.value->HasFile(std::string(path))
            ? ResourceProbe{ResourceProbeState::Found, std::string(path)} : ResourceProbe{});
    };
    source.read = [&](std::string_view path) { return game.value->ReadFile(std::string(path)); };
    Require(resolver.AddSource(std::move(source)).Succeeded(), "游戏读取来源注册失败");
    auto archive = MapArchiveSession::Open(mapPath);
    Require(archive.Succeeded(), "真实地图只读打开失败");
    ResourceSource mapSource;
    mapSource.id = "map-" + archive.value->SourceSha256();
    mapSource.layer = ResourceLayer::MapImport;
    mapSource.priority = ResourcePriority::MapArchive;
    mapSource.probe = [&](std::string_view path) {
        return Result<ResourceProbe>::Success(archive.value->HasMember(path)
            ? ResourceProbe{ResourceProbeState::Found, std::string(path)} : ResourceProbe{});
    };
    mapSource.read = [&](std::string_view path) { return archive.value->ReadMember(path); };
    Require(resolver.AddSource(std::move(mapSource)).Succeeded(), "地图资源层注册失败");
    const auto member = archive.value->ReadMember("war3map.w3e");
    Require(member.Succeeded(), "真实地图 W3E 读取失败");
    const auto terrain = MapData::ParseMapTerrain(*member.value);
    Require(terrain.Succeeded(), "真实地图 W3E 解析失败");
    const auto catalog = LoadTerrainMaterialCatalog(resolver);
    for (const auto& diagnostic : catalog.diagnostics) std::cout << diagnostic.code << ": " << diagnostic.message << '\n';
    Require(catalog.Succeeded(), "真实 Terrain/CliffTypes/Water 表解析失败");
    const auto snapshot = terrain.value->Snapshot();
    const auto cliffTypes = ResolveTerrainCliffTypes(*catalog.value, snapshot->header);
    Require(cliffTypes.Succeeded(), "真实悬崖目录绑定失败");
    const auto selection = ResolveTerrainGroundSelection(*snapshot, *cliffTypes.value);
    Require(selection.Succeeded(), "真实普通悬崖派生失败");
    std::size_t grassToSquare = 0;
    for (std::size_t i = 0; i < snapshot->vertices.size(); ++i) {
        const auto raw = snapshot->vertices[i].GroundTextureIndex(), selected = selection.value->indices[i];
        const auto& palette = snapshot->header.groundTilesets;
        grassToSquare += raw < palette.size() && selected < palette.size()
            && palette[raw] == "Fgrd" && palette[selected] == "Ysqd";
    }
    for (const auto& diagnostic : selection.diagnostics) std::cout << diagnostic.code << ": " << diagnostic.message << '\n';
    std::cout << "ordinary-ground read-only derived=" << selection.value->derivedVertices
        << " changed=" << selection.value->changedVertices << " rampDeferred=" << selection.value->rampNeighborhoodVertices
        << " unknownCliff=" << selection.value->unknownCliffVertices
        << " missingPaired=" << selection.value->missingPairedGroundVertices << " FgrdToYsqd=" << grassToSquare << '\n';
    const auto materials = ResolveTerrainMaterialSet(*catalog.value, terrain.value->Snapshot()->header, resolver);
    for (const auto& diagnostic : materials.diagnostics) std::cout << diagnostic.code << ": " << diagnostic.message << '\n';
    Require(materials.Succeeded(), "真实经典材质目录查询失败");
    const auto frame0 = LoadTerrainWaterFrame(materials.value->water.definition, 0, resolver);
    const auto frame1 = LoadTerrainWaterFrame(materials.value->water.definition, 1, resolver);
    Require(frame0.Succeeded() && frame1.Succeeded(), "真实 Water.slk 的前两帧未能完整解码");
    Require(archive.value->VerifySourceUnchanged().Succeeded(), "只读验收期间地图 SHA 变化");
    std::cout << "materials metadata-only map-over-game sourceSha=" << archive.value->SourceSha256()
        << " ground=" << materials.value->ground.size() << " cliff=" << materials.value->cliffs.size()
        << " water=" << materials.value->water.definition.source.id
        << " waterOffset=" << materials.value->water.definition.heightOffsetWorld
        << " frames=" << materials.value->water.frames.size() << '\n';
    std::cout << "water frames 0=" << frame0.value->resource.canonicalPath << " sha=" << frame0.value->sourceSha256
        << " 1=" << frame1.value->resource.canonicalPath << " sha=" << frame1.value->sourceSha256 << '\n';
    for (const auto& ground : materials.value->ground)
        std::cout << ground.definition.source.id << " " << ground.texture.resource.canonicalPath << " "
            << ground.texture.width << 'x' << ground.texture.height << " atlas=" << ground.atlasColumns << "x4\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || argc == 3, "用法：terrain-material-tests [游戏目录 独立地图路径]，可选入口只读元数据");
        TestCatalogAndBindings();
        TestTablesAndLimits();
        TestMetadataAndFailures();
        TestExplicitWaterFrame();
        TestResolverLayers();
        if (argc == 3) InspectGameMap(std::filesystem::path(argv[1]), std::filesystem::path(argv[2]));
        std::cout << "TerrainMaterials tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
