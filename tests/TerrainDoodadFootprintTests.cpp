#include "War3MapCore/MapData/TerrainDoodadFootprint.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
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
Bytes RawRle(const Bytes& source) {
    Bytes bytes(source.begin(), source.begin() + 18); bytes[2] = std::byte{10};
    const auto stride = std::to_integer<unsigned>(source[16]) / 8;
    std::size_t at = 18;
    while (at < source.size()) {
        const auto pixels = std::min<std::size_t>(128, (source.size() - at) / stride);
        bytes.push_back(static_cast<std::byte>(pixels - 1));
        bytes.insert(bytes.end(), source.begin() + at, source.begin() + at + pixels * stride);
        at += pixels * stride;
    }
    return bytes;
}
void StandardFooter(Bytes& bytes) {
    constexpr std::array<unsigned char, 26> footer{0,0,0,0,0,0,0,0,
        'T','R','U','E','V','I','S','I','O','N','-','X','F','I','L','E','.',0};
    for (const auto byte : footer) bytes.push_back(static_cast<std::byte>(byte));
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
        for (const std::uint8_t grid : {std::uint8_t{2}, std::uint8_t{4}}) for (unsigned q=0; q<4; ++q) {
            std::vector<std::uint8_t> actual(256,2), expected(256,2);
            // 同一非对称3x2图在两种格宽分别有独立起点，不能只验证控制位不验证网格。
            const auto x = grid == 2 ? (q%2 ? 3u : 4u) : (q%2 ? 2u : 3u);
            const auto y = grid == 2 ? (q%2 ? 4u : 3u) : (q%2 ? 3u : 2u);
            const auto width = q%2 ? 3u : 2u;
            for (std::size_t i=0; i<6; ++i) expected[(y+i/width)*16+x+i%width] |= matrices[q][i];
            Check(AddDoodadFootprint(actual,Header(),Placement(angles[q]),image,grid,-1.0f,contribution));
            Require(actual==expected,"walkable projection preserves other static attributes and writes only literal control40");
            Check(AddDoodadFootprint(actual,Header(),Placement(angles[q]),image,grid,-1.0f,contribution));
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
    auto rle=RawRle(bytes); Require(!DecodeDoodadFootprintTga(rle).Succeeded(),"legacy borrowed entry still refuses RLE");
    auto large=bytes; large[13]=std::byte{1}; Require(!DecodeDoodadFootprintTga(large).Succeeded(),"width over budget refuses");
}
void RleUsesOwnedPixelsAndPreservesLiteralProjection() {
    constexpr std::array<std::array<std::uint8_t,6>,4> matrices{{
        {0,0x31,4,6,2,0x35}, {2,4,0,0x35,6,0x31},
        {0x35,2,6,4,0x31,0}, {0x31,6,0x35,0,4,2}}};
    constexpr std::array<float,4> angles{0,1.57079637f,3.14159274f,4.712389f};
    for (const bool top : {false,true}) for (const bool alpha : {false,true}) {
        const auto raw = Pattern(top,alpha); auto encoded = RawRle(raw);
        if (top) StandardFooter(encoded);
        const auto original = encoded;
        auto image = Take(DecodeDoodadFootprintTgaBudgeted(encoded, raw.size() - 18));
        const auto* allocation = image.decodedPixels.get();
        Require(allocation != nullptr && image.decodedBytes == raw.size()-18
            && image.footprint.pixels.data() == allocation, "RLE owns its exact decoded payload");
        auto moved = std::move(image);
        Require(moved.decodedPixels.get() == allocation && moved.footprint.pixels.data() == allocation,
            "moving the result preserves the pixel span and owner");
        Require(std::equal(moved.footprint.pixels.begin(), moved.footprint.pixels.end(), raw.begin()+18, raw.end()),
            "24-bit BGR and 32-bit BGRA unpack unchanged in file order");
        for (unsigned q=0; q<4; ++q) {
            std::vector<std::uint8_t> actual(256,0x40);
            Check(AddDoodadFootprint(actual,Header(),Placement(angles[q]),moved.footprint,2,-1.0f));
            Require(actual == Expected(q%2?3:4,q%2?4:3,q%2?3:2,matrices[q]),
                "cross-row raw RLE preserves independent rectangular rotation and origin expectations");
        }
        Require(encoded == original, "RLE decoding and projection do not change the frozen source");
        std::fill(encoded.begin(),encoded.end(),std::byte{});
        Require(std::equal(moved.footprint.pixels.begin(), moved.footprint.pixels.end(), raw.begin()+18, raw.end()),
            "RLE pixels do not borrow the compressed input");
        const auto uncompressed = Take(DecodeDoodadFootprintTgaBudgeted(raw,0));
        Require(!uncompressed.decodedPixels && uncompressed.decodedBytes == 0
            && uncompressed.footprint.pixels.data() == raw.data()+18,
            "type2 needs no decoded budget and remains zero-copy");
    }
}
void RlePacketBoundsUniformityAndBudget() {
    // 4x2：原样黑/蓝、重复四个绿（跨行）、原样红/蓝红。
    auto encoded = Pattern(true,true,4,2); encoded.resize(18); encoded[2]=std::byte{10};
    const std::array<unsigned,23> packets{1, 0,0,0,255, 1,0,0,0,
        0x83, 0,1,0,0, 1, 0,0,1,0, 1,0,1,0};
    for (const auto byte : packets) encoded.push_back(static_cast<std::byte>(byte));
    auto image=Take(DecodeDoodadFootprintTgaBudgeted(encoded,32));
    const std::array<unsigned,32> expected{0,0,0,255, 1,0,0,0, 0,1,0,0, 0,1,0,0,
        0,1,0,0, 0,1,0,0, 0,0,1,0, 1,0,1,0};
    Require(std::equal(expected.begin(),expected.end(),image.footprint.pixels.begin(),
        [](unsigned left,std::byte right){return left==std::to_integer<unsigned>(right);}),
        "mixed raw and repeated packets decode across row boundaries");
    const auto denied=DecodeDoodadFootprintTgaBudgeted(encoded,31);
    Require(!denied.Succeeded() && !denied.value && denied.diagnostics.front().code=="TERRAIN_DOODAD_TGA_DECODE_BUDGET",
        "one-byte-short decoded budget returns no partial image");
    auto uniform=Pattern(true,true,2,2); uniform.resize(18); uniform[2]=std::byte{10};
    uniform.insert(uniform.end(),{std::byte{0x83},std::byte{1},std::byte{},std::byte{},std::byte{255}});
    image=Take(DecodeDoodadFootprintTgaBudgeted(uniform,16));
    Require(image.footprint.uniform && image.footprint.uniformAttributes==4,"repeated BGRA is uniform");
    auto alpha=Pattern(true,true,2,2);
    for (std::size_t at=18;at<alpha.size();at+=4) {
        alpha[at]=std::byte{1}; alpha[at+1]=alpha[at+2]=alpha[at+3]=std::byte{};
    }
    alpha[21]=std::byte{255};
    Require(!Take(DecodeDoodadFootprintTgaBudgeted(RawRle(alpha),16)).footprint.uniform,
        "equal RLE RGB with unequal alpha never becomes uniform");
    for (const unsigned count : {1u,128u}) for (const bool repeat : {false,true}) {
        auto raw=Pattern(true,false,count,1), packet=RawRle(raw);
        if (repeat) {
            packet.resize(18); packet.push_back(static_cast<std::byte>(0x80u | (count-1)));
            packet.insert(packet.end(),{std::byte{1},std::byte{},std::byte{}});
        }
        const auto decoded=Take(DecodeDoodadFootprintTgaBudgeted(packet,count*3));
        Require(decoded.footprint.pixels.size()==count*3 && decoded.footprint.uniform==(repeat || count==1),
            count==1 ? "single-pixel packet is accepted" : "128-pixel packet is accepted");
    }
    const auto reject=[&](Bytes bytes,const char* code) {
        const auto before=bytes;
        const auto result=DecodeDoodadFootprintTgaBudgeted(bytes,1024);
        Require(!result.Succeeded() && !result.value && result.diagnostics.front().code==code,
            "invalid RLE reports the precise refusal and no image");
        Require(bytes==before,"invalid RLE leaves source bytes unchanged");
    };
    auto bad=uniform; bad.resize(18); reject(bad,"TERRAIN_UNIFORM_TGA_TRUNCATED");
    bad=uniform; bad.pop_back(); reject(bad,"TERRAIN_UNIFORM_TGA_TRUNCATED");
    bad=RawRle(Pattern()); bad.pop_back(); reject(bad,"TERRAIN_UNIFORM_TGA_TRUNCATED");
    bad=uniform; bad[18]=std::byte{0x84}; reject(bad,"TERRAIN_DOODAD_TGA_RLE_OVERRUN");
    bad=uniform; bad[18]=std::byte{4}; reject(bad,"TERRAIN_DOODAD_TGA_RLE_OVERRUN");
    bad=uniform; bad.push_back(std::byte{}); reject(bad,"TERRAIN_UNIFORM_TGA_TRAILING");
    bad=uniform; StandardFooter(bad); bad[bad.size()-26]=std::byte{1}; reject(bad,"TERRAIN_UNIFORM_TGA_TRAILING");
    bad=uniform; StandardFooter(bad); bad.pop_back(); reject(bad,"TERRAIN_UNIFORM_TGA_TRAILING");
    for (const auto field : {0u,1u,7u,8u,10u}) {
        bad=uniform; bad[field]=std::byte{1}; reject(bad,"TERRAIN_UNIFORM_TGA_FORMAT");
    }
    for (const auto bit : {0x10u,0x40u,0x80u}) {
        bad=uniform; bad[17]|=static_cast<std::byte>(bit); reject(bad,"TERRAIN_UNIFORM_TGA_FORMAT");
    }
    Require(!DecodeDoodadFootprintTga(uniform).Succeeded() && !DecodeUniformDoodadFootprintTga(uniform).Succeeded(),
        "legacy and special-compatible decoders keep the old type2-only domain");
}
} // namespace
int main() {
    try {
        RotationsOriginsAndChannels(); FixedRotationUsesEarlierSnapDimensions();
        WalkableSuppressionSharesProjectionWithoutRgbBits();
        ClippingOverlapAndFailureAtomicity(); AlphaAffectsUniformityAndOldDomainRemainsValid();
        RleUsesOwnedPixelsAndPreservesLiteralProjection(); RlePacketBoundsUniformityAndBudget();
        std::cout << "Native patterned doodad footprints passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
