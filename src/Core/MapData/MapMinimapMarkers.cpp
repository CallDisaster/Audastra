#include "War3MapCore/MapData/MapMinimapMarkers.h"

#include <bit>
#include <new>
#include <stdexcept>
#include <utility>

// 根据公开格式独立实现，仅参考字段布局，不复制第三方解析器：
// https://github.com/Drake53/War3Net/blob/master/src/War3Net.Build.Core/Serialization/Binary/Environment/MapPreviewIcons.cs
// https://github.com/Drake53/War3Net/blob/master/src/War3Net.Build.Core/Serialization/Binary/Environment/PreviewIcon.cs
// https://github.com/flowtsohg/mdx-m3-viewer/blob/master/src/parsers/w3x/mmp/minimapicon.ts
namespace War3Map::MapData {
namespace {

constexpr std::size_t kHeaderBytes = 8, kRecordBytes = 16;

Status Reject(const char* code, const char* message, std::size_t offset = 0) {
    auto result = Status::Failure("MapMinimapMarkers", code, message);
    result.diagnostics.front().origin = OriginSpan{"war3map.mmp", offset, 0, {}};
    return result;
}

bool ValidLimits(const MinimapMarkerLimits& limits) {
    const MinimapMarkerLimits hard;
    return limits.maxBytes <= hard.maxBytes && limits.maxMarkers <= hard.maxMarkers;
}

std::int32_t I32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + shift / 8])) << shift;
    return std::bit_cast<std::int32_t>(value);
}

void PutI32(std::vector<std::byte>& bytes, std::int32_t value) {
    const auto raw = std::bit_cast<std::uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((raw >> shift) & 0xff));
}

} // namespace

Result<MinimapMarkersSnapshot> ParseMapMinimapMarkers(std::span<const std::byte> bytes,
    const MinimapMarkerLimits& limits) {
    const auto fail = [](const Status& status) -> Result<MinimapMarkersSnapshot> { return {{}, status.diagnostics}; };
    if (!ValidLimits(limits)) return fail(Reject("MINIMAP_MARKER_LIMITS", "调用方只能收紧 MMP 硬预算。"));
    if (bytes.size() > limits.maxBytes) return fail(Reject("MINIMAP_MARKER_BYTE_LIMIT", "MMP 超过读取字节预算。"));
    if (bytes.size() < kHeaderBytes) return fail(Reject("MINIMAP_MARKER_TRUNCATED", "MMP 头部不足 8 字节。", bytes.size()));
    const auto version = I32(bytes, 0);
    if (version != 0) return fail(Reject("MINIMAP_MARKER_VERSION", "只读取 MMP v0，未知版本不降级试读。"));
    const auto countRaw = I32(bytes, 4);
    if (countRaw < 0) return fail(Reject("MINIMAP_MARKER_NEGATIVE_COUNT", "MMP 记录数量不能为负。", 4));
    const auto count = static_cast<std::size_t>(countRaw);
    if (count > limits.maxMarkers) return fail(Reject("MINIMAP_MARKER_COUNT_LIMIT", "MMP 记录数量超过预算。", 4));
    const auto remaining = bytes.size() - kHeaderBytes;
    if (count > remaining / kRecordBytes)
        return fail(Reject("MINIMAP_MARKER_TRUNCATED", "MMP 记录在字段中间截断。", bytes.size()));
    if (remaining != count * kRecordBytes)
        return fail(Reject("MINIMAP_MARKER_TRAILING_DATA", "MMP 记录后有未识别字节，拒绝丢弃。", kHeaderBytes + count * kRecordBytes));
    try {
        MinimapMarkersSnapshot result;
        result.version = version;
        result.records.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto offset = kHeaderBytes + index * kRecordBytes;
            MinimapMarkerRecord record;
            record.typeRaw = I32(bytes, offset);
            record.positionRaw = {I32(bytes, offset + 4), I32(bytes, offset + 8)};
            for (std::size_t channel = 0; channel < record.colorBgra.size(); ++channel)
                record.colorBgra[channel] = std::to_integer<std::uint8_t>(bytes[offset + 12 + channel]);
            result.records.push_back(record);
        }
        return Result<MinimapMarkersSnapshot>::Success(std::move(result));
    } catch (const std::bad_alloc&) {
        return fail(Reject("MINIMAP_MARKER_ALLOCATION", "MMP 分配失败，未返回部分记录。"));
    } catch (const std::length_error&) {
        return fail(Reject("MINIMAP_MARKER_ALLOCATION", "MMP 容器容量不足，未返回部分记录。"));
    }
}

Result<std::vector<std::byte>> SerializeMapMinimapMarkers(const MinimapMarkersSnapshot& markers,
    const MinimapMarkerLimits& limits) {
    const auto fail = [](const Status& status) -> Result<std::vector<std::byte>> { return {{}, status.diagnostics}; };
    if (!ValidLimits(limits)) return fail(Reject("MINIMAP_MARKER_LIMITS", "调用方只能收紧 MMP 硬预算。"));
    if (markers.version != 0) return fail(Reject("MINIMAP_MARKER_VERSION", "只写出 MMP v0，禁止降级未知版本。"));
    if (markers.records.size() > limits.maxMarkers)
        return fail(Reject("MINIMAP_MARKER_COUNT_LIMIT", "MMP 记录数量超过预算。", 4));
    if (limits.maxBytes < kHeaderBytes || markers.records.size() > (limits.maxBytes - kHeaderBytes) / kRecordBytes)
        return fail(Reject("MINIMAP_MARKER_BYTE_LIMIT", "MMP 写出超过字节预算。"));
    try {
        std::vector<std::byte> result;
        result.reserve(kHeaderBytes + markers.records.size() * kRecordBytes);
        PutI32(result, markers.version);
        PutI32(result, static_cast<std::int32_t>(markers.records.size()));
        for (const auto& record : markers.records) {
            PutI32(result, record.typeRaw);
            PutI32(result, record.positionRaw[0]);
            PutI32(result, record.positionRaw[1]);
            for (const auto channel : record.colorBgra) result.push_back(static_cast<std::byte>(channel));
        }
        return Result<std::vector<std::byte>>::Success(std::move(result));
    } catch (const std::bad_alloc&) {
        return fail(Reject("MINIMAP_MARKER_ALLOCATION", "MMP 写出分配失败，未返回部分输出。"));
    } catch (const std::length_error&) {
        return fail(Reject("MINIMAP_MARKER_ALLOCATION", "MMP 写出容量不足，未返回部分输出。"));
    }
}

} // namespace War3Map::MapData
