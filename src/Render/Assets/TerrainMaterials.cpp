#include "War3MapCore/Render/TerrainMaterials.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>

namespace War3Map::Render {
namespace {

constexpr std::string_view kSubsystem = "Render.TerrainMaterials";

struct Failure {
    std::vector<Diagnostic> diagnostics;
};

[[noreturn]] void Fail(std::string code, std::string message) {
    throw Failure{{{DiagnosticSeverity::Error, std::string(kSubsystem),
        std::move(code), std::move(message), std::nullopt}}};
}

template <typename T>
T Take(Result<T> result) {
    if (!result.Succeeded()) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}

void ValidateLimits(const TerrainMaterialLimits& limits) {
    const TerrainMaterialLimits hard;
    if (limits.maxTableBytes > hard.maxTableBytes || limits.maxRowsPerTable > hard.maxRowsPerTable
        || limits.maxCellsPerTable > hard.maxCellsPerTable || limits.maxCellBytes > hard.maxCellBytes
        || limits.maxTilesetEntries > hard.maxTilesetEntries || limits.maxWaterFrames > hard.maxWaterFrames
        || limits.maxTextureBytes > hard.maxTextureBytes || limits.maxTotalTextureBytes > hard.maxTotalTextureBytes
        || limits.maxTextureDimension > hard.maxTextureDimension
        || limits.maxTotalTexturePixels > hard.maxTotalTexturePixels)
        Fail("TERRAIN_MATERIAL_LIMITS", "材质目录限额只能收紧，不能放宽硬上限。");
}

std::string Where(const TerrainMaterialRow& row) {
    return row.table.canonicalPath + " 行 " + std::to_string(row.row) + "（" + row.id + "）";
}

bool Rawcode(std::string_view text) {
    return text.size() == 4 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c >= 33 && c <= 126;
    });
}

std::string_view Required(const TerrainMaterialRow& row, std::string_view field) {
    const auto found = row.fields.find(field);
    if (found == row.fields.end() || found->second.empty() || found->second == "_")
        Fail("TERRAIN_MATERIAL_FIELD", Where(row) + " 缺少字段 " + std::string(field));
    return found->second;
}

double Number(const TerrainMaterialRow& row, std::string_view field, double minimum, double maximum) {
    auto text = Required(row, field);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    double value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
        || !std::isfinite(value) || value < minimum || value > maximum)
        Fail("TERRAIN_MATERIAL_NUMBER", Where(row) + " 字段 " + std::string(field) + " 不是范围内的有限数。");
    return value;
}

std::uint32_t Integer(const TerrainMaterialRow& row, std::string_view field,
    std::uint32_t minimum, std::uint32_t maximum) {
    const auto value = Number(row, field, minimum, maximum);
    if (value != std::floor(value))
        Fail("TERRAIN_MATERIAL_NUMBER", Where(row) + " 字段 " + std::string(field) + " 必须为整数。");
    return static_cast<std::uint32_t>(value);
}

std::string SafePath(std::string path, const TerrainMaterialRow& row) {
    const auto canonical = Resources::CanonicalImportPath(path);
    if (!canonical.Succeeded())
        Fail("TERRAIN_MATERIAL_PATH", Where(row) + " 含不安全的资源路径：" + path);
    return *canonical.value;
}

std::string TexturePath(const TerrainMaterialRow& row, std::string_view dir, std::string_view file) {
    auto path = std::string(Required(row, dir)) + '\\' + std::string(Required(row, file));
    if (!Text::LowerAscii(path).ends_with(".blp")) path += ".blp";
    return SafePath(std::move(path), row);
}

std::vector<TerrainMaterialRow> ReadRows(const Resources::ResourceResolver& resolver,
    std::string_view path, std::string_view idColumn, const TerrainMaterialLimits& limits,
    Resources::ResourceLocation& tableLocation) {
    const Resources::TerrainTableLimits tableLimits{limits.maxTableBytes, limits.maxRowsPerTable,
        limits.maxCellsPerTable, limits.maxCellBytes};
    auto captured = Resources::CaptureTerrainTable(resolver, path, idColumn, tableLimits);
    // 保留渲染API的既有诊断合同；ResourceResolver/SlkCells/Hash底层诊断原样传递。
    for (auto& diagnostic : captured.diagnostics) {
        if (diagnostic.subsystem != "Resources.TerrainTable") continue;
        diagnostic.subsystem = kSubsystem;
        if (diagnostic.code == "TERRAIN_TABLE_BUDGET") diagnostic.code = "TERRAIN_MATERIAL_TABLE_BUDGET";
        else if (diagnostic.code.starts_with("TERRAIN_TABLE_"))
            diagnostic.code = "TERRAIN_MATERIAL_" + diagnostic.code.substr(std::string_view("TERRAIN_TABLE_").size());
    }
    auto snapshot = Take(std::move(captured));
    tableLocation = snapshot.Location();
    return snapshot.Rows();
}

std::array<std::uint8_t, 4> Color(const TerrainMaterialRow& row, std::string_view prefix) {
    std::array<std::uint8_t, 4> result{};
    constexpr std::array<std::string_view, 4> channels{"r", "g", "b", "a"};
    for (std::size_t index = 0; index < channels.size(); ++index)
        result[index] = static_cast<std::uint8_t>(Integer(row, std::string(prefix) + std::string(channels[index]), 0, 255));
    return result;
}

WaterMaterialDefinition ParseWater(TerrainMaterialRow row, const TerrainMaterialLimits& limits) {
    WaterMaterialDefinition water;
    water.texturePrefix = SafePath(std::string(Required(row, "texfile")), row);
    water.heightOffsetTiles = Number(row, "height", -512, 512);
    water.heightOffsetWorld = water.heightOffsetTiles * MapData::TerrainGridSnapshot::tileSize;
    water.frameCount = Integer(row, "numtex", 1, static_cast<std::uint32_t>(limits.maxWaterFrames));
    water.framesPerSecond = Number(row, "texrate", 0, 10000);
    water.textureOffset = Number(row, "texoffset", -1000000, 1000000);
    water.cells = Integer(row, "cells", 1, 4096);
    water.alphaMode = Integer(row, "alphamode", 0, 255);
    water.lighting = Integer(row, "lighting", 0, 1) != 0;
    water.impassable = Integer(row, "impassable", 0, 1) != 0;
    water.minimapColor = {
        static_cast<std::uint8_t>(Integer(row, "mmred", 0, 255)),
        static_cast<std::uint8_t>(Integer(row, "mmgreen", 0, 255)),
        static_cast<std::uint8_t>(Integer(row, "mmblue", 0, 255)),
        static_cast<std::uint8_t>(Integer(row, "mmalpha", 0, 255))};
    water.shallowMinimum = Color(row, "smin_");
    water.shallowMaximum = Color(row, "smax_");
    water.deepMinimum = Color(row, "dmin_");
    water.deepMaximum = Color(row, "dmax_");
    water.source = std::move(row);
    return water;
}

std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index)
        result |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8);
    return result;
}

enum class BlpMipScope { AllDeclared, First };

TerrainTextureMetadata InspectBlp(const Resources::ResolvedResource& resource, const TerrainMaterialLimits& limits,
    BlpMipScope scope = BlpMipScope::AllDeclared) {
    const std::span<const std::byte> bytes(resource.bytes);
    const auto error = [&](std::string message) {
        Fail("TERRAIN_MATERIAL_BLP", resource.location.canonicalPath + "：" + std::move(message));
    };
    if (bytes.size() < 156 || bytes[0] != std::byte{'B'} || bytes[1] != std::byte{'L'}
        || bytes[2] != std::byte{'P'} || bytes[3] != std::byte{'1'})
        error("需要完整的经典 BLP1 头。");
    TerrainTextureMetadata result{resource.location, U32(bytes, 12), U32(bytes, 16), U32(bytes, 4), U32(bytes, 8)};
    if (result.width == 0 || result.height == 0 || result.width > limits.maxTextureDimension
        || result.height > limits.maxTextureDimension)
        error("纹理尺寸无效或超出预算。");
    std::size_t payloadStart = 0;
    if (result.compression == 0) {
        if (bytes.size() < 160 || U32(bytes, 156) > bytes.size() - 160) error("JPEG 共享头越界。");
        payloadStart = 160 + U32(bytes, 156);
    } else if (result.compression == 1) {
        payloadStart = 156 + 1024;
        if (bytes.size() < payloadStart) error("调色板被截断。");
        if (result.alphaBits != 0 && result.alphaBits != 1 && result.alphaBits != 4 && result.alphaBits != 8)
            error("不支持的调色板 alpha 位数。");
    } else error("不支持的 BLP1 压缩类型。");
    // 元数据查询保留全声明范围校验；单帧水面只解码 mip0，不能因未消费的后级损坏丢掉可用像素。
    const std::size_t levels = scope == BlpMipScope::First ? 1 : 16;
    for (std::size_t level = 0; level < levels; ++level) {
        const auto offset = U32(bytes, 28 + level * 4);
        const auto count = U32(bytes, 92 + level * 4);
        if (level != 0 && offset == 0 && count == 0) continue;
        if (offset < payloadStart || count == 0 || offset > bytes.size() || count > bytes.size() - offset)
            error("mip " + std::to_string(level) + " 范围无效或被截断。");
        if (result.compression == 1) {
            const auto pixels = static_cast<std::size_t>(std::max(1u, result.width >> level))
                * std::max(1u, result.height >> level);
            const auto minimum = pixels + (pixels * result.alphaBits + 7) / 8;
            if (count < minimum) error("调色板 mip 的索引或 alpha 平面被截断。");
        }
    }
    return result;
}

class TextureReader {
public:
    TextureReader(const Resources::ResourceResolver& resolver, const TerrainMaterialLimits& limits)
        : resolver_(resolver), limits_(limits) {}

    TerrainTextureMetadata Read(std::string_view path) {
        const auto canonical = Take(Resources::CanonicalImportPath(path));
        if (const auto found = cache_.find(canonical); found != cache_.end()) return found->second;
        const auto resource = Take(resolver_.Read(canonical));
        if (resource.bytes.size() > limits_.maxTextureBytes
            || resource.bytes.size() > limits_.maxTotalTextureBytes - bytes_)
            Fail("TERRAIN_MATERIAL_TEXTURE_BUDGET", canonical + " 超出纹理读取字节预算。");
        auto metadata = InspectBlp(resource, limits_);
        const auto pixels = static_cast<std::size_t>(metadata.width) * metadata.height;
        if (pixels > limits_.maxTotalTexturePixels - pixels_)
            Fail("TERRAIN_MATERIAL_TEXTURE_BUDGET", canonical + " 超出纹理像素计数预算（尚未解码像素）。");
        bytes_ += resource.bytes.size();
        pixels_ += pixels;
        cache_.emplace(canonical, metadata);
        return metadata;
    }

private:
    const Resources::ResourceResolver& resolver_;
    const TerrainMaterialLimits& limits_;
    std::size_t bytes_ = 0;
    std::size_t pixels_ = 0;
    std::map<std::string, TerrainTextureMetadata, std::less<>> cache_;
};

std::string WaterFramePath(std::string_view prefix, std::uint32_t index) {
    return std::string(prefix) + (index < 10 ? "0" : "") + std::to_string(index) + ".blp";
}

template <typename T, typename Operation>
Result<T> Checked(Operation operation) {
    try {
        return Result<T>::Success(operation());
    } catch (Failure& failure) {
        Result<T> result;
        result.diagnostics = std::move(failure.diagnostics);
        return result;
    } catch (const std::bad_alloc&) {
        return Result<T>::Failure(std::string(kSubsystem), "TERRAIN_MATERIAL_ALLOCATION", "材质目录内存分配失败。");
    } catch (const std::length_error&) {
        return Result<T>::Failure(std::string(kSubsystem), "TERRAIN_MATERIAL_ALLOCATION", "材质目录超出容器容量。");
    }
}

} // namespace

Result<TerrainMaterialCatalog> LoadTerrainMaterialCatalog(
    const Resources::ResourceResolver& resolver, const TerrainMaterialLimits& limits) {
    return Checked<TerrainMaterialCatalog>([&] {
        ValidateLimits(limits);
        TerrainMaterialCatalog catalog;
        auto groundRows = ReadRows(resolver, "TerrainArt\\Terrain.slk", "tileid", limits, catalog.tables[0]);
        auto cliffRows = ReadRows(resolver, "TerrainArt\\CliffTypes.slk", "cliffid", limits, catalog.tables[1]);
        auto waterRows = ReadRows(resolver, "TerrainArt\\Water.slk", "waterid", limits, catalog.tables[2]);
        for (auto& row : groundRows) {
            GroundMaterialDefinition definition;
            definition.texturePath = TexturePath(row, "dir", "file");
            definition.source = std::move(row);
            const auto id = definition.source.id;
            catalog.ground.emplace(id, std::move(definition));
        }
        for (auto& row : cliffRows) {
            CliffMaterialDefinition definition;
            definition.declaredTexturePath = TexturePath(row, "texdir", "texfile");
            definition.cliffModelDirectory = SafePath(std::string(Required(row, "cliffmodeldir")), row);
            definition.rampModelDirectory = SafePath(std::string(Required(row, "rampmodeldir")), row);
            definition.groundTile = Required(row, "groundtile");
            const auto upper = row.fields.find("uppertile");
            if (upper != row.fields.end() && upper->second != "_") definition.upperTile = upper->second;
            if (!Rawcode(definition.groundTile) || (!definition.upperTile.empty() && !Rawcode(definition.upperTile)))
                Fail("TERRAIN_MATERIAL_ID", Where(row) + " 的 groundTile/upperTile 不是四字节 rawcode。");
            definition.cliffClass = Required(row, "cliffclass");
            definition.source = std::move(row);
            const auto id = definition.source.id;
            catalog.cliffs.emplace(id, std::move(definition));
        }
        for (auto& row : waterRows) {
            auto definition = ParseWater(std::move(row), limits);
            const auto id = definition.source.id;
            catalog.water.emplace(id, std::move(definition));
        }
        return catalog;
    });
}

Result<TerrainMaterialSet> ResolveTerrainMaterialSet(const TerrainMaterialCatalog& catalog,
    const MapData::TerrainHeader& terrain, const Resources::ResourceResolver& resolver,
    const TerrainMaterialLimits& limits) {
    auto result = Checked<TerrainMaterialSet>([&] {
        ValidateLimits(limits);
        if (terrain.version != 11) Fail("TERRAIN_MATERIAL_VERSION", "材质目录只支持经典 W3E v11。");
        if (terrain.groundTilesets.size() > limits.maxTilesetEntries || terrain.cliffTilesets.size() > limits.maxTilesetEntries)
            Fail("TERRAIN_MATERIAL_TILESET_BUDGET", "W3E 材质 rawcode 表超出预算。");
        TerrainMaterialSet materials;
        TextureReader textures(resolver, limits);
        for (const auto& id : terrain.groundTilesets) {
            const auto found = catalog.ground.find(id);
            if (found == catalog.ground.end()) Fail("TERRAIN_MATERIAL_UNKNOWN_GROUND", "Terrain.slk 中没有地面 rawcode：" + id);
            GroundMaterial ground;
            ground.definition = found->second;
            ground.texture = textures.Read(ground.definition.texturePath);
            const auto width = ground.texture.width, height = ground.texture.height;
            if (height % 4 != 0 || (width != height && width != height * 2))
                Fail("TERRAIN_MATERIAL_ATLAS", ground.texture.resource.canonicalPath + " 不是 4×4 或 8×4 方形单元格 atlas。");
            ground.atlasColumns = width == height ? 4 : 8;
            ground.atlasCellPixels = height / 4;
            materials.ground.push_back(std::move(ground));
        }
        for (const auto& id : terrain.cliffTilesets) {
            const auto found = catalog.cliffs.find(id);
            if (found == catalog.cliffs.end()) Fail("TERRAIN_MATERIAL_UNKNOWN_CLIFF", "CliffTypes.slk 中没有悬崖 rawcode：" + id);
            CliffMaterial cliff;
            cliff.definition = found->second;
            cliff.declaredTexture = textures.Read(cliff.definition.declaredTexturePath);
            materials.cliffs.push_back(std::move(cliff));
        }
        const auto waterId = std::string(1, static_cast<char>(terrain.mainTileset)) + "Sha";
        const auto water = catalog.water.find(waterId);
        if (water == catalog.water.end()) Fail("TERRAIN_MATERIAL_UNKNOWN_WATER", "Water.slk 中没有地形对应的水材质：" + waterId);
        materials.water.definition = water->second;
        // Catalog 是普通值对象，调用方可能修改过它；重新检查输出数值与循环约束。
        const auto& definition = materials.water.definition;
        if (definition.frameCount == 0 || definition.frameCount > limits.maxWaterFrames)
            Fail("TERRAIN_MATERIAL_WATER_FRAMES", "水材质帧数无效或超出预算。");
        if (!std::isfinite(definition.heightOffsetTiles) || std::abs(definition.heightOffsetTiles) > 512
            || definition.heightOffsetWorld != definition.heightOffsetTiles * MapData::TerrainGridSnapshot::tileSize
            || !std::isfinite(definition.framesPerSecond) || definition.framesPerSecond < 0 || definition.framesPerSecond > 10000
            || !std::isfinite(definition.textureOffset) || std::abs(definition.textureOffset) > 1000000
            || definition.cells == 0 || definition.cells > 4096 || definition.alphaMode > 255)
            Fail("TERRAIN_MATERIAL_NUMBER", "水材质目录含有无效、超限或不一致的派生数值。");
        for (std::uint32_t frame = 0; frame < definition.frameCount; ++frame) {
            const auto path = WaterFramePath(definition.texturePrefix, frame);
            auto texture = textures.Read(path);
            if (!materials.water.frames.empty() && (texture.width != materials.water.frames.front().width
                || texture.height != materials.water.frames.front().height))
                Fail("TERRAIN_MATERIAL_WATER_DIMENSIONS", texture.resource.canonicalPath + " 与水动画首帧的尺寸不一致。");
            materials.water.frames.push_back(std::move(texture));
        }
        return materials;
    });
    if (result.Succeeded() && !result.value->cliffs.empty())
        result.diagnostics.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem),
            "TERRAIN_MATERIAL_CLIFF_SUBSTITUTION_UNVERIFIED",
            "悬崖只定位 SLK 声明纹理；原版按 tileset 替换资源的身份尚未验证，不能作为完整悬崖贴图结果。", std::nullopt});
    return result;
}

Result<std::vector<std::optional<CliffTypeSource>>> ResolveTerrainCliffTypes(
    const TerrainMaterialCatalog& catalog, const MapData::TerrainHeader& terrain) {
    return Checked<std::vector<std::optional<CliffTypeSource>>>([&] {
        if (terrain.cliffTilesets.size() > TerrainMaterialLimits{}.maxTilesetEntries
            || catalog.tables[1].canonicalPath.empty())
            Fail("TERRAIN_CLIFF_PREVIEW_CATALOG", "悬崖目录缺少来源身份或 palette 超出已有预算。");
        std::vector<std::optional<CliffTypeSource>> types;
        types.reserve(terrain.cliffTilesets.size());
        for (const auto& id : terrain.cliffTilesets) {
            const auto found = catalog.cliffs.find(id);
            if (found == catalog.cliffs.end()) { types.emplace_back(); continue; }
            const auto& row = found->second;
            if (row.source.id != id || row.source.table.canonicalPath != catalog.tables[1].canonicalPath)
                Fail("TERRAIN_CLIFF_PREVIEW_CATALOG", "CliffTypes.slk 行身份与 W3E 悬崖 rawcode 不一致：" + id);
            types.push_back(CliffTypeSource{row.cliffModelDirectory, row.declaredTexturePath, row.rampModelDirectory,
                row.groundTile, row.upperTile});
        }
        return types;
    });
}

Result<TerrainWaterFrame> LoadTerrainWaterFrame(const WaterMaterialDefinition& definition, std::uint32_t index,
    const Resources::ResourceResolver& resolver, const TerrainMaterialLimits& limits) {
    return Checked<TerrainWaterFrame>([&] {
        ValidateLimits(limits);
        if (definition.frameCount == 0 || definition.frameCount > limits.maxWaterFrames)
            Fail("TERRAIN_MATERIAL_WATER_FRAMES", "水材质帧数无效或超出预算。");
        if (index >= definition.frameCount)
            Fail("TERRAIN_MATERIAL_WATER_FRAME_INDEX", "所请求的水面帧超出 Water.slk 声明的范围。");
        const auto prefix = Take(Resources::CanonicalImportPath(definition.texturePrefix));
        const auto selectedPath = WaterFramePath(prefix, index);
        auto selected = Take(resolver.Read(selectedPath));
        if (selected.bytes.size() > limits.maxTextureBytes || selected.bytes.size() > limits.maxTotalTextureBytes)
            Fail("TERRAIN_MATERIAL_TEXTURE_BUDGET", selectedPath + " 超出纹理读取字节预算。");
        const auto selectedMetadata = InspectBlp(selected, limits, BlpMipScope::First);
        const auto selectedPixels = static_cast<std::size_t>(selectedMetadata.width) * selectedMetadata.height;
        if (selectedPixels > limits.maxTotalTexturePixels)
            Fail("TERRAIN_MATERIAL_TEXTURE_BUDGET", selectedPath + " 超出纹理像素预算。");
        if (index != 0) {
            const auto firstPath = WaterFramePath(prefix, 0);
            auto first = Take(resolver.Read(firstPath));
            if (first.bytes.size() > limits.maxTextureBytes
                || first.bytes.size() > limits.maxTotalTextureBytes - selected.bytes.size())
                Fail("TERRAIN_MATERIAL_TEXTURE_BUDGET", firstPath + " 与所选帧超出合计纹理读取预算。");
            const auto firstMetadata = InspectBlp(first, limits, BlpMipScope::First);
            const auto firstPixels = static_cast<std::size_t>(firstMetadata.width) * firstMetadata.height;
            if (firstPixels > limits.maxTotalTexturePixels - selectedPixels)
                Fail("TERRAIN_MATERIAL_TEXTURE_BUDGET", firstPath + " 与所选帧超出合计纹理像素预算。");
            if (selectedMetadata.width != firstMetadata.width || selectedMetadata.height != firstMetadata.height)
                Fail("TERRAIN_MATERIAL_WATER_DIMENSIONS", selectedPath + " 与水动画首帧尺寸不一致。");
        }
        auto image = Take(Image::DecodeBlp(selected.bytes));
        if (!image.Valid() || image.width != selectedMetadata.width || image.height != selectedMetadata.height)
            Fail("TERRAIN_MATERIAL_BLP", selectedPath + " 解码后的尺寸与 BLP1 头不一致。");
        return TerrainWaterFrame{index, std::move(selected.location), Take(Sha256(selected.bytes)), std::move(image)};
    });
}

} // namespace War3Map::Render
