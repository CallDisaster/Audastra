#include "War3MapCore/MapData/TerrainEditing.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <utility>

namespace War3Map::MapData {
namespace {

constexpr std::uint32_t kMaximumRadius = 16;
constexpr std::size_t kMaximumPoints = 65536;
constexpr std::size_t kMaximumGroundTilesets = 16;
constexpr int kMaximumCliffLevel = 14;
constexpr int kMaximumLevelStep = 2;      // 相邻顶点悬崖层级差上限（悬崖模型只有 0/1/2 级高差）
constexpr std::uint16_t kMaximumGroundRaw = 0x3fff;

Status Failure(const char* code, std::string message) { return Status::Failure("MapTerrain.Edit", code, std::move(message)); }

void PutU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>((value >> shift) & 0xff));
}
void PutU16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xff));
    out.push_back(static_cast<std::byte>(value >> 8));
}

bool Consistent(const TerrainGridSnapshot& terrain) {
    const auto& header = terrain.header;
    return header.width >= 2 && header.height >= 2
        && static_cast<std::uint64_t>(header.width) * header.height == terrain.vertices.size();
}

bool ClassicLayout(const TerrainGridSnapshot& terrain) {
    // 同时核对顶点来源；改 header.version 不能把 v12 顶点伪装成可写经典记录。
    return terrain.header.version == 11 && std::none_of(terrain.vertices.begin(), terrain.vertices.end(),
        [](const auto& vertex) { return vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw != 0; });
}

double GroundZ(std::uint16_t raw, int level) { return (static_cast<double>(raw) - 8192.0) / 4.0 + 128.0 * (level - 2); }

std::uint16_t GroundRaw(double z, int level) {
    const double raw = std::round((z - 128.0 * (level - 2)) * 4.0 + 8192.0);
    return static_cast<std::uint16_t>(std::clamp(raw, 0.0, static_cast<double>(kMaximumGroundRaw)));
}

// 原版编辑器实测分布（多张暴雪对战图统计）：0/4/8/12/16/17 各约 15%，其余逐级递减。
std::uint8_t RandomVariation(std::uint32_t seed, std::uint32_t x, std::uint32_t y) {
    std::uint64_t value = (static_cast<std::uint64_t>(seed) << 32) ^ (static_cast<std::uint64_t>(x) << 16) ^ y;
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    constexpr std::array<std::uint32_t, 4> kWeights{150, 17, 7, 2};
    std::uint32_t pick = static_cast<std::uint32_t>(value % 1004);
    for (std::uint8_t variation = 0; variation < 18; ++variation) {
        const auto weight = variation >= 16 ? 150u : kWeights[variation % 4];
        if (pick < weight) return variation;
        pick -= weight;
    }
    return 0;
}

struct Footprint {
    std::int64_t cx = 0, cy = 0;
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // 包含范围 [x0,x1]×[y0,y1]
};

std::optional<Footprint> FootprintAt(const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point, std::uint32_t radius) {
    const auto& header = terrain.header;
    const double u = (point.x - header.offset[0]) / TerrainGridSnapshot::tileSize;
    const double v = (point.y - header.offset[1]) / TerrainGridSnapshot::tileSize;
    // 有限的世界坐标仍可能远超 llround 的整数范围；先按地图范围裁剪。
    if (!std::isfinite(u) || !std::isfinite(v) || u < -0.5 || v < -0.5
        || u >= static_cast<double>(header.width) - 0.5 || v >= static_cast<double>(header.height) - 0.5) return std::nullopt;
    const auto cx = static_cast<std::int64_t>(std::llround(u)), cy = static_cast<std::int64_t>(std::llround(v));
    if (cx < 0 || cy < 0 || cx >= header.width || cy >= header.height) return std::nullopt;
    const auto low = [&](std::int64_t c) { return static_cast<std::uint32_t>(std::max<std::int64_t>(0, c - radius)); };
    return Footprint{cx, cy, low(cx), low(cy),
        static_cast<std::uint32_t>(std::min<std::int64_t>(header.width - 1, cx + radius)),
        static_cast<std::uint32_t>(std::min<std::int64_t>(header.height - 1, cy + radius))};
}

// 笔刷内的权重：方形恒为 1；圆形在半径外为空；falloff 为余弦衰减（升降、平滑用）。
std::optional<double> Weight(const TerrainBrush& brush, const Footprint& at, std::uint32_t x, std::uint32_t y, bool falloff) {
    const double dx = static_cast<double>(x) - at.cx, dy = static_cast<double>(y) - at.cy;
    const double distance = std::hypot(dx, dy);
    const double r = brush.radius;
    if (brush.shape == TerrainBrushShape::Circle && distance > r + 0.25) return std::nullopt;
    if (!falloff || brush.radius == 0) return 1.0;
    const double edge = brush.shape == TerrainBrushShape::Circle ? r + 1.0 : r * std::numbers::sqrt2 + 1.0;
    return 0.5 * (1.0 + std::cos(std::numbers::pi * std::min(distance / edge, 1.0)));
}

class Changes {
public:
    void Touch(std::uint32_t x, std::uint32_t y) {
        if (empty_) { x0_ = x1_ = x; y0_ = y1_ = y; empty_ = false; return; }
        x0_ = std::min(x0_, x); x1_ = std::max(x1_, x); y0_ = std::min(y0_, y); y1_ = std::max(y1_, y);
    }
    [[nodiscard]] TerrainStrokeResult Result(bool header) const {
        TerrainStrokeResult result;
        result.headerChanged = header;
        if (!empty_) { result.x0 = x0_; result.y0 = y0_; result.x1 = x1_ + 1; result.y1 = y1_ + 1; }
        return result;
    }
private:
    bool empty_ = true;
    std::uint32_t x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
};

bool SameRaw(const TerrainVertex& a, const TerrainVertex& b) {
    return a.groundHeightRaw == b.groundHeightRaw && a.waterHeightAndFlagsRaw == b.waterHeightAndFlagsRaw
        && a.textureAndFlagsRaw == b.textureAndFlagsRaw && a.variationRaw == b.variationRaw && a.cliffAndLayerRaw == b.cliffAndLayerRaw;
}

void SetLevel(TerrainVertex& vertex, int level, std::optional<std::uint8_t> texture) {
    const auto cliffTexture = texture ? *texture : vertex.CliffTextureIndex();
    vertex.cliffAndLayerRaw = static_cast<std::uint8_t>((cliffTexture << 4) | (level & 0x0f));
}

// 悬崖层级变化后向外传播，使相邻（八邻域）层级差不超过 2；边界顶点不改。
void LimitLevelSteps(TerrainGridSnapshot& terrain, std::vector<std::uint32_t> queue, Changes& changes) {
    const auto width = terrain.header.width, height = terrain.header.height;
    while (!queue.empty()) {
        const auto index = queue.back(); queue.pop_back();
        const auto x = index % width, y = index / width;
        const int level = terrain.vertices[index].CliffLevel();
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto nx = static_cast<std::int64_t>(x) + dx, ny = static_cast<std::int64_t>(y) + dy;
                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                const auto neighbor = static_cast<std::uint32_t>(ny) * width + static_cast<std::uint32_t>(nx);
                auto& vertex = terrain.vertices[neighbor];
                if (vertex.IsBoundary()) continue;
                const int current = vertex.CliffLevel();
                const int limited = std::clamp(current, level - kMaximumLevelStep, level + kMaximumLevelStep);
                if (limited == current) continue;
                SetLevel(vertex, limited, std::nullopt);
                changes.Touch(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny));
                queue.push_back(neighbor);
            }
        }
    }
}

void RefreshPositions(TerrainGridSnapshot& terrain, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    const auto& header = terrain.header;
    x1 = std::min(x1, header.width); y1 = std::min(y1, header.height);
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            auto& vertex = terrain.vertices[static_cast<std::size_t>(y) * header.width + x];
            vertex.position = {static_cast<float>(static_cast<double>(header.offset[0]) + x * 128.0),
                static_cast<float>(static_cast<double>(header.offset[1]) + y * 128.0),
                static_cast<float>(GroundZ(vertex.groundHeightRaw, vertex.CliffLevel()))};
            vertex.waterBaseHeight = static_cast<float>((static_cast<double>(vertex.waterHeightAndFlagsRaw & 0x3fff) - 8192.0) / 4.0);
        }
    }
}

Status Validate(const TerrainGridSnapshot& terrain, const TerrainBrush& brush, std::span<const TerrainStrokePoint> points) {
    if (!Consistent(terrain)) return Failure("TERRAIN_EDIT_INCONSISTENT", "地形快照的宽高与顶点数量不一致");
    if (!ClassicLayout(terrain)) return Failure("TERRAIN_BRUSH_READ_ONLY", "该地形布局只读；笔刷只支持经典 W3E v11");
    if (brush.radius > kMaximumRadius) return Failure("TERRAIN_BRUSH_RADIUS", "笔刷半径超过 16 个顶点");
    if (points.empty() || points.size() > kMaximumPoints) return Failure("TERRAIN_STROKE_POINTS", "笔刷点数量必须在 1 到 65536 之间");
    for (const auto& point : points)
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return Failure("TERRAIN_STROKE_POINTS", "笔刷点坐标不是有限数");
    if (brush.anchor && ((brush.anchor->groundHeightRaw & 0x8000) != 0 || brush.anchor->cliffLevel > 15))
        return Failure("TERRAIN_STROKE_ANCHOR", "笔划基准包含不支持的地面原始高度或悬崖层级");
    switch (brush.kind) {
    case TerrainBrushKind::Texture:
        if (brush.groundTexture.size() != 4) return Failure("TERRAIN_BRUSH_TEXTURE", "地面贴图必须是四字符 rawcode");
        if (std::find(terrain.header.groundTilesets.begin(), terrain.header.groundTilesets.end(), brush.groundTexture)
                == terrain.header.groundTilesets.end() && terrain.header.groundTilesets.size() >= kMaximumGroundTilesets)
            return Failure("TERRAIN_TILESET_FULL", "地图已使用 16 种地面贴图，不能再加入新的贴图");
        break;
    case TerrainBrushKind::Raise: case TerrainBrushKind::Lower:
        if (!std::isfinite(brush.strength) || brush.strength <= 0 || brush.strength > 1024)
            return Failure("TERRAIN_BRUSH_STRENGTH", "升降幅度必须在 0 到 1024 之间");
        break;
    case TerrainBrushKind::Smooth:
        if (!std::isfinite(brush.strength) || brush.strength <= 0 || brush.strength > 1)
            return Failure("TERRAIN_BRUSH_STRENGTH", "平滑强度必须在 0 到 1 之间");
        break;
    case TerrainBrushKind::CliffLevel:
        if (brush.levelDelta < -1 || brush.levelDelta > 1) return Failure("TERRAIN_BRUSH_LEVEL", "悬崖层级只能升降一级或保持同级");
        if (brush.cliffTexture >= terrain.header.cliffTilesets.size())
            return Failure("TERRAIN_BRUSH_CLIFF_TEXTURE", "悬崖贴图不在地图悬崖贴图表中");
        break;
    case TerrainBrushKind::Blight: case TerrainBrushKind::Plateau:
        break;
    default:
        return Failure("TERRAIN_BRUSH_KIND", "未知的地形笔刷");
    }
    return Status::Ok();
}

} // namespace

Result<std::vector<std::byte>> SerializeMapTerrain(const TerrainGridSnapshot& terrain) {
    const auto& header = terrain.header;
    if (!Consistent(terrain)) return Result<std::vector<std::byte>>::Failure("MapTerrain.Write", "TERRAIN_WRITE_INCONSISTENT",
        "地形宽高与顶点数量不一致，不能写出");
    if (!ClassicLayout(terrain)) return Result<std::vector<std::byte>>::Failure("MapTerrain.Write", "TERRAIN_WRITE_VERSION",
        "只写出经典 W3E v11；禁止将新版顶点降为七字节记录");
    for (const auto* list : {&header.groundTilesets, &header.cliffTilesets})
        for (const auto& id : *list)
            if (id.size() != 4) return Result<std::vector<std::byte>>::Failure("MapTerrain.Write", "TERRAIN_WRITE_TILESET",
                "贴图表项必须是四字节 rawcode");
    std::vector<std::byte> out;
    out.reserve(37 + 4 * (header.groundTilesets.size() + header.cliffTilesets.size()) + terrain.vertices.size() * 7);
    for (const char c : std::string_view("W3E!")) out.push_back(static_cast<std::byte>(c));
    PutU32(out, static_cast<std::uint32_t>(header.version));
    out.push_back(static_cast<std::byte>(header.mainTileset));
    PutU32(out, header.usesCustomTileset ? 1u : 0u);
    for (const auto* list : {&header.groundTilesets, &header.cliffTilesets}) {
        PutU32(out, static_cast<std::uint32_t>(list->size()));
        for (const auto& id : *list) for (const char c : id) out.push_back(static_cast<std::byte>(c));
    }
    PutU32(out, header.width);
    PutU32(out, header.height);
    for (const float coordinate : header.offset) PutU32(out, std::bit_cast<std::uint32_t>(coordinate));
    for (const auto& vertex : terrain.vertices) {
        PutU16(out, vertex.groundHeightRaw);
        PutU16(out, vertex.waterHeightAndFlagsRaw);
        out.push_back(static_cast<std::byte>(vertex.textureAndFlagsRaw));
        out.push_back(static_cast<std::byte>(vertex.variationRaw));
        out.push_back(static_cast<std::byte>(vertex.cliffAndLayerRaw));
    }
    return Result<std::vector<std::byte>>::Success(std::move(out));
}

void RefreshTerrainDerived(TerrainGridSnapshot& terrain, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    if (!Consistent(terrain)) return;
    RefreshPositions(terrain, x0, y0, x1, y1);
    const auto& header = terrain.header;
    terrain.minimum = terrain.maximum = terrain.vertices.front().position;
    terrain.unresolvedGroundTextures = terrain.unresolvedCliffTextures = 0;
    for (const auto& vertex : terrain.vertices) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            terrain.minimum[axis] = std::min(terrain.minimum[axis], vertex.position[axis]);
            terrain.maximum[axis] = std::max(terrain.maximum[axis], vertex.position[axis]);
        }
        if (vertex.GroundTextureIndex() >= header.groundTilesets.size()) ++terrain.unresolvedGroundTextures;
        if (vertex.CliffTextureIndex() != 15 && vertex.CliffTextureIndex() >= header.cliffTilesets.size()) ++terrain.unresolvedCliffTextures;
    }
}

std::optional<float> SampleGroundHeight(const TerrainGridSnapshot& terrain, double x, double y) {
    const auto width = terrain.header.width, height = terrain.header.height;
    if (!Consistent(terrain) || !std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    const auto& origin = terrain.vertices.front().position;
    const double u = (x - origin[0]) / TerrainGridSnapshot::tileSize;
    const double v = (y - origin[1]) / TerrainGridSnapshot::tileSize;
    if (!(u >= 0 && v >= 0 && u <= width - 1 && v <= height - 1)) return std::nullopt;
    const auto column = std::min(static_cast<std::uint32_t>(u), width - 2), row = std::min(static_cast<std::uint32_t>(v), height - 2);
    const double fu = u - column, fv = v - row;
    const auto z = [&](std::uint32_t dx, std::uint32_t dy) {
        return static_cast<double>(terrain.vertices[static_cast<std::size_t>(row + dy) * width + column + dx].position[2]);
    };
    const double result = fu + fv <= 1 ? z(0, 0) + fu * (z(1, 0) - z(0, 0)) + fv * (z(0, 1) - z(0, 0))
                                        : z(1, 1) + (1 - fu) * (z(0, 1) - z(1, 1)) + (1 - fv) * (z(1, 0) - z(1, 1));
    if (!std::isfinite(result)) return std::nullopt;
    return static_cast<float>(result);
}

Result<TerrainStrokeAnchor> CaptureTerrainStrokeAnchor(
    const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point) {
    if (!Consistent(terrain) || !std::isfinite(point.x) || !std::isfinite(point.y))
        return Result<TerrainStrokeAnchor>::Failure("MapTerrain.Edit", "TERRAIN_STROKE_ANCHOR",
            "地形快照或笔划起点无效，不能固定笔刷基准");
    const auto at = FootprintAt(terrain, point, 0);
    if (!at) return Result<TerrainStrokeAnchor>::Failure("MapTerrain.Edit", "TERRAIN_STROKE_ANCHOR_OUTSIDE",
        "笔划起点不在地图顶点范围内");
    const auto& vertex = terrain.vertices[static_cast<std::size_t>(at->cy) * terrain.header.width + static_cast<std::size_t>(at->cx)];
    if ((vertex.groundHeightRaw & 0x8000) != 0)
        return Result<TerrainStrokeAnchor>::Failure("MapTerrain.Edit", "TERRAIN_STROKE_ANCHOR",
            "笔划起点的地面原始高度含未支持的 bit15");
    return Result<TerrainStrokeAnchor>::Success({vertex.groundHeightRaw, vertex.CliffLevel()});
}

Result<TerrainStrokeResult> ApplyTerrainStroke(
    TerrainGridSnapshot& terrain, const TerrainBrush& brush, std::span<const TerrainStrokePoint> points) {
    if (auto status = Validate(terrain, brush, points); !status.Succeeded()) {
        Result<TerrainStrokeResult> failed;
        failed.diagnostics = std::move(status.diagnostics);
        return failed;
    }
    auto& header = terrain.header;
    const auto width = header.width;
    // 先确定第一个有效落点；地图外的整笔是纯空操作，不追加未使用的贴图表项。
    std::optional<Footprint> first;
    for (const auto& point : points) {
        first = FootprintAt(terrain, point, 0);
        if (first) break;
    }
    if (!first) return Result<TerrainStrokeResult>::Success({});
    const auto& firstVertex = terrain.vertices[static_cast<std::size_t>(first->cy) * width + static_cast<std::size_t>(first->cx)];
    const TerrainStrokeAnchor anchor = brush.anchor.value_or(TerrainStrokeAnchor{firstVertex.groundHeightRaw, firstVertex.CliffLevel()});
    if ((anchor.groundHeightRaw & 0x8000) != 0)
        return Result<TerrainStrokeResult>::Failure("MapTerrain.Edit", "TERRAIN_STROKE_ANCHOR",
            "笔划基准的地面原始高度含未支持的 bit15");
    Changes changes;
    bool headerChanged = false;
    std::uint8_t textureIndex = 0;
    if (brush.kind == TerrainBrushKind::Texture) {
        const auto found = std::find(header.groundTilesets.begin(), header.groundTilesets.end(), brush.groundTexture);
        textureIndex = static_cast<std::uint8_t>(found - header.groundTilesets.begin());
        if (found == header.groundTilesets.end()) { header.groundTilesets.push_back(brush.groundTexture); headerChanged = true; }
    }
    const int targetLevel = std::clamp(static_cast<int>(anchor.cliffLevel) + brush.levelDelta, 0, kMaximumCliffLevel);
    std::vector<std::uint32_t> levelChanged;
    for (const auto& point : points) {
        const auto at = FootprintAt(terrain, point, brush.radius);
        if (!at) continue;
        // 平滑需要施加前的邻域高度：先算完整个笔刷范围再写入。
        std::vector<std::pair<std::size_t, TerrainVertex>> pending;
        for (std::uint32_t y = at->y0; y <= at->y1; ++y) {
            for (std::uint32_t x = at->x0; x <= at->x1; ++x) {
                const bool falloff = brush.kind == TerrainBrushKind::Raise || brush.kind == TerrainBrushKind::Lower
                    || brush.kind == TerrainBrushKind::Smooth;
                const auto weight = Weight(brush, *at, x, y, falloff);
                if (!weight) continue;
                const auto index = static_cast<std::size_t>(y) * width + x;
                auto vertex = terrain.vertices[index];
                const bool shaping = brush.kind != TerrainBrushKind::Texture && brush.kind != TerrainBrushKind::Blight;
                if (shaping && vertex.IsBoundary()) continue;
                const int level = vertex.CliffLevel();
                switch (brush.kind) {
                case TerrainBrushKind::Texture:
                    if (vertex.GroundTextureIndex() != textureIndex) {
                        vertex.textureAndFlagsRaw = static_cast<std::uint8_t>((vertex.textureAndFlagsRaw & 0xf0) | textureIndex);
                        vertex.variationRaw = static_cast<std::uint8_t>((vertex.variationRaw & 0xe0) | RandomVariation(brush.seed, x, y));
                    }
                    break;
                case TerrainBrushKind::Blight:
                    vertex.textureAndFlagsRaw = static_cast<std::uint8_t>(brush.value ? (vertex.textureAndFlagsRaw | 0x20)
                                                                                      : (vertex.textureAndFlagsRaw & ~0x20));
                    break;
                case TerrainBrushKind::Raise: case TerrainBrushKind::Lower: {
                    const double delta = (brush.kind == TerrainBrushKind::Raise ? 1.0 : -1.0) * brush.strength * *weight;
                    vertex.groundHeightRaw = GroundRaw(GroundZ(vertex.groundHeightRaw, level) + delta, level);
                    break;
                }
                case TerrainBrushKind::Plateau:
                    vertex.groundHeightRaw = anchor.groundHeightRaw;
                    break;
                case TerrainBrushKind::Smooth: {
                    double sum = 0; int count = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const auto nx = static_cast<std::int64_t>(x) + dx, ny = static_cast<std::int64_t>(y) + dy;
                            if (nx < 0 || ny < 0 || nx >= width || ny >= header.height) continue;
                            sum += terrain.vertices[static_cast<std::size_t>(ny) * width + static_cast<std::size_t>(nx)].position[2];
                            ++count;
                        }
                    }
                    const double z = vertex.position[2];
                    vertex.groundHeightRaw = GroundRaw(z + (sum / count - z) * brush.strength * *weight, level);
                    break;
                }
                case TerrainBrushKind::CliffLevel:
                    SetLevel(vertex, targetLevel, brush.cliffTexture);
                    break;
                }
                if (SameRaw(vertex, terrain.vertices[index])) continue;
                pending.emplace_back(index, vertex);
            }
        }
        Changes local;
        for (const auto& [index, vertex] : pending) {
            const bool levelMoved = vertex.CliffLevel() != terrain.vertices[index].CliffLevel();
            terrain.vertices[index] = vertex;
            local.Touch(static_cast<std::uint32_t>(index % width), static_cast<std::uint32_t>(index / width));
            if (levelMoved) levelChanged.push_back(static_cast<std::uint32_t>(index));
        }
        if (!levelChanged.empty()) LimitLevelSteps(terrain, std::exchange(levelChanged, {}), local);
        // 下一个笔刷点（平滑、升降）要看到本点的结果；包围盒等统计在整笔结束后重算一次。
        if (const auto touched = local.Result(false); !touched.Empty()) {
            RefreshPositions(terrain, touched.x0, touched.y0, touched.x1, touched.y1);
            changes.Touch(touched.x0, touched.y0); changes.Touch(touched.x1 - 1, touched.y1 - 1);
        }
    }
    auto result = changes.Result(headerChanged);
    if (!result.Empty() || headerChanged) RefreshTerrainDerived(terrain, result.x0, result.y0, result.x1, result.y1);
    return Result<TerrainStrokeResult>::Success(result);
}

} // namespace War3Map::MapData
