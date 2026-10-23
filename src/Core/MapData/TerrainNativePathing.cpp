#include "War3MapCore/MapData/TerrainNativePathing.h"

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

void CheckVertex(const TerrainVertex& vertex,
    std::optional<std::pair<std::size_t, std::size_t>> location = {}) {
    const char* code = nullptr;
    const char* detail = nullptr;
    if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw != 0) {
        code = "TERRAIN_NATIVE_VERSION"; detail = "不是经典 W3E v11 字段布局。";
    } else if (vertex.IsRamp()) {
        code = "TERRAIN_NATIVE_RAMP_UNSUPPORTED"; detail = "含坡道，尚未接入完整原版坡道分类。";
    } else if ((vertex.groundHeightRaw & 0xc000) != 0 || vertex.UnknownWaterFlags() != 0) {
        code = "TERRAIN_NATIVE_VERTEX_FLAGS"; detail = "含未闭合的原始高度或水标志，不能猜测其通行意义。";
    }
    if (code) Fail(code, "顶点" + (location ? Coordinate(location->first, location->second) : std::string{}) + detail);
}

TerrainNativeWaterState WaterState(const TerrainVertex& vertex, const TerrainNativeWaterRules& rules) {
    if (!vertex.HasWater()) return {};
    const double ground = (static_cast<double>(vertex.groundHeightRaw) - 8192.0) / 4.0
        + 128.0 * (static_cast<int>(vertex.CliffLevel()) - 2);
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

bool IsNativeTerrainCliffSentinelTransition(
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after, std::size_t index) noexcept {
    const auto& header = before.header;
    if (header.version != 11 || after.header.version != 11
        || header.width < 3 || header.height < 3 || header.width > 2049 || header.height > 2049
        || header.width != after.header.width || header.height != after.header.height
        || header.mainTileset != after.header.mainTileset || header.offset != after.header.offset
        || header.cliffTilesets != after.header.cliffTilesets
        || before.vertices.size() != std::size_t(header.width) * header.height
        || before.vertices.size() != after.vertices.size() || index >= before.vertices.size()) return false;
    const auto oldSlot = before.vertices[index].CliffTextureIndex();
    const auto newSlot = after.vertices[index].CliffTextureIndex();
    if ((oldSlot == 15) == (newSlot == 15)) return false;
    const auto concreteSlot = oldSlot == 15 ? newSlot : oldSlot;
    if (concreteSlot >= 15 || concreteSlot >= header.cliffTilesets.size()) return false;
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
                || (vertex.CliffTextureIndex() != 15 && vertex.CliffTextureIndex() >= header.cliffTilesets.size()))
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
        bool wet = false;
        for (std::size_t i = 0; i < terrain.vertices.size(); ++i) {
            CheckVertex(terrain.vertices[i], std::pair{i % columns, i / columns});
            wet = wet || terrain.vertices[i].HasWater();
        }
        if (wet && !input.water)
            Fail("TERRAIN_NATIVE_WATER_RULE_MISSING", "地形含水标志，但缺少实际水高度、深度阈值和不可通行属性。");
        const auto palette = ReadPalette(terrain, input.groundRules);
        std::vector<std::byte> result(cells);
        for (std::size_t y = 0; y + 1 < rows; ++y) for (std::size_t x = 0; x + 1 < columns; ++x) {
            const auto sw = y * columns + x;
            const std::array<const TerrainVertex*, 4> corners{
                &terrain.vertices[sw], &terrain.vertices[sw + 1],
                &terrain.vertices[sw + columns], &terrain.vertices[sw + columns + 1]};
            const bool blocked = std::any_of(corners.begin() + 1, corners.end(),
                [&](const auto* corner) { return corner->CliffLevel() != corners.front()->CliffLevel(); });
            std::array<TerrainNativeWaterState, 4> water{};
            if (input.water) for (std::size_t c = 0; c < corners.size(); ++c) water[c] = WaterState(*corners[c], *input.water);
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
