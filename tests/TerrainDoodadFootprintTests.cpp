#include "War3MapCore/MapData/TerrainDoodadFootprint.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Check(const Status& result) {
    if (result.Succeeded()) return;
    std::string message;
    for (const auto& d : result.diagnostics) message += d.code + ": " + d.message + "\n";
    throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    Check({result.diagnostics}); Require(result.value.has_value(), "missing result"); return std::move(*result.value);
}
// 六个像素分别是黑、蓝、绿、红、蓝绿、蓝红。预期按字面原生属性书写。
Bytes Pattern(bool top = true, bool alpha = false, unsigned width = 3, unsigned height = 2) {
    Bytes bytes(18); bytes[2] = std::byte{2};
    bytes[12] = static_cast<std::byte>(width); bytes[14] = static_cast<std::byte>(height);
    bytes[16] = alpha ? std::byte{32} : std::byte{24};
    bytes[17] = static_cast<std::byte>((top ? 32 : 0) | (alpha ? 8 : 0));
    const std::array<std::array<unsigned, 3>, 6> pixels{{{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,0},{1,0,1}}};
    for (unsigned row = 0; row < height; ++row) for (unsigned x = 0; x < width; ++x) {
        const auto y = top ? row : height - 1 - row;
        const auto& pixel = pixels[(y * width + x) % pixels.size()];
        for (const auto c : pixel) bytes.push_back(static_cast<std::byte>(c));
        if (alpha) bytes.push_back(std::byte{}); // alpha零不清掉通行像素。
    }
    return bytes;
}
TerrainHeader Header() { TerrainHeader h; h.width = h.height = 5; return h; }
DoodadPlacement Placement(float radians = 0, float x = 128, float y = 128) {
    DoodadPlacement p; p.rawcode = "Doo0"; p.position = {x, y, 0}; p.scale = {1,1,1}; p.rotation = radians; return p;
}
std::vector<std::uint8_t> Expected(unsigned x, unsigned y, unsigned width, std::span<const std::uint8_t> pixels) {
    std::vector<std::uint8_t> result(16 * 16, 0x40);
    for (std::size_t i = 0; i < pixels.size(); ++i) result[(y + i / width) * 16 + x + i % width] |= pixels[i];
    return result;
}
void RotationsOriginsAndChannels() {
    // 原版角度零并非图像恒等矩阵。使用不对称3x2文字矩阵作独立预期。
    const std::array<std::array<std::uint8_t,6>,4> matrices{{
        {0,0x31,4,6,2,0x35}, {2,4,0,0x35,6,0x31},
        {0x35,2,6,4,0x31,0}, {0x31,6,0x35,0,4,2}}};
    const std::array<float,4> angles{0.0f,1.57079637f,3.14159274f,4.712389f};
    for (bool top : {false,true}) for (bool alpha : {false,true}) {
        const auto bytes = Pattern(top, alpha); const auto before = bytes;
        const auto image = Take(DecodeDoodadFootprintTga(bytes));
        Require(!image.uniform && image.width == 3 && image.height == 2, "decode rectangular pattern");
        for (unsigned q = 0; q < 4; ++q) {
            std::vector<std::uint8_t> actual(256, 0x40);
            Check(AddDoodadFootprint(actual, Header(), Placement(angles[q]), image, 2, -1.0f));
            Require(actual == Expected(q % 2 ? 3 : 4, q % 2 ? 4 : 3, q % 2 ? 3 : 2, matrices[q]),
                "all pixels and untouched cells match the independent rotated matrix");
        }
        Require(bytes == before, "borrowed source remains byte-identical");
    }
    const auto bytes = Pattern(); const auto image = Take(DecodeDoodadFootprintTga(bytes));
    std::vector<std::uint8_t> negative(256,0x40), repeated(256,0x40);
    Check(AddDoodadFootprint(negative, Header(), Placement(-1.57079637f), image, 2, -1.0f));
    Check(AddDoodadFootprint(repeated, Header(), Placement(4.712389f), image, 2, -1.0f));
    Require(negative == repeated, "negative radians normalize before native quantization");
    auto scaled = Placement(); scaled.scale = {-8,0,100};
    std::vector<std::uint8_t> actual(256,0x40);
    Check(AddDoodadFootprint(actual, Header(), scaled, image, 2, -1.0f));
    Require(actual == Expected(4,3,2,matrices[0]), "native footprint is not resized by model scale");
}
void FixedRotationUsesEarlierSnapDimensions() {
    // 4x2使两轴half-grid标志不同，能抓到用fixedRot重新吸附XY的错误。
    const auto bytes = Pattern(true,false,4,2); const auto image = Take(DecodeDoodadFootprintTga(bytes));
    std::vector<std::uint8_t> actual(256,0x40);
    Check(AddDoodadFootprint(actual, Header(), Placement(), image, 2, 90.0f));
    constexpr std::array<std::uint8_t,8> expected{0x31,2,4,0,4,0,0x35,6};
    Require(actual == Expected(3,3,4,expected), "fixedRot rotates pixels after the original XY snap");
    std::vector<std::uint8_t> forcedZero(256,0x40), unconstrained(256,0x40);
    Check(AddDoodadFootprint(forcedZero, Header(), Placement(), image, 2, 0.0f));
    Check(AddDoodadFootprint(unconstrained, Header(), Placement(), image, 2, -1.0f));
    Require(forcedZero == unconstrained, "exact zero fixedRot is valid");
}
void ClippingOverlapAndFailureAtomicity() {
    const auto bytes = Pattern(); const auto image = Take(DecodeDoodadFootprintTga(bytes));
    std::vector<std::uint8_t> edge(256,0x40);
    // XY=-64吸附至-32，起点=(-1,-2)，只留下第三行第二列F。
    Check(AddDoodadFootprint(edge, Header(), Placement(0,-64,-64), image, 2, -1.0f));
    auto expected = std::vector<std::uint8_t>(256,0x40); expected[0] |= 0x35;
    Require(edge == expected, "negative crop retains original source pixel and truncation convention");
    std::vector<std::uint8_t> repeated(256,0x40);
    for (unsigned i=0; i<40; ++i) Check(AddDoodadFootprint(repeated, Header(), Placement(), image, 2, -1.0f));
    constexpr std::array<std::uint8_t,6> matrix{0,0x31,4,6,2,0x35};
    Require(repeated == Expected(4,3,2,matrix), "overlapping instances accumulate existence, including beyond native saturation");
    const auto reject = [&](DoodadPlacement p, std::optional<float> fixed, const char* code) {
        const auto original = repeated;
        const auto result = AddDoodadFootprint(repeated, Header(), p, image, 2, fixed);
        Require(!result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& d){return d.code == code;}), "failure reports its precise cause");
        Require(repeated == original, "refusal leaves every output cell unchanged");
    };
    reject(Placement(), std::nullopt, "TERRAIN_DOODAD_FIXED_ROTATION");
    reject(Placement(), std::numeric_limits<float>::infinity(), "TERRAIN_DOODAD_FIXED_ROTATION");
    reject(Placement(0.785398185f), -1.0f, "TERRAIN_DOODAD_ANGLE_AMBIGUOUS");
    reject(Placement(), 45.0f, "TERRAIN_DOODAD_ANGLE_AMBIGUOUS");
    reject(Placement(0,128,std::nextafter(128.0f,0.0f)), -1.0f, "TERRAIN_UNIFORM_SNAP_AMBIGUOUS");
    reject(Placement(std::numeric_limits<float>::quiet_NaN()), -1.0f, "TERRAIN_UNIFORM_PLACEMENT_DOMAIN");
    // 临界点两侧有明确间隔后按不同方向生成，不能把所有相邻角度都拒绝。
    std::vector<std::uint8_t> low(256,0x40), high(256,0x40);
    Check(AddDoodadFootprint(low, Header(), Placement(0.785f), image, 2, -1.0f));
    Check(AddDoodadFootprint(high, Header(), Placement(0.786f), image, 2, -1.0f));
    Require(low != high, "angles on opposite sides of the boundary select different matrices");
}
void WalkableSuppressionSharesProjectionWithoutRgbBits() {
    constexpr auto contribution = TerrainNativeFootprintContribution::WalkableTerrainSuppression;
    // 与RGB投影测试分别书写字面预期；黑像素不抑制，所有其它颜色都只贡献40。
    constexpr std::array<std::array<std::uint8_t,6>,4> matrices{{
        {0,0x40,0x40,0x40,0x40,0x40}, {0x40,0x40,0,0x40,0x40,0x40},
        {0x40,0x40,0x40,0x40,0x40,0}, {0x40,0x40,0x40,0,0x40,0x40}}};
    constexpr std::array<float,4> angles{0,1.57079637f,3.14159274f,4.712389f};
    for (bool top : {false,true}) for (bool alpha : {false,true}) {
        const auto bytes = Pattern(top,alpha); const auto original = bytes;
        const auto image = Take(DecodeDoodadFootprintTga(bytes));
        for (unsigned q=0; q<4; ++q) {
            std::vector<std::uint8_t> actual(256,2), expected(256,2);
            const auto x = q%2 ? 3u : 4u, y = q%2 ? 4u : 3u, width = q%2 ? 3u : 2u;
            for (std::size_t i=0; i<6; ++i) expected[(y+i/width)*16+x+i%width] |= matrices[q][i];
            Check(AddDoodadFootprint(actual,Header(),Placement(angles[q]),image,2,-1.0f,contribution));
            Require(actual==expected,"walkable projection preserves other static attributes and writes only literal control40");
            Check(AddDoodadFootprint(actual,Header(),Placement(angles[q]),image,2,-1.0f,contribution));
            Require(actual==expected,"duplicate bridges do not erase or multiply the static contribution");
        }
        std::vector<std::uint8_t> cropped(256,0), expected(256,0); expected[0]=0x40;
        Check(AddDoodadFootprint(cropped,Header(),Placement(0,-64,-64),image,2,-1.0f,contribution));
        Require(cropped==expected,"walkable clipping retains the original nonzero source pixel");
        const auto before=cropped;
        Require(!AddDoodadFootprint(cropped,Header(),Placement(),image,2,-1.0f,
            static_cast<TerrainNativeFootprintContribution>(255)).Succeeded(),"unknown contribution is rejected");
        Require(cropped==before && bytes==original,"invalid contribution is atomic and borrowed pixels remain unchanged");
    }
    auto uniform=Pattern(true,true,2,2);
    for (unsigned i=0;i<4;++i) {
        uniform[18+i*4]=std::byte{1}; uniform[19+i*4]=uniform[20+i*4]=uniform[21+i*4]=std::byte{};
    }
    const auto colored=Take(DecodeDoodadFootprintTga(uniform));
    std::vector<std::uint8_t> actual(256,0), expected(256,0);
    for (unsigned y=4;y<6;++y) for (unsigned x=4;x<6;++x) expected[y*16+x]=0x40;
    Check(AddDoodadFootprint(actual,Header(),Placement(0.785398185f),colored,2,0.0f,contribution));
    Require(actual==expected,"complete uniform square uses the common rectangle and nonzero RGB with alpha0 suppresses");
    for (unsigned i=0;i<4;++i) { uniform[18+i*4]=std::byte{}; uniform[21+i*4]=std::byte{255}; }
    std::fill(actual.begin(),actual.end(),0);
    Check(AddDoodadFootprint(actual,Header(),Placement(),Take(DecodeDoodadFootprintTga(uniform)),2,0.0f,contribution));
    Require(std::all_of(actual.begin(),actual.end(),[](auto value){return value==0;}),"uniform black with opaque alpha contributes no suppression");
}
void AlphaAffectsUniformityAndOldDomainRemainsValid() {
    Bytes bytes(18+2*2*4,std::byte{1});
    std::fill(bytes.begin(),bytes.begin()+18,std::byte{});
    bytes[2]=std::byte{2}; bytes[12]=bytes[14]=std::byte{2}; bytes[16]=std::byte{32}; bytes[17]=std::byte{8};
    auto image=Take(DecodeDoodadFootprintTga(bytes)); Require(image.uniform,"same BGRA is uniform");
    std::vector<std::uint8_t> actual(256,0x40), old(256,0x40);
    Check(AddDoodadFootprint(actual,Header(),Placement(0.785398185f),image,2,std::nullopt));
    Check(AddUniformDoodadFootprint(old,Header(),Placement(0.785398185f),{2,0x37},2));
    Require(actual==old,"old uniform angle-independent domain and output remain intact");
    bytes[21]=std::byte{};
    image=Take(DecodeDoodadFootprintTga(bytes)); Require(!image.uniform,"alpha change triggers native angular snapping");
    const auto before=actual;
    Require(!AddDoodadFootprint(actual,Header(),Placement(0.785398185f),image,2,-1.0f).Succeeded(),
        "equal RGB with unequal alpha cannot bypass angular ambiguity");
    Require(actual==before,"alpha-sensitive rejection is atomic");
    auto truncated=bytes; truncated.pop_back(); Require(!DecodeDoodadFootprintTga(truncated).Succeeded(),"truncated image refuses");
    auto right=bytes; right[17]|=std::byte{16}; Require(!DecodeDoodadFootprintTga(right).Succeeded(),"unproved right origin refuses");
    auto rle=bytes; rle[2]=std::byte{10}; Require(!DecodeDoodadFootprintTga(rle).Succeeded(),"unproved RLE refuses");
    auto large=bytes; large[13]=std::byte{1}; Require(!DecodeDoodadFootprintTga(large).Succeeded(),"width over budget refuses");
}
} // namespace
int main() {
    try {
        RotationsOriginsAndChannels(); FixedRotationUsesEarlierSnapDimensions();
        WalkableSuppressionSharesProjectionWithoutRgbBits();
        ClippingOverlapAndFailureAtomicity(); AlphaAffectsUniformityAndOldDomainRemainsValid();
        std::cout << "Native patterned doodad footprints passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
