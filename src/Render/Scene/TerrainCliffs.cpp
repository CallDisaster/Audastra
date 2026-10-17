#include "War3MapCore/Render/TerrainCliffs.h"

#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <new>
#include <unordered_map>

namespace War3Map::Render {
namespace {

constexpr std::string_view kSubsystem = "Render.TerrainCliffs";
constexpr std::size_t kChunkVertices = 65536;
constexpr std::uint8_t kReservedCliffType = 15;

std::array<std::size_t, 4> GroundCellCorners(const MapData::TerrainGridSnapshot& terrain,
    std::uint32_t x, std::uint32_t y) {
    const auto sw = static_cast<std::size_t>(y) * terrain.header.width + x;
    return {sw, sw + 1, sw + terrain.header.width, sw + terrain.header.width + 1};
}

// 本机 1.27a 0x6F725350：材质取值次序不是模型形状字母的次序；非15的非法索引也不能跳过。
std::uint8_t OrdinaryCliffTypeIndex(const MapData::TerrainGridSnapshot& terrain,
    std::uint32_t x, std::uint32_t y) {
    for (const auto index : GroundCellCorners(terrain, x, y)) {
        const auto type = terrain.vertices[index].CliffTextureIndex();
        if (type != kReservedCliffType) return type;
    }
    const auto lastX = x < terrain.header.width - 2 ? x + 2 : terrain.header.width - 1;
    const auto lastY = y < terrain.header.height - 2 ? y + 2 : terrain.header.height - 1;
    for (auto nx = x > 1 ? x - 2 : 0; nx <= lastX; ++nx)
        for (auto ny = y > 1 ? y - 2 : 0; ny <= lastY; ++ny) {
            const auto type = terrain.vertices[static_cast<std::size_t>(ny) * terrain.header.width + nx].CliffTextureIndex();
            if (type != kReservedCliffType) return type;
        }
    return 0;
}

struct CachedModel {
    std::optional<MdxModel> model;
};

// 按贴图路径把烘焙几何分块写出，块满 65536 顶点时开新块。
class MeshSink {
public:
    explicit MeshSink(std::vector<BakedModelMesh>& output) : output_(output) {}
    BakedModelMesh& Reserve(const std::string& texture, std::size_t vertices) {
        auto& index = current_[texture];
        if (!index || output_[*index - 1].vertices.size() + vertices > kChunkVertices) {
            output_.push_back({{}, {}, texture});
            index = output_.size();
        }
        return output_[*index - 1];
    }

private:
    std::vector<BakedModelMesh>& output_;
    std::map<std::string, std::optional<std::size_t>> current_;
};

// 一次模型放置：外沿四角（西南、西北、东北、东南）与覆盖的格数（x 向 columns、y 向 rows）。
struct Footprint {
    std::array<const MapData::TerrainVertex*, 4> corners{}; // SW, NW, NE, SE
    std::uint32_t columns = 1, rows = 1;
};

// 坡道角记 L/H/X、其余 A/B/C；层级差超过 2 时返回空。
std::optional<std::string> ShapeLetters(const Footprint& footprint, bool rampLetters) {
    std::uint8_t base = 15;
    for (const auto* corner : footprint.corners) base = std::min(base, corner->CliffLevel());
    std::string name;
    for (const auto* corner : footprint.corners) {
        const auto difference = corner->CliffLevel() - base;
        if (difference > 2) return std::nullopt;
        name.push_back((rampLetters && corner->IsRamp() ? "LHX" : "ABC")[difference]);
    }
    return name;
}

// z'=z+h(x,y)：法线使用高度形变 Jacobian 的逆转置，保留作者硬边，不重建/焊接几何法线。
// 边界取 footprint 内侧单边导数，严格外侧仅将对应轴导数清零，与位置的 clamp 一致。
std::array<float, 3> ReliefNormal(const std::array<float, 3>& normal, const std::array<float, 3>& position,
    const std::array<float, 4>& relief, float width, float depth, float u, float v) {
    // 原烘焙提交会拒绝非有限或 abs>1e7 的分量；不能借归一化扩大来源接受范围。
    if (std::any_of(normal.begin(), normal.end(), [](float n) { return !std::isfinite(n) || std::abs(n) > 1e7f; }))
        return normal;
    const float squared = normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
    // 保留 shader 既有的小向量 up 近似，也保留精确零/带符号零的源字节。
    if (!(squared > 1e-12f) || normal[2] == 0) return normal;
    if (std::all_of(relief.begin(), relief.end(), [&](float value) { return value == relief.front(); })) return normal;
    const double hx = position[0] < -width || position[0] > 0 ? 0.0
        : ((1.0 - v) * (static_cast<double>(relief[3]) - relief[0])
            + v * (static_cast<double>(relief[2]) - relief[1])) / width;
    const double hy = position[1] < 0 || position[1] > depth ? 0.0
        : ((1.0 - u) * (static_cast<double>(relief[1]) - relief[0])
            + u * (static_cast<double>(relief[2]) - relief[3])) / depth;
    if ((hx == 0 && hy == 0) || !std::isfinite(hx) || !std::isfinite(hy)) return normal;
    const std::array<double, 3> transformed{normal[0] - hx * normal[2], normal[1] - hy * normal[2], normal[2]};
    const double scale = std::max({std::abs(transformed[0]), std::abs(transformed[1]), std::abs(transformed[2])});
    const std::array<double, 3> scaled{transformed[0] / scale, transformed[1] / scale, transformed[2] / scale};
    const double length = std::sqrt(scaled[0] * scaled[0] + scaled[1] * scaled[1] + scaled[2] * scaled[2]);
    return {static_cast<float>(scaled[0] / length), static_cast<float>(scaled[1] / length), static_cast<float>(scaled[2] / length)};
}

// 模型局部原点在外沿东侧（x 向负方向延伸 columns 格、y 向正方向 rows 格）；顶点按外沿四角
// 去掉层级后的地面起伏双线性抬升。
void Bake(MeshSink& sink, const MdxModel& model, const Footprint& footprint, const std::string& replaceable) {
    const float width = 128.0f * static_cast<float>(footprint.columns), depth = 128.0f * static_cast<float>(footprint.rows);
    std::uint8_t base = 15;
    for (const auto* corner : footprint.corners) base = std::min(base, corner->CliffLevel());
    std::array<float, 4> relief{}; // SW, NW, NE, SE
    for (std::size_t i = 0; i < 4; ++i)
        relief[i] = footprint.corners[i]->position[2] - 128.0f * (static_cast<float>(footprint.corners[i]->CliffLevel()) - 2.0f);
    const float originX = footprint.corners[0]->position[0] + width, originY = footprint.corners[0]->position[1];
    const float baseZ = 128.0f * (static_cast<float>(base) - 2.0f);
    for (const auto& geoset : model.geosets) {
        if (!geoset.visibleInStand || geoset.indices.empty() || geoset.positions.size() > kChunkVertices) continue;
        const auto& material = model.materials[geoset.materialId];
        if (material.layers.empty()) continue;
        const auto& texture = model.textures[material.layers.front().textureId];
        const auto& texturePath = texture.replaceableId == 0 ? texture.path : replaceable;
        auto& mesh = sink.Reserve(texturePath, geoset.positions.size());
        const auto first = static_cast<std::uint16_t>(mesh.vertices.size());
        for (std::size_t i = 0; i < geoset.positions.size(); ++i) {
            const auto& p = geoset.positions[i];
            const float u = std::clamp((p[0] + width) / width, 0.0f, 1.0f), v = std::clamp(p[1] / depth, 0.0f, 1.0f);
            const float lift = (relief[0] * (1 - u) + relief[3] * u) * (1 - v) + (relief[1] * (1 - u) + relief[2] * u) * v;
            // 缺失法线保留既有 up 近似；仅对实际存在且通过原门的非小向量修正方向。
            const auto n = i < geoset.normals.size()
                ? ReliefNormal(geoset.normals[i], p, relief, width, depth, u, v) : std::array<float, 3>{0, 0, 1};
            const auto uv = i < geoset.uvs.size() ? geoset.uvs[i] : std::array<float, 2>{0, 0};
            mesh.vertices.push_back({originX + p[0], originY + p[1], baseZ + p[2] + lift, n[0], n[1], n[2], uv[0], uv[1]});
        }
        for (const auto index : geoset.indices) mesh.indices.push_back(static_cast<std::uint16_t>(first + index));
    }
}

} // namespace

Result<TerrainGroundSelection> ResolveTerrainGroundSelection(const MapData::TerrainGridSnapshot& terrain,
    std::span<const std::optional<CliffTypeSource>> cliffTypes) {
    using SelectionResult = Result<TerrainGroundSelection>;
    const auto width = terrain.header.width, height = terrain.header.height;
    if (terrain.header.version != 11 || width < 2 || height < 2 || width > 2049 || height > 2049
        || static_cast<std::uint64_t>(width) * height != terrain.vertices.size()
        || cliffTypes.size() != terrain.header.cliffTilesets.size()
        || terrain.header.groundTilesets.empty() || terrain.header.groundTilesets.size() > 256)
        return SelectionResult::Failure(std::string(kSubsystem), "TERRAIN_GROUND_SELECTION_GRID",
            "普通悬崖地面派生需要有效经典网格和与原 palette 同序的目录。");
    try {
        TerrainGroundSelection output;
        output.indices.reserve(terrain.vertices.size());
        for (const auto& vertex : terrain.vertices) output.indices.push_back(vertex.GroundTextureIndex());
        constexpr std::array<std::array<std::uint32_t, 2>, 4> offsets{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
            bool hasRamp = false;
            for (auto ny = y > 0 ? y - 1 : 0; ny <= std::min(y + 1, height - 1); ++ny)
                for (auto nx = x > 0 ? x - 1 : 0; nx <= std::min(x + 1, width - 1); ++nx)
                    hasRamp |= terrain.vertices[static_cast<std::size_t>(ny) * width + nx].IsRamp();
            if (hasRamp) { ++output.rampNeighborhoodVertices; continue; }
            std::optional<std::array<std::uint32_t, 2>> selectedCell;
            std::uint8_t minimumLevel = 0;
            // 0x6F72E210：相邻格按 (x,y)、(x-1,y)、(x,y-1)、(x-1,y-1)，首个异层格胜出。
            for (const auto& delta : offsets) {
                if (x < delta[0] || y < delta[1]) continue;
                const auto cx = x - delta[0], cy = y - delta[1];
                if (cx + 1 >= width || cy + 1 >= height) continue;
                const auto corners = GroundCellCorners(terrain, cx, cy);
                auto low = terrain.vertices[corners[0]].CliffLevel(), high = low;
                for (const auto corner : corners) {
                    low = std::min(low, terrain.vertices[corner].CliffLevel());
                    high = std::max(high, terrain.vertices[corner].CliffLevel());
                }
                if (low != high) { selectedCell = std::array{cx, cy}; minimumLevel = low; break; }
            }
            if (!selectedCell) { ++output.retainedFlatVertices; continue; }
            ++output.ordinaryCliffVertices;
            const auto typeIndex = OrdinaryCliffTypeIndex(terrain, (*selectedCell)[0], (*selectedCell)[1]);
            if (typeIndex >= cliffTypes.size() || !cliffTypes[typeIndex]) { ++output.unknownCliffVertices; continue; }
            const auto& type = *cliffTypes[typeIndex];
            const auto index = static_cast<std::size_t>(y) * width + x;
            const bool lower = terrain.vertices[index].CliffLevel() == minimumLevel;
            std::string_view paired = lower ? type.groundTile : type.upperTile;
            if (!lower && (paired.empty() || paired.front() == '_')) paired = type.groundTile;
            // 不模拟原版运行时扩充 palette 或 NOTH 派生15，也不改原始地形以填补未知语义。
            const auto& palette = terrain.header.groundTilesets;
            const auto found = std::find(palette.begin(), palette.end(), paired);
            if (paired.empty() || paired.front() == '_' || paired == "NOTH" || found == palette.end()) {
                ++output.missingPairedGroundVertices; continue;
            }
            const auto selected = static_cast<std::uint8_t>(found - palette.begin());
            // 原版派生半字节 14=使用 raw、15=NOTH；本单元不把它们当普通 palette 索引。
            if (selected >= 14) { ++output.missingPairedGroundVertices; continue; }
            ++output.derivedVertices;
            output.changedVertices += selected != output.indices[index];
            output.indices[index] = selected;
        }
        auto result = SelectionResult::Success(std::move(output));
        const auto& coverage = *result.value;
        result.diagnostics.push_back({DiagnosticSeverity::Info, std::string(kSubsystem), "TERRAIN_GROUND_SELECTION_SCOPE",
            "普通无坡道悬崖地面：派生 " + std::to_string(coverage.derivedVertices) + " 顶点、改变 "
            + std::to_string(coverage.changedVertices) + "；坡道邻域 " + std::to_string(coverage.rampNeighborhoodVertices)
            + " 与无普通悬崖邻格 " + std::to_string(coverage.retainedFlatVertices)
            + " 顶点保留原地面。坡道及平层内部标志不在本次覆盖；未改原地形或 palette。", std::nullopt});
        if (coverage.unknownCliffVertices || coverage.missingPairedGroundVertices)
            result.diagnostics.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem), "TERRAIN_GROUND_SELECTION_UNRESOLVED",
                "悬崖类型未解析 " + std::to_string(coverage.unknownCliffVertices) + "、配对地面缺失/保留槽/未支持 "
                + std::to_string(coverage.missingPairedGroundVertices) + " 顶点保留原地面，不猜测或扩充 palette。", std::nullopt});
        return result;
    } catch (const std::bad_alloc&) {
        return SelectionResult::Failure(std::string(kSubsystem), "TERRAIN_GROUND_SELECTION_MEMORY", "地面派生索引分配失败。");
    }
}

std::optional<std::string> CliffShapeName(std::uint8_t southWest, std::uint8_t northWest,
    std::uint8_t northEast, std::uint8_t southEast) {
    const auto base = std::min({southWest, northWest, northEast, southEast});
    std::string name;
    for (const auto level : {southWest, northWest, northEast, southEast}) {
        if (level - base > 2) return std::nullopt;
        name.push_back(static_cast<char>('A' + (level - base)));
    }
    return name;
}

std::size_t ApplyRampHeights(MapData::TerrainGridSnapshot& terrain) {
    const auto width = terrain.header.width, height = terrain.header.height;
    if (width == 0 || height == 0 || static_cast<std::uint64_t>(width) * height != terrain.vertices.size()) return 0;
    std::vector<float> lift(terrain.vertices.size(), 0.0f);
    std::size_t lifted = 0;
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        if (const auto heightOffset = MapData::TerrainRampHeightOffset(terrain, x, y)) {
            lift[static_cast<std::size_t>(y) * width + x] = *heightOffset;
            ++lifted;
        }
    }
    if (lifted == 0) return 0;
    for (std::size_t i = 0; i < lift.size(); ++i) terrain.vertices[i].position[2] += lift[i];
    terrain.minimum[2] = terrain.maximum[2] = terrain.vertices.front().position[2];
    for (const auto& vertex : terrain.vertices) {
        terrain.minimum[2] = std::min(terrain.minimum[2], vertex.position[2]);
        terrain.maximum[2] = std::max(terrain.maximum[2], vertex.position[2]);
    }
    return lifted;
}

Result<TerrainCliffs> BuildTerrainCliffs(const MapData::TerrainGridSnapshot& terrain,
    std::span<const std::optional<CliffTypeSource>> cliffTypes, const ModelFileReader& read) {
    const auto width = terrain.header.width, height = terrain.header.height;
    if (!read) return Result<TerrainCliffs>::Failure(std::string(kSubsystem), "TERRAIN_CLIFF_READER", "没有提供模型读取器。");
    if (width < 2 || height < 2 || static_cast<std::uint64_t>(width) * height != terrain.vertices.size()
        || static_cast<std::uint64_t>(width - 1) * (height - 1) != terrain.tiles.size())
        return Result<TerrainCliffs>::Failure(std::string(kSubsystem), "TERRAIN_CLIFF_GRID", "地形网格尺寸无效。");
    TerrainCliffs output;
    output.coveredTiles.assign(terrain.tiles.size(), 0);
    MeshSink sink(output.meshes);
    std::unordered_map<std::string, CachedModel> cache;
    std::map<std::string, std::size_t> missing;
    const auto load = [&](const std::string& path) -> const MdxModel* {
        auto found = cache.find(path);
        if (found == cache.end()) {
            CachedModel entry;
            if (auto bytes = read(path); bytes.Succeeded())
                if (auto model = ParseMdx(*bytes.value); model.Succeeded()) entry.model = std::move(*model.value);
            found = cache.emplace(path, std::move(entry)).first;
        }
        return found->second.model ? &*found->second.model : nullptr;
    };
    const auto vertexAt = [&](std::uint32_t x, std::uint32_t y) { return &terrain.vertices[static_cast<std::size_t>(y) * width + x]; };
    const auto tileAt = [&](std::uint32_t x, std::uint32_t y) { return static_cast<std::size_t>(y) * (width - 1) + x; };
    // 坡道仍保留既有预览选择；普通悬崖在下方使用共用的原版材质选择。
    const auto typeOf = [&](const Footprint& footprint) -> const CliffTypeSource* {
        for (const auto* v : footprint.corners) {
            const auto index = v->CliffTextureIndex();
            if (index != kReservedCliffType && index < cliffTypes.size() && cliffTypes[index]) return &*cliffTypes[index];
        }
        return nullptr;
    };
    // 变体：原版每种形状的变体数不同，缺文件时逐级退到 0。
    const auto loadShape = [&](const std::string& directory, const std::string& shape, int variation) -> const MdxModel* {
        for (; variation >= 0; --variation)
            if (const auto* model = load("Doodads\\Terrain\\" + directory + "\\" + directory + shape + std::to_string(variation) + ".mdx"))
                return model;
        return nullptr;
    };
    for (std::uint32_t y = 0; y + 1 < height; ++y) for (std::uint32_t x = 0; x + 1 < width; ++x) {
        const auto tileIndex = tileAt(x, y);
        const Footprint single{{vertexAt(x, y), vertexAt(x, y + 1), vertexAt(x + 1, y + 1), vertexAt(x + 1, y)}};
        const auto& c = single.corners;
        const bool levelsDiffer = std::any_of(c.begin(), c.end(), [&](const auto* v) { return v->CliffLevel() != c[0]->CliffLevel(); });
        const bool anyRamp = std::any_of(c.begin(), c.end(), [](const auto* v) { return v->IsRamp(); });
        const bool allRamp = std::all_of(c.begin(), c.end(), [](const auto* v) { return v->IsRamp(); });
        if (levelsDiffer && allRamp) { ++output.rampSurfaceTiles; continue; }
        if (levelsDiffer) ++output.cliffTiles;
        if (output.coveredTiles[tileIndex]) { output.placedTiles += levelsDiffer; continue; }
        if (anyRamp) {
            // 坡道边：以本格为起点，沿 y（坡道角在东/西列）或沿 x（坡道角在南/北行）覆盖两格。
            bool placed = false;
            for (const bool alongY : {true, false}) {
                const std::uint32_t columns = alongY ? 1 : 2, rows = alongY ? 2 : 1;
                if (x + columns >= width || y + rows >= height) continue;
                const auto second = alongY ? tileAt(x, y + 1) : tileAt(x + 1, y);
                if (output.coveredTiles[second]) continue;
                const Footprint piece{{vertexAt(x, y), vertexAt(x, y + rows), vertexAt(x + columns, y + rows), vertexAt(x + columns, y)},
                    columns, rows};
                const auto ramp = [&](std::size_t i) { return piece.corners[i]->IsRamp(); };
                const bool sided = alongY ? ramp(0) == ramp(1) && ramp(2) == ramp(3) && ramp(0) != ramp(3)
                                          : ramp(0) == ramp(3) && ramp(1) == ramp(2) && ramp(0) != ramp(1);
                if (!sided) continue;
                const auto* type = typeOf(piece);
                const auto shape = ShapeLetters(piece, true);
                if (!type || type->rampDirectory.empty() || !shape) continue;
                const auto* model = loadShape(type->rampDirectory, *shape, piece.corners[0]->CliffVariation());
                if (!model) continue;
                Bake(sink, *model, piece, type->texturePath);
                output.coveredTiles[tileIndex] = output.coveredTiles[second] = 1;
                output.placedTiles += levelsDiffer;
                ++output.rampPieces;
                placed = true;
                break;
            }
            if (placed || !levelsDiffer) continue;
        }
        if (!levelsDiffer) continue;
        const auto shape = ShapeLetters(single, false);
        if (!shape) { ++output.tooHighTiles; continue; }
        const auto typeIndex = OrdinaryCliffTypeIndex(terrain, x, y);
        const auto* type = anyRamp ? typeOf(single)
            : typeIndex < cliffTypes.size() && cliffTypes[typeIndex] ? &*cliffTypes[typeIndex] : nullptr;
        if (!type || type->modelDirectory.empty()) { ++output.unknownTypeTiles; continue; }
        const auto* model = loadShape(type->modelDirectory, *shape, c[0]->CliffVariation());
        if (!model) { ++output.missingModelTiles; ++missing[type->modelDirectory + *shape]; continue; }
        Bake(sink, *model, single, type->texturePath);
        output.coveredTiles[tileIndex] = 1;
        ++output.placedTiles;
    }
    if (!missing.empty()) {
        std::string text = "悬崖模型不可用，相关格保留高度色：";
        std::size_t listed = 0;
        for (const auto& [name, count] : missing) {
            if (listed++ == 12) { text += " 等"; break; }
            text += (listed > 1 ? "、" : "") + name + "（" + std::to_string(count) + "）";
        }
        output.warnings.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem), "TERRAIN_CLIFF_MODEL", std::move(text), std::nullopt});
    }
    return Result<TerrainCliffs>::Success(std::move(output));
}

} // namespace War3Map::Render
