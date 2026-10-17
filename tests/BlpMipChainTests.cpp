#include "War3MapCore/Image/BlpMipChain.h"
#include "War3MapCore/Render/TerrainTextureAtlas.h"
#include "fixtures/TinyJpegFixture.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::Image;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename T>
void Rejected(const Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const Diagnostic& diagnostic) { return diagnostic.code == code; }), "没有按指定错误拒绝BLP链");
}

void Put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned part = 0; part < 4; ++part)
        bytes[offset + part] = static_cast<std::byte>((value >> (part * 8)) & 255);
}

// 原创小纹理：top全红、mip1全绿、mip2全蓝，低级故意不是top下采样。
std::vector<std::byte> Paletted(std::uint32_t width, std::uint32_t height, unsigned maximumLevels = 16) {
    std::vector<std::byte> bytes(1180);
    bytes[0] = std::byte{'B'}; bytes[1] = std::byte{'L'}; bytes[2] = std::byte{'P'}; bytes[3] = std::byte{'1'};
    Put32(bytes, 4, 1); Put32(bytes, 12, width); Put32(bytes, 16, height);
    Put32(bytes, 156, 0x00ff0000); Put32(bytes, 160, 0x0000ff00); Put32(bytes, 164, 0x000000ff);
    for (unsigned level = 0; level < maximumLevels; ++level) {
        const auto w = std::max(1u, width >> level), h = std::max(1u, height >> level);
        const auto pixels = w * h;
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(bytes.size()));
        Put32(bytes, 92 + level * 4, pixels);
        bytes.insert(bytes.end(), pixels, static_cast<std::byte>(level % 3));
        if (w == 1 && h == 1) break;
    }
    return bytes;
}

std::vector<std::byte> Jpeg(std::uint32_t width, std::uint32_t height,
    const std::vector<std::vector<unsigned char>>& mips, std::size_t shared = 71) {
    std::vector<std::byte> bytes(160 + shared);
    bytes[0] = std::byte{'B'}; bytes[1] = std::byte{'L'}; bytes[2] = std::byte{'P'}; bytes[3] = std::byte{'1'};
    Put32(bytes, 12, width); Put32(bytes, 16, height); Put32(bytes, 156, static_cast<std::uint32_t>(shared));
    for (std::size_t at = 0; at < shared; ++at) bytes[160 + at] = static_cast<std::byte>(mips.front()[at]);
    for (std::size_t level = 0; level < mips.size(); ++level) {
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(bytes.size()));
        Put32(bytes, 92 + level * 4, static_cast<std::uint32_t>(mips[level].size() - shared));
        for (std::size_t at = shared; at < mips[level].size(); ++at) bytes.push_back(static_cast<std::byte>(mips[level][at]));
    }
    return bytes;
}

Resources::ResolvedResource Resource(std::vector<std::byte> bytes) {
    return {{"TEST\\ORIGINAL.BLP", Resources::ResourceLayer::MapImport, Resources::ResourcePriority::MapArchive,
        "original-fixture", "Test\\Original.blp"}, std::move(bytes)};
}

void Solid(const DecodedImage& image, unsigned channel) {
    Require(image.Valid(), "保留层尺寸/字节无效");
    for (std::size_t at = 0; at < image.rgba.size(); at += 4) {
        for (unsigned component = 0; component < 3; ++component)
            Require(image.rgba[at + component] == static_cast<std::byte>(component == channel ? 255 : 0), "原始mip像素被重建或交换");
        Require(image.rgba[at + 3] == std::byte{255}, "不透明调色板alpha改变");
    }
}

void TestNonAtlasRectanglesAndSourcePixels() {
    for (const auto dimensions : {BlpMipChainDimensions{3, 5}, BlpMipChainDimensions{1, 8}, BlpMipChainDimensions{8, 1}}) {
        const auto bytes = Paletted(dimensions.width, dimensions.height);
        const auto original = bytes;
        const auto header = ReadBlpMipChainDimensions(bytes);
        Require(header.Succeeded() && header.value->width == dimensions.width && header.value->height == dimensions.height,
            "通用前置尺寸门拒绝矩形");
        const auto decoded = DecodeBlpMipChain(bytes);
        Require(decoded.Succeeded() && decoded.value->validatedMipCount == decoded.value->additionalMips.size() + 1
            && decoded.value->stop.reason == BlpMipChainStopReason::Complete, "小尺寸/矩形原链未完整保留");
        const auto& chain = *decoded.value;
        Solid(chain.image, 0);
        for (std::size_t level = 0; level < chain.additionalMips.size(); ++level) {
            const auto& mip = chain.additionalMips[level];
            Require(mip.width == std::max(1u, dimensions.width >> (level + 1))
                && mip.height == std::max(1u, dimensions.height >> (level + 1)), "矩形mip尺寸改变");
            Solid(mip, static_cast<unsigned>((level + 1) % 3));
        }
        Require(bytes == original, "共享解码修改源数据");
        const auto direct = DecodeBlp(bytes);
        Require(direct.Succeeded() && direct.value->rgba == chain.image.rgba, "共享top与唯一Core像素解码器不同");
    }
}

void TestStopFactsAndBadMip0() {
    auto bytes = Paletted(3, 5, 1);
    auto decoded = DecodeBlpMipChain(bytes);
    Require(decoded.Succeeded() && decoded.value->validatedMipCount == 1
        && decoded.value->stop.reason == BlpMipChainStopReason::UndeclaredMip
        && decoded.value->stop.level == 1 && !decoded.value->stop.hasLaterDeclarations, "明确无低级的尾部事实丢失");
    bytes = Paletted(3, 5);
    Put32(bytes, 32, 0); Put32(bytes, 96, 0);
    decoded = DecodeBlpMipChain(bytes);
    Require(decoded.Succeeded() && decoded.value->validatedMipCount == 1
        && decoded.value->stop.reason == BlpMipChainStopReason::UndeclaredMip
        && decoded.value->stop.hasLaterDeclarations, "中间空洞误认为正常缺尾");
    bytes = Paletted(3, 5);
    Put32(bytes, 32, static_cast<std::uint32_t>(bytes.size() + 1));
    decoded = DecodeBlpMipChain(bytes);
    Require(decoded.Succeeded() && decoded.value->validatedMipCount == 1
        && decoded.value->stop.reason == BlpMipChainStopReason::InvalidMipRange && decoded.value->stop.level == 1,
        "坏后级未保留有效前缀/截止事实");
    Put32(bytes, 28, 0);
    Rejected(DecodeBlpMipChain(bytes), "BLP_MIP_MIP");
}

void TestExactBudgets() {
    const auto bytes = Paletted(3, 5); // 15 + 2 + 1 pixels = 72 RGBA字节。
    BlpMipChainLimits limits;
    limits.maxInputBytes = bytes.size(); limits.maxDimension = 5; limits.maxRgbaBytes = 60;
    limits.maxRetainedRgbaBytes = 72; limits.maxTotalMipPixels = 18;
    auto decoded = DecodeBlpMipChain(bytes, limits);
    Require(decoded.Succeeded() && decoded.value->RgbaByteCount() == 72 && decoded.value->validatedMipCount == 3,
        "完整链的精确预算被拒绝或计费遗漏");
    --limits.maxInputBytes; Rejected(DecodeBlpMipChain(bytes, limits), "BLP_MIP_INPUT_BUDGET");
    limits.maxInputBytes = bytes.size(); --limits.maxDimension;
    Rejected(DecodeBlpMipChain(bytes, limits), "BLP_MIP_DIMENSION");
    limits.maxDimension = 5; --limits.maxRgbaBytes;
    Rejected(DecodeBlpMipChain(bytes, limits), "BLP_MIP_RGBA_BUDGET");
    limits.maxRgbaBytes = 60; limits.maxRetainedRgbaBytes = 59;
    Rejected(DecodeBlpMipChain(bytes, limits), "BLP_MIP_RETAINED_BUDGET");
    limits.maxRetainedRgbaBytes = 71;
    decoded = DecodeBlpMipChain(bytes, limits);
    Require(decoded.Succeeded() && decoded.value->RgbaByteCount() == 68 && decoded.value->validatedMipCount == 2
        && decoded.value->stop.reason == BlpMipChainStopReason::RetainedRgbaBudget && decoded.value->stop.level == 2,
        "后级保留预算没有准确截止");
    limits.maxRetainedRgbaBytes = 72; limits.maxTotalMipPixels = 17;
    decoded = DecodeBlpMipChain(bytes, limits);
    Require(decoded.Succeeded() && decoded.value->validatedMipCount == 2
        && decoded.value->stop.reason == BlpMipChainStopReason::TotalPixelBudget, "总像素预算事实丢失");
    limits.maxTotalMipPixels = 14;
    Rejected(DecodeBlpMipChain(bytes, limits), "BLP_MIP_MIP_BUDGET");
    const auto narrow = Paletted(1, 8);
    Require(narrow.size() == 1195, "工作集原创fixture大小改变");
    limits = {}; limits.maxWorkingBytes = 67972; // 原合同的input/wrapper/retained/65536/当前RGBA峰值。
    decoded = DecodeBlpMipChain(narrow, limits);
    Require(decoded.Succeeded() && decoded.value->validatedMipCount == 4, "精确工作集峰值被拒绝");
    --limits.maxWorkingBytes;
    decoded = DecodeBlpMipChain(narrow, limits);
    Require(decoded.Succeeded() && decoded.value->validatedMipCount == 3 && decoded.value->stop.level == 3
        && decoded.value->stop.reason == BlpMipChainStopReason::WorkingBudget
        && !decoded.value->stop.diagnostics.empty(), "后级工作集预算应保留前缀和原因");
    limits.maxWorkingBytes = 67950;
    Rejected(DecodeBlpMipChain(narrow, limits), "BLP_MIP_WORKING_BUDGET");
    limits = {}; ++limits.maxDimension;
    Rejected(DecodeBlpMipChain(bytes, limits), "BLP_MIP_LIMITS");
}

void TestJpegFailureFactsAndAtlasAdapter() {
    const auto top = TestFixtures::MakeTinyGrayJpeg(8, 8), lower = TestFixtures::MakeTinyGrayJpeg(4, 4);
    auto good = Jpeg(8, 8, {top.bytes, lower.bytes});
    auto decoded = DecodeBlpMipChain(good);
    Require(decoded.Succeeded() && decoded.value->validatedMipCount == 2, "共享JPEG头的连续链未保留");
    BlpMipChainLimits jpegLimits; jpegLimits.maxJpegScans = 1;
    const auto scanLimited = DecodeBlpMipChain(good, jpegLimits);
    Require(scanLimited.Succeeded() && scanLimited.value->validatedMipCount == 1
        && scanLimited.value->stop.reason == BlpMipChainStopReason::JpegBudget
        && !scanLimited.value->stop.diagnostics.empty(), "累计JPEG扫描预算截止事实丢失");
    jpegLimits.maxJpegScans = 0;
    Rejected(DecodeBlpMipChain(good, jpegLimits), "BLP_MIP_JPEG_BUDGET");
    auto broken = lower.bytes;
    broken.erase(broken.begin() + static_cast<std::ptrdiff_t>(lower.entropyOffset));
    auto badLower = Jpeg(8, 8, {top.bytes, broken});
    decoded = DecodeBlpMipChain(badLower);
    Require(decoded.Succeeded() && decoded.value->validatedMipCount == 1
        && decoded.value->stop.reason == BlpMipChainStopReason::DecodeFailure && decoded.value->stop.level == 1
        && !decoded.value->stop.diagnostics.empty() && decoded.diagnostics.empty(), "坏后级像素失败与成功警告混淆");
    Rejected(DecodeBlpMipChain(Jpeg(8, 8, {lower.bytes})), "BLP_MIP_JPEG");
    const auto atlasBad = Render::DecodeTerrainTextureAtlas(Resource(badLower));
    Require(atlasBad.Succeeded() && (*atlasBad.value)->validatedMipCount == 1 && atlasBad.diagnostics.empty(),
        "Atlas坏后级截断合同改变");
    auto invalidLayout = Paletted(3, 5); Put32(invalidLayout, 8, 3);
    Render::TerrainAtlasLimits atlasLimits; atlasLimits.maxRgbaBytes = 0;
    Rejected(Render::DecodeTerrainTextureAtlas(Resource(invalidLayout), atlasLimits), "TERRAIN_ATLAS_LAYOUT");
    atlasLimits.maxDimension = 4;
    Rejected(Render::DecodeTerrainTextureAtlas(Resource(invalidLayout), atlasLimits), "TERRAIN_ATLAS_DIMENSION");
    const auto atlasBytes = Paletted(8, 8);
    const auto shared = DecodeBlpMipChain(atlasBytes);
    const auto atlas = Render::DecodeTerrainTextureAtlas(Resource(atlasBytes));
    Require(shared.Succeeded() && atlas.Succeeded() && (*atlas.value)->image.rgba == shared.value->image.rgba
        && (*atlas.value)->additionalMips.size() == shared.value->additionalMips.size(), "Atlas适配改变top或层数");
    for (std::size_t level = 0; level < shared.value->additionalMips.size(); ++level)
        Require((*atlas.value)->additionalMips[level].rgba == shared.value->additionalMips[level].rgba, "Atlas适配改变原低级像素");
    atlasLimits = {}; atlasLimits.maxRetainedRgbaBytes = 320;
    const auto limited = Render::DecodeTerrainTextureAtlas(Resource(atlasBytes), atlasLimits);
    Require(limited.Succeeded() && (*limited.value)->validatedMipCount == 2 && limited.diagnostics.empty(),
        "Atlas后级预算截止增加诊断或改变有效前缀");
    auto padded = top.bytes; padded.push_back(0xff);
    decoded = DecodeBlpMipChain(Jpeg(8, 8, {padded}));
    const auto warning = Render::DecodeTerrainTextureAtlas(Resource(Jpeg(8, 8, {padded})));
    Require(decoded.Succeeded() && decoded.diagnostics.size() == 1
        && decoded.diagnostics[0].code == "BLP_MIP_JPEG_TRAILING_DATA"
        && warning.Succeeded() && warning.diagnostics.size() == 1
        && warning.diagnostics[0].subsystem == "Render.TerrainAtlas"
        && warning.diagnostics[0].code == "TERRAIN_ATLAS_JPEG_TRAILING_DATA"
        && warning.diagnostics[0].message.starts_with("TEST\\ORIGINAL.BLP：mip 0"), "Atlas既有尾数据诊断映射改变");
}

} // namespace

int main() {
    try {
        TestNonAtlasRectanglesAndSourcePixels();
        TestStopFactsAndBadMip0();
        TestExactBudgets();
        TestJpegFailureFactsAndAtlasAdapter();
        std::cout << "BlpMipChain tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
