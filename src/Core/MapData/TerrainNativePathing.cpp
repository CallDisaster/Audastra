#include "War3MapCore/MapData/TerrainNativePathing.h"
#include "TerrainPaletteEditInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.NativePathing";
constexpr std::size_t kMaximumCells = 8192ull * 8192ull;
constexpr double kMaximumCoordinate = 16777216.0;
constexpr double kPackedCoordinateMinimum = -32768.0;
constexpr double kPackedCoordinateMaximum = 32640.0;
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::digits == 24);
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);
struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}

template<class T, class F> Result<T> Guard(F&& operation) {
    try { return Result<T>::Success(operation()); }
    catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Result<T>::Failure(kSubsystem, "TERRAIN_NATIVE_BUDGET", "通行网格分配失败，未返回部分结果。");
    }
    catch (const std::length_error&) {
        return Result<T>::Failure(kSubsystem, "TERRAIN_NATIVE_BUDGET", "通行网格超过容器容量，未返回部分结果。");
    }
}

std::string Coordinate(std::size_t x, std::size_t y) {
    return "(" + std::to_string(x) + "," + std::to_string(y) + ")";
}

void CheckWaterRules(const TerrainNativeWaterRules& rules) {
    if (!std::isfinite(rules.height) || std::abs(rules.height) > 65536.0f
        || !std::isfinite(rules.deepLevelPathing) || rules.deepLevelPathing < 0
        || rules.deepLevelPathing > 65536.0f)
        Fail("TERRAIN_NATIVE_WATER_RULE", "水高度和深水阈值必须是完整、有限且在支持范围内的实际规则。");
}

void CheckVertex(const TerrainVertex& vertex, bool dryRamp = false,
    std::optional<std::pair<std::size_t, std::size_t>> location = {}) {
    const char* code = nullptr;
    const char* detail = nullptr;
    if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw != 0) {
        code = "TERRAIN_NATIVE_VERSION"; detail = "不是经典 W3E v11 字段布局。";
    } else if (vertex.IsRamp() && !dryRamp) {
        code = "TERRAIN_NATIVE_RAMP_UNSUPPORTED"; detail = "水分类尚未支持坡道有效地高。";
    } else if ((vertex.groundHeightRaw & 0xc000) != 0 || vertex.UnknownWaterFlags() != 0) {
        code = "TERRAIN_NATIVE_VERTEX_FLAGS"; detail = "含未闭合的原始高度或水标志，不能猜测其通行意义。";
    }
    if (code) Fail(code, "顶点" + (location ? Coordinate(location->first, location->second) : std::string{}) + detail);
}

TerrainNativeWaterState WaterState(const TerrainVertex& vertex, const TerrainNativeWaterRules& rules,
    double rampOffset = 0) {
    if (!vertex.HasWater()) return {};
    const double base = (static_cast<double>(vertex.groundHeightRaw) - 8192.0) / 4.0
        + 128.0 * (static_cast<int>(vertex.CliffLevel()) - 2);
    // 608880 在坡道调用前丢弃ST0，偏移加回已存F32基底；加法后没有返回F32落点。
    // 经典14bit raw与4bit层级形成的四分之一单位值及32/64/96偏移在此域均精确。
    const double ground = vertex.IsRamp() ? static_cast<double>(static_cast<float>(base)) + rampOffset : base;
    // 保持原版的 float 存储落点；surface 相加之后没有新的 float 落点。
    const float offset = rules.height * 128.0f;
    const double surface = (static_cast<double>(vertex.waterHeightAndFlagsRaw & 0x3fff) - 8192.0) / 4.0
        + static_cast<double>(offset);
    const auto depth = ground > surface ? 0 : ground <= surface - static_cast<double>(rules.deepLevelPathing) ? 2 : 1;
    return {static_cast<std::uint8_t>(depth), surface > ground};
}

bool FiniteCoordinate(double value) {
    return std::isfinite(value) && std::abs(value) <= kMaximumCoordinate;
}

bool ExactPrimarySpan(double left, double right, double span) {
    // TwoDiff 保留 binary64 相减丢失的尾数；仅检查 span 的位数会漏过
    // 如 128 - (-最小binary32次正规数) 这种已经丢失低位的跨度。
    const double leftVirtual = right - span;
    const double rightVirtual = span + leftVirtual;
    const double leftRoundoff = leftVirtual - left;
    const double rightRoundoff = right - rightVirtual;
    if (rightRoundoff + leftRoundoff != 0.0) return false;
    int exponent = 0;
    const double significant = std::ldexp(std::frexp(span, &exponent), 26);
    return std::trunc(significant) == significant;
}

struct NativeBoundsProjection {
    double left = 0;
    double bottom = 0;
    double right = 0;
    double top = 0;
    double width = 0;
    float height = 0;

    static bool FromOrigin(double x, double y, double originX, double originY,
        double edgeX, float edgeY) {
        const float deltaX = static_cast<float>(x - originX);
        const float deltaY = static_cast<float>(y - originY);
        // 原版主轴平方最多52位，分子最多50位，x87均能精确保存。
        // 在本支持域，商若不等于0/1，与边界有超过binary64舍入误差的
        // 距离，故可约去共同边长；这里不把主轴商落为binary32。
        const double horizontal = static_cast<double>(deltaX) / edgeX;
        // 次轴的边长、差值和最终商均有binary32落点。两个24位数的
        // 商可用binary64中间值后落binary32，不会跨越其舍入中点。
        const float vertical = static_cast<float>(static_cast<double>(deltaY) / edgeY);
        return horizontal >= 0.0 && horizontal <= 1.0 && vertical >= 0.0f && vertical <= 1.0f;
    }

    bool Contains(double x, double y) const {
        // 原版先从SW投影，失败后从NE重试；合法端点的正向deltaX
        // 可能舍入到跨度外，不能删掉第二组，也不能分轴任意拼接。
        return FromOrigin(x, y, left, bottom, width, height)
            || FromOrigin(x, y, right, top, -width, -height);
    }
};

NativeBoundsProjection PrepareBounds(const TerrainNativePlayableBounds& bounds) {
    for (const auto value : {bounds.left, bounds.bottom, bounds.right, bounds.top})
        if (static_cast<double>(static_cast<float>(value)) != value)
            Fail("TERRAIN_NATIVE_BOUNDS_PRECISION", "世界可玩范围不是实际 binary32 值，不能静默取整后判断通行。");
    const double width = bounds.right - bounds.left;
    if (!ExactPrimarySpan(bounds.left, bounds.right, width))
        Fail("TERRAIN_NATIVE_BOUNDS_PRECISION", "世界可玩范围的 X 跨度超出已证明的26位精度域，尚不能等价重现原版投影。");
    return {bounds.left, bounds.bottom, bounds.right, bounds.top, width,
        static_cast<float>(bounds.top - bounds.bottom)};
}

void CheckPackedCoordinates(const TerrainHeader& header) {
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double first = header.offset[axis];
        const auto count = axis == 0 ? header.width : header.height;
        const double last = first + static_cast<double>(count - 1) * 128.0;
        if (std::fmod(first, 128.0) != 0.0 || first < kPackedCoordinateMinimum
            || last > kPackedCoordinateMaximum)
            Fail("TERRAIN_NATIVE_COORDINATE_DOMAIN", "地形偏移须128对齐，全部顶点世界XY须位于原版九位坐标范围 [-32768,32640]；未支持的取整或回绕不能猜测。");
    }
}

using GroundProperties = std::array<bool, 3>;
using GroundPalette = std::array<GroundProperties, 16>;

bool ZeroRawcode(std::string_view rawcode) {
    return rawcode.size() == 4 && std::all_of(rawcode.begin(), rawcode.end(), [](char c) { return c == 0; });
}

void CheckRawcode(std::string_view rawcode) {
    if (rawcode.size() != 4 || std::any_of(rawcode.begin(), rawcode.end(), [](unsigned char c) { return c >= 0x80; }))
        Fail("TERRAIN_NATIVE_GROUND_RAWCODE", "地面材质标识不是受支持的四字节正标识或全零哨兵。");
}

GroundPalette ReadPalette(const TerrainGridSnapshot& terrain, std::span<const TerrainNativeGroundRule> rules) {
    if (terrain.header.groundTilesets.size() > 256 || rules.size() > 256)
        Fail("TERRAIN_NATIVE_GROUND_RULE", "地面槽或通行规则数量超过经典地形输入预算。");
    for (std::size_t i = 0; i < rules.size(); ++i) {
        CheckRawcode(rules[i].rawcode);
        for (std::size_t j = 0; j < i; ++j) if (rules[i].rawcode == rules[j].rawcode)
            Fail("TERRAIN_NATIVE_GROUND_AMBIGUOUS", "地面材质有重复通行规则：" + rules[i].rawcode);
    }
    GroundPalette result{};
    std::array<bool, 16> used{};
    for (const auto& vertex : terrain.vertices) used[vertex.GroundTextureIndex()] = true;
    for (std::size_t index = 0; index < result.size() && index < terrain.header.groundTilesets.size(); ++index) {
        if (!used[index]) continue;
        const auto& rawcode = terrain.header.groundTilesets[index];
        CheckRawcode(rawcode);
        if (ZeroRawcode(rawcode)) { result[index] = {true, true, true}; continue; }
        const auto found = std::find_if(rules.begin(), rules.end(), [&](const auto& rule) { return rule.rawcode == rawcode; });
        if (found == rules.end())
            Fail("TERRAIN_NATIVE_GROUND_RULE_MISSING", "已使用地面槽 " + std::to_string(index) + " 缺少完整实际通行规则：" + rawcode);
        result[index] = {found->walkable, found->flyable, found->buildable};
    }
    return result;
}

std::uint8_t EncodeStatic(std::uint8_t attributes) {
    return static_cast<std::uint8_t>(((attributes & 0x07) << 1) | ((attributes & 0x38) << 2));
}

// 5A81A0/5A7DD0/5A8370：只读同一冻结W3E，邻域缺点保持缺失。
// x外层、y内层的索引决定EW=1/7、NS=3/5，不能换成row-major。
class NativeRampClassifier {
public:
    NativeRampClassifier(const TerrainGridSnapshot& terrain, std::uint8_t maximum)
        : terrain_(terrain), maximum_(maximum) {}

    char Classify(int x, int y) const {
        const auto& center = *Vertex(x, y);
        const bool high = High(x, y);
        if (!center.IsRamp()) return high ? 'B' : 'A';
        if (high) return 'H';
        switch (LowKind(x, y)) {
        case 0: return 'L';
        case 1: return 'R';
        case 2: return 'S';
        default: return '!';
        }
    }

private:
    const TerrainVertex* Vertex(int x, int y) const {
        if (x < 0 || y < 0 || x >= static_cast<int>(terrain_.header.width)
            || y >= static_cast<int>(terrain_.header.height)) return nullptr;
        return &terrain_.vertices[static_cast<std::size_t>(y) * terrain_.header.width + x];
    }

    bool High(int x, int y) const {
        int minimum = maximum_, maximum = 0;
        for (int dy = 0; dy <= 1; ++dy) for (int dx = 0; dx <= 1; ++dx) {
            const int qx = x - dx, qy = y - dy;
            if (qx < 0 || qy < 0 || qx + 1 >= static_cast<int>(terrain_.header.width)
                || qy + 1 >= static_cast<int>(terrain_.header.height)) continue;
            for (int cy = 0; cy <= 1; ++cy) for (int cx = 0; cx <= 1; ++cx) {
                const auto& vertex = *Vertex(qx + cx, qy + cy);
                if (!vertex.IsRamp()) continue;
                minimum = std::min(minimum, static_cast<int>(vertex.CliffLevel()));
                maximum = std::max(maximum, static_cast<int>(vertex.CliffLevel()));
            }
        }
        return minimum != maximum && Vertex(x, y)->CliffLevel() == maximum;
    }

    int LowKind(int x, int y) const {
        const int level = Vertex(x, y)->CliffLevel();
        int minimumAll = level, maximumAll = level, minimumRamp = level, maximumRamp = level;
        bool allNineRamp = true;
        std::array<const TerrainVertex*, 9> nine{};
        for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
            const auto* vertex = Vertex(x + dx, y + dy);
            nine[static_cast<std::size_t>((dx + 1) * 3 + dy + 1)] = vertex;
            if (!vertex) { allNineRamp = false; continue; }
            minimumAll = std::min(minimumAll, static_cast<int>(vertex->CliffLevel()));
            maximumAll = std::max(maximumAll, static_cast<int>(vertex->CliffLevel()));
            if (!vertex->IsRamp()) { allNineRamp = false; continue; }
            minimumRamp = std::min(minimumRamp, static_cast<int>(vertex->CliffLevel()));
            maximumRamp = std::max(maximumRamp, static_cast<int>(vertex->CliffLevel()));
        }
        const int rangeAll = maximumAll - minimumAll, rangeRamp = maximumRamp - minimumRamp;
        if ((allNineRamp && rangeAll == 0) || rangeRamp > 1) return -1;
        const auto oppositeDiff = [&](std::size_t a, std::size_t b) {
            return nine[a] && nine[b] && nine[a]->IsRamp() && nine[b]->IsRamp()
                && nine[a]->CliffLevel() != nine[b]->CliffLevel();
        };
        const bool eastWest = oppositeDiff(1, 7), northSouth = oppositeDiff(3, 5);
        if (allNineRamp && !eastWest && !northSouth) return 2;
        if (eastWest && northSouth) return rangeAll <= 1 ? 2 : -1;
        if (eastWest || northSouth) return rangeAll <= 1 ? 1 : -1;
        return 0;
    }

    const TerrainGridSnapshot& terrain_;
    std::uint8_t maximum_;
};

std::vector<char> RampLabels(const TerrainGridSnapshot& terrain, std::uint8_t maximum,
    std::size_t maximumBytes, bool dryOnly = false) {
    const auto& header = terrain.header;
    if (header.version != 11)
        Fail("TERRAIN_NATIVE_VERSION", "坡道分类只支持经典W3E v11。");
    if (header.width < 2 || header.height < 2 || header.width > 2049 || header.height > 2049
        || terrain.vertices.size() != static_cast<std::size_t>(header.width) * header.height)
        Fail("TERRAIN_NATIVE_DIMENSIONS", "坡道分类需要完整的经典地形顶点网格。");
    if (maximum > 14)
        Fail("TERRAIN_NATIVE_RAMP_RULE_MISSING", "实际Terrain.MaxHeight派生值超出坡道0..14支持域。");
    if (terrain.vertices.size() > maximumBytes)
        Fail("TERRAIN_NATIVE_BUDGET", "坡道角分类数组超过本次剩余预算。");
    for (std::size_t i = 0; i < terrain.vertices.size(); ++i) {
        CheckVertex(terrain.vertices[i], true, std::pair{i % header.width, i / header.width});
        if (dryOnly && terrain.vertices[i].HasWater())
            Fail("TERRAIN_NATIVE_WATER_RAMP_UNSUPPORTED", "干坡道分类要求完整快照无水，不能忽略邻域水标志。");
    }
    std::vector<char> labels(terrain.vertices.size());
    const NativeRampClassifier classify(terrain, maximum);
    for (std::uint32_t y = 0; y < header.height; ++y) for (std::uint32_t x = 0; x < header.width; ++x)
        labels[static_cast<std::size_t>(y) * header.width + x] = classify.Classify(static_cast<int>(x), static_cast<int>(y));
    return labels;
}

// 5A8460(a4=0)/5A7FA0：S的种类使用同一快照九点的完整角分类，
// 每个角分类仍读取自己的邻域；不能只用中心原始ramp位或四个地格角。
double SpecialRampHeightOffset(const std::array<char, 9>& nine) {
    int lowCount = 0, highCount = 0;
    std::size_t lastLow = 0, lastHigh = 0;
    for (const auto at : {0u, 2u, 6u, 8u}) {
        if (nine[at] == 'L') { ++lowCount; lastLow = at; }
        if (nine[at] == 'H') { ++highCount; lastHigh = at; }
    }
    const auto axes = [&](std::size_t at, char far) {
        const auto x3 = (at / 3) * 3, y3 = at % 3;
        return nine[x3 + 1] == 'R' && nine[3 + y3] == 'R'
            && nine[7 - x3] == far && nine[5 - y3] == far;
    };
    if (lowCount == 1 && highCount == 3 && axes(lastLow, 'H')) return 96.0;
    if (lowCount == 3 && highCount == 1 && axes(lastHigh, 'L')) return 32.0;
    if (lowCount == 2 && highCount == 2 && nine[8 - lastLow] == 'L'
        && nine[1] == 'R' && nine[7] == 'R' && nine[3] == 'R' && nine[5] == 'R') return 64.0;
    return 0.0;
}

double RampHeightOffset(const TerrainGridSnapshot& terrain, std::span<const char> labels,
    std::size_t x, std::size_t y) {
    const auto columns = static_cast<std::size_t>(terrain.header.width);
    const char center = labels[y * columns + x];
    if (center == 'R') return 64.0;
    if (center != 'S' || x == 0 || y == 0 || x + 1 >= columns || y + 1 >= terrain.header.height)
        return 0.0; // H(a4=0)或5A7FA0缺邻点时为零。
    std::array<char, 9> nine{};
    for (std::size_t dx = 0; dx < 3; ++dx) for (std::size_t dy = 0; dy < 3; ++dy)
        nine[dx * 3 + dy] = labels[(y + dy - 1) * columns + x + dx - 1];
    return SpecialRampHeightOffset(nine);
}

std::uint8_t TerrainMask(const TerrainVertex& nearest, const TerrainNativeWaterState& water,
    const GroundProperties& material, bool paletteValid, bool playable, bool blocked,
    bool allStrictWater, bool impassable) {
    std::uint8_t mask = nearest.IsBlight() ? 0x20 : 0;
    if (!playable) return static_cast<std::uint8_t>(mask | 0xce);
    const bool waterBlocks = impassable && water.depth != 0;
    if (waterBlocks || water.depth == 2 || blocked || !paletteValid || !material[0]) mask |= 0x02;
    if (!paletteValid || !material[1]) mask |= 0x04;
    if (water.depth != 0 || blocked || !paletteValid || !material[2]) mask |= 0x08;
    if (waterBlocks || (water.depth != 2 && !allStrictWater) || !paletteValid) mask |= 0x40;
    if (waterBlocks || (water.depth != 2 && blocked && !allStrictWater) || !paletteValid) mask |= 0x80;
    return mask;
}
} // namespace

Result<TerrainNativeWaterState> ClassifyNativeTerrainWater(
    const TerrainVertex& vertex, const TerrainNativeWaterRules& rules) {
    return Guard<TerrainNativeWaterState>([&] {
        CheckWaterRules(rules);
        CheckVertex(vertex);
        return WaterState(vertex, rules);
    });
}

Result<std::vector<char>> ClassifyNativeDryRampCorners(
    const TerrainGridSnapshot& terrain, std::uint8_t maximumCliffLevel, std::size_t maximumBytes) {
    return Guard<std::vector<char>>([&] { return RampLabels(terrain, maximumCliffLevel, maximumBytes, true); });
}

Result<TerrainNativeRampState> QueryNativeTerrainRamp(const TerrainGridSnapshot& terrain,
    std::uint8_t maximumCliffLevel, std::uint32_t x, std::uint32_t y) {
    return Guard<TerrainNativeRampState>([&] {
        const auto& h = terrain.header;
        if (h.version != 11 || h.width < 2 || h.height < 2 || h.width > 2049 || h.height > 2049
            || terrain.vertices.size() != std::size_t(h.width) * h.height || x >= h.width || y >= h.height)
            Fail("TERRAIN_NATIVE_DIMENSIONS", "坡道点查询需要完整经典网格内的有效坐标。");
        if (maximumCliffLevel > 14)
            Fail("TERRAIN_NATIVE_RAMP_RULE_MISSING", "坡道查询需要实际0到14最大层级规则。");
        for (int dx = -2; dx <= 2; ++dx) for (int dy = -2; dy <= 2; ++dy) {
            const int nx = int(x) + dx, ny = int(y) + dy;
            if (nx < 0 || ny < 0 || nx >= int(h.width) || ny >= int(h.height)) continue;
            CheckVertex(terrain.vertices[std::size_t(ny) * h.width + nx], true,
                std::pair{std::size_t(nx), std::size_t(ny)});
        }
        const NativeRampClassifier classifier(terrain, maximumCliffLevel);
        std::array<char, 9> nine{};
        for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
            const int nx = int(x) + dx, ny = int(y) + dy;
            nine[std::size_t((dx + 1) * 3 + dy + 1)] = nx < 0 || ny < 0 || nx >= int(h.width) || ny >= int(h.height)
                ? '!' : classifier.Classify(nx, ny);
        }
        const auto corner = nine[4];
        const float offset = corner == 'R' ? 64.0f
            : corner == 'S' && x && y && x + 1 < h.width && y + 1 < h.height
                ? static_cast<float>(SpecialRampHeightOffset(nine)) : 0.0f;
        bool belongs = false, unblocked = false;
        for (int dx = 0; dx <= 1; ++dx) for (int dy = 0; dy <= 1; ++dy) {
            const int cellX = int(x) - 1 + dx, cellY = int(y) - 1 + dy;
            if (cellX < 0 || cellY < 0 || cellX + 1 >= int(h.width) || cellY + 1 >= int(h.height)) continue;
            unsigned weight = 0;
            for (int cx = 0; cx <= 1; ++cx) for (int cy = 0; cy <= 1; ++cy) {
                const auto label = nine[std::size_t((dx + cx) * 3 + dy + cy)];
                weight += label == 'R' ? 1u : label == 'S' ? 2u : 0u;
            }
            belongs |= weight != 0; unblocked |= weight > 1;
        }
        return TerrainNativeRampState{corner, offset, belongs, unblocked};
    });
}

bool IsNativeTerrainCliffSentinelTransition(
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after, std::size_t index) noexcept {
    const auto& header = before.header;
    if (header.version != 11 || after.header.version != 11
        || header.width < 3 || header.height < 3 || header.width > 2049 || header.height > 2049
        || header.width != after.header.width || header.height != after.header.height
        || header.mainTileset != after.header.mainTileset || header.offset != after.header.offset
        || !TerrainPaletteEditDetail::SupportsCliffTableDelta(header.cliffTilesets, after.header.cliffTilesets)
        || before.vertices.size() != std::size_t(header.width) * header.height
        || before.vertices.size() != after.vertices.size() || index >= before.vertices.size()) return false;
    const auto oldSlot = before.vertices[index].CliffTextureIndex();
    const auto newSlot = after.vertices[index].CliffTextureIndex();
    if ((oldSlot == 15) == (newSlot == 15)) return false;
    const auto concreteSlot = oldSlot == 15 ? newSlot : oldSlot;
    const auto& concrete = oldSlot == 15 ? after : before;
    if (concreteSlot >= 15 || concreteSlot >= concrete.header.cliffTilesets.size()) return false;
    const auto x = index % header.width, y = index / header.width;
    if (x == 0 || y == 0 || x + 1 >= header.width || y + 1 >= header.height) return false;
    const auto& sentinel = oldSlot == 15 ? before : after;
    const auto level = sentinel.vertices[index].CliffLevel();
    for (auto ny = y - 1; ny <= y + 1; ++ny) for (auto nx = x - 1; nx <= x + 1; ++nx) {
        const auto at = ny * header.width + nx;
        // 四个相邻地格全部平等价于其九个角点层级相同；只看轴向四邻会漏掉对角悬崖。
        if (sentinel.vertices[at].CliffLevel() != level) return false;
        for (const auto* grid : {&before, &after}) {
            const auto& vertex = grid->vertices[at];
            if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw != 0
                || (vertex.groundHeightRaw & 0xc000) != 0 || vertex.UnknownWaterFlags() != 0
                || vertex.IsRamp() || vertex.IsBoundary() || vertex.IsMapEdge()
                || vertex.CliffLevel() > 14 || vertex.CliffVariation() > 2
                || (vertex.CliffTextureIndex() != 15 && vertex.CliffTextureIndex() >= grid->header.cliffTilesets.size()))
                return false;
        }
    }
    return true;
}

Result<std::vector<std::byte>> GenerateTerrainNativePathing(
    const TerrainNativePathingInput& input, std::size_t maximumBytes) {
    return Guard<std::vector<std::byte>>([&] {
        if (!input.terrain) Fail("TERRAIN_NATIVE_INPUT", "缺少通行生成所需的完整地形网格。");
        const auto& terrain = *input.terrain;
        const auto& header = terrain.header;
        if (header.version != 11) Fail("TERRAIN_NATIVE_VERSION", "原版通行生成只支持经典 W3E v11。");
        if (header.width < 2 || header.height < 2 || header.width > 2049 || header.height > 2049)
            Fail("TERRAIN_NATIVE_DIMENSIONS", "地形尺寸不在经典通行网格支持范围内。");
        const auto columns = static_cast<std::size_t>(header.width);
        const auto rows = static_cast<std::size_t>(header.height);
        if (terrain.vertices.size() != columns * rows)
            Fail("TERRAIN_NATIVE_DIMENSIONS", "地形顶点数量与声明宽高不一致。");
        const auto width = (columns - 1) * 4;
        const auto height = (rows - 1) * 4;
        if (height > kMaximumCells / width || height > maximumBytes / width)
            Fail("TERRAIN_NATIVE_BUDGET", "完整通行网格超过本次保存剩余预算。");
        const auto cells = width * height;
        const auto& bounds = input.playableBounds;
        if (!FiniteCoordinate(bounds.left) || !FiniteCoordinate(bounds.bottom)
            || !FiniteCoordinate(bounds.right) || !FiniteCoordinate(bounds.top)
            || bounds.left >= bounds.right || bounds.bottom >= bounds.top
            || !FiniteCoordinate(header.offset[0]) || !FiniteCoordinate(header.offset[1])
            || !FiniteCoordinate(static_cast<double>(header.offset[0]) + (columns - 1) * 128.0)
            || !FiniteCoordinate(static_cast<double>(header.offset[1]) + (rows - 1) * 128.0))
            Fail("TERRAIN_NATIVE_BOUNDS", "通行生成需要有限、未取整且有效的世界可玩范围与地形偏移。");
        CheckPackedCoordinates(header);
        const auto projection = PrepareBounds(bounds);
        if ((input.specialDoodadCount != 0 && input.specialAttributes.size() != cells)
            || (input.specialDoodadCount == 0 && !input.specialAttributes.empty()))
            Fail("TERRAIN_NATIVE_SPECIAL_UNSUPPORTED", "地图含 " + std::to_string(input.specialDoodadCount)
                + " 条特殊地形摆放，但未提供与完整记录对应的实际通行层。");
        for (const auto value : input.specialAttributes)
            if ((value & 0x30) || (value != 0 && !(value & 1)))
                Fail("TERRAIN_NATIVE_SPECIAL_FLAGS", "特殊地形通行层含未知位或缺少覆盖标记。");
        if (!input.staticAttributes.empty() && input.staticAttributes.size() != cells)
            Fail("TERRAIN_NATIVE_STATIC_DIMENSIONS", "静态对象层必须为空或完整覆盖全部通行格。");
        for (std::size_t i = 0; i < input.staticAttributes.size(); ++i) if (input.staticAttributes[i] & 0x80)
            Fail("TERRAIN_NATIVE_STATIC_FLAGS", "静态对象通行格" + Coordinate(i % width, i / width) + "含未支持的内部属性位。");
        if (input.water) CheckWaterRules(*input.water);
        bool wet = false, ramp = false;
        for (std::size_t i = 0; i < terrain.vertices.size(); ++i) {
            CheckVertex(terrain.vertices[i], true, std::pair{i % columns, i / columns});
            wet = wet || terrain.vertices[i].HasWater();
            ramp = ramp || terrain.vertices[i].IsRamp();
        }
        if (ramp && (!input.maximumCliffLevel || *input.maximumCliffLevel > 14))
            Fail("TERRAIN_NATIVE_RAMP_RULE_MISSING", "坡道需要实际Terrain.MaxHeight派生的0..14层级上限。");
        if (wet && !input.water)
            Fail("TERRAIN_NATIVE_WATER_RULE_MISSING", "地形含水标志，但缺少实际水高度、深度阈值和不可通行属性。");
        const auto palette = ReadPalette(terrain, input.groundRules);
        std::vector<char> labels;
        if (ramp) {
            if (terrain.vertices.size() > maximumBytes - cells)
                Fail("TERRAIN_NATIVE_BUDGET", "完整通行网格及坡道角分类超过本次保存剩余预算。");
            labels = RampLabels(terrain, *input.maximumCliffLevel, maximumBytes - cells);
        }
        std::vector<std::byte> result(cells);
        for (std::size_t y = 0; y + 1 < rows; ++y) for (std::size_t x = 0; x + 1 < columns; ++x) {
            const auto sw = y * columns + x;
            const std::array<const TerrainVertex*, 4> corners{
                &terrain.vertices[sw], &terrain.vertices[sw + 1],
                &terrain.vertices[sw + columns], &terrain.vertices[sw + columns + 1]};
            bool blocked = std::any_of(corners.begin() + 1, corners.end(),
                [&](const auto* corner) { return corner->CliffLevel() != corners.front()->CliffLevel(); });
            if (ramp) {
                // 5A8570四角按NW/NE/SE/SW，R权1、S权2；无R/S才回退层级差。
                unsigned weight = 0;
                for (const auto at : {sw + columns, sw + columns + 1, sw + 1, sw})
                    weight += labels[at] == 'R' ? 1u : labels[at] == 'S' ? 2u : 0u;
                if (weight != 0) blocked = weight <= 1;
            }
            std::array<TerrainNativeWaterState, 4> water{};
            if (input.water) for (std::size_t c = 0; c < corners.size(); ++c) {
                // 无水早返不取坡道偏移；有水的N与strict四角均消费各自完整快照。
                const auto offset = ramp && corners[c]->HasWater() && corners[c]->IsRamp()
                    ? RampHeightOffset(terrain, labels, x + c % 2, y + c / 2) : 0.0;
                water[c] = WaterState(*corners[c], *input.water, offset);
            }
            const bool allStrictWater = std::all_of(water.begin(), water.end(), [](const auto& state) { return state.strictCoverage; });
            const double left = static_cast<double>(header.offset[0]) + x * 128.0;
            const double bottom = static_cast<double>(header.offset[1]) + y * 128.0;
            // 世界范围检查四角；Boundary/MapEdge 则只检查 Q 自己的 SW 记录。
            const bool playable = projection.Contains(left, bottom) && projection.Contains(left + 128.0, bottom)
                && projection.Contains(left, bottom + 128.0) && projection.Contains(left + 128.0, bottom + 128.0)
                && !corners.front()->IsBoundary() && !corners.front()->IsMapEdge();
            std::array<std::uint8_t, 4> terrainMasks{};
            for (std::size_t corner = 0; corner < corners.size(); ++corner) {
                const auto& vertex = *corners[corner];
                // 原版最终属性查询只取地面低四位，不消费普通悬崖派生材质。
                const auto index = vertex.GroundTextureIndex();
                const bool paletteValid = index < header.groundTilesets.size();
                terrainMasks[corner] = TerrainMask(vertex, water[corner], palette[index], paletteValid,
                    playable, blocked, allStrictWater, input.water && input.water->impassable);
            }
            for (std::size_t localY = 0; localY < 4; ++localY) for (std::size_t localX = 0; localX < 4; ++localX) {
                // (p+2)/4 与 (q+2)/4；不能用所属地格 SW 代替最近顶点。
                const auto corner = (localY >= 2 ? 2 : 0) + (localX >= 2 ? 1 : 0);
                const auto output = (y * 4 + localY) * width + x * 4 + localX;
                const auto objects = input.staticAttributes.empty() ? std::uint8_t{0} : input.staticAttributes[output];
                auto mask = terrainMasks[corner];
                const auto special = input.specialAttributes.empty() ? std::uint8_t{0} : input.specialAttributes[output];
                if ((special & 1) && playable) {
                    // 5E8540先查边界和水，再分派special；special不读取地面表或普通悬崖。
                    mask = static_cast<std::uint8_t>((special & 0xce) | (corners[corner]->IsBlight() ? 0x20 : 0));
                    if (input.water && input.water->impassable && water[corner].depth != 0) mask |= 0xca;
                    else {
                        if (water[corner].depth == 2) mask |= 0x02;
                        if (water[corner].depth != 0) mask |= 0x08;
                    }
                }
                result[output] = static_cast<std::byte>(EncodeStatic(objects) | ((objects & 0x40) ? 0 : mask));
            }
        }
        return result;
    });
}

} // namespace War3Map::MapData
