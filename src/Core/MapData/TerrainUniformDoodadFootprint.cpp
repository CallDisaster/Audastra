#include "War3MapCore/MapData/TerrainDoodadFootprint.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <optional>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.UniformDoodadFootprint";
constexpr std::uint32_t kMaximumSide = 256;
constexpr double kMaximumPosition = 16777216.0;
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::digits == 24);
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);

bool SupportedFloatEnvironment() {
    // 包络依赖IEEE渐进下溢，不能让渲染线程遗留的FTZ/DAZ把负小量当作零。
    // 只检查调用线程，不替调用者修改舍入状态。
    if (std::fegetround() != FE_TONEAREST) return false;
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    if ((_mm_getcsr() & 0xe040u) != 0) return false; // 同时核对SSE自己的舍入位。
#endif
    return true;
}

bool ValidAttributes(std::uint8_t value) {
    // 红通道总是同时置0x31，不能把外部提供的不完整位组合当作解码结果。
    const auto red = value & 0x31;
    return (value & ~0x37u) == 0 && (red == 0 || red == 0x31);
}

// x87的精度和舍入状态不由地图提供。用相邻binary32值包住一次运算的
// PC24/53/64结果及F32落存；double结果刚好丢掉微小低位时也向外留一格。
double OutwardFloat(double value, bool upper) {
    const float direction = upper ? std::numeric_limits<float>::infinity()
                                  : -std::numeric_limits<float>::infinity();
    return static_cast<double>(std::nextafter(static_cast<float>(value), direction));
}

std::optional<std::int64_t> ProjectAxis(float position, float origin, std::uint32_t side,
    std::uint8_t snapCells, bool secondAxis, std::uint32_t projectedSide) {
    const float grid = static_cast<float>(snapCells) * 32.0f;
    const bool half = (side / snapCells) % 2 != 0;
    const double bias = half ? 0.0 : 0.5;
    const double delta = static_cast<double>(position) - origin;
    double snappedIndex;
    if (std::fmod(position, 32.0f) == 0.0f) {
        // 已支持的格点域仍精确计算；差值、64/128的商及半格都可精确表示。
        snappedIndex = std::floor(delta / grid + bias);
    } else {
        // 原版X差值留在x87、Y差值落F32；1/grid是精确的2的幂倒数。
        // 两轴共用更宽的包络，包含差值落存、加bias和floor实参double落存。
        const double lower = std::floor(OutwardFloat(OutwardFloat(delta, false) / grid + bias, false));
        const double upper = std::floor(OutwardFloat(OutwardFloat(delta, true) / grid + bias, true));
        if (lower != upper) return std::nullopt;
        snappedIndex = lower;
    }
    // 唯一格点确定后，基点、半格和尺寸修正都是有界的16/32整数倍。
    const double snappedBase = snappedIndex * grid + origin;
    // 主轴的基点先落F32，次轴在加半格后才落F32，保留两条原始路径。
    const float primaryBase = static_cast<float>(snappedBase);
    const float snapped = static_cast<float>((secondAxis ? snappedBase : primaryBase)
        + (half ? grid * 0.5 : 0.0));
    float start = static_cast<float>(static_cast<double>(snapped) - projectedSide * 16.0);
    if (projectedSide % 2 == 0) start = static_cast<float>(static_cast<double>(start) + 16.0);
    // 负半格须向零，不能用floor。已验证的幅值使结果远小于int64上限。
    return static_cast<std::int64_t>((static_cast<double>(start) - origin) / 32.0);
}
// 原版初始化使用两个binary32常量相乘再落存；不能用double的pi/180替代。
constexpr float kRadiansPerDegree = std::bit_cast<float>(std::uint32_t{0x3c8efa36});
std::optional<unsigned> QuarterTurn(float radians) {
    double lower = radians, upper = radians;
    const double turnLow = OutwardFloat(static_cast<double>(kRadiansPerDegree) * 360, false);
    const double turnHigh = OutwardFloat(static_cast<double>(kRadiansPerDegree) * 360, true);
    for (unsigned iterations = 0; lower < 0; ++iterations) {
        if (upper >= 0 || iterations >= 59) return std::nullopt;
        lower = OutwardFloat(lower + turnLow, false);
        upper = OutwardFloat(upper + turnHigh, true);
    }
    const double quarterLow = OutwardFloat(static_cast<double>(kRadiansPerDegree) * 90, false);
    const double quarterHigh = OutwardFloat(static_cast<double>(kRadiansPerDegree) * 90, true);
    const double lo = std::trunc(OutwardFloat(OutwardFloat(lower / quarterHigh, false) + 0.5, false));
    const double hi = std::trunc(OutwardFloat(OutwardFloat(upper / quarterLow, true) + 0.5, true));
    if (lo != hi) return std::nullopt;
    return static_cast<unsigned>(lo) % 4;
}

std::uint8_t PixelAttributes(std::span<const std::byte> pixel) {
    return static_cast<std::uint8_t>((pixel[2] != std::byte{} ? 0x31 : 0)
        | (pixel[1] != std::byte{} ? 2 : 0) | (pixel[0] != std::byte{} ? 4 : 0));
}
Result<TerrainDoodadFootprint> DecodeFootprintTga(std::span<const std::byte> bytes, bool requireSquare) {
    using Return = Result<TerrainDoodadFootprint>;
    const auto fail = [](const char* code, const char* message) { return Return::Failure(kSubsystem, code, message); };
    if (bytes.size() < 18) return fail("TERRAIN_UNIFORM_TGA_TRUNCATED", "通行贴图的TGA文件头不完整。");
    const auto byte = [&bytes](std::size_t at) { return std::to_integer<unsigned>(bytes[at]); };
    const auto word = [&byte](std::size_t at) { return byte(at) | (byte(at + 1) << 8); };
    const auto depth = byte(16), descriptor = byte(17);
    if (byte(0) != 0 || byte(1) != 0 || byte(2) != 2 || word(3) != 0 || word(5) != 0
        || byte(7) != 0 || word(8) != 0 || word(10) != 0 || (depth != 24 && depth != 32)
        || (descriptor & 0xd0u) != 0 || (descriptor & 15u) != (depth == 32 ? 8u : 0u))
        return fail("TERRAIN_UNIFORM_TGA_FORMAT",
            "通行贴图仅支持无ID和色表、原点坐标为零、左原点无交错的未压缩24位RGB或32位RGBA TGA。");
    const auto width = word(12), height = word(14);
    if (width == 0 || height == 0 || width > kMaximumSide || height > kMaximumSide)
        return fail("TERRAIN_UNIFORM_TGA_DIMENSIONS", "通行贴图的宽高必须分别为1至256像素。");
    if (requireSquare && width != height)
        return fail("TERRAIN_UNIFORM_TGA_DIMENSIONS", "同色占地接口需要正方形贴图。");
    const std::size_t pixelBytes = depth / 8;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    const std::size_t pixelEnd = 18 + count * pixelBytes;
    if (bytes.size() < pixelEnd)
        return fail("TERRAIN_UNIFORM_TGA_TRUNCATED", "通行贴图的TGA像素载荷不完整。");
    if (bytes.size() != pixelEnd) {
        // 已证实际资源使用两个零offset加TRUEVISION-XFILE.\0；不忽略未知尾随区。
        constexpr std::array<unsigned char, 26> footer{
            0, 0, 0, 0, 0, 0, 0, 0, 'T', 'R', 'U', 'E', 'V', 'I', 'S', 'I', 'O', 'N',
            '-', 'X', 'F', 'I', 'L', 'E', '.', 0};
        if (bytes.size() - pixelEnd != footer.size()
            || !std::equal(footer.begin(), footer.end(), bytes.begin() + pixelEnd,
                [](unsigned char expected, std::byte actual) { return expected == std::to_integer<unsigned>(actual); }))
            return fail("TERRAIN_UNIFORM_TGA_TRAILING", "通行贴图含未支持或不完整的TGA附加区。");
    }
    const std::array<unsigned, 4> first{byte(18), byte(19), byte(20), depth == 32 ? byte(21) : 255u};
    bool uniform = true;
    for (std::size_t index = 1, at = 18 + pixelBytes; index < count; ++index, at += pixelBytes) {
        if (byte(at) != first[0] || byte(at + 1) != first[1] || byte(at + 2) != first[2]
            || (depth == 32 && byte(at + 3) != first[3])) { uniform = false; break; }
    }
    return Return::Success({width, height, bytes.subspan(18, count * pixelBytes),
        static_cast<std::uint8_t>(pixelBytes), (descriptor & 0x20u) != 0, uniform,
        PixelAttributes(bytes.subspan(18, pixelBytes))});
}
} // namespace

Result<TerrainDoodadFootprint> DecodeDoodadFootprintTga(std::span<const std::byte> bytes) {
    return DecodeFootprintTga(bytes, false);
}

Result<TerrainUniformDoodadFootprint> DecodeUniformDoodadFootprintTga(std::span<const std::byte> bytes) {
    using Return = Result<TerrainUniformDoodadFootprint>;
    auto decoded = DecodeFootprintTga(bytes, true);
    if (!decoded.Succeeded() || !decoded.value) return {{}, std::move(decoded.diagnostics)};
    if (!decoded.value->uniform)
        return Return::Failure(kSubsystem, "TERRAIN_UNIFORM_TGA_NONUNIFORM", "同色占地接口需要完整BGRA一致的像素。");
    return Return::Success({decoded.value->width, decoded.value->uniformAttributes});
}

namespace {
Status ValidatePlacement(std::span<std::uint8_t> staticAttributes, const TerrainHeader& terrain,
    const DoodadPlacement& placement, std::uint8_t snapCells) {
    const auto fail = [](const char* code, const char* message) { return Status::Failure(kSubsystem, code, message); };
    if (terrain.version != 11 || terrain.width < 2 || terrain.height < 2
        || terrain.width > 2049 || terrain.height > 2049)
        return fail("TERRAIN_UNIFORM_GRID_SHAPE", "同色占地需要完整经典v11地形和有效顶点尺寸。");
    const auto columns = static_cast<std::size_t>(terrain.width - 1) * 4;
    const auto rows = static_cast<std::size_t>(terrain.height - 1) * 4;
    if (staticAttributes.size() != columns * rows)
        return fail("TERRAIN_UNIFORM_GRID_SHAPE", "静态对象通行层长度与完整地形通行网格不符。");
    if (!SupportedFloatEnvironment())
        return fail("TERRAIN_UNIFORM_FLOAT_ENVIRONMENT", "当前线程的浮点模式不支持占地精确吸附，需要默认舍入且保留渐进下溢。");
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double first = terrain.offset[axis];
        const auto vertices = axis == 0 ? terrain.width : terrain.height;
        const double last = first + static_cast<double>(vertices - 1) * 128.0;
        if (!std::isfinite(first) || std::fmod(first, 128.0) != 0.0 || first < -32768.0 || last > 32640.0)
            return fail("TERRAIN_UNIFORM_COORDINATE_DOMAIN", "地形偏移须128对齐，全部顶点XY须处于原版九位坐标域。");
        const double position = placement.position[axis];
        if (!std::isfinite(position) || std::abs(position) > kMaximumPosition)
            return fail("TERRAIN_UNIFORM_COORDINATE_DOMAIN", "同色占地实例XY须有限，且绝对值不超过2^24。");
    }
    if (!std::isfinite(placement.position[2])
        || std::any_of(placement.scale.begin(), placement.scale.end(), [](float value) { return !std::isfinite(value); })
        || !std::isfinite(placement.rotation) || placement.rotation < -360.0f || placement.rotation > 360.0f)
        return fail("TERRAIN_UNIFORM_PLACEMENT_DOMAIN", "同色占地姿态须有限，旋转须处于已支持的[-360,360]范围。");
    if (snapCells != 2 && snapCells != 4)
        return fail("TERRAIN_UNIFORM_SNAP_CELLS", "同色占地吸附网格只能使用实际类别确定的2格或4格。");
    return Status::Ok();
}

struct UniformRectangle {
    std::int64_t firstX, firstY, lastX, lastY;
    [[nodiscard]] bool Empty() const noexcept { return firstX >= lastX || firstY >= lastY; }
    bool operator==(const UniformRectangle&) const = default;
};
static_assert(2 * sizeof(std::optional<UniformRectangle>) <= TerrainUniformGridInvariantBudgetBytes);

std::optional<UniformRectangle> ProjectUniformRectangle(const TerrainHeader& terrain,
    const DoodadPlacement& placement, std::uint32_t side, std::uint8_t snapCells) {
    const auto x0 = ProjectAxis(placement.position[0], terrain.offset[0], side, snapCells, false, side);
    const auto y0 = ProjectAxis(placement.position[1], terrain.offset[1], side, snapCells, true, side);
    if (!x0 || !y0) return std::nullopt;
    const auto columns = static_cast<std::int64_t>(terrain.width - 1) * 4;
    const auto rows = static_cast<std::int64_t>(terrain.height - 1) * 4;
    return UniformRectangle{std::max<std::int64_t>(0, *x0), std::max<std::int64_t>(0, *y0),
        std::min(columns, *x0 + side), std::min(rows, *y0 + side)};
}

void AddUniformRectangle(std::span<std::uint8_t> attributes, const TerrainHeader& terrain,
    const UniformRectangle& rectangle, std::uint8_t value) {
    if (rectangle.Empty() || value == 0) return;
    const auto columns = static_cast<std::size_t>(terrain.width - 1) * 4;
    for (auto y = rectangle.firstY; y < rectangle.lastY; ++y) {
        const auto row = static_cast<std::size_t>(y) * columns;
        for (auto x = rectangle.firstX; x < rectangle.lastX; ++x)
            attributes[row + static_cast<std::size_t>(x)] |= value;
    }
}
Status AddUniformFootprint(std::span<std::uint8_t> staticAttributes, const TerrainHeader& terrain,
    const DoodadPlacement& placement, const TerrainUniformDoodadFootprint& footprint, std::uint8_t snapCells,
    TerrainNativeFootprintContribution contribution) {
    const auto fail = [](const char* code, const char* message) { return Status::Failure(kSubsystem, code, message); };
    auto valid = ValidatePlacement(staticAttributes, terrain, placement, snapCells);
    if (!valid.Succeeded()) return valid;
    if (footprint.side == 0 || footprint.side > kMaximumSide || !ValidAttributes(footprint.attributes))
        return fail("TERRAIN_UNIFORM_FOOTPRINT", "同色占地尺寸或原版通道属性组合无效。");
    const auto rectangle = ProjectUniformRectangle(terrain, placement, footprint.side, snapCells);
    if (!rectangle)
        return fail("TERRAIN_UNIFORM_SNAP_AMBIGUOUS", "同色占地坐标靠近吸附边界，原版浮点舍入的结果不能唯一确定。");
    // 此后无失败分支。均匀像素使逐像素裁边可化为相同的矩形，不移动原起点。
    const auto attributes = contribution == TerrainNativeFootprintContribution::WalkableTerrainSuppression
        ? static_cast<std::uint8_t>(footprint.attributes ? 0x40 : 0) : footprint.attributes;
    AddUniformRectangle(staticAttributes, terrain, *rectangle, attributes);
    return Status::Ok();
}
} // namespace

Status AddUniformDoodadFootprint(std::span<std::uint8_t> staticAttributes, const TerrainHeader& terrain,
    const DoodadPlacement& placement, const TerrainUniformDoodadFootprint& footprint, std::uint8_t snapCells) {
    return AddUniformFootprint(staticAttributes, terrain, placement, footprint, snapCells,
        TerrainNativeFootprintContribution::StaticRgbAttributes);
}

Status AddGridInvariantUniformDoodadFootprint(std::span<std::uint8_t> staticAttributes, const TerrainHeader& terrain,
    const DoodadPlacement& placement, const TerrainUniformDoodadFootprint& footprint) {
    const auto fail = [](const char* code, const char* message) { return Status::Failure(kSubsystem, code, message); };
    // 姿态验证与原2格入口完全共用；两个合法常量网格各自仍须取得唯一投影。
    const auto valid = ValidatePlacement(staticAttributes, terrain, placement, 2);
    if (!valid.Succeeded()) return valid;
    if (footprint.side == 0 || footprint.side > kMaximumSide || !ValidAttributes(footprint.attributes))
        return fail("TERRAIN_UNIFORM_FOOTPRINT", "同色占地尺寸或原版通道属性组合无效。");
    const auto two = ProjectUniformRectangle(terrain, placement, footprint.side, 2);
    const auto four = ProjectUniformRectangle(terrain, placement, footprint.side, 4);
    if (!two || !four)
        return fail("TERRAIN_UNIFORM_SNAP_AMBIGUOUS", "同色占地的2格或4格投影不能唯一确定，未合并实例贡献。");
    // 相同非零属性的完整贡献由裁剪矩形唯一决定；必须在接触已有OR层之前比较。
    if (footprint.attributes != 0 && !(two->Empty() && four->Empty()) && *two != *four)
        return fail("TERRAIN_UNIFORM_GRID_VARIANT", "该实例的2格与4格完整地图内占地贡献不同，尚不能保存C类占地。");
    AddUniformRectangle(staticAttributes, terrain, *two, footprint.attributes);
    return Status::Ok();
}

Status AddDoodadFootprint(std::span<std::uint8_t> staticAttributes, const TerrainHeader& terrain,
    const DoodadPlacement& placement, const TerrainDoodadFootprint& footprint, std::uint8_t snapCells,
    std::optional<float> fixedRotationDegrees, TerrainNativeFootprintContribution contribution) {
    const auto fail = [](const char* code, const char* message) { return Status::Failure(kSubsystem, code, message); };
    if (contribution != TerrainNativeFootprintContribution::StaticRgbAttributes
        && contribution != TerrainNativeFootprintContribution::WalkableTerrainSuppression)
        return fail("TERRAIN_DOODAD_FOOTPRINT_CONTRIBUTION", "占地贡献类别不属于已解释的静态RGB或可行走地形抑制。");
    if (footprint.width == 0 || footprint.height == 0 || footprint.width > kMaximumSide || footprint.height > kMaximumSide
        || (footprint.pixelBytes != 3 && footprint.pixelBytes != 4)
        || footprint.pixels.size() != std::size_t{footprint.width} * footprint.height * footprint.pixelBytes)
        return fail("TERRAIN_DOODAD_FOOTPRINT", "占地像素视图与尺寸或格式不符。");
    if (footprint.uniform && footprint.width == footprint.height)
        return AddUniformFootprint(staticAttributes, terrain, placement,
            {footprint.width, footprint.uniformAttributes}, snapCells, contribution);
    auto valid = ValidatePlacement(staticAttributes, terrain, placement, snapCells);
    if (!valid.Succeeded()) return valid;
    if (!fixedRotationDegrees || !std::isfinite(*fixedRotationDegrees)
        || *fixedRotationDegrees < -360 || *fixedRotationDegrees > 360)
        return fail("TERRAIN_DOODAD_FIXED_ROTATION", "图案或矩形占地需要实际、有限且处于[-360,360]的fixedRot字段。");
    const auto snapTurn = QuarterTurn(placement.rotation);
    if (!snapTurn)
        return fail("TERRAIN_DOODAD_ANGLE_AMBIGUOUS", "占地旋转靠近原版四方向取整边界，无法唯一确定。");
    auto drawTurn = snapTurn;
    if (*fixedRotationDegrees >= 0) {
        const double angle = static_cast<double>(*fixedRotationDegrees) * kRadiansPerDegree;
        const auto lower = QuarterTurn(static_cast<float>(OutwardFloat(angle, false)));
        const auto upper = QuarterTurn(static_cast<float>(OutwardFloat(angle, true)));
        if (!lower || lower != upper)
            return fail("TERRAIN_DOODAD_ANGLE_AMBIGUOUS", "fixedRot旋转靠近原版四方向取整边界，无法唯一确定。");
        drawTurn = lower;
    }
    const auto snapWidth = *snapTurn % 2 ? footprint.width : footprint.height;
    const auto snapHeight = *snapTurn % 2 ? footprint.height : footprint.width;
    const auto width = *drawTurn % 2 ? footprint.width : footprint.height;
    const auto height = *drawTurn % 2 ? footprint.height : footprint.width;
    const auto x0 = ProjectAxis(placement.position[0], terrain.offset[0], snapWidth, snapCells, false, width);
    const auto y0 = ProjectAxis(placement.position[1], terrain.offset[1], snapHeight, snapCells, true, height);
    if (!x0 || !y0)
        return fail("TERRAIN_UNIFORM_SNAP_AMBIGUOUS", "占地坐标靠近原版吸附边界，无法唯一确定。");
    const auto columns = static_cast<std::size_t>(terrain.width - 1) * 4;
    const auto rows = static_cast<std::size_t>(terrain.height - 1) * 4;
    const auto lastX = std::min<std::int64_t>(columns, *x0 + width);
    const auto lastY = std::min<std::int64_t>(rows, *y0 + height);
    // 四档索引访问原图；裁剪只改变目标范围，不重新排列贴图原点。
    for (auto y = std::max<std::int64_t>(0, *y0); y < lastY; ++y) {
        for (auto x = std::max<std::int64_t>(0, *x0); x < lastX; ++x) {
            const auto dx = static_cast<std::uint32_t>(x - *x0), dy = static_cast<std::uint32_t>(y - *y0);
            std::uint32_t source = 0;
            switch (*drawTurn) {
            case 0: source = dy + dx * height; break;
            case 1: source = width - 1 - dx + dy * width; break;
            case 2: source = width * height - dx * height - dy - 1; break;
            case 3: source = dx + width * (height - 1 - dy); break;
            }
            const auto sx = source % footprint.width, sy = source / footprint.width;
            const auto fileRow = footprint.topOrigin ? sy : footprint.height - 1 - sy;
            const auto at = (std::size_t{fileRow} * footprint.width + sx) * footprint.pixelBytes;
            const auto rgb = PixelAttributes(footprint.pixels.subspan(at, footprint.pixelBytes));
            const auto attributes = contribution == TerrainNativeFootprintContribution::WalkableTerrainSuppression
                ? static_cast<std::uint8_t>(rgb ? 0x40 : 0) : rgb;
            staticAttributes[static_cast<std::size_t>(y) * columns + static_cast<std::size_t>(x)] |= attributes;
        }
    }
    return Status::Ok();
}

} // namespace War3Map::MapData
