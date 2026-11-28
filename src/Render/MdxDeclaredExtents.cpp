#include "War3MapCore/Render/MdxDeclaredExtents.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace War3Map::Render {
namespace {
constexpr std::string_view kDomain = "Render.MdxExtents";
constexpr std::size_t kModelChunkBytes = 372, kModelExtentOffset = 340;
constexpr std::size_t kSequenceBytes = 132, kSequenceExtentOffset = 104;

template<class T> Result<T> Fail(const char* code, std::string message) {
    return Result<T>::Failure(std::string(kDomain), code, std::move(message));
}

std::uint32_t Word(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (unsigned k = 0; k < 4; ++k) value |= std::to_integer<std::uint32_t>(bytes[at + k]) << (8 * k);
    return value;
}
void PutWord(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned k = 0; k < 4; ++k) bytes[at + k] = std::byte((value >> (8 * k)) & 0xFF);
}
bool Tag(std::span<const std::byte> bytes, std::size_t at, const char (&tag)[5]) {
    return std::memcmp(bytes.data() + at, tag, 4) == 0;
}
MdxDeclaredExtent ReadExtent(std::span<const std::byte> bytes, std::size_t at) {
    MdxDeclaredExtent extent;
    extent.offset = at;
    extent.radius = std::bit_cast<float>(Word(bytes, at));
    for (unsigned a = 0; a < 3; ++a) {
        extent.minimum[a] = std::bit_cast<float>(Word(bytes, at + 4 + 4 * a));
        extent.maximum[a] = std::bit_cast<float>(Word(bytes, at + 16 + 4 * a));
    }
    return extent;
}

// 一个几何体内的顺序读取：只在 [at, end) 内前进，越界即失败。
struct Cursor {
    std::span<const std::byte> bytes;
    std::size_t at = 0, end = 0;
    bool Skip(std::size_t count) {
        if (count > end - at) return false;
        at += count;
        return true;
    }
    bool Array(const char (&tag)[5], std::size_t element) {
        if (end - at < 8 || !Tag(bytes, at, tag)) return false;
        const std::size_t count = Word(bytes, at + 4);
        at += 8;
        return count <= (end - at) / element && Skip(count * element);
    }
};

bool ReadGeosets(std::span<const std::byte> bytes, std::size_t begin, std::size_t end, MdxDeclaredExtents& out,
    std::string& problem) {
    for (std::size_t at = begin, index = 0; at < end; ++index) {
        const auto where = "第 " + std::to_string(index + 1) + " 个几何体";
        if (end - at < 4) { problem = where + "的大小字段不完整。"; return false; }
        const std::size_t inclusive = Word(bytes, at);
        if (inclusive < 4 || inclusive > end - at) { problem = where + "的大小超出 GEOS 块。"; return false; }
        Cursor cursor{bytes, at + 4, at + inclusive};
        // 材质、选择组、选择标志各 4 字节之后是主范围，再是逐动作范围的条数与内容。
        if (!cursor.Array("VRTX", 12) || !cursor.Array("NRMS", 12) || !cursor.Array("PTYP", 4) || !cursor.Array("PCNT", 4)
            || !cursor.Array("PVTX", 2) || !cursor.Array("GNDX", 1) || !cursor.Array("MTGC", 4) || !cursor.Array("MATS", 4)
            || !cursor.Skip(12) || cursor.end - cursor.at < kMdxExtentBytes + 4) {
            problem = where + "的字段顺序或长度与经典 MDX 不符，找不到范围字段。";
            return false;
        }
        out.geosets.push_back(ReadExtent(bytes, cursor.at));
        cursor.at += kMdxExtentBytes;
        const std::size_t count = Word(bytes, cursor.at);
        cursor.at += 4;
        if (count > (cursor.end - cursor.at) / kMdxExtentBytes) {
            problem = where + "的逐动作范围条数超出几何体。";
            return false;
        }
        for (std::size_t item = 0; item < count; ++item)
            out.geosetSequences.push_back(ReadExtent(bytes, cursor.at + item * kMdxExtentBytes));
        at += inclusive;
    }
    return true;
}

// 单精度向外取：最小值不大于原值，最大值不小于原值。超出单精度时为空。
std::optional<float> Outward(double value, bool upward) {
    if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max())) return std::nullopt;
    auto result = static_cast<float>(value);
    if (upward && double(result) < value) result = std::nextafter(result, std::numeric_limits<float>::infinity());
    if (!upward && double(result) > value) result = std::nextafter(result, -std::numeric_limits<float>::infinity());
    if (!std::isfinite(result)) return std::nullopt;
    return result;
}
} // namespace

bool MdxExtentUsable(const MdxDeclaredExtent& extent) noexcept {
    if (!std::isfinite(extent.radius) || extent.radius < 0) return false;
    for (unsigned a = 0; a < 3; ++a)
        if (!std::isfinite(extent.minimum[a]) || !std::isfinite(extent.maximum[a]) || extent.minimum[a] > extent.maximum[a])
            return false;
    return true;
}

Result<MdxDeclaredExtents> ReadMdxDeclaredExtents(std::span<const std::byte> bytes, MdxExtentScope scope) {
    using R = MdxDeclaredExtents;
    if (bytes.size() < 4 || !Tag(bytes, 0, "MDLX")) return Fail<R>("MDX_EXTENT_MAGIC", "不是 MDX 文件（缺少 MDLX 头）。");
    MdxDeclaredExtents out;
    bool version = false, model = false, sequences = false, geosets = false;
    for (std::size_t at = 4; at < bytes.size();) {
        if (bytes.size() - at < 8) return Fail<R>("MDX_EXTENT_LAYOUT", "第 " + std::to_string(at) + " 字节处的块头不完整。");
        const std::size_t size = Word(bytes, at + 4), body = at + 8;
        if (size > bytes.size() - body)
            return Fail<R>("MDX_EXTENT_LAYOUT", "第 " + std::to_string(at) + " 字节处的块超出文件末尾。");
        const auto duplicate = [](const char* tag) {
            return Fail<R>("MDX_EXTENT_LAYOUT", std::string(tag) + " 块出现多次，不能确定游戏采用哪一份。");
        };
        if (Tag(bytes, at, "VERS")) {
            if (version) return duplicate("VERS");
            if (size != 4) return Fail<R>("MDX_EXTENT_LAYOUT", "VERS 块大小不是 4 字节。");
            if (const auto value = Word(bytes, body); value != 800)
                return Fail<R>("MDX_EXTENT_VERSION", "只支持经典 MDX 版本 800（此文件为 " + std::to_string(value) + "）。");
            version = true;
        } else if (Tag(bytes, at, "MODL")) {
            if (model) return duplicate("MODL");
            if (size != kModelChunkBytes) return Fail<R>("MDX_EXTENT_LAYOUT", "MODL 块大小不是 372 字节。");
            out.model = ReadExtent(bytes, body + kModelExtentOffset);
            model = true;
        } else if (Tag(bytes, at, "SEQS")) {
            if (sequences) return duplicate("SEQS");
            if (size % kSequenceBytes) return Fail<R>("MDX_EXTENT_LAYOUT", "SEQS 块大小不是 132 字节的整数倍。");
            for (std::size_t record = body; record < body + size; record += kSequenceBytes)
                out.sequences.push_back(ReadExtent(bytes, record + kSequenceExtentOffset));
            sequences = true;
        } else if (scope == MdxExtentScope::All && Tag(bytes, at, "GEOS")) {
            if (geosets) return duplicate("GEOS");
            std::string problem;
            if (!ReadGeosets(bytes, body, body + size, out, problem)) return Fail<R>("MDX_EXTENT_LAYOUT", std::move(problem));
            geosets = true;
        }
        at = body + size;
    }
    if (!version) return Fail<R>("MDX_EXTENT_VERSION", "缺少 VERS 块，不能确认是经典 MDX 版本 800。");
    if (!model) return Fail<R>("MDX_EXTENT_LAYOUT", "缺少 MODL 块。");
    return Result<R>::Success(std::move(out));
}

Result<MdxWidenedExtents> WidenMdxDeclaredExtents(std::span<const std::byte> bytes,
    const std::array<double, 3>& minimum, const std::array<double, 3>& maximum) {
    using R = MdxWidenedExtents;
    for (unsigned a = 0; a < 3; ++a)
        if (!std::isfinite(minimum[a]) || !std::isfinite(maximum[a]) || minimum[a] > maximum[a])
            return Fail<R>("MDX_EXTENT_TARGET", "要包含的范围无效。");
    auto read = ReadMdxDeclaredExtents(bytes, MdxExtentScope::All);
    if (!read.Succeeded() || !read.value) return {{}, std::move(read.diagnostics)};
    const auto& declared = *read.value;
    MdxWidenedExtents out{std::vector<std::byte>(bytes.begin(), bytes.end()), 0};
    std::vector<const MdxDeclaredExtent*> all{&declared.model};
    for (const auto* list : {&declared.sequences, &declared.geosets, &declared.geosetSequences})
        for (const auto& extent : *list) all.push_back(&extent);
    for (const auto* extent : all) {
        if (!MdxExtentUsable(*extent))
            return Fail<R>("MDX_EXTENT_INVALID", "第 " + std::to_string(extent->offset)
                + " 字节处的范围数值无效（非有限、半径为负或最小大于最大），不能加宽。");
        bool contains = true;
        for (unsigned a = 0; a < 3; ++a)
            contains = contains && double(extent->minimum[a]) <= minimum[a] && double(extent->maximum[a]) >= maximum[a];
        if (contains) continue;
        std::array<float, 3> low{}, high{};
        double diagonal = 0;
        for (unsigned a = 0; a < 3; ++a) {
            const auto lower = Outward(std::min(double(extent->minimum[a]), minimum[a]), false);
            const auto upper = Outward(std::max(double(extent->maximum[a]), maximum[a]), true);
            if (!lower || !upper) return Fail<R>("MDX_EXTENT_RANGE", "加宽后的范围超出单精度可表示的数值。");
            low[a] = *lower; high[a] = *upper;
            diagonal += (double(high[a]) - low[a]) * (double(high[a]) - low[a]);
        }
        const auto half = Outward(0.5 * std::sqrt(diagonal), true);
        if (!half) return Fail<R>("MDX_EXTENT_RANGE", "加宽后的半径超出单精度可表示的数值。");
        PutWord(out.bytes, extent->offset, std::bit_cast<std::uint32_t>(std::max(extent->radius, *half)));
        for (unsigned a = 0; a < 3; ++a) {
            PutWord(out.bytes, extent->offset + 4 + 4 * a, std::bit_cast<std::uint32_t>(low[a]));
            PutWord(out.bytes, extent->offset + 16 + 4 * a, std::bit_cast<std::uint32_t>(high[a]));
        }
        ++out.changed;
    }
    return Result<R>::Success(std::move(out));
}

} // namespace War3Map::Render
