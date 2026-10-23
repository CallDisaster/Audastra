#include "War3MapCore/MapData/TerrainUniformDoodadFootprint.h"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Check(Status status) {
    if (!status.Succeeded()) {
        std::string message = "operation failed";
        for (const auto& diagnostic : status.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "operation failed";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
void Refused(const Bytes& bytes, std::string_view code) {
    const auto original = bytes;
    const auto result = DecodeUniformDoodadFootprintTga(bytes);
    Require(!result.Succeeded() && !result.value && HasCode(result.diagnostics, code), "unsupported TGA must name its failure without a partial footprint");
    Require(bytes == original, "failed decode preserves the original resource bytes");
}

Bytes Tga(unsigned side, unsigned depth, std::array<unsigned char, 4> bgra = {255, 0, 255, 255},
    bool top = false, bool footer = false) {
    Bytes bytes(18);
    bytes[2] = std::byte{2};
    for (const auto at : {12u, 14u}) {
        bytes[at] = static_cast<std::byte>(side & 255);
        bytes[at + 1] = static_cast<std::byte>((side >> 8) & 255);
    }
    bytes[16] = static_cast<std::byte>(depth);
    bytes[17] = static_cast<std::byte>((top ? 0x20 : 0) | (depth == 32 ? 8 : 0));
    for (unsigned pixel = 0; pixel < side * side; ++pixel)
        for (unsigned channel = 0; channel < depth / 8; ++channel) bytes.push_back(static_cast<std::byte>(bgra[channel]));
    if (footer) {
        bytes.insert(bytes.end(), 8, std::byte{});
        for (const char c : std::string_view("TRUEVISION-XFILE.")) bytes.push_back(static_cast<std::byte>(c));
        bytes.push_back(std::byte{});
    }
    return bytes;
}

TerrainHeader Header(std::uint32_t width = 3, std::uint32_t height = 3) {
    TerrainHeader header; header.width = width; header.height = height;
    return header;
}
DoodadPlacement Placement(float x, float y) {
    DoodadPlacement placement;
    placement.rawcode = "Doo0"; placement.position = {x, y, 0}; placement.scale = {1, 1, 1};
    return placement;
}
void Rectangle(const std::vector<std::uint8_t>& cells, std::size_t columns,
    std::size_t x0, std::size_t y0, std::size_t x1, std::size_t y1, std::uint8_t mask, std::uint8_t base = 0) {
    for (std::size_t y = 0; y < cells.size() / columns; ++y) for (std::size_t x = 0; x < columns; ++x) {
        const auto expected = static_cast<std::uint8_t>(base | (x >= x0 && x < x1 && y >= y0 && y < y1 ? mask : 0));
        if (cells[y * columns + x] != expected)
            throw std::runtime_error("literal rectangle mismatch at " + std::to_string(x) + "," + std::to_string(y)
                + ": expected " + std::to_string(expected) + ", got " + std::to_string(cells[y * columns + x]));
    }
}
void RefusedAdd(std::vector<std::uint8_t>& cells, const TerrainHeader& header, const DoodadPlacement& placement,
    TerrainUniformDoodadFootprint footprint, std::uint8_t snapCells, std::string_view code) {
    const auto original = cells;
    const auto result = AddUniformDoodadFootprint(cells, header, placement, footprint, snapCells);
    Require(!result.Succeeded() && HasCode(result.diagnostics, code), "invalid footprint input must be rejected explicitly");
    Require(cells == original, "every failure is atomic, including failure on the second coordinate");
}

void RawChannelsAndUniformAlpha() {
    // 字面原生属性：B->4，G->2，R->0x31。不能拿WPM文件位代替内部属性。
    constexpr std::array<unsigned char, 8> expected{0, 4, 2, 6, 0x31, 0x35, 0x33, 0x37};
    for (unsigned combination = 0; combination < expected.size(); ++combination) {
        const std::array<unsigned char, 4> pixel{
            static_cast<unsigned char>(combination & 1 ? 1 : 0),
            static_cast<unsigned char>(combination & 2 ? 128 : 0),
            static_cast<unsigned char>(combination & 4 ? 255 : 0), 0};
        for (const auto depth : {24u, 32u}) for (const bool top : {false, true}) for (const bool footer : {false, true}) {
            const auto bytes = Tga(2, depth, pixel, top, footer);
            const auto original = bytes;
            const auto footprint = Take(DecodeUniformDoodadFootprintTga(bytes));
            Require(footprint.side == 2 && footprint.attributes == expected[combination], "BGR channel nonzero predicates match native internal attributes");
            Require(bytes == original, "successful decode preserves original bytes, orientation and footer");
        }
    }
    for (const auto side : {1u, 4u, 256u}) for (const auto alpha : std::array<unsigned char, 3>{0, 127, 255}) {
        const auto footprint = Take(DecodeUniformDoodadFootprintTga(Tga(side, 32, {255, 0, 255, alpha})));
        Require(footprint.side == side && footprint.attributes == 0x35, "uniform alpha is ignored by mask conversion but the full pixel stays uniform");
    }
    for (unsigned channel = 0; channel < 4; ++channel) {
        auto bytes = Tga(2, 32, {1, 1, 1, 1});
        bytes[18 + 3 * 4 + channel] = std::byte{2};
        Refused(bytes, "TERRAIN_UNIFORM_TGA_NONUNIFORM"); // 1->2仍非零，不能只比较mask。
    }
    auto rgb = Tga(2, 24, {1, 1, 1, 255}); rgb.back() = std::byte{2};
    Refused(rgb, "TERRAIN_UNIFORM_TGA_NONUNIFORM");
}

void TgaBoundariesRejectBeforePixelAccess() {
    const auto good = Tga(2, 24);
    for (std::size_t length = 0; length < 18; ++length)
        Refused(Bytes(good.begin(), good.begin() + length), "TERRAIN_UNIFORM_TGA_TRUNCATED");
    auto truncated = good; truncated.pop_back(); Refused(truncated, "TERRAIN_UNIFORM_TGA_TRUNCATED");
    for (const auto [at, value] : std::array<std::pair<std::size_t, unsigned>, 13>{
        {{0, 1}, {1, 1}, {2, 10}, {2, 3}, {3, 1}, {5, 1}, {7, 24}, {8, 1}, {10, 1},
         {16, 16}, {17, 0x10}, {17, 0x40}, {17, 0x80}}}) {
        auto bytes = good; bytes[at] = static_cast<std::byte>(value);
        Refused(bytes, "TERRAIN_UNIFORM_TGA_FORMAT");
    }
    auto alphaDescriptor = Tga(2, 32); alphaDescriptor[17] = std::byte{};
    Refused(alphaDescriptor, "TERRAIN_UNIFORM_TGA_FORMAT");
    auto rgbDescriptor = good; rgbDescriptor[17] = std::byte{8};
    Refused(rgbDescriptor, "TERRAIN_UNIFORM_TGA_FORMAT");
    for (const auto side : {0u, 257u, 65535u}) {
        auto bytes = good;
        for (const auto at : {12u, 14u}) {
            bytes[at] = static_cast<std::byte>(side & 255); bytes[at + 1] = static_cast<std::byte>(side >> 8);
        }
        Refused(bytes, "TERRAIN_UNIFORM_TGA_DIMENSIONS");
    }
    auto rectangle = good; rectangle[14] = std::byte{3};
    Refused(rectangle, "TERRAIN_UNIFORM_TGA_DIMENSIONS");
    auto garbage = good; garbage.push_back(std::byte{1});
    Refused(garbage, "TERRAIN_UNIFORM_TGA_TRAILING");
    const auto footer = Tga(2, 24, {255, 0, 255, 255}, false, true);
    for (const auto at : {good.size(), good.size() + 4, footer.size() - 2, footer.size() - 1}) {
        auto bytes = footer; bytes[at] ^= std::byte{1};
        Refused(bytes, "TERRAIN_UNIFORM_TGA_TRAILING");
    }
    auto shortFooter = footer; shortFooter.pop_back();
    Refused(shortFooter, "TERRAIN_UNIFORM_TGA_TRAILING");
}

void NativeSnappingHasIndependentLiteralRectangles() {
    struct Case { unsigned side; std::uint8_t snap; float position; std::size_t first, last; };
    // O=0、8x8通行格。先按64/128世界单位吸附，再做半格和向零投影。
    // 预期直接列出；不调用待测函数或重写同一公式来生成golden。
    constexpr std::array cases{
        Case{1, 2, 0, 0, 1}, Case{1, 2, 32, 1, 2},
        Case{2, 2, 0, 0, 2}, Case{2, 2, 64, 2, 4},
        Case{3, 2, 0, 0, 3}, Case{3, 2, 64, 1, 4},
        Case{4, 2, 0, 0, 3}, Case{4, 2, 32, 0, 4},
        Case{6, 2, 0, 0, 5}, Case{6, 2, 64, 0, 6},
        Case{8, 2, 0, 0, 5}, Case{8, 2, 64, 0, 7},
        Case{1, 4, 64, 3, 4}, Case{2, 4, 64, 3, 5},
        Case{3, 4, 0, 0, 2}, Case{3, 4, 64, 2, 5},
        Case{4, 4, 0, 0, 4}, Case{4, 4, 128, 4, 8},
        Case{6, 4, 0, 0, 6}, Case{6, 4, 128, 3, 8},
        Case{8, 4, 0, 0, 5}, Case{8, 4, 64, 0, 8}};
    for (const auto& example : cases) {
        std::vector<std::uint8_t> cells(64);
        Check(AddUniformDoodadFootprint(cells, Header(), Placement(example.position, example.position), {example.side, 0x35}, example.snap));
        Rectangle(cells, 8, example.first, example.first, example.last, example.last, 0x35);
    }
    std::vector<std::uint8_t> cells(64);
    Check(AddUniformDoodadFootprint(cells, Header(), Placement(-32, 64), {2, 4}, 2));
    Rectangle(cells, 8, 0, 2, 1, 4, 4); // 起点-1，只裁一列；floor会错误裁掉两列。
    cells.assign(64, 0);
    Check(AddUniformDoodadFootprint(cells, Header(), Placement(64, -32), {2, 4}, 2));
    Rectangle(cells, 8, 2, 0, 4, 1, 4);
    cells.assign(64, 0);
    Check(AddUniformDoodadFootprint(cells, Header(), Placement(256, 256), {4, 2}, 2));
    Rectangle(cells, 8, 6, 6, 8, 8, 2);
    for (const auto position : {-512.0f, 512.0f, -16777216.0f, 16777216.0f}) {
        cells.assign(64, 0x40);
        Check(AddUniformDoodadFootprint(cells, Header(), Placement(position, position), {2, 0x35}, 2));
        Rectangle(cells, 8, 0, 0, 0, 0, 0, 0x40);
    }
    cells.assign(64, 0);
    Check(AddUniformDoodadFootprint(cells, Header(), Placement(128, 128), {256, 0x31}, 2));
    Rectangle(cells, 8, 0, 0, 8, 8, 0x31);
}

void OriginsRotationScaleAndStride() {
    auto header = Header(3, 2); header.offset = {-128, 256};
    auto placement = Placement(-32, 320);
    const auto originalPosition = placement.position;
    const auto originalScale = placement.scale;
    for (const auto rotation : {-360.0f, -1.0f, 0.0f, 0.785398185f, 1.57079637f, 3.14159274f, 6.28318548f, 360.0f}) {
        placement.rotation = rotation;
        placement.scale = {0, -2, 128}; // 可见模型缩放不改变通行纹理尺寸。
        placement.flags = 0xff; placement.lifePercent = 0; // 分类/注册资格由上层证明，不从wire byte猜测。
        std::vector<std::uint8_t> cells(32);
        Check(AddUniformDoodadFootprint(cells, header, placement, {4, 0x35}, 2));
        Rectangle(cells, 8, 2, 0, 6, 4, 0x35);
        Require(placement.position == originalPosition && placement.rotation == rotation
            && placement.scale == std::array<float, 3>{0, -2, 128} && placement.flags == 0xff
            && header.offset == std::array<float, 2>{-128, 256}, "rasterization never rewrites placement or terrain");
    }
    placement.scale = originalScale;
    header = Header(512, 2); header.offset = {-32768, 0};
    std::vector<std::uint8_t> edge(2044 * 4);
    Check(AddUniformDoodadFootprint(edge, header, Placement(32608, 0), {2, 4}, 2));
    Rectangle(edge, 2044, 2042, 0, 2044, 2, 4);
}

void FractionalCoordinatesHaveIndependentRectangles() {
    struct Case { unsigned side; std::uint8_t snap; float position; std::size_t first, last; };
    // 在原版吸附边界两侧取明确距离；预期是手工列出的完整矩形。
    constexpr std::array cases{
        Case{2, 2, 1.0f, 0, 2}, Case{2, 2, 31.0f, 0, 2},
        Case{2, 2, 63.5f, 0, 2}, Case{2, 2, 64.5f, 2, 4},
        Case{4, 2, 31.5f, 0, 3}, Case{4, 2, 32.5f, 0, 4},
        Case{4, 2, 96.5f, 2, 6},
        Case{2, 4, 63.5f, 0, 2}, Case{2, 4, 64.5f, 3, 5},
        Case{2, 2, -0.5f, 0, 1}, Case{2, 2, -64.5f, 0, 0}};
    for (const auto& example : cases) {
        std::vector<std::uint8_t> cells(64, 0x40);
        Check(AddUniformDoodadFootprint(cells, Header(), Placement(example.position, example.position), {example.side, 0x35}, example.snap));
        Rectangle(cells, 8, example.first, example.first, example.last, example.last, 0x35, 0x40);
    }
    struct Axes { float x, y; std::size_t x0, y0, x1, y1; };
    constexpr std::array axes{
        Axes{64, 1, 2, 0, 4, 2}, Axes{1, 64, 0, 2, 2, 4},
        Axes{63.5f, 64.5f, 0, 2, 2, 4}, Axes{64.5f, 63.5f, 2, 0, 4, 2},
        Axes{-0.5f, 64.5f, 0, 2, 1, 4}, Axes{64.5f, -0.5f, 2, 0, 4, 1}};
    for (const auto& example : axes) {
        std::vector<std::uint8_t> cells(64);
        Check(AddUniformDoodadFootprint(cells, Header(), Placement(example.x, example.y), {2, 4}, 2));
        Rectangle(cells, 8, example.x0, example.y0, example.x1, example.y1, 4);
    }
    auto header = Header(3, 2); header.offset = {-128, 256};
    std::vector<std::uint8_t> cells(32);
    Check(AddUniformDoodadFootprint(cells, header, Placement(-31.5f, 320.5f), {4, 2}, 2));
    Rectangle(cells, 8, 2, 0, 6, 4, 2);
    // 最小正负F32也不能因向零刷除而落入旧的“32对齐”分支。
    cells.assign(64, 0);
    Check(AddUniformDoodadFootprint(cells, Header(), Placement(std::numeric_limits<float>::denorm_min(), 1), {4, 4}, 2));
    Rectangle(cells, 8, 0, 0, 3, 3, 4);
    cells.assign(64, 0);
    Check(AddUniformDoodadFootprint(cells, Header(), Placement(-std::numeric_limits<float>::min(), 1), {2, 4}, 2));
    Rectangle(cells, 8, 0, 0, 1, 2, 4);
}

void AmbiguousSnappingRefusesBeforeWriting() {
    std::vector<std::uint8_t> cells(64, 0x40);
    for (const auto side : {2u, 4u}) {
        const float boundary = side == 2 ? 64.0f : 32.0f;
        for (const float coordinate : {std::nextafter(boundary, 0.0f), std::nextafter(boundary, 128.0f)}) {
            RefusedAdd(cells, Header(), Placement(coordinate, 1), {side, 4}, 2, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS");
            RefusedAdd(cells, Header(), Placement(1, coordinate), {side, 4}, 2, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS");
        }
    }
    for (const float coordinate : {std::nextafter(64.0f, 0.0f), std::nextafter(64.0f, 128.0f)})
        RefusedAdd(cells, Header(), Placement(1, coordinate), {2, 0}, 4, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS");
    RefusedAdd(cells, Header(), Placement(-std::numeric_limits<float>::denorm_min(), 1), {2, 4}, 2, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS");
    for (const auto axis : {0u, 1u}) {
        auto header = Header(); header.offset[axis] = -32768;
        auto placement = Placement(1, 1); placement.position[axis] = 16777215;
        RefusedAdd(cells, header, placement, {2, 4}, 2, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS");
    }
}

void NonDefaultFloatModesAreRejectedWithoutMutation() {
    struct RestoreFloatMode {
        std::fenv_t environment;
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        unsigned csr = _mm_getcsr();
#endif
        RestoreFloatMode() { Require(std::fegetenv(&environment) == 0, "capture floating point environment"); }
        ~RestoreFloatMode() {
            std::fesetenv(&environment);
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
            _mm_setcsr(csr);
#endif
        }
    };
    std::vector<std::uint8_t> cells(64, 0x40);
    for (const auto rounding : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        RestoreFloatMode restore;
        Require(std::fesetround(rounding) == 0, "set test rounding mode");
        RefusedAdd(cells, Header(), Placement(64.5f, 1), {2, 4}, 2, "TERRAIN_UNIFORM_FLOAT_ENVIRONMENT");
        Require(std::fegetround() == rounding, "rejected calculation does not reset caller rounding mode");
    }
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    for (const auto mask : {0x40u, 0x8000u, 0x8040u, 0x2000u, 0x4000u, 0x6000u}) {
        RestoreFloatMode restore;
        const auto expectedCsr = _mm_getcsr() | mask;
        _mm_setcsr(expectedCsr);
        RefusedAdd(cells, Header(), Placement(-std::numeric_limits<float>::min(), 1), {2, 4}, 2, "TERRAIN_UNIFORM_FLOAT_ENVIRONMENT");
        Require(_mm_getcsr() == expectedCsr, "rejected calculation preserves caller SSE rounding, FTZ, DAZ and exception flags");
    }
#endif
    Check(AddUniformDoodadFootprint(cells, Header(), Placement(64.5f, 1), {2, 4}, 2));
    Rectangle(cells, 8, 2, 0, 4, 2, 4, 0x40);
}

void OrCompositionIsStableAcrossOrderAndOverlapCount() {
    std::vector<std::uint8_t> first(64, 0x40), reverse(64, 0x40);
    const auto header = Header();
    Check(AddUniformDoodadFootprint(first, header, Placement(96, 96), {4, 0x31}, 2));
    Check(AddUniformDoodadFootprint(first, header, Placement(64, 64), {2, 6}, 2));
    Check(AddUniformDoodadFootprint(reverse, header, Placement(64, 64), {2, 6}, 2));
    Check(AddUniformDoodadFootprint(reverse, header, Placement(96, 96), {4, 0x31}, 2));
    Require(first == reverse, "static contribution is independent of object order");
    for (std::size_t y = 0; y < 8; ++y) for (std::size_t x = 0; x < 8; ++x) {
        const auto expected = x >= 2 && x < 4 && y >= 2 && y < 4 ? 0x77
            : x >= 2 && x < 6 && y >= 2 && y < 6 ? 0x71 : 0x40;
        Require(first[y * 8 + x] == expected, "overlap OR retains every earlier attribute including suppression");
    }
    for (unsigned count = 2; count <= 32; ++count) {
        Check(AddUniformDoodadFootprint(first, header, Placement(96, 96), {4, 0x31}, 2));
        Require(first == reverse, "31 and 32 overlapping additions do not wrap or clear attributes");
    }
    Check(AddUniformDoodadFootprint(first, header, Placement(0, 0), {256, 0}, 4));
    Require(first == reverse, "uniform black adds no contribution and preserves the existing grid");
}

void GridInvariantInstancesHaveIndependentLiteralContributions() {
    const auto header = Header(9, 9);
    struct Case { unsigned side; float position; std::size_t first, last; };
    // 两套合法投影相同的完整裁剪矩形，以字面范围独立给定，不调用被测投影制造预期。
    constexpr std::array cases{Case{4, 64, 0, 4}, Case{4, 192, 4, 8}, Case{8, 512, 12, 20},
        Case{8, 0, 0, 5}, Case{8, -128, 0, 1}, Case{8, 1024, 28, 32}};
    for (const auto& example : cases) {
        std::vector<std::uint8_t> cells(32 * 32, 0x40);
        Check(AddGridInvariantUniformDoodadFootprint(cells, header, Placement(example.position, example.position), {example.side, 4}));
        Rectangle(cells, 32, example.first, example.first, example.last, example.last, 4, 0x40);
    }
    auto negative = header; negative.offset = {-128, -128};
    std::vector<std::uint8_t> cells(32 * 32);
    Check(AddGridInvariantUniformDoodadFootprint(cells, negative, Placement(-64, -64), {4, 0x31}));
    Rectangle(cells, 32, 0, 0, 4, 4, 0x31);
    cells.assign(32 * 32, 0x40);
    Check(AddGridInvariantUniformDoodadFootprint(cells, header, Placement(-256, -256), {8, 4}));
    Rectangle(cells, 32, 0, 0, 0, 0, 0, 0x40); // 两投影均完全在图外。
    Check(AddGridInvariantUniformDoodadFootprint(cells, header, Placement(544, 512), {8, 0}));
    Rectangle(cells, 32, 0, 0, 0, 0, 0, 0x40); // 零RGB的两套合法贡献都为空。
    const auto transparent = Take(DecodeUniformDoodadFootprintTga(Tga(8, 32, {0, 255, 0, 0})));
    cells.assign(32 * 32, 0);
    Check(AddGridInvariantUniformDoodadFootprint(cells, header, Placement(512, 512), transparent));
    Rectangle(cells, 32, 12, 12, 20, 20, 2); // alpha0不能清除非零绿色贡献。
}

void GridInvariantFailureCannotBeHiddenByExistingOrOrZeroColor() {
    const auto header = Header(9, 9);
    const auto refused = [&](DoodadPlacement placement, TerrainUniformDoodadFootprint footprint, std::string_view code) {
        std::vector<std::uint8_t> cells(32 * 32, 0x37); const auto before = cells;
        const auto status = AddGridInvariantUniformDoodadFootprint(cells, header, placement, footprint);
        Require(!status.Succeeded() && HasCode(status.diagnostics, code), "two/four difference or uncertainty is explicit");
        Require(cells == before, "even a fully OR-covered target cannot conceal a per-instance failure");
    };
    for (const auto axis : {0u, 1u}) {
        auto placement = Placement(512, 512); placement.position[axis] += 32;
        refused(placement, {8, 4}, "TERRAIN_UNIFORM_GRID_VARIANT");
        placement = Placement(512, 512); placement.position[axis] = std::nextafter(544.0f, 512.0f);
        refused(placement, {8, 0}, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS"); // 2格不确定，零颜色也拒绝。
        placement.position[axis] = std::nextafter(576.0f, 512.0f);
        refused(placement, {8, 0}, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS"); // 4格不确定。
    }
    auto placement = Placement(-256, -256); placement.rotation = std::numeric_limits<float>::quiet_NaN();
    refused(placement, {8, 0}, "TERRAIN_UNIFORM_PLACEMENT_DOMAIN");
    placement = Placement(512, 512); placement.scale[2] = std::numeric_limits<float>::infinity();
    refused(placement, {8, 0}, "TERRAIN_UNIFORM_PLACEMENT_DOMAIN");
    placement = Placement(512, 512); placement.position[0] = std::numeric_limits<float>::infinity();
    refused(placement, {8, 0}, "TERRAIN_UNIFORM_COORDINATE_DOMAIN");
}

void InvalidInputsAreAtomic() {
    const auto goodHeader = Header(); const auto goodPlacement = Placement(64, 64);
    std::vector<std::uint8_t> cells(64, 0x40);
    for (const auto size : {0u, 63u, 65u}) {
        std::vector<std::uint8_t> wrong(size, 0x40);
        RefusedAdd(wrong, goodHeader, goodPlacement, {2, 4}, 2, "TERRAIN_UNIFORM_GRID_SHAPE");
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        auto header = goodHeader;
        if (mode == 0) header.version = 12;
        else if (mode == 1) header.width = 1;
        else if (mode == 2) header.height = 2050;
        else header.width = std::numeric_limits<std::uint32_t>::max();
        RefusedAdd(cells, header, goodPlacement, {2, 4}, 2, "TERRAIN_UNIFORM_GRID_SHAPE");
    }
    for (const auto offset : {32.0f, -32896.0f, 32640.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto header = goodHeader; header.offset[1] = offset;
        RefusedAdd(cells, header, goodPlacement, {2, 4}, 2, "TERRAIN_UNIFORM_COORDINATE_DOMAIN");
    }
    for (const auto coordinate : {16777248.0f, -16777248.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto placement = goodPlacement; placement.position[1] = coordinate;
        RefusedAdd(cells, goodHeader, placement, {2, 4}, 2, "TERRAIN_UNIFORM_COORDINATE_DOMAIN");
    }
    for (const auto rotation : {std::nextafter(360.0f, 361.0f), std::nextafter(-360.0f, -361.0f),
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto placement = goodPlacement; placement.rotation = rotation;
        RefusedAdd(cells, goodHeader, placement, {2, 4}, 2, "TERRAIN_UNIFORM_PLACEMENT_DOMAIN");
    }
    auto placement = goodPlacement; placement.position[2] = std::numeric_limits<float>::quiet_NaN();
    RefusedAdd(cells, goodHeader, placement, {2, 4}, 2, "TERRAIN_UNIFORM_PLACEMENT_DOMAIN");
    placement = goodPlacement; placement.scale[2] = std::numeric_limits<float>::infinity();
    RefusedAdd(cells, goodHeader, placement, {2, 4}, 2, "TERRAIN_UNIFORM_PLACEMENT_DOMAIN");
    for (const auto side : {0u, 257u, std::numeric_limits<std::uint32_t>::max()})
        RefusedAdd(cells, goodHeader, goodPlacement, {side, 4}, 2, "TERRAIN_UNIFORM_FOOTPRINT");
    for (const auto mask : std::array<unsigned char, 8>{1, 0x10, 0x20, 0x30, 0x38, 0x40, 0x80, 0xff})
        RefusedAdd(cells, goodHeader, goodPlacement, {2, mask}, 2, "TERRAIN_UNIFORM_FOOTPRINT");
    for (const auto snap : std::array<unsigned char, 5>{0, 1, 3, 5, 255})
        RefusedAdd(cells, goodHeader, goodPlacement, {2, 4}, snap, "TERRAIN_UNIFORM_SNAP_CELLS");
    placement = goodPlacement; placement.position[1] = std::nextafter(64.0f, 0.0f);
    RefusedAdd(cells, goodHeader, placement, {2, 0}, 2, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS");
}
} // namespace

int main() {
    try {
        RawChannelsAndUniformAlpha();
        TgaBoundariesRejectBeforePixelAccess();
        NativeSnappingHasIndependentLiteralRectangles();
        OriginsRotationScaleAndStride();
        FractionalCoordinatesHaveIndependentRectangles();
        AmbiguousSnappingRefusesBeforeWriting();
        NonDefaultFloatModesAreRejectedWithoutMutation();
        OrCompositionIsStableAcrossOrderAndOverlapCount();
        GridInvariantInstancesHaveIndependentLiteralContributions();
        GridInvariantFailureCannotBeHiddenByExistingOrOrZeroColor();
        InvalidInputsAreAtomic();
        std::cout << "Terrain uniform doodad footprint tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
