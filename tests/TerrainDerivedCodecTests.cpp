#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/MapData/MapShadow.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "codec operation failed";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}

template<class T> void Rejected(const Result<T>& result, const char* code) {
    Require(!result.Succeeded() && !result.value, "rejected codec must not expose partial output");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& item) { return item.code == code; }),
        "codec rejection must keep its precise reason");
}

Bytes BytesOf(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

// 手写小端布局，不借用产品 writer 构造输入。两记录均有非默认颜色；第二个
// 包含未知负类型及 int32 极值坐标，用于证明原始记录不被删掉、截断或归一。
Bytes Markers() {
    return BytesOf({
        0, 0, 0, 0, 2, 0, 0, 0,
        2, 0, 0, 0, 0x23, 0x01, 0, 0, 0x45, 0x02, 0, 0, 0x17, 0x38, 0x59, 0x7a,
        0xfe, 0xff, 0xff, 0xff, 0, 0, 0, 0x80, 0xff, 0xff, 0xff, 0x7f, 0xff, 0x80, 0x01, 0
    });
}

void MarkersPreserveEveryRecordByte() {
    const auto original = Markers();
    auto parsed = Take(ParseMapMinimapMarkers(original));
    Require(parsed.version == 0 && parsed.records.size() == 2, "MMP header is decoded independently");
    const auto& first = parsed.records.front();
    Require(first.typeRaw == 2 && first.positionRaw == std::array<std::int32_t, 2>{0x123, 0x245}
        && first.colorBgra == std::array<std::uint8_t, 4>{0x17, 0x38, 0x59, 0x7a}, "record field and BGRA order match hand bytes");
    const auto& last = parsed.records.back();
    Require(last.typeRaw == -2 && last.positionRaw[0] == std::numeric_limits<std::int32_t>::min()
        && last.positionRaw[1] == std::numeric_limits<std::int32_t>::max(), "unknown signed values are preserved as raw fields");
    Require(Take(SerializeMapMinimapMarkers(parsed)) == original, "MMP read-write keeps all source bytes");
    parsed.records.front().positionRaw[0] = -7;
    auto expected = original;
    expected[12] = std::byte{0xf9};
    expected[13] = expected[14] = expected[15] = std::byte{0xff};
    Require(Take(SerializeMapMinimapMarkers(parsed)) == expected, "one edited raw coordinate changes only its exact four bytes");
    const auto empty = BytesOf({0, 0, 0, 0, 0, 0, 0, 0});
    Require(Take(SerializeMapMinimapMarkers(Take(ParseMapMinimapMarkers(empty)))) == empty,
        "zero records is a present eight-byte member, never a synthesized missing file");
}

void MarkersRejectUnknownBoundariesAndBudgets() {
    const auto original = Markers();
    for (std::size_t size = 0; size < original.size(); ++size) {
        const auto result = ParseMapMinimapMarkers(std::span(original).first(size));
        Rejected(result, "MINIMAP_MARKER_TRUNCATED");
    }
    auto invalid = original; invalid.push_back(std::byte{0x71});
    Rejected(ParseMapMinimapMarkers(invalid), "MINIMAP_MARKER_TRAILING_DATA");
    invalid = original; invalid[0] = std::byte{1};
    Rejected(ParseMapMinimapMarkers(invalid), "MINIMAP_MARKER_VERSION");
    invalid = original;
    std::fill(invalid.begin() + 4, invalid.begin() + 8, std::byte{0xff});
    Rejected(ParseMapMinimapMarkers(invalid), "MINIMAP_MARKER_NEGATIVE_COUNT");
    invalid[7] = std::byte{0x7f};
    Rejected(ParseMapMinimapMarkers(invalid), "MINIMAP_MARKER_COUNT_LIMIT");
    MinimapMarkerLimits limits;
    limits.maxBytes = original.size() - 1;
    Rejected(ParseMapMinimapMarkers(original, limits), "MINIMAP_MARKER_BYTE_LIMIT");
    auto parsed = Take(ParseMapMinimapMarkers(original));
    Rejected(SerializeMapMinimapMarkers(parsed, limits), "MINIMAP_MARKER_BYTE_LIMIT");
    limits = {}; limits.maxMarkers = 1;
    Rejected(ParseMapMinimapMarkers(original, limits), "MINIMAP_MARKER_COUNT_LIMIT");
    Rejected(SerializeMapMinimapMarkers(parsed, limits), "MINIMAP_MARKER_COUNT_LIMIT");
    limits = {}; ++limits.maxMarkers;
    Rejected(ParseMapMinimapMarkers(original, limits), "MINIMAP_MARKER_LIMITS");
    Rejected(SerializeMapMinimapMarkers(parsed, limits), "MINIMAP_MARKER_LIMITS");
    limits = {}; ++limits.maxBytes;
    Rejected(ParseMapMinimapMarkers(original, limits), "MINIMAP_MARKER_LIMITS");
    Rejected(SerializeMapMinimapMarkers(parsed, limits), "MINIMAP_MARKER_LIMITS");
    parsed.version = 1;
    Rejected(SerializeMapMinimapMarkers(parsed), "MINIMAP_MARKER_VERSION");
    limits = {}; limits.maxBytes = 7;
    Rejected(SerializeMapMinimapMarkers(MinimapMarkersSnapshot{}, limits), "MINIMAP_MARKER_BYTE_LIMIT");
    Require(original == Markers(), "all codec failures leave caller bytes unchanged");
}

void ShadowsKeepRawStrengthsAndExplicitDimensions() {
    const auto original = BytesOf({0, 1, 7, 0x55, 254, 255});
    auto parsed = Take(ParseMapShadow(original, 3, 2));
    Require(parsed.width == 3 && parsed.height == 2
        && parsed.intensities == std::vector<std::uint8_t>{0, 1, 7, 0x55, 254, 255}, "SHD keeps all nonbinary strengths");
    Require(Take(SerializeMapShadow(parsed)) == original, "SHD read-write is byte identical");
    parsed.intensities[4] = 0x31;
    auto expected = original; expected[4] = std::byte{0x31};
    Require(Take(SerializeMapShadow(parsed)) == expected, "one strength edit changes only the selected byte");
    Require(Take(SerializeMapShadow(Take(ParseMapShadow(original, 2, 3)))) == original,
        "headerless bytes do not guess aspect ratio or spatial direction");
}

void ShadowsRejectMissingCellsAndOverflow() {
    const auto original = BytesOf({0, 1, 7, 0x55, 254, 255});
    Rejected(ParseMapShadow(original, 0, 2), "SHADOW_DIMENSIONS");
    Rejected(ParseMapShadow(original, 3, 0), "SHADOW_DIMENSIONS");
    Rejected(ParseMapShadow({}, 3, 2), "SHADOW_TRUNCATED");
    Rejected(ParseMapShadow(std::span(original).first(5), 3, 2), "SHADOW_TRUNCATED");
    Rejected(ParseMapShadow(original, 1, 5), "SHADOW_TRAILING_DATA");
    Rejected(ParseMapShadow(original, std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max()), "SHADOW_GRID_LIMIT");
    auto parsed = Take(ParseMapShadow(original, 3, 2));
    parsed.intensities.pop_back();
    Rejected(SerializeMapShadow(parsed), "SHADOW_INCONSISTENT");
    parsed = Take(ParseMapShadow(original, 3, 2));
    ShadowReadLimits limits; limits.maxCells = 5;
    Rejected(ParseMapShadow(original, 3, 2, limits), "SHADOW_GRID_LIMIT");
    Rejected(SerializeMapShadow(parsed, limits), "SHADOW_GRID_LIMIT");
    limits = {}; limits.maxDimension = 2;
    Rejected(ParseMapShadow(original, 3, 2, limits), "SHADOW_GRID_LIMIT");
    limits = {}; limits.maxBytes = 5;
    Rejected(ParseMapShadow(original, 3, 2, limits), "SHADOW_BYTE_LIMIT");
    Rejected(SerializeMapShadow(parsed, limits), "SHADOW_BYTE_LIMIT");
    for (const auto field : {0, 1, 2}) {
        limits = {};
        if (field == 0) ++limits.maxBytes;
        if (field == 1) ++limits.maxDimension;
        if (field == 2) ++limits.maxCells;
        Rejected(ParseMapShadow(original, 3, 2, limits), "SHADOW_LIMITS");
        Rejected(SerializeMapShadow(parsed, limits), "SHADOW_LIMITS");
    }
    Require(original == BytesOf({0, 1, 7, 0x55, 254, 255}), "SHD failures leave source bytes intact");
}

} // namespace

int main() {
    try {
        MarkersPreserveEveryRecordByte();
        MarkersRejectUnknownBoundariesAndBudgets();
        ShadowsKeepRawStrengthsAndExplicitDimensions();
        ShadowsRejectMissingCellsAndOverflow();
        std::cout << "Terrain derived codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
