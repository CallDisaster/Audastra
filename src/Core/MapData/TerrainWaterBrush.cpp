#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/MapData/TerrainCliffEditing.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "TerrainWaterBrushInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace War3Map::MapData {
namespace {

constexpr double kDegreeFactor = 0.01745329424738884;
constexpr double kPi = 3.1415927410125732;
constexpr float kHeightSlack = 6.400000095367432f;
constexpr double kHeightEpsilon = 0x1p-22;
constexpr std::string_view kSubsystem = "MapTerrain.WaterBrush";

struct Rejection { std::string code, text; };

[[noreturn]] void Reject(std::string code, std::string text) { throw Rejection{std::move(code), std::move(text)}; }

double Base(const TerrainVertex& vertex) { return double(vertex.groundHeightRaw & 0x3fff) / 4.0 - 2048.0; }
double Ground(const TerrainVertex& vertex) { return Base(vertex) + 128.0 * (int(vertex.CliffLevel()) - 2); }
double Water(const TerrainVertex& vertex) { return double(vertex.waterHeightAndFlagsRaw & 0x3fff) / 4.0 - 2048.0; }

bool SameRaw(const TerrainVertex& a, const TerrainVertex& b) {
    return a.groundHeightRaw == b.groundHeightRaw && a.waterHeightAndFlagsRaw == b.waterHeightAndFlagsRaw
        && a.textureAndFlagsRaw == b.textureAndFlagsRaw && a.variationRaw == b.variationRaw
        && a.cliffAndLayerRaw == b.cliffAndLayerRaw;
}

void Include(TerrainStrokeResult& bounds, std::uint32_t x, std::uint32_t y) {
    if (bounds.Empty()) bounds = {x, y, x + 1, y + 1, false};
    else { bounds.x0 = std::min(bounds.x0, x); bounds.y0 = std::min(bounds.y0, y);
        bounds.x1 = std::max(bounds.x1, x + 1); bounds.y1 = std::max(bounds.y1, y + 1); }
}

void ValidateGrid(const TerrainGridSnapshot& terrain) {
    const auto& h = terrain.header;
    if (h.version != 11 || h.width < 2 || h.height < 2 || h.width > 2049 || h.height > 2049
        || std::uint64_t(h.width) * h.height != terrain.vertices.size()
        || !std::isfinite(h.offset[0]) || !std::isfinite(h.offset[1])
        || std::abs(double(h.offset[0])) > 16777216.0 || std::abs(double(h.offset[1])) > 16777216.0
        || std::abs(double(h.offset[0]) + double(h.width - 1) * 128.0) > 16777216.0
        || std::abs(double(h.offset[1]) + double(h.height - 1) * 128.0) > 16777216.0)
        Reject("TERRAIN_WATER_LAYOUT", "水刷需要完整、有限坐标的经典 W3E v11 地形。");
}

void ValidateVertex(const TerrainVertex& vertex, std::uint32_t x, std::uint32_t y, bool allowRamp = false) {
    const auto where = [&] { return "（" + std::to_string(x) + "," + std::to_string(y) + "）"; };
    if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw || vertex.groundHeightRaw > 0x3fff
        || vertex.UnknownWaterFlags())
        Reject("TERRAIN_WATER_VERTEX_DOMAIN", "实际水刷处理区含未知高度/布局位" + where());
    if (vertex.IsRamp() && !allowRamp) Reject("TERRAIN_WATER_RAMP_DOMAIN", "实际水刷处理区含尚未支持的旧坡道" + where());
}

struct GridPoint { std::uint32_t x, y; };

// 水刷、悬崖和坡道共用字段算法，但保留各自语义与锚点；无水模式不伪造水刷种类。
struct StructuralBrush {
    TerrainBrushShape shape;
    std::uint32_t size;
    std::uint8_t cliffTexture;
    std::uint32_t randomSeed;
    TerrainWaterStrokeAnchor anchor;
    std::optional<TerrainWaterBrushKind> waterKind;
    int cliffDelta = 0;
    TerrainWaterAlgorithm waterAlgorithm = TerrainWaterAlgorithm::NativeRamps;
    std::optional<bool> cornerRamp;
    TerrainCornerRampAlgorithm cornerAlgorithm = TerrainCornerRampAlgorithm::DryOnly;
    bool straightRamp = false;
    TerrainStraightRampAlgorithm straightAlgorithm = TerrainStraightRampAlgorithm::Independent;
};

std::optional<GridPoint> Snap(const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point) {
    const double x = (point.x - terrain.header.offset[0]) / 128.0;
    const double y = (point.y - terrain.header.offset[1]) / 128.0;
    if (!std::isfinite(x) || !std::isfinite(y) || x < -0.5 || y < -0.5
        || x >= double(terrain.header.width) - 0.5 || y >= double(terrain.header.height) - 0.5) return {};
    const auto sx = std::round(x), sy = std::round(y);
    if (sx < 0 || sy < 0) return {};
    return GridPoint{static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy)};
}

// 原脏块是 4×4 地格；顶点恰好位于块界时依 x 外/y 内顺序加入两侧块。
struct DirtyBlocks {
    std::vector<std::uint32_t> entries;
    std::vector<std::uint8_t> present;
    explicit DirtyBlocks(std::size_t count) : present(count) { entries.reserve(count); }
};

class StructuralTransaction {
public:
    StructuralTransaction(TerrainGridSnapshot& candidate, const TerrainWaterLoadedRules& rules,
        const StructuralBrush& brush, const DoodadPlacements& doodads,
        std::span<const TerrainWaterSpecialDoodadShape> shapes, const TerrainWaterBrushLimits& limits, std::size_t initialWork)
        : grid_(candidate), rules_(rules), brush_(brush), shapes_(shapes), limits_(limits),
          width_(candidate.header.width), height_(candidate.header.height), blockWidth_((width_ - 2) / 4 + 1),
          blockHeight_((height_ - 2) / 4 + 1), marker_(candidate.vertices.size()), visited_(candidate.vertices.size()),
          random_(brush.randomSeed), slopeTangent_(static_cast<float>(std::tan(double(rules.maxSlopeDegrees) * kDegreeFactor))) {
        shapeBySource_.resize(shapes.size());
        liveSpecial_.reserve(shapes.size());
        for (const auto& shape : shapes) shapeBySource_[shape.sourceRecordIndex] = &shape;
        // 原版只把有实际占地的记录加入运行时数组；空pathTex仍属于原始DOO。
        for (std::uint32_t i = 0; i < doodads.specialDoodads.size(); ++i)
            if (shapeBySource_[i]->hasFootprint) liveSpecial_.push_back(i);
        result_.removedSpecialSourceRecords.reserve(shapes.size());
        result_.work = initialWork;
    }

    TerrainWaterBrushResult Run(std::span<const TerrainStrokePoint> points, const TerrainGridSnapshot& original) {
        const double anchorBase = double(brush_.anchor.groundHeightRaw) / 4.0 - 2048.0;
        const double anchorGround = anchorBase + 128.0 * (int(brush_.anchor.cliffLevel) - 2)
            + (brush_.waterKind && brush_.waterAlgorithm == TerrainWaterAlgorithm::NativeRamps ? brush_.anchor.rampHeightOffset : 0);
        const double uniformWater = brush_.anchor.hasWater
            ? double(brush_.anchor.waterHeightRaw) / 4.0 - 2048.0 : anchorGround;
        const auto waterRaw = brush_.waterKind ? Encode(uniformWater) : std::uint16_t{0};
        int targetLevel = int(brush_.anchor.cliffLevel) + brush_.cliffDelta;
        if (brush_.waterKind) targetLevel = int(brush_.anchor.cliffLevel) - (*brush_.waterKind == TerrainWaterBrushKind::Deep ? 2 : 1);
        if (brush_.waterKind && brush_.anchor.hasWater) {
            const double target = uniformWater + rules_.waterLayerOffset
                - (*brush_.waterKind == TerrainWaterBrushKind::Deep ? rules_.deepWaterThreshold : 0.0);
            targetLevel = int(brush_.anchor.cliffLevel) + Truncate((target - anchorGround) / 128.0);
            if (anchorGround > target) --targetLevel; // 整步负差也明确多减一级。
        }
        for (const auto& point : points) {
            if (brush_.cornerRamp) { ApplyNativeRamp(point); continue; }
            const auto center = Snap(grid_, point);
            if (!center) Reject("TERRAIN_WATER_OUTSIDE", "水刷落点位于地图外，整笔未修改。");
            if (brush_.waterKind) {
                Footprint(*center, brush_.size, [&](auto x, auto y) {
                    auto next = Read(x, y);
                    next.textureAndFlagsRaw |= 0x40;
                    next.waterHeightAndFlagsRaw = static_cast<std::uint16_t>((next.waterHeightAndFlagsRaw & 0xc000) | waterRaw);
                    Commit(x, y, next);
                });
                auto platform = NewBlocks();
                Footprint(*center, brush_.size + 2, [&](auto x, auto y) {
                    auto next = Read(x, y); next.groundHeightRaw = brush_.anchor.groundHeightRaw;
                    Commit(x, y, next); Gather(platform, x, y);
                });
                EachBlock(platform, [&](auto x, auto y) { LimitRaw(x, y); });
                ClearMarkers();
            }
            ApplyCliff(*center, targetLevel);
        }
        // 岸线和第二轮层级收敛仍可改写 L；提交前再次核对最终实际处理域。
        if (!brush_.cornerRamp) CheckSpecialDoodads(false);
        // 只比较实际访问过的记录；没有全图坡道或未知位拒绝。
        Work(grid_.vertices.size());
        for (std::uint32_t y = 0; y < height_; ++y) for (std::uint32_t x = 0; x < width_; ++x) {
            const auto index = Index(x, y);
            if (visited_[index] && !SameRaw(original.vertices[index], grid_.vertices[index])) {
                Include(result_.changed, x, y); ++result_.changedVertices;
            }
        }
        if (!result_.changed.Empty()) {
            Work(grid_.vertices.size());
            Work(std::size_t(result_.changed.x1 - result_.changed.x0) * (result_.changed.y1 - result_.changed.y0));
            RefreshTerrainDerived(grid_, result_.changed.x0, result_.changed.y0, result_.changed.x1, result_.changed.y1);
            result_.requiresPathingRebuild = true;
            result_.requiresObjectValidityRefresh = true;
        }
        result_.finalRandomState = random_;
        return std::move(result_);
    }

    void WriteSpecialOutput(const DoodadPlacements& source, DoodadPlacements& candidate,
        TerrainWaterBrushResult& result) const {
        if (result.removedSpecialSourceRecords.empty()) return;
        if (source.specialDoodads.size() > limits_.maximumWork - result.work)
            Reject("TERRAIN_WATER_WORK_BUDGET", "特殊模型候选映射超过本笔工作量预算。");
        result.work += source.specialDoodads.size();
        // active子序列使用原版swap-last后的源记录顺序。空占地不参与XY查找、
        // 删除或末项覆盖，仍逐字保留其记录及相互顺序，不仿照原版保存时丢弃。
        std::size_t write = 0, active = 0;
        for (std::size_t i = 0; i < source.specialDoodads.size(); ++i) {
            if (!shapeBySource_[i]->hasFootprint) candidate.specialDoodads[write++] = source.specialDoodads[i];
            else if (active < liveSpecial_.size())
                candidate.specialDoodads[write++] = source.specialDoodads[liveSpecial_[active++]];
        }
        candidate.specialDoodads.resize(write);
    }

private:
    struct Frame { std::uint32_t x, y, depth; int next = 0, level = 0; std::uint8_t palette = 0; };
    TerrainGridSnapshot& grid_;
    const TerrainWaterLoadedRules& rules_;
    const StructuralBrush& brush_;
    std::span<const TerrainWaterSpecialDoodadShape> shapes_;
    std::vector<const TerrainWaterSpecialDoodadShape*> shapeBySource_;
    std::vector<std::uint32_t> liveSpecial_;
    const TerrainWaterBrushLimits& limits_;
    std::uint32_t width_, height_, blockWidth_, blockHeight_;
    std::vector<std::uint8_t> marker_, visited_;
    std::uint32_t random_;
    float slopeTangent_;
    TerrainWaterBrushResult result_;

    std::size_t Index(std::uint32_t x, std::uint32_t y) const { return std::size_t(y) * width_ + x; }
    std::size_t BlockCount() const { return std::size_t(blockWidth_) * blockHeight_; }
    bool Inside(int x, int y) const { return x >= 0 && y >= 0 && std::uint32_t(x) < width_ && std::uint32_t(y) < height_; }
    void Work(std::size_t count = 1) {
        if (count > limits_.maximumWork - result_.work) Reject("TERRAIN_WATER_WORK_BUDGET", "水刷超过显式工作量预算，整笔未修改。");
        result_.work += count;
    }
    TerrainVertex& Inspect(std::uint32_t x, std::uint32_t y) {
        Work(); auto& vertex = grid_.vertices[Index(x, y)];
        ValidateVertex(vertex, x, y, !brush_.waterKind || brush_.waterAlgorithm == TerrainWaterAlgorithm::NativeRamps);
        if (vertex.CliffTextureIndex() != 15 && vertex.CliffTextureIndex() >= grid_.header.cliffTilesets.size())
            Reject("TERRAIN_WATER_CLIFF_RESOURCE", "实际处理点的悬崖 palette 不在源 W3E 表中。");
        return vertex;
    }
    TerrainVertex& Read(std::uint32_t x, std::uint32_t y) {
        auto& vertex = Inspect(x, y);
        visited_[Index(x, y)] = 1; Include(result_.processed, x, y); return vertex;
    }
    TerrainNativeRampState RampState(std::uint32_t x, std::uint32_t y) {
        // 九角各读自己的最多25点邻域，另含分类/高度判定；固定上界预收，
        // 实际字段检查也纳入本笔处理域。绝不缓存编辑前标签用于动态清理。
        Work(512);
        for (int dx = -2; dx <= 2; ++dx) for (int dy = -2; dy <= 2; ++dy)
            if (Inside(int(x) + dx, int(y) + dy))
                (void)Read(std::uint32_t(int(x) + dx), std::uint32_t(int(y) + dy));
        auto state = QueryNativeTerrainRamp(grid_, rules_.maximumCliffLevel, x, y);
        if (!state.Succeeded() || !state.value)
            Reject("TERRAIN_CLIFF_RAMP_DOMAIN", "实际旧坡道分类邻域含未支持字段，整笔未修改。");
        return *state.value;
    }
    double GroundAt(std::uint32_t x, std::uint32_t y, const TerrainVertex& vertex) {
        return vertex.IsRamp() ? double(static_cast<float>(Ground(vertex))) + RampState(x, y).heightOffset : Ground(vertex);
    }
    DirtyBlocks NewBlocks() { Work(BlockCount()); return DirtyBlocks(BlockCount()); }
    bool Commit(std::uint32_t x, std::uint32_t y, const TerrainVertex& next) {
        auto& old = Read(x, y); if (SameRaw(old, next)) return false; old = next; return true;
    }
    static int Truncate(double value) {
        if (!std::isfinite(value) || value < std::numeric_limits<int>::min() + 32.0
            || value > std::numeric_limits<int>::max() - 32.0)
            Reject("TERRAIN_WATER_NUMERIC_DOMAIN", "水刷中间值超过有限整数域。");
        return static_cast<int>(std::trunc(value));
    }
    static std::uint16_t Encode(double base) {
        const auto raw = TerrainWaterBrushDetail::EncodeBaseHeight(base);
        if (!raw) Reject("TERRAIN_WATER_HEIGHT_DOMAIN", "水刷输出超出无未知位的 14 位高度域，整笔未修改。");
        return *raw;
    }
    std::uint8_t Variation() { random_ = random_ * 214013u + 2531011u;
        return static_cast<std::uint8_t>(((random_ >> 16) & 0x7fff) % 3); }
    void SetCliff(TerrainVertex& vertex, int level, int palette, bool randomize) {
        if (level < 0 || level > 14) Reject("TERRAIN_WATER_LEVEL_DOMAIN", "水限制目标层级超过有限可写域。");
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>((palette < 0 ? vertex.CliffTextureIndex() : palette) * 16 + level);
        if (randomize) vertex.variationRaw = static_cast<std::uint8_t>((vertex.variationRaw & 0x1f) | (Variation() << 5));
    }
    template<class Operation> void Footprint(GridPoint center, std::uint32_t size, Operation operation) {
        const int radius = int(size) - 1;
        for (int x = std::max(0, int(center.x) - radius); x <= std::min(int(width_) - 1, int(center.x) + radius); ++x)
            for (int y = std::max(0, int(center.y) - radius); y <= std::min(int(height_) - 1, int(center.y) + radius); ++y) {
                Work(); const int dx = x - int(center.x), dy = y - int(center.y);
                if (brush_.shape == TerrainBrushShape::Circle && dx * dx + dy * dy > radius * radius) continue;
                operation(std::uint32_t(x), std::uint32_t(y));
            }
    }
    void Gather(DirtyBlocks& list, std::uint32_t x, std::uint32_t y) {
        const auto lowX = x == 0 ? 0 : (x - 1) / 4, lowY = y == 0 ? 0 : (y - 1) / 4;
        for (auto bx = lowX; bx <= std::min(x / 4, blockWidth_ - 1); ++bx)
            for (auto by = lowY; by <= std::min(y / 4, blockHeight_ - 1); ++by) {
                Work(); const auto index = by * blockWidth_ + bx;
                if (!list.present[index]) { list.present[index] = 1; list.entries.push_back(index); }
            }
    }
    template<class Operation> void EachBlock(const DirtyBlocks& list, Operation operation) {
        for (const auto block : list.entries) {
            const auto x0 = (block % blockWidth_) * 4, y0 = (block / blockWidth_) * 4;
            for (auto x = x0; x <= std::min(x0 + 4, width_ - 1); ++x)
                for (auto y = y0; y <= std::min(y0 + 4, height_ - 1); ++y) { Work(); operation(x, y); }
        }
    }
    void ClearMarkers() { Work(marker_.size()); std::fill(marker_.begin(), marker_.end(), 0); }
    std::optional<std::uint16_t> FirstWet(std::uint32_t x, std::uint32_t y) {
        for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy)
            if (Inside(int(x) + dx, int(y) + dy)) {
                const auto& vertex = Read(std::uint32_t(int(x) + dx), std::uint32_t(int(y) + dy));
                if (vertex.HasWater()) return vertex.waterHeightAndFlagsRaw & 0x3fff;
            }
        return {};
    }
    std::pair<float, float> RawBounds(std::uint32_t x, std::uint32_t y) {
        const double cliffBase = 128.0 * Read(x, y).CliffLevel() - 256.0;
        float high = static_cast<float>(double(rules_.maxHeight) - cliffBase);
        float low = static_cast<float>(std::min(double(rules_.minHeight) - cliffBase, 0.0));
        if (rules_.maxSlopeDegrees < 85.0f) {
            for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
                if ((dx == 0 && dy == 0) || !Inside(int(x) + dx, int(y) + dy)) continue;
                const double distance = dx != 0 && dy != 0 ? 128.0 * 1.4142099618911743 : 128.0;
                const double allowed = distance * slopeTangent_;
                const double base = Base(Read(std::uint32_t(int(x) + dx), std::uint32_t(int(y) + dy)));
                high = std::min(high, static_cast<float>(base + allowed));
                if (base - allowed > low) low = static_cast<float>(base - allowed);
            }
        }
        if (rules_.enforceWaterLimits) {
            if (Read(x, y).HasWater()) low = std::max(low, -64.0f);
            else if (FirstWet(x, y)) low = std::max(low, -kHeightSlack * 0.5f);
            if (low > high) high = static_cast<float>(double(low) + kHeightSlack);
        }
        if (high < low) std::swap(high, low);
        if (!std::isfinite(low) || !std::isfinite(high)) Reject("TERRAIN_WATER_NUMERIC_DOMAIN", "水刷上下界不是有限数。");
        return {low, high};
    }
    bool ClampRaw(std::uint32_t x, std::uint32_t y, std::uint32_t depth) {
        const auto index = Index(x, y);
        if (marker_[index] && marker_[index] <= depth) return false;
        auto next = Read(x, y); const double old = Base(next); const auto [low, high] = RawBounds(x, y);
        // 两次比较都以原 B 为基准；下界判断最后覆盖上界选择。
        double target = old > high ? high : old; if (old < low) target = low;
        if (target == old) return false;
        next.groundHeightRaw = Encode(target);
        if (!Commit(x, y, next)) return false;
        marker_[index] = static_cast<std::uint8_t>(depth + 1); return true;
    }
    void CheckDepth(std::uint32_t depth) {
        if (depth > limits_.maximumRecursionDepth) Reject("TERRAIN_WATER_DEPTH_BUDGET", "水刷有序递归超过深度预算，整笔未修改。");
    }
    void LimitRaw(std::uint32_t x, std::uint32_t y) {
        if (!ClampRaw(x, y, 1)) return;
        std::vector<Frame> stack; stack.reserve(limits_.maximumRecursionDepth); stack.push_back({x, y, 1});
        while (!stack.empty()) {
            Work();
            auto& frame = stack.back(); if (frame.next == 9) { stack.pop_back(); continue; }
            const int slot = frame.next++, dx = slot / 3 - 1, dy = slot % 3 - 1;
            const int nx = int(frame.x) + dx, ny = int(frame.y) + dy;
            if ((dx == 0 && dy == 0) || !Inside(nx, ny)) continue;
            const auto depth = frame.depth + 1; CheckDepth(depth);
            if (ClampRaw(std::uint32_t(nx), std::uint32_t(ny), depth)) stack.push_back({std::uint32_t(nx), std::uint32_t(ny), depth});
        }
    }
    bool CliffCell(int x, int y) {
        if (x < 0 || y < 0 || x + 1 >= int(width_) || y + 1 >= int(height_)) return false;
        const auto level = Read(std::uint32_t(x), std::uint32_t(y)).CliffLevel();
        bool cliff = false;
        for (int dx = 0; dx <= 1; ++dx) for (int dy = 0; dy <= 1; ++dy)
            cliff = (Read(std::uint32_t(x + dx), std::uint32_t(y + dy)).CliffLevel() != level) || cliff;
        return cliff;
    }
    std::uint8_t EffectivePalette(int x, int y) {
        constexpr std::array<std::array<int, 2>, 4> corners{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
        for (const auto& corner : corners) {
            const auto& v = Read(std::uint32_t(x + corner[0]), std::uint32_t(y + corner[1]));
            if (v.CliffTextureIndex() != 15) return v.CliffTextureIndex();
        }
        for (int dx = -2; dx <= 2; ++dx) for (int dy = -2; dy <= 2; ++dy)
            if (Inside(x + dx, y + dy)) {
                const auto palette = Read(std::uint32_t(x + dx), std::uint32_t(y + dy)).CliffTextureIndex();
                if (palette != 15) return palette;
            }
        return 0;
    }
    void LimitCliff(std::uint32_t x, std::uint32_t y, int palette) {
        std::vector<Frame> stack; stack.reserve(limits_.maximumRecursionDepth);
        stack.push_back({x, y, 1, 0, Read(x, y).CliffLevel()});
        while (!stack.empty()) {
            Work();
            auto& frame = stack.back(); if (frame.next == 9) { stack.pop_back(); continue; }
            const int slot = frame.next++, dx = slot / 3 - 1, dy = slot % 3 - 1;
            const int nx = int(frame.x) + dx, ny = int(frame.y) + dy;
            if (!Inside(nx, ny)) continue;
            const auto index = Index(std::uint32_t(nx), std::uint32_t(ny));
            if (marker_[index] && marker_[index] <= frame.depth) continue;
            auto next = Read(std::uint32_t(nx), std::uint32_t(ny));
            const int target = std::clamp(std::clamp(int(next.CliffLevel()), frame.level - 2, frame.level + 2), 0, int(rules_.maximumCliffLevel));
            if (target == next.CliffLevel()) continue;
            const auto depth = frame.depth + 1; CheckDepth(depth);
            SetCliff(next, target, palette, true); Commit(std::uint32_t(nx), std::uint32_t(ny), next);
            marker_[index] = static_cast<std::uint8_t>(depth);
            stack.push_back({std::uint32_t(nx), std::uint32_t(ny), depth, 0, target});
        }
    }
    void ConvergeCliffs(const DirtyBlocks& blocks, int palette) {
        EachBlock(blocks, [&](auto x, auto y) { LimitCliff(x, y, palette); }); ClearMarkers();
    }
    void RepairConflict(std::uint32_t x, std::uint32_t y, int majority) {
        std::optional<int> conflict;
        for (int dx = -2; dx <= 1 && !conflict; ++dx) for (int dy = -2; dy <= 1; ++dy) {
            const int cx = int(x) + dx, cy = int(y) + dy;
            if (!CliffCell(cx, cy)) continue;
            const auto palette = EffectivePalette(cx, cy);
            if (palette != brush_.cliffTexture) { conflict = palette; break; }
        }
        if (!conflict) return;
        for (int dx = -2; dx <= 2; ++dx) for (int dy = -2; dy <= 2; ++dy) {
            if ((dx == 0 && dy == 0) || !Inside(int(x) + dx, int(y) + dy)) continue;
            const auto nx = std::uint32_t(int(x) + dx), ny = std::uint32_t(int(y) + dy);
            auto next = Read(nx, ny);
            if (next.CliffTextureIndex() == brush_.cliffTexture || next.CliffLevel() == majority) continue;
            SetCliff(next, majority, *conflict, false); Commit(nx, ny, next);
            auto local = NewBlocks(); Gather(local, nx, ny); ConvergeCliffs(local, *conflict);
        }
    }
    void CheckSpecialDoodads(bool remove = true) {
        // 5F3560先收集XY，5F3450逐次查当前首项，5F3290再末项覆盖。
        // 不按最初索引删除，不去重XY，也不把扫描阶段与删除阶段交织。
        std::vector<std::array<std::int32_t, 2>> pending;
        pending.reserve(liveSpecial_.size());
        for (const auto source : liveSpecial_) {
            const auto& shape = *shapeBySource_[source];
            Work();
            if (!shape.hasFootprint) continue;
            const auto columns = shape.x1 - shape.x0 + 1;
            const int base = int(Inspect(shape.x0, shape.y0).CliffLevel()) - shape.relativeCliffLevels.front();
            bool matches = true;
            for (auto x = shape.x0; x <= shape.x1 && matches; ++x) for (auto y = shape.y0; y <= shape.y1; ++y) {
                Work();
                const auto index = std::size_t(y - shape.y0) * columns + x - shape.x0;
                if (Inspect(x, y).CliffLevel() != base + shape.relativeCliffLevels[index]) { matches = false; break; }
            }
            if (matches) continue;
            if (!remove) Reject("TERRAIN_WATER_SPECIAL_DOODAD_DELETE", "后续岸线使剩余特殊模型失配，不能另加原版没有的删除阶段。");
            pending.push_back(shape.position);
        }
        for (const auto& position : pending) {
            auto found = liveSpecial_.end();
            for (auto it = liveSpecial_.begin(); it != liveSpecial_.end(); ++it) {
                Work();
                if (shapeBySource_[*it]->position == position) { found = it; break; }
            }
            if (found == liveSpecial_.end()) continue;
            result_.removedSpecialSourceRecords.push_back(*found);
            *found = liveSpecial_.back(); liveSpecial_.pop_back();
        }
    }
    void CleanWater(const DirtyBlocks& blocks) {
        if (rules_.enforceWaterLimits) EachBlock(blocks, [&](auto x, auto y) {
            auto next = Read(x, y); const double ground = GroundAt(x, y, next);
            if (next.HasWater()) {
                bool remove = Water(next) + rules_.waterLayerOffset < ground;
                if (!remove) for (int dx = -1; dx <= 1 && !remove; ++dx) for (int dy = -1; dy <= 1; ++dy) {
                    if (!Inside(int(x) + dx, int(y) + dy)) continue;
                    const auto& neighbor = Read(std::uint32_t(int(x) + dx), std::uint32_t(int(y) + dy));
                    if (neighbor.HasWater() && (neighbor.waterHeightAndFlagsRaw & 0x3fff) != (next.waterHeightAndFlagsRaw & 0x3fff)) { remove = true; break; }
                }
                if (remove) { next.textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x40); Commit(x, y, next); }
            } else if (const auto water = FirstWet(x, y)) {
                next.waterHeightAndFlagsRaw = static_cast<std::uint16_t>((next.waterHeightAndFlagsRaw & 0xc000) | *water);
                const double surface = double(*water) / 4.0 - 2048.0 + rules_.waterLayerOffset;
                if (ground < surface) {
                    const int target = std::clamp(Truncate((surface - ground) / 128.0) + int(next.CliffLevel()) + 1, 0, int(rules_.maximumCliffLevel));
                    SetCliff(next, target, brush_.cliffTexture, false); marker_[Index(x, y)] = 1;
                }
                Commit(x, y, next);
            }
        });
        ConvergeCliffs(blocks, brush_.cliffTexture);
    }
    void CleanBlight(const DirtyBlocks& blocks) {
        EachBlock(blocks, [&](auto x, auto y) {
            auto next = Read(x, y); if (!next.IsBlight()) return;
            const auto texture = next.GroundTextureIndex();
            if (texture >= grid_.header.groundTilesets.size()) Reject("TERRAIN_WATER_BLIGHT_RESOURCE", "荒芜处理点缺少地面资源身份。");
            const auto& rawcode = grid_.header.groundTilesets[texture];
            auto rule = rules_.groundRules.begin();
            for (; rule != rules_.groundRules.end(); ++rule) { Work(); if (rule->rawcode == rawcode) break; }
            if (rule == rules_.groundRules.end()) Reject("TERRAIN_WATER_BLIGHT_RESOURCE", "荒芜处理点缺少已加载地面资源规则：" + rawcode);
            bool clear = !rule->permitsBlight || (next.HasWater() && Water(next) + rules_.waterLayerOffset > GroundAt(x, y, next));
            // 57E5C0(a4=1)：有坡道邻格时只查5A82D0(mode1)，不能再混入raw悬崖或special。
            const auto ramp = !clear && (!brush_.waterKind || brush_.waterAlgorithm == TerrainWaterAlgorithm::NativeRamps)
                ? RampState(x, y) : TerrainNativeRampState{};
            if (!clear && ramp.belongsToRampCell) clear = ramp.hasUnblockedRampCell;
            for (int dx = 0; dx <= 1 && !clear && !ramp.belongsToRampCell; ++dx) for (int dy = 0; dy <= 1; ++dy) {
                const int cx = int(x) - dx, cy = int(y) - dy;
                if (CliffCell(cx, cy)) { clear = true; break; }
                for (const auto source : liveSpecial_) {
                    const auto& shape = *shapeBySource_[source];
                    Work(); if (cx >= int(shape.x0) && cx < int(shape.x1) && cy >= int(shape.y0) && cy < int(shape.y1)) { clear = true; break; }
                }
                if (clear) break;
            }
            if (clear) { next.textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x20); Commit(x, y, next); }
        });
    }
    void HeightFalloff(const DirtyBlocks& blocks) {
        EachBlock(blocks, [&](auto x, auto y) {
            const auto center = TerrainWaterBrushDetail::MakeHeightFalloffCenter(GroundAt(x, y, Read(x, y)), rules_.minHeight, rules_.maxHeight);
            if (!center) return;
            if (!std::isfinite(center->radiusSquared) || center->radiusSquared <= 0 || center->extent > 32767)
                Reject("TERRAIN_WATER_FALLOFF_DOMAIN", "地高衰减范围超出有限数值域。");
            const auto side = std::uint64_t(center->extent) * 2 + 1;
            if (side * side > limits_.maximumWork - result_.work)
                Reject("TERRAIN_WATER_WORK_BUDGET", "地高衰减扫描超出工作量预算，整笔未修改。");
            for (int dx = -center->extent; dx <= center->extent; ++dx) for (int dy = -center->extent; dy <= center->extent; ++dy) {
                Work(); const int nx = int(x) + dx, ny = int(y) + dy; if (!Inside(nx, ny)) continue;
                const auto weight = TerrainWaterBrushDetail::HeightFalloffWeight(*center, dx * dx + dy * dy);
                if (!weight) continue;
                auto next = Read(std::uint32_t(nx), std::uint32_t(ny));
                if (!TerrainWaterBrushDetail::HeightFalloffWrites(GroundAt(std::uint32_t(nx), std::uint32_t(ny), next), rules_.minHeight, rules_.maxHeight, *weight)) continue;
                next.groundHeightRaw = Encode(Base(next) + *weight); Commit(std::uint32_t(nx), std::uint32_t(ny), next);
            }
        });
    }
    void PropagatePalette(std::uint32_t x, std::uint32_t y) {
        if (!marker_[Index(x, y)]) return;
        std::vector<Frame> stack; stack.reserve(limits_.maximumRecursionDepth);
        stack.push_back({x, y, 1, 0, 0, Read(x, y).CliffTextureIndex()});
        while (!stack.empty()) {
            Work();
            auto& frame = stack.back(); if (frame.next == 16) { stack.pop_back(); continue; }
            const int slot = frame.next++, cell = slot / 4, corner = slot % 4;
            const int cx = int(frame.x) + cell / 2 - 1, cy = int(frame.y) + cell % 2 - 1;
            if (!CliffCell(cx, cy)) continue;
            const auto nx = std::uint32_t(cx + corner / 2), ny = std::uint32_t(cy + corner % 2);
            const auto index = Index(nx, ny);
            if (marker_[index] && marker_[index] <= frame.depth) continue;
            auto next = Read(nx, ny); if (next.CliffTextureIndex() == frame.palette) continue;
            const auto depth = frame.depth + 1; CheckDepth(depth);
            SetCliff(next, next.CliffLevel(), frame.palette, false); Commit(nx, ny, next);
            marker_[index] = static_cast<std::uint8_t>(depth);
            stack.push_back({nx, ny, depth, 0, 0, frame.palette});
        }
    }
    void ApplyCliff(GridPoint center, int target) {
        auto blocks = NewBlocks(); std::array<std::size_t, 16> oldLevels{};
        Footprint(center, brush_.size, [&](auto x, auto y) {
            auto next = Read(x, y); int low = 0;
            if (rules_.enforceWaterLimits && next.HasWater()) {
                const float surface = static_cast<float>(Water(next) + rules_.waterLayerOffset);
                low = std::max(0, int(next.CliffLevel()) - (Truncate((GroundAt(x, y, next) - double(surface)) / 128.0) + 2));
            }
            target = std::max(std::min(target, int(rules_.maximumCliffLevel)), low);
            if (target == next.CliffLevel() && next.CliffTextureIndex() == brush_.cliffTexture) return;
            ++oldLevels[next.CliffLevel()]; SetCliff(next, target, brush_.cliffTexture, true);
            Commit(x, y, next); marker_[Index(x, y)] = 1;
            for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
                if ((dx == 0 && dy == 0) || !Inside(int(x) + dx, int(y) + dy)) continue;
                const auto nx = std::uint32_t(int(x) + dx), ny = std::uint32_t(int(y) + dy);
                auto neighbor = Read(nx, ny); SetCliff(neighbor, neighbor.CliffLevel(), brush_.cliffTexture, true); Commit(nx, ny, neighbor);
            }
            Gather(blocks, x, y);
        });
        const auto majority = std::max_element(oldLevels.begin(), oldLevels.end());
        if (*majority != 0) {
            const int radius = int(brush_.size) - 1;
            for (int x = std::max(0, int(center.x) - radius); x <= std::min(int(width_) - 1, int(center.x) + radius); ++x)
                for (int y = std::max(0, int(center.y) - radius); y <= std::min(int(height_) - 1, int(center.y) + radius); ++y)
                    RepairConflict(std::uint32_t(x), std::uint32_t(y), int(majority - oldLevels.begin()));
        }
        ConvergeCliffs(blocks, brush_.cliffTexture);
        // 5A8BF0：按脏块x外/y内在live网格分类，只清除已不属于含R/S地格的旧位。
        // 旧v1水刷仍在Read拒绝坡道；v2和普通悬崖共用完整当前分类。
        EachBlock(blocks, [&](auto x, auto y) {
            auto next = Read(x, y);
            if (next.IsRamp() && !RampState(x, y).belongsToRampCell) {
                next.textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x10);
                Commit(x, y, next);
            }
        });
        CheckSpecialDoodads(); CleanWater(blocks); CleanBlight(blocks); HeightFalloff(blocks);
        const int radius = int(brush_.size) - 1;
        for (int x = std::max(0, int(center.x) - radius); x < std::min(int(width_), int(center.x) + radius + 1); ++x)
            for (int y = std::max(0, int(center.y) - radius); y < std::min(int(height_), int(center.y) + radius + 1); ++y) {
                Work();
                marker_[Index(std::uint32_t(x), std::uint32_t(y))] = 1;
            }
        EachBlock(blocks, [&](auto x, auto y) { PropagatePalette(x, y); }); ClearMarkers();
    }
    void ApplyNativeRamp(const TerrainStrokePoint& point) {
        Work(64); // 三点直坡或九点拐角及紧邻几何检查，先预收其有限工作。
        const auto reject = [&](const char* suffix, const char* message) {
            Reject(std::string(brush_.straightRamp ? "TERRAIN_STRAIGHT_RAMP_" : "TERRAIN_CORNER_RAMP_") + suffix, message);
        };
        TerrainCornerRampPlan plan; std::size_t selectedCount = 9;
        if (brush_.straightRamp) {
            const auto planned = PlanTerrainStraightRamp(grid_, point, brush_.straightAlgorithm);
            if (!planned.Succeeded() || !planned.value)
                Reject(planned.diagnostics.empty() ? "TERRAIN_STRAIGHT_RAMP_STEP" : planned.diagnostics.front().code,
                    planned.diagnostics.empty() ? "无法规划一级直坡。" : planned.diagnostics.front().message);
            std::copy(planned.value->vertexIndices.begin(), planned.value->vertexIndices.end(), plan.vertexIndices.begin());
            plan.highX = planned.value->highX; plan.highY = planned.value->highY;
            plan.directionX = planned.value->directionX; plan.directionY = planned.value->directionY;
            selectedCount = 3;
        } else {
            const auto planned = PlanTerrainCornerRamp(grid_, point, brush_.cornerAlgorithm);
            if (!planned.Succeeded() || !planned.value)
                Reject(planned.diagnostics.empty() ? "TERRAIN_CORNER_RAMP_STEP" : planned.diagnostics.front().code,
                    planned.diagnostics.empty() ? "无法规划拐角坡道。" : planned.diagnostics.front().message);
            plan = *planned.value;
        }
        const auto selectedIndices = std::span<const std::uint32_t>(plan.vertexIndices).first(selectedCount);
        const auto high = Read(plan.highX, plan.highY);
        if (high.CliffLevel() > rules_.maximumCliffLevel)
            reject("LEVEL", "坡道层级超过本次实际Terrain.MaxHeight规则。");
        auto blocks = NewBlocks();
        const bool connected = brush_.straightRamp && brush_.straightAlgorithm == TerrainStraightRampAlgorithm::Connected;
        const auto selected = [&](std::uint32_t x, std::uint32_t y) {
            return std::find(selectedIndices.begin(), selectedIndices.end(), std::uint32_t(Index(x, y))) != selectedIndices.end();
        };
        constexpr std::array<std::string_view, 24> patterns{
            "HRAA","RHAA","AARH","AAHR","AHRA","ARHA","HAAR","RAAH",
            "HRAB","RHBA","BARH","ABHR","BHRA","ARHB","HBAR","RABH",
            "RLAA","LRAA","AALR","AARL","ARLA","ALRA","RAAL","LAAR"};
        const auto eachCell = [&](auto operation) {
            for (const auto block : blocks.entries) {
                const auto x0 = (block % blockWidth_) * 4, y0 = (block / blockWidth_) * 4;
                for (auto x = x0; x < x0 + 4; ++x) for (auto y = y0; y < y0 + 4; ++y) { Work(); operation(x, y); }
            }
        };
        const auto classifyCell = [&](std::uint32_t x, std::uint32_t y) {
            return std::array<char, 4>{RampState(x, y + 1).corner, RampState(x + 1, y + 1).corner,
                RampState(x + 1, y).corner, RampState(x, y).corner};
        };
        const auto weight = [](const auto& corners) {
            unsigned result = 0; for (const auto c : corners) result += c == 'R' ? 1u : c == 'S' ? 2u : 0u; return result;
        };
        const auto matches = [&](const auto& corners) {
            return std::find(patterns.begin(), patterns.end(), std::string_view(corners.data(), corners.size())) != patterns.end();
        };
        const auto checkLiveCells = [&](bool before) {
            // 5A8F10 / 5A8650：每次都从当前候选取完整R/S分类，不重用before标签。
            eachCell([&](auto x, auto y) {
                const auto corners = classifyCell(x, y); const auto sum = weight(corners);
                if (sum && sum <= 1 && !matches(corners))
                    reject(before ? "BEFORE_INVALID" : "INVALID", "原版坡道live地格合法性检查未通过，整笔未修改。");
            });
        };
        const auto checkDomain = [&](bool before) {
            // 原独立模式仍要求halo内只有本次三/九点。连接模式接受同纯直台阶的
            // 完整三点条带；检查before和after，不以目标点合法替代旧邻坡的完整性。
            EachBlock(blocks, [&](auto x, auto y) {
                if (x < 2 || y < 2 || x + 2 >= width_ || y + 2 >= height_)
                    reject("NEIGHBORHOOD", "坡道实际脏块和分类halo必须完整在地图内。");
                for (int ox = -2; ox <= 2; ++ox) for (int oy = -2; oy <= 2; ++oy) {
                    const auto nx = std::uint32_t(int(x) + ox), ny = std::uint32_t(int(y) + oy);
                    const auto& vertex = Read(nx, ny);
                    const int sx = (int(nx) - int(plan.highX)) * plan.directionX;
                    const int sy = (int(ny) - int(plan.highY)) * plan.directionY;
                    if ((brush_.cornerAlgorithm == TerrainCornerRampAlgorithm::DryOnly && vertex.HasWater())
                        || vertex.IsBoundary() || vertex.IsMapEdge()
                        || vertex.groundHeightRaw != high.groundHeightRaw
                        || vertex.CliffTextureIndex() != high.CliffTextureIndex()
                        || vertex.CliffLevel() != high.CliffLevel() - ((sx > 0 || sy > 0) ? 1 : 0))
                        reject(before ? "BEFORE_DOMAIN" : "DOMAIN", "坡道实际脏块及halo需等地高、同悬崖材质的一级台阶，不能相交边界、其它方向或旧算法不支持的水。");
                    if (!connected) {
                        if (vertex.IsRamp() != (selected(nx, ny) && *brush_.cornerRamp))
                            reject("DOMAIN", "独立坡道的实际脏块及halo不能相交其它旧坡道。");
                        continue;
                    }
                    if (!before && selected(nx, ny) && vertex.IsRamp() != *brush_.cornerRamp)
                        reject("REQUEST", "原版补角闭包未保留请求的完整目标条带，整笔未修改。");
                    if (!vertex.IsRamp()) continue;
                    const int normal = sx + sy;
                    if (normal < 0 || normal > 2)
                        reject(before ? "BEFORE_DOMAIN" : "DOMAIN", "连接直坡的旧R位越出同一条三点法向条带。");
                    const int tangent = -(int(nx) - int(plan.highX)) * plan.directionY
                        + (int(ny) - int(plan.highY)) * plan.directionX;
                    for (int step = 0; step < 3; ++step) {
                        const int rx = int(plan.highX) + step * plan.directionX - tangent * plan.directionY;
                        const int ry = int(plan.highY) + step * plan.directionY + tangent * plan.directionX;
                        if (!Inside(rx, ry) || !Read(std::uint32_t(rx), std::uint32_t(ry)).IsRamp())
                            reject(before ? "BEFORE_DOMAIN" : "DOMAIN", "连接直坡的每条相邻旧坡道必须有完整三点R位。");
                    }
                }
            });
        };
        if (connected) {
            // 收集本次目标的完整候选域；无R净差也验证相同before/after合同。
            for (const auto index : selectedIndices) Gather(blocks, index % width_, index / width_);
            checkDomain(true); checkLiveCells(true);
        }
        bool changedRamp = false;
        // 5A8FD0：仅对实际R位变化点提交并按x外/y内Gather。
        for (const auto index : selectedIndices) {
            const auto x = index % width_, y = index / width_; auto next = Read(x, y);
            if (next.IsRamp() == *brush_.cornerRamp) continue;
            next.textureAndFlagsRaw = std::uint8_t(*brush_.cornerRamp
                ? next.textureAndFlagsRaw | 0x10 : next.textureAndFlagsRaw & ~0x10);
            Commit(x, y, next); changedRamp = true;
            if (!connected) Gather(blocks, x, y);
        }
        // 无R位净变化仍须验收同一完整接受域，不能借no-op绕过邻坡/水/模型拒绝。
        // 这些块仅供只读检查，不触发原版仅对实际变化块执行的荒芜结束器。
        if (!changedRamp && !connected) for (const auto index : selectedIndices) Gather(blocks, index % width_, index / width_);
        checkDomain(false);
        for (const auto& shape : shapes_) {
            Work();
            if (shape.hasFootprint && !result_.processed.Empty() && shape.x0 < result_.processed.x1
                && shape.x1 > result_.processed.x0 && shape.y0 < result_.processed.y1 && shape.y1 > result_.processed.y0)
                reject("SPECIAL", "坡道实际处理域与特殊地形模型相交，当前不授予删除或改写资格。");
        }
        checkLiveCells(false);
        // 5A8D10先完整扫描/去重，后统一置R；只接受闭包不越本次三/九个选点。
        std::array<std::uint32_t, 9> additions{}; std::size_t count = 0;
        eachCell([&](auto x, auto y) {
            const auto corners = classifyCell(x, y);
            if (!weight(corners) || matches(corners)) return;
            for (const auto offset : {std::array{0u, 1u}, std::array{1u, 1u}, std::array{1u, 0u}, std::array{0u, 0u}}) {
                const auto nx = x + offset[0], ny = y + offset[1];
                if (Read(nx, ny).IsRamp()) continue;
                if (!selected(nx, ny)) reject("CLOSURE", "原版补角闭包超出本次选点，当前形态明确拒绝。");
                const auto id = std::uint32_t(Index(nx, ny));
                if (std::find(additions.begin(), additions.begin() + count, id) == additions.begin() + count) additions[count++] = id;
            }
        });
        for (std::size_t i = 0; i < count; ++i) {
            const auto x = additions[i] % width_, y = additions[i] / width_; auto next = Read(x, y);
            next.textureAndFlagsRaw |= 0x10; Commit(x, y, next);
        }
        if (connected) { checkDomain(false); checkLiveCells(false); }
        if (changedRamp) CleanBlight(blocks); // 原ramp荒芜结束器；不执行普通cliff结束器。
    }
};

std::size_t ValidateInputs(const TerrainGridSnapshot& terrain, const TerrainWaterLoadedRules& rules, const StructuralBrush& brush,
    std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> shapes, const TerrainWaterBrushLimits& limits) {
    ValidateGrid(terrain); const TerrainWaterBrushLimits hard;
    if (!limits.maximumWork || limits.maximumWork > hard.maximumWork || !limits.maximumPoints || limits.maximumPoints > hard.maximumPoints
        || !limits.maximumCandidateBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes
        || !limits.maximumRecursionDepth || limits.maximumRecursionDepth > hard.maximumRecursionDepth)
        Reject("TERRAIN_WATER_LIMITS", "水刷预算只能收紧硬上限，不能为零。");
    if (limits.maximumCandidateBytes < sizeof(TerrainGridSnapshot))
        Reject("TERRAIN_WATER_MEMORY_BUDGET", "水刷候选预算不足以容纳快照。");
    std::size_t work = 0;
    const auto charge = [&](std::size_t count = 1) {
        if (count > limits.maximumWork - work) Reject("TERRAIN_WATER_WORK_BUDGET", "水刷输入检查超过工作量预算。");
        work += count;
    };
    if (points.empty() || points.size() > limits.maximumPoints) Reject("TERRAIN_WATER_POINTS", "水刷点数量不在显式预算内。");
    if (brush.cornerRamp) charge(64); // 准备入口的固定几何查询也计入同笔总工作，不只计owned阶段。
    if (brush.size < 1 || brush.size > 8 || (brush.shape != TerrainBrushShape::Circle && brush.shape != TerrainBrushShape::Square)
        || (brush.waterKind && *brush.waterKind != TerrainWaterBrushKind::Shallow && *brush.waterKind != TerrainWaterBrushKind::Deep)
        || (brush.waterAlgorithm != TerrainWaterAlgorithm::LegacyFlat && brush.waterAlgorithm != TerrainWaterAlgorithm::NativeRamps)
        || (!brush.waterKind && (brush.cliffDelta < -1 || brush.cliffDelta > 1)))
        Reject("TERRAIN_WATER_BRUSH", "水刷种类、形状或经典尺寸非法。");
    const std::array numeric{rules.maxSlopeDegrees, rules.minHeight, rules.maxHeight, rules.waterLayerOffset, rules.deepWaterThreshold};
    if (std::any_of(numeric.begin(), numeric.end(), [](auto value) { return !std::isfinite(value); })
        || rules.maxSlopeDegrees < 0 || rules.maxSlopeDegrees > 90 || rules.deepWaterThreshold < 0 || rules.maximumCliffLevel > 14)
        Reject("TERRAIN_WATER_RULES", "已加载水刷参数不在有限支持域内。");
    if (brush.anchor.groundHeightRaw > 0x3fff || brush.anchor.waterHeightRaw > 0x3fff || brush.anchor.cliffLevel > 14)
        Reject("TERRAIN_WATER_ANCHOR", "固定水刷锚点的原始字段非法。");
    if (brush.waterKind && brush.waterAlgorithm == TerrainWaterAlgorithm::NativeRamps) {
        charge(512);
        const auto captured = CaptureTerrainWaterStrokeAnchor(terrain, points.front(), rules.maximumCliffLevel);
        if (!captured.Succeeded() || !captured.value)
            Reject("TERRAIN_WATER_ANCHOR", "固定水刷起点的完整坡道邻域无法验证。");
        const auto& actual = *captured.value;
        if (actual.groundHeightRaw != brush.anchor.groundHeightRaw || actual.waterHeightRaw != brush.anchor.waterHeightRaw
            || actual.cliffLevel != brush.anchor.cliffLevel || actual.hasWater != brush.anchor.hasWater
            || actual.rampHeightOffset != brush.anchor.rampHeightOffset)
            Reject("TERRAIN_WATER_ANCHOR", "固定水刷锚点不是本笔起点在实际规则下的完整原始字段和有效地高。");
    } else if (brush.anchor.rampHeightOffset)
        Reject("TERRAIN_WATER_ANCHOR", "旧水刷或普通悬崖不得附带新水算法锚点偏移。");
    if (rules.groundRules.size() > 256 || rules.availableCliffTilesets.size() > 256
        || terrain.header.groundTilesets.size() > 256 || terrain.header.cliffTilesets.size() > 256)
        Reject("TERRAIN_WATER_RULES", "水刷资源表超过经典读取预算。");
    if (brush.cliffTexture >= terrain.header.cliffTilesets.size() || brush.cliffTexture >= 15)
        Reject("TERRAIN_WATER_CLIFF_RESOURCE", "所选悬崖槽位超出 W3E 资源表。");
    for (const auto& point : points) { charge(); if (!Snap(terrain, point)) Reject("TERRAIN_WATER_OUTSIDE", "水刷落点不是有限地图内坐标。"); }
    for (const auto* ids : {&terrain.header.groundTilesets, &terrain.header.cliffTilesets, &rules.availableCliffTilesets})
        for (const auto& id : *ids) { charge(); if (id.size() != 4) Reject("TERRAIN_WATER_RULES", "地形资源身份必须是四字节 rawcode。"); }
    charge(rules.availableCliffTilesets.size());
    if (std::find(rules.availableCliffTilesets.begin(), rules.availableCliffTilesets.end(), terrain.header.cliffTilesets[brush.cliffTexture]) == rules.availableCliffTilesets.end())
        Reject("TERRAIN_WATER_CLIFF_RESOURCE", "所选悬崖槽位没有对应的已加载资源。");
    for (const auto& entry : rules.groundRules) { charge(); if (entry.rawcode.size() != 4)
        Reject("TERRAIN_WATER_RULES", "地面规则身份必须是四字节 rawcode。"); }
    for (std::size_t i = 0; i < rules.groundRules.size(); ++i) for (std::size_t j = 0; j < i; ++j)
        { charge(); if (rules.groundRules[i].rawcode == rules.groundRules[j].rawcode) Reject("TERRAIN_WATER_RULES", "已加载地面规则存在重复身份。"); }
    if (doodads.specialVersion != 0 || shapes.size() != doodads.specialDoodads.size())
        Reject("TERRAIN_WATER_SPECIAL_DOODAD_FACTS", "需要实际解析的 DOO special 段及逐记录资源形态。");
    // 完整计量在任何 shapeSeen/candidate/辅助容器分配之前进行。
    std::size_t estimate = sizeof(TerrainGridSnapshot);
    const auto add = [&](std::size_t count, std::size_t bytes) {
        if (bytes && count > (limits.maximumCandidateBytes - estimate) / bytes)
            Reject("TERRAIN_WATER_MEMORY_BUDGET", "水刷候选及辅助结构超过内存预算。");
        estimate += count * bytes;
    };
    add(terrain.vertices.size(), sizeof(TerrainVertex) + 2);
    add(terrain.tiles.size(), sizeof(TerrainTile));
    add(terrain.header.groundTilesets.size() + terrain.header.cliffTilesets.size(), sizeof(std::string) + 4);
    add(shapes.size(), sizeof(std::uint8_t) + sizeof(void*) + 2 * sizeof(std::uint32_t) + sizeof(std::array<std::int32_t, 2>));
    // 候选会复制整个DOO，而不只复制special尾。先按实际展开结构计量并收费，
    // 再调用无分配的共享wire计量器；禁止为检查预算先序列化或复制嵌套记录。
    add(1, sizeof(DoodadPlacements));
    add(doodads.OriginalBytes().size(), 1); charge(doodads.OriginalBytes().size());
    add(doodads.doodads.size(), sizeof(DoodadPlacement)); charge(doodads.doodads.size());
    add(doodads.specialDoodads.size(), sizeof(SpecialDoodadPlacement)); charge(doodads.specialDoodads.size());
    const auto string = [&](const std::string& value) { add(value.size() + 1, 1); charge(value.size()); };
    for (const auto& placement : doodads.doodads) {
        string(placement.rawcode);
        if (placement.skinId) string(*placement.skinId);
        add(placement.itemSets.size(), sizeof(PlacementItemSet)); charge(placement.itemSets.size());
        for (const auto& set : placement.itemSets) {
            add(set.items.size(), sizeof(PlacementItemChoice)); charge(set.items.size());
            for (const auto& item : set.items) string(item.rawcode);
        }
    }
    for (const auto& placement : doodads.specialDoodads) string(placement.rawcode);
    PlacementReadLimits placementLimits;
    placementLimits.maxBytes = std::min(placementLimits.maxBytes, limits.maximumCandidateBytes);
    placementLimits.maxPlacements = std::min(placementLimits.maxPlacements, limits.maximumWork);
    placementLimits.maxNestedRecords = std::min(placementLimits.maxNestedRecords, limits.maximumWork);
    const auto projection = MeasureClassicPlacementWire(doodads, {}, placementLimits);
    if (!projection.Succeeded() || !projection.value)
        Reject("TERRAIN_WATER_SPECIAL_DOODAD_FACTS", "装饰物输入不在完整经典往返及本笔预算内。");
    charge(projection.value->placementCount); charge(projection.value->nestedRecordCount);
    add(std::size_t((terrain.header.width - 2) / 4 + 1) * ((terrain.header.height - 2) / 4 + 1), 3 * (sizeof(std::uint32_t) + 1));
    add(limits.maximumRecursionDepth, 64);
    charge(shapes.size() * 2);
    std::vector<bool> shapeSeen(shapes.size());
    for (const auto& shape : shapes) {
        if (shape.sourceRecordIndex >= doodads.specialDoodads.size() || shapeSeen[shape.sourceRecordIndex])
            Reject("TERRAIN_WATER_SPECIAL_DOODAD_FACTS", "DOO special 资源形态没有一一绑定源记录。");
        shapeSeen[shape.sourceRecordIndex] = true; const auto& source = doodads.specialDoodads[shape.sourceRecordIndex];
        if (shape.rawcode.size() != 4 || source.rawcode.size() != 4 || shape.rawcode != source.rawcode
            || shape.variation != source.variation || shape.position != source.position
            || (!shape.hasFootprint && !shape.relativeCliffLevels.empty())
            || (shape.hasFootprint && (shape.x0 >= shape.x1 || shape.y0 >= shape.y1 || shape.x1 >= terrain.header.width || shape.y1 >= terrain.header.height
            || shape.relativeCliffLevels.size() != std::uint64_t(shape.x1 - shape.x0 + 1) * (shape.y1 - shape.y0 + 1))))
            Reject("TERRAIN_WATER_SPECIAL_DOODAD_FACTS", "DOO special 资源形态与源记录或地图范围不一致。");
        if (!shape.hasFootprint) continue;
        const auto columns = shape.x1 - shape.x0 + 1;
        const int base = int(terrain.vertices[std::size_t(shape.y0) * terrain.header.width + shape.x0].CliffLevel()) - shape.relativeCliffLevels.front();
        for (auto y = shape.y0; y <= shape.y1; ++y) for (auto x = shape.x0; x <= shape.x1; ++x) {
            charge();
            const auto relative = shape.relativeCliffLevels[std::size_t(y - shape.y0) * columns + x - shape.x0];
            if (relative < 0 || relative > 2 || terrain.vertices[std::size_t(y) * terrain.header.width + x].CliffLevel() != base + relative)
                Reject("TERRAIN_WATER_SPECIAL_DOODAD_SOURCE_SHAPE", "原特殊模型形状已失配，不能借水刷清理坏源数据。");
        }
    }
    if (!terrain.tiles.empty() && terrain.tiles.size() != std::uint64_t(terrain.header.width - 1) * (terrain.header.height - 1))
        Reject("TERRAIN_WATER_SOURCE_DERIVED", "源地形地格索引数量不自洽。");
    std::array<float, 3> minimum{}, maximum{};
    std::size_t groundMisses = 0, cliffMisses = 0;
    for (std::size_t i = 0; i < terrain.vertices.size(); ++i) {
        charge(); const auto& vertex = terrain.vertices[i];
        const std::array<float, 3> expected{
            static_cast<float>(double(terrain.header.offset[0]) + double(i % terrain.header.width) * 128.0),
            static_cast<float>(double(terrain.header.offset[1]) + double(i / terrain.header.width) * 128.0),
            static_cast<float>((double(vertex.groundHeightRaw) - 8192.0) / 4.0 + 128.0 * (int(vertex.CliffLevel()) - 2))};
        const auto expectedWater = static_cast<float>(Water(vertex));
        if (vertex.position != expected || vertex.waterBaseHeight != expectedWater)
            Reject("TERRAIN_WATER_SOURCE_DERIVED", "源地形位置或水基底与其原始 W3E 字段不自洽。");
        if (i == 0) minimum = maximum = expected;
        for (std::size_t axis = 0; axis < 3; ++axis) { minimum[axis] = std::min(minimum[axis], expected[axis]); maximum[axis] = std::max(maximum[axis], expected[axis]); }
        if (vertex.GroundTextureIndex() >= terrain.header.groundTilesets.size()) ++groundMisses;
        if (vertex.CliffTextureIndex() != 15 && vertex.CliffTextureIndex() >= terrain.header.cliffTilesets.size()) ++cliffMisses;
    }
    if (minimum != terrain.minimum || maximum != terrain.maximum || groundMisses != terrain.unresolvedGroundTextures || cliffMisses != terrain.unresolvedCliffTextures)
        Reject("TERRAIN_WATER_SOURCE_DERIVED", "源地形边界或资源计数与顶点不自洽。");
    for (std::uint32_t tile = 0; tile < terrain.tiles.size(); ++tile) for (std::uint32_t corner = 0; corner < 4; ++corner) {
        charge(); if (terrain.tiles[tile].vertices[corner] != *ResolveTerrainTileCornerIndex(terrain.header.width, terrain.header.height, tile, corner))
            Reject("TERRAIN_WATER_SOURCE_DERIVED", "源地形地格角索引不自洽。");
    }
    // 候选原始记录复制及两张辅助位表初始化同样计入总工作量。
    charge(terrain.vertices.size() * 3 + terrain.tiles.size());
    return work;
}

} // namespace

namespace TerrainWaterBrushDetail {

std::optional<std::uint16_t> EncodeBaseHeight(double base) {
    const double value = std::trunc((base + 2048.0) * 4.0);
    if (!std::isfinite(value) || value < 0 || value > 16383) return {};
    return static_cast<std::uint16_t>(value);
}

std::optional<HeightFalloffCenter> MakeHeightFalloffCenter(double ground, float minimum, float maximum) {
    const float low = std::min(minimum, -256.0f);
    double target;
    if (ground > maximum) target = maximum;
    else if (ground < low) target = low;
    else return {};
    const float delta = static_cast<float>(target - ground);
    if (std::abs(double(delta)) < kHeightEpsilon) return {};
    const double radius = std::abs(double(delta)) / std::tan(kDegreeFactor * 30.0);
    const double extent = std::trunc(radius / 128.0) + 2;
    if (!std::isfinite(extent) || extent > std::numeric_limits<std::int32_t>::max())
        Reject("TERRAIN_WATER_FALLOFF_DOMAIN", "地高衰减半径超过整数域。");
    return HeightFalloffCenter{delta, static_cast<float>(radius * radius), radius, static_cast<std::int32_t>(extent)};
}

std::optional<float> HeightFalloffWeight(const HeightFalloffCenter& center, std::int32_t distanceSquared) {
    if (distanceSquared < 0 || center.radiusSquared <= 0) return {};
    const double rho = std::sqrt(((double(distanceSquared) * 128.0) * 128.0) / double(center.radiusSquared));
    if (rho > 1.0) return {};
    return static_cast<float>(((std::cos((rho * kPi) * 0.5) + 1.0) * double(center.delta)) * 0.5);
}

bool HeightFalloffWrites(double ground, float minimum, float maximum, float weight) {
    return ground > double(maximum) + double(weight) || ground < double(std::min(minimum, -256.0f)) + double(weight);
}

} // namespace TerrainWaterBrushDetail

Result<TerrainWaterStrokeAnchor> CaptureTerrainWaterStrokeAnchor(const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point) {
    try {
        ValidateGrid(terrain); const auto at = Snap(terrain, point);
        if (!at) Reject("TERRAIN_WATER_ANCHOR", "水刷锚点必须是有限地图内坐标。");
        const auto& vertex = terrain.vertices[std::size_t(at->y) * terrain.header.width + at->x]; ValidateVertex(vertex, at->x, at->y);
        return Result<TerrainWaterStrokeAnchor>::Success({vertex.groundHeightRaw,
            static_cast<std::uint16_t>(vertex.waterHeightAndFlagsRaw & 0x3fff), vertex.CliffLevel(), vertex.HasWater()});
    } catch (const Rejection& error) { return Result<TerrainWaterStrokeAnchor>::Failure(std::string(kSubsystem), error.code, error.text); }
}

Result<TerrainWaterStrokeAnchor> CaptureTerrainWaterStrokeAnchor(const TerrainGridSnapshot& terrain,
    const TerrainStrokePoint& point, std::uint8_t maximumCliffLevel, std::size_t maximumWork) {
    try {
        if (maximumWork < 512) Reject("TERRAIN_WATER_WORK_BUDGET", "完整水刷锚点查询超出本笔工作预算。");
        ValidateGrid(terrain); const auto at = Snap(terrain, point);
        if (!at) Reject("TERRAIN_WATER_ANCHOR", "水刷锚点必须是有限地图内坐标。");
        const auto& vertex = terrain.vertices[std::size_t(at->y) * terrain.header.width + at->x];
        ValidateVertex(vertex, at->x, at->y, true);
        const auto state = QueryNativeTerrainRamp(terrain, maximumCliffLevel, at->x, at->y);
        if (!state.Succeeded() || !state.value) return {{}, state.diagnostics};
        return Result<TerrainWaterStrokeAnchor>::Success({vertex.groundHeightRaw,
            static_cast<std::uint16_t>(vertex.waterHeightAndFlagsRaw & 0x3fff), vertex.CliffLevel(), vertex.HasWater(),
            static_cast<std::uint8_t>(state.value->heightOffset)});
    } catch (const Rejection& error) { return Result<TerrainWaterStrokeAnchor>::Failure(std::string(kSubsystem), error.code, error.text); }
}

namespace {
Result<TerrainWaterStrokeCandidate> PrepareStructuralStroke(const TerrainGridSnapshot& source, const TerrainWaterLoadedRules& rules,
    const StructuralBrush& brush, std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes, const TerrainWaterBrushLimits& limits) {
    try {
        const auto initialWork = ValidateInputs(source, rules, brush, points, doodads, specialShapes, limits);
        auto candidate = source;
        StructuralTransaction transaction(candidate, rules, brush, doodads, specialShapes, limits, initialWork);
        auto result = transaction.Run(points, source);
        auto nextDoodads = doodads;
        transaction.WriteSpecialOutput(doodads, nextDoodads, result);
        return Result<TerrainWaterStrokeCandidate>::Success({std::move(candidate), std::move(result), std::move(nextDoodads)});
    } catch (const Rejection& error) { return Result<TerrainWaterStrokeCandidate>::Failure(std::string(kSubsystem), error.code, error.text); }
    catch (const std::bad_alloc&) { return Result<TerrainWaterStrokeCandidate>::Failure(std::string(kSubsystem), "TERRAIN_WATER_ALLOCATION", "水刷候选分配失败，原地形未修改。"); }
    catch (const std::length_error&) { return Result<TerrainWaterStrokeCandidate>::Failure(std::string(kSubsystem), "TERRAIN_WATER_ALLOCATION", "水刷候选超出容器容量，原地形未修改。"); }
}
} // namespace

Result<TerrainWaterStrokeCandidate> PrepareTerrainWaterStroke(const TerrainGridSnapshot& source, const TerrainWaterLoadedRules& rules,
    const TerrainWaterBrush& brush, std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes, const TerrainWaterBrushLimits& limits) {
    return PrepareStructuralStroke(source, rules, {brush.shape, brush.size, brush.cliffTexture, brush.randomSeed,
        brush.anchor, brush.kind, 0, brush.algorithm}, points, doodads, specialShapes, limits);
}

Result<TerrainWaterStrokeCandidate> PrepareTerrainCliffStroke(const TerrainGridSnapshot& source, const TerrainWaterLoadedRules& rules,
    const TerrainCliffBrush& brush, std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes, const TerrainWaterBrushLimits& limits) {
    return PrepareStructuralStroke(source, rules, {brush.shape, brush.size, brush.cliffTexture, brush.randomSeed,
        {brush.anchor.groundHeightRaw, 0, brush.anchor.cliffLevel, false}, {}, brush.levelDelta},
        points, doodads, specialShapes, limits);
}

Result<TerrainWaterStrokeCandidate> PrepareTerrainCornerRampStroke(const TerrainGridSnapshot& source, const TerrainWaterLoadedRules& rules,
    const TerrainBrush& brush, std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes, const TerrainWaterBrushLimits& limits,
    TerrainCornerRampAlgorithm algorithm) {
    if (!IsCanonicalTerrainCornerRampBrush(brush) || points.size() != 1)
        return Result<TerrainWaterStrokeCandidate>::Failure(std::string(kSubsystem), "TERRAIN_CORNER_RAMP_BRUSH",
            "拐角坡道只支持单个canonical落点及创建/移除选项，不能组合其它笔刷字段。");
    const auto plan = PlanTerrainCornerRamp(source, points.front(), algorithm);
    if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
    const auto& high = source.vertices[std::size_t(plan.value->highY) * source.header.width + plan.value->highX];
    return PrepareStructuralStroke(source, rules, {TerrainBrushShape::Circle, 1, high.CliffTextureIndex(), 0,
        {high.groundHeightRaw, 0, high.CliffLevel(), false}, {}, 0, TerrainWaterAlgorithm::NativeRamps, brush.value, algorithm},
        points, doodads, specialShapes, limits);
}

Result<TerrainWaterStrokeCandidate> PrepareTerrainStraightRampStroke(const TerrainGridSnapshot& source, const TerrainWaterLoadedRules& rules,
    const TerrainBrush& brush, std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes, const TerrainWaterBrushLimits& limits,
    TerrainStraightRampAlgorithm algorithm) {
    if (!IsCanonicalTerrainStraightRampBrush(brush) || points.size() != 1)
        return Result<TerrainWaterStrokeCandidate>::Failure(std::string(kSubsystem), "TERRAIN_STRAIGHT_RAMP_BRUSH",
            "一级直坡只支持单个canonical落点及创建/移除选项，不能组合其它笔刷字段。");
    const auto plan = PlanTerrainStraightRamp(source, points.front(), algorithm);
    if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
    const auto& high = source.vertices[std::size_t(plan.value->highY) * source.header.width + plan.value->highX];
    return PrepareStructuralStroke(source, rules, {TerrainBrushShape::Circle, 1, high.CliffTextureIndex(), 0,
        {high.groundHeightRaw, 0, high.CliffLevel(), false}, {}, 0, TerrainWaterAlgorithm::NativeRamps,
        brush.value, TerrainCornerRampAlgorithm::WetTerrain, true, algorithm}, points, doodads, specialShapes, limits);
}

Result<TerrainWaterBrushResult> ApplyTerrainWaterStroke(TerrainGridSnapshot& terrain, const TerrainWaterLoadedRules& rules,
    const TerrainWaterBrush& brush, std::span<const TerrainStrokePoint> points, const DoodadPlacements& doodads,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes, const TerrainWaterBrushLimits& limits) {
    auto prepared = PrepareTerrainWaterStroke(terrain, rules, brush, points, doodads, specialShapes, limits);
    if (!prepared.Succeeded() || !prepared.value)
        return {std::nullopt, std::move(prepared.diagnostics)};
    if (!prepared.value->result.removedSpecialSourceRecords.empty())
        return Result<TerrainWaterBrushResult>::Failure(std::string(kSubsystem), "TERRAIN_WATER_SPECIAL_DOODAD_DELETE", "此兼容入口不能同时提交DOO删除，请使用完整水刷候选事务。");
    terrain = std::move(prepared.value->terrain);
    return Result<TerrainWaterBrushResult>::Success(std::move(prepared.value->result));
}

} // namespace War3Map::MapData
