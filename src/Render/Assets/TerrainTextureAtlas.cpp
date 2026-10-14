#include "War3MapCore/Render/TerrainTextureAtlas.h"

#include "War3MapCore/Hash.h"

#include <algorithm>
#include <array>
#include <new>
#include <span>
#include <stdexcept>

namespace War3Map::Render {
namespace {

using Bytes = std::span<const std::byte>;
using Snapshot = std::shared_ptr<const TerrainAtlasSnapshot>;
constexpr std::string_view kSubsystem = "Render.TerrainAtlas";

struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(std::string code, std::string message) {
    throw Failure{{{DiagnosticSeverity::Error, std::string(kSubsystem), std::move(code),
        std::move(message), std::nullopt}}};
}

template <typename T>
T Take(Result<T> result) {
    if (!result.Succeeded()) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}

std::uint8_t U8(Bytes bytes, std::size_t offset) { return std::to_integer<std::uint8_t>(bytes[offset]); }
std::uint32_t U32(Bytes bytes, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index) result |= static_cast<std::uint32_t>(U8(bytes, offset + index)) << (index * 8);
    return result;
}
std::uint32_t Be16(Bytes bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(U8(bytes, offset)) << 8) | U8(bytes, offset + 1);
}
void Put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 255);
}

void ValidateLimits(const TerrainAtlasLimits& limits) {
    const TerrainAtlasLimits hard;
    if (limits.maxInputBytes > hard.maxInputBytes || limits.maxDimension > hard.maxDimension
        || limits.maxRgbaBytes > hard.maxRgbaBytes || limits.maxRetainedRgbaBytes > hard.maxRetainedRgbaBytes
        || limits.maxTotalMipPixels > hard.maxTotalMipPixels
        || limits.maxWorkingBytes > hard.maxWorkingBytes || limits.maxJpegSegments > hard.maxJpegSegments
        || limits.maxJpegScans > hard.maxJpegScans)
        Fail("TERRAIN_ATLAS_LIMITS", "Atlas 限额只能收紧，不能放宽硬上限。");
}

struct Mip {
    std::uint32_t offset = 0, size = 0, width = 0, height = 0;
};
struct Header {
    std::uint32_t compression = 0, alpha = 0, width = 0, height = 0;
    std::size_t prefix = 0;
    std::vector<Mip> mips;
};

Header Inspect(Bytes bytes, const TerrainAtlasLimits& limits) {
    if (bytes.size() > limits.maxInputBytes) Fail("TERRAIN_ATLAS_INPUT_BUDGET", "BLP 输入超出字节预算。");
    if (bytes.size() < 160 || U8(bytes, 0) != 'B' || U8(bytes, 1) != 'L'
        || U8(bytes, 2) != 'P' || U8(bytes, 3) != '1')
        Fail("TERRAIN_ATLAS_FORMAT", "Atlas 需要完整经典 BLP1，不支持 BLP2/DXT。");
    Header header{U32(bytes, 4), U32(bytes, 8), U32(bytes, 12), U32(bytes, 16), 0, {}};
    if (header.width == 0 || header.height == 0 || header.width > limits.maxDimension || header.height > limits.maxDimension)
        Fail("TERRAIN_ATLAS_DIMENSION", "Atlas 尺寸无效或超出预算。");
    if (header.height % 4 != 0 || (header.width != header.height && header.width != header.height * 2))
        Fail("TERRAIN_ATLAS_LAYOUT", "Atlas 不是 4x4 或 8x4 方形单元格布局。");
    if (static_cast<std::size_t>(header.width) * header.height * 4 > limits.maxRgbaBytes)
        Fail("TERRAIN_ATLAS_RGBA_BUDGET", "Atlas RGBA 超出输出预算。");
    // JPEG 的 alpha 在图像第四组件里，头部位数只决定用不用；与贴图解码相同，8 及以上使用，其余视为不透明。
    if (header.compression == 0) header.alpha = header.alpha >= 8 ? 8 : 0;
    if (header.alpha != 0 && header.alpha != 1 && header.alpha != 4 && header.alpha != 8)
        Fail("TERRAIN_ATLAS_ALPHA_UNSUPPORTED", "BLP alpha 位数只能是 0/1/4/8。");
    if (header.compression == 0) {
        if (U32(bytes, 156) > bytes.size() - 160) Fail("TERRAIN_ATLAS_MIP", "JPEG 共享头越界。");
        header.prefix = 160 + U32(bytes, 156);
    } else if (header.compression == 1) {
        header.prefix = 156 + 1024;
        if (bytes.size() < header.prefix) Fail("TERRAIN_ATLAS_MIP", "BLP 调色板被截断。");
    } else Fail("TERRAIN_ATLAS_FORMAT", "不支持的 BLP1 压缩类型。");
    std::size_t totalPixels = 0;
    for (std::size_t level = 0; level < 16; ++level) {
        const Mip mip{U32(bytes, 28 + level * 4), U32(bytes, 92 + level * 4),
            std::max(1u, header.width >> level), std::max(1u, header.height >> level)};
        // mip0 任何问题都拒绝；后续层与游戏一样只用到第一处缺失、越界或 1x1 之后为止，前面的层照常使用。
        if (level != 0 && ((mip.offset == 0 && mip.size == 0)
            || (header.mips.back().width == 1 && header.mips.back().height == 1))) break;
        const auto pixels = static_cast<std::size_t>(mip.width) * mip.height;
        const bool bounded = mip.offset >= header.prefix && mip.size != 0 && mip.offset <= bytes.size()
            && mip.size <= bytes.size() - mip.offset
            && (header.compression != 1 || mip.size >= pixels + (pixels * header.alpha + 7) / 8);
        if (level != 0 && (!bounded || pixels > limits.maxTotalMipPixels - totalPixels
            || totalPixels + pixels > limits.maxRetainedRgbaBytes / 4)) break;
        if (!bounded)
            Fail("TERRAIN_ATLAS_MIP", "mip 0 缺失、越界或被截断。");
        if (pixels > limits.maxTotalMipPixels - totalPixels)
            Fail("TERRAIN_ATLAS_MIP_BUDGET", "全部 mip 像素数超出解码工作预算。");
        totalPixels += pixels;
        if (totalPixels > limits.maxRetainedRgbaBytes / 4)
            Fail("TERRAIN_ATLAS_RETAINED_BUDGET", "原始 mip 链 RGBA 总量超出保留预算。");
        header.mips.push_back(mip);
    }
    return header;
}

// 仅为 Core DecodeBlp 包装当前 mip，不实现第二套像素解码器。
std::vector<std::byte> WrapMip(Bytes input, const Header& header, const Mip& mip) {
    std::vector<std::byte> wrapper(header.prefix + mip.size);
    std::copy_n(input.begin(), header.prefix, wrapper.begin());
    Put32(wrapper, 12, mip.width); Put32(wrapper, 16, mip.height);
    std::fill(wrapper.begin() + 28, wrapper.begin() + 156, std::byte{0});
    Put32(wrapper, 28, static_cast<std::uint32_t>(header.prefix)); Put32(wrapper, 92, mip.size);
    std::copy_n(input.begin() + mip.offset, mip.size, wrapper.begin() + static_cast<std::ptrdiff_t>(header.prefix));
    return wrapper;
}

struct JpegComponent { std::uint8_t id = 0, h = 0, v = 0, quant = 0; };

// 只检查段结构、帧身份和分配预算；Huffman/IDCT 由已有 Core 解码器负责。
std::size_t InspectJpeg(Bytes jpeg, const Mip& mip, std::uint32_t alpha,
    const TerrainAtlasLimits& limits, std::size_t& segments, std::size_t& scans) {
    const auto invalid = [](std::string message) { Fail("TERRAIN_ATLAS_JPEG", std::move(message)); };
    if (jpeg.size() < 4 || U8(jpeg, 0) != 0xff || U8(jpeg, 1) != 0xd8) invalid("JPEG 缺少 SOI。");
    std::vector<JpegComponent> components;
    std::array<bool, 4> quant{}, dc{}, ac{};
    std::array<bool, 256> seenComponent{};
    bool ended = false, progressive = false;
    std::size_t localScans = 0, memory = 0;
    for (std::size_t at = 2; at < jpeg.size();) {
        if (U8(jpeg, at) != 0xff) invalid("JPEG 段边界不是 marker。");
        while (at < jpeg.size() && U8(jpeg, at) == 0xff) ++at;
        if (at >= jpeg.size()) invalid("JPEG marker 被截断。");
        const auto marker = U8(jpeg, at++);
        if (++segments > limits.maxJpegSegments) Fail("TERRAIN_ATLAS_JPEG_BUDGET", "JPEG 段数超出预算。");
        if (marker == 0xd9) {
            if (at != jpeg.size()) invalid("JPEG EOI 后还有未解释的 mip 数据。");
            ended = true;
            break;
        }
        if (marker == 0 || marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            invalid("JPEG 段边界出现无效/不支持的独立 marker。");
        if (jpeg.size() - at < 2) invalid("JPEG 段长度被截断。");
        const auto length = Be16(jpeg, at);
        if (length < 2 || length > jpeg.size() - at) invalid("JPEG 段范围越界。");
        const auto data = jpeg.subspan(at + 2, length - 2);
        at += length;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (!components.empty() || data.size() < 6 || U8(data, 0) != 8) invalid("JPEG 帧头重复、截断或不是 8-bit。");
            if (Be16(data, 1) != mip.height || Be16(data, 3) != mip.width)
                invalid("JPEG SOF 尺寸与 BLP 当前 mip 不一致。");
            const auto count = U8(data, 5);
            if ((count != 1 && count != 3 && count != 4) || data.size() != 6u + count * 3u)
                invalid("JPEG 组件数或帧头长度不受支持。");
            if (alpha == 8 && count != 4) invalid("8-bit alpha 的 JPEG 缺少独立第四组件。");
            progressive = marker == 0xc2;
            unsigned maxH = 1, maxV = 1;
            for (std::size_t index = 0; index < count; ++index) {
                const JpegComponent component{U8(data, 6 + index * 3),
                    static_cast<std::uint8_t>(U8(data, 7 + index * 3) >> 4),
                    static_cast<std::uint8_t>(U8(data, 7 + index * 3) & 15), U8(data, 8 + index * 3)};
                if (component.h < 1 || component.h > 4 || component.v < 1 || component.v > 4 || component.quant > 3
                    || std::any_of(components.begin(), components.end(), [&](const auto& old) { return old.id == component.id; }))
                    invalid("JPEG 组件身份或采样因子非法。");
                maxH = std::max(maxH, static_cast<unsigned>(component.h));
                maxV = std::max(maxV, static_cast<unsigned>(component.v));
                components.push_back(component);
            }
            const auto mcuColumns = (mip.width + 8 * maxH - 1) / (8 * maxH);
            const auto mcuRows = (mip.height + 8 * maxV - 1) / (8 * maxV);
            std::size_t coefficients = 0, largestPlane = 0;
            for (const auto& component : components) {
                if (maxH % component.h != 0 || maxV % component.v != 0) invalid("Core 不支持此 JPEG 采样因子组合。");
                const auto plane = static_cast<std::size_t>(mcuColumns) * mcuRows * component.h * component.v * 64;
                coefficients += plane * sizeof(std::int16_t);
                largestPlane = std::max(largestPlane, plane);
            }
            // 同时计入 Core 的系数、IDCT 单平面、组件像素和最终 RGBA，故意取保守和。
            memory = coefficients + largestPlane + static_cast<std::size_t>(mip.width) * mip.height * (count + 4u);
        } else if (marker == 0xdb) {
            for (std::size_t index = 0; index < data.size();) {
                const auto precision = U8(data, index) >> 4, id = U8(data, index) & 15;
                ++index;
                const std::size_t size = precision == 0 ? 64 : 128;
                if (precision > 1 || id > 3 || size > data.size() - index) invalid("JPEG 量化表范围无效。");
                for (std::size_t coefficient = 0; coefficient < 64; ++coefficient)
                    if ((precision == 0 ? U8(data, index + coefficient) : Be16(data, index + coefficient * 2)) == 0)
                        invalid("JPEG 量化表包含非法零值。");
                quant[id] = true;
                index += size;
            }
        } else if (marker == 0xc4) {
            for (std::size_t index = 0; index < data.size();) {
                if (data.size() - index < 17) invalid("JPEG Huffman 表被截断。");
                const auto type = U8(data, index) >> 4, id = U8(data, index) & 15;
                if (type > 1 || id > 3) invalid("JPEG Huffman 表类型或索引无效。");
                ++index;
                unsigned symbols = 0;
                int available = 1;
                for (std::size_t lengthIndex = 0; lengthIndex < 16; ++lengthIndex) {
                    const auto count = U8(data, index++);
                    available = available * 2 - count;
                    if (available <= 0) invalid("JPEG Huffman 码长过度分配或使用保留的全1码。");
                    symbols += count;
                }
                if (symbols == 0 || symbols > 256 || symbols > data.size() - index) invalid("JPEG Huffman 符号范围无效。");
                for (unsigned symbol = 0; symbol < symbols; ++symbol) {
                    const auto value = U8(data, index + symbol);
                    if ((type == 0 && value > 11) || (type == 1 && (value & 15) > 10))
                        invalid("8-bit JPEG Huffman 系数类别无效。");
                }
                (type == 0 ? dc : ac)[id] = true;
                index += symbols;
            }
        } else if (marker == 0xdd) {
            if (data.size() != 2) invalid("JPEG DRI 长度无效。");
        } else if (marker == 0xda) {
            if (components.empty() || data.empty()) invalid("JPEG 扫描缺少帧头。");
            const auto count = U8(data, 0);
            if (count < 1 || count > components.size() || data.size() != 1u + count * 2u + 3u)
                invalid("JPEG 扫描组件或长度无效。");
            const auto ss = U8(data, 1 + count * 2), se = U8(data, 2 + count * 2);
            const auto ah = U8(data, 3 + count * 2) >> 4, al = U8(data, 3 + count * 2) & 15;
            if ((!progressive && (ss != 0 || se != 63 || ah != 0 || al != 0))
                || (progressive && (se > 63 || ss > se || (ss == 0 && se != 0)
                    || (ss != 0 && count != 1) || al > 13 || ah > 13 || (ah != 0 && ah != al + 1))))
                invalid("JPEG 扫描系数范围无效。");
            std::array<bool, 256> current{};
            for (std::size_t index = 0; index < count; ++index) {
                const auto id = U8(data, 1 + index * 2), tables = U8(data, 2 + index * 2);
                const auto component = std::find_if(components.begin(), components.end(), [&](const auto& value) { return value.id == id; });
                const auto dcId = tables >> 4, acId = tables & 15;
                if (component == components.end() || current[id] || dcId > 3 || acId > 3 || !quant[component->quant])
                    invalid("JPEG 扫描引用无效组件或缺少量化表。");
                if ((!progressive || (ss == 0 && ah == 0)) && !dc[dcId]) invalid("JPEG 扫描缺少 DC Huffman 表。");
                if ((!progressive || ss != 0) && !ac[acId]) invalid("JPEG 扫描缺少 AC Huffman 表。");
                current[id] = true;
                if (ss == 0 && ah == 0) seenComponent[id] = true;
            }
            if (++scans > limits.maxJpegScans) Fail("TERRAIN_ATLAS_JPEG_BUDGET", "JPEG 扫描数超出预算。");
            ++localScans;
            // 只定位下一 marker，0xFF00 与重启 marker 属于当前熵段；合法性由 Core 检查。
            while (at < jpeg.size()) {
                if (U8(jpeg, at) != 0xff) { ++at; continue; }
                const auto start = at;
                while (at < jpeg.size() && U8(jpeg, at) == 0xff) ++at;
                if (at >= jpeg.size()) invalid("JPEG 熵段末尾 marker 被截断。");
                const auto next = U8(jpeg, at);
                if (next == 0 || (next >= 0xd0 && next <= 0xd7)) { ++at; continue; }
                at = start;
                break;
            }
        } else if (!((marker >= 0xe0 && marker <= 0xef) || marker == 0xfe)) {
            invalid("不支持的 JPEG marker：" + std::to_string(marker));
        }
    }
    if (!ended || localScans == 0 || components.empty()
        || std::any_of(components.begin(), components.end(), [&](const auto& value) { return !seenComponent[value.id]; }))
        invalid("JPEG 缺少完整 EOI、扫描或组件初始 DC 数据。");
    return memory;
}

} // namespace

Result<Snapshot> DecodeTerrainTextureAtlas(const Resources::ResolvedResource& resource, const TerrainAtlasLimits& limits) {
    try {
        ValidateLimits(limits);
        const Bytes input(resource.bytes);
        const auto header = Inspect(input, limits);
        auto snapshot = std::make_shared<TerrainAtlasSnapshot>();
        snapshot->source = resource.location;
        snapshot->sourceBytes = input.size();
        snapshot->sourceWidth = header.width; snapshot->sourceHeight = header.height;
        snapshot->columns = header.width == header.height ? 4 : 8;
        std::size_t segments = 0, scans = 0;
        for (std::size_t level = 0; level < header.mips.size(); ++level) try {
            const auto& mip = header.mips[level];
            const auto minimumWorking = input.size() + header.prefix + mip.size + snapshot->RgbaByteCount() + 65536;
            if (minimumWorking > limits.maxWorkingBytes)
                Fail("TERRAIN_ATLAS_WORKING_BUDGET", "mip 包装前已超出工作集预算。");
            auto wrapper = WrapMip(input, header, mip);
            std::size_t working = input.size() + wrapper.size() + snapshot->RgbaByteCount() + 65536;
            if (header.compression == 0) {
                const auto jpeg = Bytes(wrapper).subspan(160);
                working += jpeg.size() + InspectJpeg(jpeg, mip, header.alpha, limits, segments, scans);
            } else working += static_cast<std::size_t>(mip.width) * mip.height * 4;
            if (working > limits.maxWorkingBytes) Fail("TERRAIN_ATLAS_WORKING_BUDGET", "mip " + std::to_string(level) + " 解码工作集超出预算。");
            auto decoded = Image::DecodeBlp(wrapper);
            if (!decoded.Succeeded()) {
                decoded.diagnostics.push_back({DiagnosticSeverity::Error, std::string(kSubsystem), "TERRAIN_ATLAS_DECODE",
                    "Core BLP 解码 mip " + std::to_string(level) + " 失败，未发布部分快照。", std::nullopt});
                throw Failure{std::move(decoded.diagnostics)};
            }
            if (!decoded.value->Valid() || decoded.value->width != mip.width || decoded.value->height != mip.height)
                Fail("TERRAIN_ATLAS_DECODE", "Core 返回像素尺寸与声明 mip 不一致。");
            if (level == 0) snapshot->image = std::move(*decoded.value);
            else snapshot->additionalMips.push_back(std::move(*decoded.value));
            ++snapshot->validatedMipCount;
        } catch (Failure&) {
            // 常见工具把最后一层写成上一层的副本（JPEG 尺寸不符）；游戏照常显示，这里从坏层截断。
            if (level == 0) throw;
            break;
        }
        const auto size = header.height / TerrainAtlasSnapshot::rows;
        for (std::uint32_t row = 0; row < TerrainAtlasSnapshot::rows; ++row)
            for (std::uint32_t column = 0; column < snapshot->columns; ++column)
                snapshot->cells.push_back({column, row, column * size, row * size, size, size});
        snapshot->sourceSha256 = Take(Sha256(input));
        return Result<Snapshot>::Success(std::move(snapshot));
    } catch (Failure& failure) {
        Result<Snapshot> result;
        result.diagnostics = std::move(failure.diagnostics);
        for (auto& diagnostic : result.diagnostics)
            diagnostic.message = resource.location.canonicalPath + "：" + diagnostic.message;
        return result;
    } catch (const std::bad_alloc&) {
        return Result<Snapshot>::Failure(std::string(kSubsystem), "TERRAIN_ATLAS_ALLOCATION", "Atlas 像素快照内存分配失败。");
    } catch (const std::length_error&) {
        return Result<Snapshot>::Failure(std::string(kSubsystem), "TERRAIN_ATLAS_ALLOCATION", "Atlas 像素快照超出容器容量。");
    }
}

Result<Snapshot> LoadTerrainTextureAtlas(const Resources::ResourceResolver& resolver,
    std::string_view virtualPath, const TerrainAtlasLimits& limits) {
    const auto resource = resolver.Read(virtualPath);
    if (!resource.Succeeded()) {
        Result<Snapshot> result;
        result.diagnostics = resource.diagnostics;
        return result;
    }
    return DecodeTerrainTextureAtlas(*resource.value, limits);
}

} // namespace War3Map::Render
