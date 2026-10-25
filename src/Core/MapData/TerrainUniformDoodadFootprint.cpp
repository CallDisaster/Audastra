#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
#include "War3MapCore/Image/BlpImage.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <new>
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
Result<TerrainDoodadFootprintImage> DecodeFootprintTga(std::span<const std::byte> bytes,
    bool requireSquare, bool allowRle, std::size_t maximumDecodedBytes) {
    using Return = Result<TerrainDoodadFootprintImage>;
    const auto fail = [](const char* code, const char* message) { return Return::Failure(kSubsystem, code, message); };
    if (bytes.size() < 18) return fail("TERRAIN_UNIFORM_TGA_TRUNCATED", "通行贴图的TGA文件头不完整。");
    const auto byte = [&bytes](std::size_t at) { return std::to_integer<unsigned>(bytes[at]); };
    const auto word = [&byte](std::size_t at) { return byte(at) | (byte(at + 1) << 8); };
    const auto depth = byte(16), descriptor = byte(17), imageType = byte(2);
    const bool rle = imageType == 10 && allowRle;
    if (byte(0) != 0 || byte(1) != 0 || (imageType != 2 && !rle) || word(3) != 0 || word(5) != 0
        || byte(7) != 0 || word(8) != 0 || word(10) != 0 || (depth != 24 && depth != 32)
        || (descriptor & 0xd0u) != 0 || (descriptor & 15u) != (depth == 32 ? 8u : 0u))
        return fail("TERRAIN_UNIFORM_TGA_FORMAT",
            "通行贴图须为无ID和色表、原点坐标为零、左原点无交错的24位RGB或32位RGBA TGA；仅普通预算入口允许type10 RLE。");
    const auto width = word(12), height = word(14);
    if (width == 0 || height == 0 || width > kMaximumSide || height > kMaximumSide)
        return fail("TERRAIN_UNIFORM_TGA_DIMENSIONS", "通行贴图的宽高必须分别为1至256像素。");
    if (requireSquare && width != height)
        return fail("TERRAIN_UNIFORM_TGA_DIMENSIONS", "同色占地接口需要正方形贴图。");
    const std::size_t pixelBytes = depth / 8;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    const std::size_t decodedBytes = count * pixelBytes;
    std::size_t pixelEnd = 18;
    if (rle) {
        // 原版按整图剩余像素处理包，包可以跨行。先完整前扫，不在坏包或预算
        // 不足时分配候选缓冲；不把footer误当可忽略的剩余压缩载荷。
        std::size_t remaining = count;
        while (remaining != 0) {
            if (pixelEnd == bytes.size())
                return fail("TERRAIN_UNIFORM_TGA_TRUNCATED", "通行贴图的RLE包头不完整。");
            const auto packet = byte(pixelEnd++);
            const std::size_t pixels = (packet & 0x7fu) + 1;
            if (pixels > remaining)
                return fail("TERRAIN_DOODAD_TGA_RLE_OVERRUN", "通行贴图的RLE包超过整图剩余像素。");
            const auto payload = (packet & 0x80u) != 0 ? pixelBytes : pixels * pixelBytes;
            if (payload > bytes.size() - pixelEnd)
                return fail("TERRAIN_UNIFORM_TGA_TRUNCATED", "通行贴图的RLE包像素载荷不完整。");
            pixelEnd += payload;
            remaining -= pixels;
        }
    } else {
        pixelEnd += decodedBytes;
        if (bytes.size() < pixelEnd)
            return fail("TERRAIN_UNIFORM_TGA_TRUNCATED", "通行贴图的TGA像素载荷不完整。");
    }
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
    TerrainDoodadFootprintImage image;
    auto pixels = bytes.subspan(18, rle ? 0 : decodedBytes);
    if (rle) {
        if (decodedBytes > maximumDecodedBytes)
            return fail("TERRAIN_DOODAD_TGA_DECODE_BUDGET", "通行贴图RLE解包像素超过同次捕获剩余预算。");
        try { image.decodedPixels = std::make_unique<std::byte[]>(decodedBytes); }
        catch (const std::bad_alloc&) {
            return fail("TERRAIN_DOODAD_TGA_DECODE_BUDGET", "通行贴图RLE解包缓冲分配失败。");
        }
        image.decodedBytes = decodedBytes;
        std::size_t source = 18, target = 0;
        while (target < decodedBytes) {
            const auto packet = byte(source++);
            const std::size_t packetPixels = (packet & 0x7fu) + 1;
            if ((packet & 0x80u) != 0) {
                for (std::size_t index = 0; index < packetPixels; ++index) {
                    std::copy_n(bytes.data() + source, pixelBytes, image.decodedPixels.get() + target);
                    target += pixelBytes;
                }
                source += pixelBytes;
            } else {
                const auto payload = packetPixels * pixelBytes;
                std::copy_n(bytes.data() + source, payload, image.decodedPixels.get() + target);
                source += payload; target += payload;
            }
        }
        pixels = {image.decodedPixels.get(), decodedBytes};
    }
    const auto pixelByte = [&pixels](std::size_t at) { return std::to_integer<unsigned>(pixels[at]); };
    const std::array<unsigned, 4> first{pixelByte(0), pixelByte(1), pixelByte(2), depth == 32 ? pixelByte(3) : 255u};
    bool uniform = true;
    for (std::size_t index = 1, at = pixelBytes; index < count; ++index, at += pixelBytes) {
        if (pixelByte(at) != first[0] || pixelByte(at + 1) != first[1] || pixelByte(at + 2) != first[2]
            || (depth == 32 && pixelByte(at + 3) != first[3])) { uniform = false; break; }
    }
    image.footprint = {width, height, pixels,
        static_cast<std::uint8_t>(pixelBytes), (descriptor & 0x20u) != 0, uniform,
        PixelAttributes(pixels.first(pixelBytes))};
    return Return::Success(std::move(image));
}
} // namespace

Result<TerrainDoodadFootprint> DecodeDoodadFootprintTga(std::span<const std::byte> bytes) {
    auto decoded = DecodeFootprintTga(bytes, false, false, 0);
    if (!decoded.Succeeded() || !decoded.value) return {{}, std::move(decoded.diagnostics)};
    return Result<TerrainDoodadFootprint>::Success(decoded.value->footprint);
}

Result<TerrainDoodadFootprintImage> DecodeDoodadFootprintTgaBudgeted(
    std::span<const std::byte> bytes, std::size_t maximumDecodedBytes) {
    return DecodeFootprintTga(bytes, false, true, maximumDecodedBytes);
}

Result<TerrainDoodadFootprintImage> DecodeDoodadFootprintBlpBudgeted(
    std::span<const std::byte> bytes, std::size_t maximumWorkingBytes) {
    using Return = Result<TerrainDoodadFootprintImage>;
    Image::BlpDecodeLimits limits;
    limits.maxDimension = kMaximumSide;
    // 公共解码器额度包含原输入；owner已单独收取输入，这里仅增加剩余工作额。
    const auto hardWorking = limits.maxWorkingBytes;
    if (bytes.size() > hardWorking)
        return Return::Failure(kSubsystem, "TERRAIN_DOODAD_BLP_BUDGET", "BLP 占地输入超过公共解码器上限。");
    limits.maxWorkingBytes = bytes.size() + std::min(maximumWorkingBytes, hardWorking - bytes.size());
    auto decoded = Image::DecodeBlpBudgeted(bytes, limits);
    if (!decoded.Succeeded() || !decoded.value) return {{}, std::move(decoded.diagnostics)};
    auto& rgba = decoded.value->image.rgba;
    // 原版loader以BGRA保留所有通道，包括alpha的同色判断；原地交换不建第二份图。
    for (std::size_t at = 0; at < rgba.size(); at += 4) std::swap(rgba[at], rgba[at + 2]);
    bool uniform = true;
    for (std::size_t at = 4; at < rgba.size(); at += 4)
        if (!std::equal(rgba.begin(), rgba.begin() + 4, rgba.begin() + at)) { uniform = false; break; }
    TerrainDoodadFootprintImage result;
    result.decodedBytes = decoded.value->workingBytes - bytes.size();
    result.decodedBlpPixels = std::move(rgba);
    result.footprint = {decoded.value->image.width, decoded.value->image.height,
        result.decodedBlpPixels, 4, true, uniform, PixelAttributes(std::span(result.decodedBlpPixels).first(4))};
    return Return::Success(std::move(result));
}

Result<TerrainUniformDoodadFootprint> DecodeUniformDoodadFootprintTga(std::span<const std::byte> bytes) {
    using Return = Result<TerrainUniformDoodadFootprint>;
    auto decoded = DecodeFootprintTga(bytes, true, false, 0);
    if (!decoded.Succeeded() || !decoded.value) return {{}, std::move(decoded.diagnostics)};
    if (!decoded.value->footprint.uniform)
        return Return::Failure(kSubsystem, "TERRAIN_UNIFORM_TGA_NONUNIFORM", "同色占地接口需要完整BGRA一致的像素。");
    return Return::Success({decoded.value->footprint.width, decoded.value->footprint.uniformAttributes});
}

namespace {
Status ValidatePlacementGeometry(const TerrainHeader& terrain,
    const DoodadPlacement& placement, std::uint8_t snapCells) {
    const auto fail = [](const char* code, const char* message) { return Status::Failure(kSubsystem, code, message); };
    if (terrain.version != 11 || terrain.width < 2 || terrain.height < 2
        || terrain.width > 2049 || terrain.height > 2049)
        return fail("TERRAIN_UNIFORM_GRID_SHAPE", "同色占地需要完整经典v11地形和有效顶点尺寸。");
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

Status ValidatePlacement(std::span<std::uint8_t> staticAttributes, const TerrainHeader& terrain,
    const DoodadPlacement& placement, std::uint8_t snapCells) {
    const auto valid = ValidatePlacementGeometry(terrain, placement, snapCells);
    if (!valid.Succeeded()) return valid;
    if (staticAttributes.size() != std::size_t{terrain.width - 1} * 4 * (terrain.height - 1) * 4)
        return Status::Failure(kSubsystem, "TERRAIN_UNIFORM_GRID_SHAPE", "静态对象通行层长度与完整地形通行网格不符。");
    return Status::Ok();
}

struct FootprintProjection {
    std::int64_t x0 = 0, y0 = 0;
    std::uint32_t width = 0, height = 0;
    unsigned drawTurn = 0;
};

Result<FootprintProjection> ProjectFootprint(const TerrainHeader& terrain, const DoodadPlacement& placement,
    const TerrainDoodadFootprint& footprint, std::uint8_t snapCells, std::optional<float> fixedRotationDegrees) {
    using Output = Result<FootprintProjection>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure(kSubsystem, code, message); };
    if (footprint.width == 0 || footprint.height == 0 || footprint.width > kMaximumSide || footprint.height > kMaximumSide
        || (footprint.pixelBytes != 3 && footprint.pixelBytes != 4)
        || footprint.pixels.size() != std::size_t{footprint.width} * footprint.height * footprint.pixelBytes)
        return fail("TERRAIN_DOODAD_FOOTPRINT", "占地像素视图与尺寸或格式不符。");
    const auto valid = ValidatePlacementGeometry(terrain, placement, snapCells);
    if (!valid.Succeeded()) return {{}, valid.diagnostics};
    FootprintProjection output;
    std::uint32_t snapWidth = footprint.width, snapHeight = footprint.height;
    output.width = footprint.width; output.height = footprint.height;
    if (!(footprint.uniform && footprint.width == footprint.height)) {
        if (!fixedRotationDegrees || !std::isfinite(*fixedRotationDegrees)
            || *fixedRotationDegrees < -360 || *fixedRotationDegrees > 360)
            return fail("TERRAIN_DOODAD_FIXED_ROTATION", "图案或矩形占地需要实际、有限且处于[-360,360]的fixedRot字段。");
        const auto snapTurn = QuarterTurn(placement.rotation);
        if (!snapTurn) return fail("TERRAIN_DOODAD_ANGLE_AMBIGUOUS", "占地旋转靠近原版四方向取整边界，无法唯一确定。");
        auto drawTurn = snapTurn;
        if (*fixedRotationDegrees >= 0) {
            const double angle = static_cast<double>(*fixedRotationDegrees) * kRadiansPerDegree;
            const auto lower = QuarterTurn(static_cast<float>(OutwardFloat(angle, false)));
            const auto upper = QuarterTurn(static_cast<float>(OutwardFloat(angle, true)));
            if (!lower || lower != upper)
                return fail("TERRAIN_DOODAD_ANGLE_AMBIGUOUS", "fixedRot旋转靠近原版四方向取整边界，无法唯一确定。");
            drawTurn = lower;
        }
        snapWidth = *snapTurn % 2 ? footprint.width : footprint.height;
        snapHeight = *snapTurn % 2 ? footprint.height : footprint.width;
        output.width = *drawTurn % 2 ? footprint.width : footprint.height;
        output.height = *drawTurn % 2 ? footprint.height : footprint.width;
        output.drawTurn = *drawTurn;
    }
    const auto x0 = ProjectAxis(placement.position[0], terrain.offset[0], snapWidth, snapCells, false, output.width);
    const auto y0 = ProjectAxis(placement.position[1], terrain.offset[1], snapHeight, snapCells, true, output.height);
    if (!x0 || !y0) return fail("TERRAIN_UNIFORM_SNAP_AMBIGUOUS", "占地坐标靠近原版吸附边界，无法唯一确定。");
    output.x0 = *x0; output.y0 = *y0;
    return Output::Success(output);
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
        return fail("TERRAIN_UNIFORM_GRID_VARIANT", "该实例的2格与4格完整地图内占地贡献不同。");
    AddUniformRectangle(staticAttributes, terrain, *two, footprint.attributes);
    return Status::Ok();
}

Result<std::array<std::int64_t, 4>> QueryDoodadFootprintRectangle(const TerrainHeader& terrain,
    const DoodadPlacement& placement, const TerrainDoodadFootprint& footprint, std::uint8_t snapCells,
    std::optional<float> fixedRotationDegrees) {
    auto projected = ProjectFootprint(terrain, placement, footprint, snapCells, fixedRotationDegrees);
    if (!projected.Succeeded() || !projected.value) return {{}, std::move(projected.diagnostics)};
    const auto& p = *projected.value;
    return Result<std::array<std::int64_t, 4>>::Success({p.x0, p.y0, p.x0 + p.width, p.y0 + p.height});
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
    const auto projected = ProjectFootprint(terrain, placement, footprint, snapCells, fixedRotationDegrees);
    if (!projected.Succeeded() || !projected.value) return {projected.diagnostics};
    const auto& p = *projected.value;
    const auto width = p.width, height = p.height, drawTurn = p.drawTurn;
    const auto columns = static_cast<std::size_t>(terrain.width - 1) * 4;
    const auto rows = static_cast<std::size_t>(terrain.height - 1) * 4;
    const auto lastX = std::min<std::int64_t>(columns, p.x0 + width);
    const auto lastY = std::min<std::int64_t>(rows, p.y0 + height);
    // 四档索引访问原图；裁剪只改变目标范围，不重新排列贴图原点。
    for (auto y = std::max<std::int64_t>(0, p.y0); y < lastY; ++y) {
        for (auto x = std::max<std::int64_t>(0, p.x0); x < lastX; ++x) {
            const auto dx = static_cast<std::uint32_t>(x - p.x0), dy = static_cast<std::uint32_t>(y - p.y0);
            std::uint32_t source = 0;
            switch (drawTurn) {
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
