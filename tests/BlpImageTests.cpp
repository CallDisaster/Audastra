#include "War3MapCore/Image/BlpImage.h"

#include "fixtures/JpegFixtures.h"
#include "fixtures/TinyJpegFixture.h"
#include "fixtures/JpegCoefficientFixture.h"
#include "../src/Core/Image/JpegDecoder.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <iostream>
#include <latch>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using War3Map::Image::DecodeBlp;
using War3Map::Image::DecodedImage;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
void RequireError(const War3Map::Result<T>& result, const std::string& code) {
    Require(!result.Succeeded(), "应拒绝输入：" + code);
    for (const auto& diagnostic : result.diagnostics) if (diagnostic.code == code) return;
    throw std::runtime_error("缺少预期诊断：" + code);
}

void PutU32(std::vector<std::byte>& out, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
}

// BLP1 JPEG 容器：共享头为空，整段 JPEG 作为 mip0。
std::vector<std::byte> WrapBlp(std::span<const unsigned char> jpeg, std::uint32_t width, std::uint32_t height, std::uint32_t alphaBits) {
    std::vector<std::byte> blp(160);
    const char magic[] = {'B', 'L', 'P', '1'};
    for (int i = 0; i < 4; ++i) blp[i] = static_cast<std::byte>(magic[i]);
    PutU32(blp, 4, 0); // JPEG
    PutU32(blp, 8, alphaBits);
    PutU32(blp, 12, width);
    PutU32(blp, 16, height);
    PutU32(blp, 20, 4);
    PutU32(blp, 24, 0); // 无 mipmap
    PutU32(blp, 28, 160);
    PutU32(blp, 92, static_cast<std::uint32_t>(jpeg.size()));
    PutU32(blp, 156, 0);
    for (const unsigned char b : jpeg) blp.push_back(static_cast<std::byte>(b));
    return blp;
}

DecodedImage Decode(std::span<const unsigned char> jpeg, std::uint32_t width, std::uint32_t height, std::uint32_t alphaBits, const char* name) {
    auto decoded = DecodeBlp(WrapBlp(jpeg, width, height, alphaBits));
    std::string reason;
    for (const auto& diagnostic : decoded.diagnostics) reason += " " + diagnostic.code + ": " + diagnostic.message;
    Require(decoded.Succeeded() && decoded.value.has_value(), std::string(name) + " 解码失败" + reason);
    Require(decoded.value->Valid() && decoded.value->width == width && decoded.value->height == height,
        std::string(name) + " 尺寸不符");
    return std::move(*decoded.value);
}

// 渐进式（多次扫描、逐次逼近精化、EOB 游程）与顺序式编码的系数相同，像素应逐字节一致。
void TestProgressiveMatchesBaseline() {
    const auto rgb = Decode(kRgbBaseline, 37, 21, 0, "rgb baseline");
    Require(Decode(kRgbProgressive, 37, 21, 0, "rgb progressive").rgba == rgb.rgba, "RGB 4:2:0 渐进式与顺序式像素不一致");

    const auto cmyk = Decode(kCmykBaseline, 29, 19, 8, "cmyk baseline");
    Require(Decode(kCmykProgressiveRestart, 29, 19, 8, "cmyk progressive").rgba == cmyk.rgba,
        "四组件渐进式（含重启间隔）与顺序式像素不一致");

    const auto gray = Decode(kGrayBaseline, 23, 17, 0, "gray baseline");
    Require(Decode(kGrayProgressive, 23, 17, 0, "gray progressive").rgba == gray.rgba, "灰度渐进式与顺序式像素不一致");
}

// 顺序式解码的纯色区域与参考解码（Pillow/libjpeg）接近，防止两条路径一起出错。
void TestBaselineSamples() {
    const auto rgb = Decode(kRgbBaseline, 37, 21, 0, "rgb baseline");
    struct Sample { std::uint32_t x, y; int r, g, b; };
    const Sample samples[] = {{4, 4, 195, 43, 38}, {12, 6, 197, 42, 40}, {24, 4, 20, 165, 220}, {33, 7, 37, 155, 225}};
    for (const auto& s : samples) {
        const auto* px = rgb.rgba.data() + (static_cast<std::size_t>(s.y) * rgb.width + s.x) * 4;
        const int got[] = {std::to_integer<int>(px[0]), std::to_integer<int>(px[1]), std::to_integer<int>(px[2])};
        const int want[] = {s.r, s.g, s.b};
        for (int c = 0; c < 3; ++c) {
            Require(std::abs(got[c] - want[c]) <= 8,
                "抽样点 (" + std::to_string(s.x) + "," + std::to_string(s.y) + ") 通道 " + std::to_string(c) +
                " 期望 " + std::to_string(want[c]) + " 实得 " + std::to_string(got[c]));
        }
        Require(std::to_integer<int>(px[3]) == 255, "三组件 JPEG 应不透明");
    }
}

// 截断数据与算术编码等不支持的方式明确拒绝，不生成部分图像。
void TestRejectsUnsupported() {
    std::vector<unsigned char> arithmetic(std::begin(kGrayBaseline), std::end(kGrayBaseline));
    bool patched = false;
    for (std::size_t i = 0; i + 1 < arithmetic.size(); ++i) {
        if (arithmetic[i] == 0xFF && arithmetic[i + 1] == 0xC0) {
            arithmetic[i + 1] = 0xC9; // SOF9：算术编码
            patched = true;
            break;
        }
    }
    Require(patched, "fixture 缺少 SOF0");
    const auto result = DecodeBlp(WrapBlp(arithmetic, 23, 17, 0));
    Require(!result.Succeeded(), "算术编码 JPEG 应拒绝");

    const std::span<const unsigned char> truncated(kRgbProgressive, sizeof(kRgbProgressive) / 2);
    Require(!DecodeBlp(WrapBlp(truncated, 37, 21, 0)).Succeeded(), "截断渐进式 JPEG 必须失败");
}

// 原创单色 8x8/16x8 JPEG：DC category=0 与 AC EOB 各一个零 bit，剩余 bit 全 1。
std::vector<unsigned char> TinyGrayJpeg(bool restart, std::size_t& entropy) {
    auto fixture = War3Map::TestFixtures::MakeTinyGrayJpeg(restart ? 16 : 8, 8, restart);
    entropy = fixture.entropyOffset;
    return std::move(fixture.bytes);
}

void TestStrictEntropyBoundaries() {
    std::size_t entropy = 0;
    const auto jpeg = TinyGrayJpeg(false, entropy);
    Require(DecodeBlp(WrapBlp(jpeg, 8, 8, 0)).Succeeded(), "合法 padding 必须成功");
    auto changed = jpeg;
    changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(entropy));
    Require(!DecodeBlp(WrapBlp(changed, 8, 8, 0)).Succeeded(), "提前 EOI 不能靠补零生成像素");
    changed = jpeg; changed[entropy] = 0x3e;
    Require(!DecodeBlp(WrapBlp(changed, 8, 8, 0)).Succeeded(), "非全1 padding 必须失败");
    changed = jpeg; changed[entropy] = 0;
    Require(DecodeBlp(WrapBlp(changed, 8, 8, 0)).Succeeded(), "经典 BLP 完整 MCU 后的全零 EOI padding 必须兼容");
    RequireError(War3Map::Image::detail::DecodeJpeg(std::as_bytes(std::span(changed))), "entropy-end");
    changed[changed.size() - 1] = 0xda;
    RequireError(DecodeBlp(WrapBlp(changed, 8, 8, 0)), "entropy-end"); // 非 EOI 仍不放宽
    changed = jpeg; changed.insert(changed.begin() + static_cast<std::ptrdiff_t>(entropy + 1), {0xff, 0});
    Require(!DecodeBlp(WrapBlp(changed, 8, 8, 0)).Succeeded(), "不能跳过剩余熵数据搜索 EOI");
    changed = jpeg; changed.insert(changed.end() - 2, 0xff);
    Require(DecodeBlp(WrapBlp(changed, 8, 8, 0)).Succeeded(), "合法 marker FF 填充必须成功");
    for (std::size_t size = 0; size < jpeg.size(); ++size)
        Require(!DecodeBlp(WrapBlp(std::span<const unsigned char>(jpeg).first(size), 8, 8, 0)).Succeeded(),
            "任意截断前缀不能生成有效 JPEG");
    const auto restarted = TinyGrayJpeg(true, entropy);
    Require(DecodeBlp(WrapBlp(restarted, 16, 8, 0)).Succeeded(), "合法 RST0 必须成功");
    changed = restarted; changed[entropy + 2] = 0xd1;
    Require(!DecodeBlp(WrapBlp(changed, 16, 8, 0)).Succeeded(), "错误重启序号必须失败");
    changed = restarted; changed[entropy] = 0x3e;
    Require(!DecodeBlp(WrapBlp(changed, 16, 8, 0)).Succeeded(), "重启前非法 padding 必须失败");
    changed = restarted; changed[entropy] = 0;
    RequireError(DecodeBlp(WrapBlp(changed, 16, 8, 0)), "restart"); // 全零不能用于 RST
    changed = restarted; changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(entropy + 1),
        changed.begin() + static_cast<std::ptrdiff_t>(entropy + 3));
    Require(!DecodeBlp(WrapBlp(changed, 16, 8, 0)).Succeeded(), "缺少 RST 不能跳过数据继续读取");
}

void TestCoefficientBoundsAndSaturation() {
    using War3Map::TestFixtures::JpegCoefficientSpec;
    using War3Map::TestFixtures::MakeCoefficientJpeg;
    const auto decode = [](const JpegCoefficientSpec& spec) {
        return DecodeBlp(WrapBlp(MakeCoefficientJpeg(spec), spec.blocks * 8, 8, 0));
    };
    JpegCoefficientSpec spec;
    for (const auto category : {12u, 31u, 255u}) {
        // category=255 只修改 DHT；fixture 位生成本身始终有界。
        spec.dcCategory = category == 255 ? 0 : category;
        auto jpeg = MakeCoefficientJpeg(spec);
        if (category == 255) {
            for (std::size_t offset = 0; offset + 21 < jpeg.size(); ++offset) {
                if (jpeg[offset] == 0xff && jpeg[offset + 1] == 0xc4) { jpeg[offset + 21] = 255; break; }
            }
        }
        RequireError(DecodeBlp(WrapBlp(jpeg, 8, 8, 0)), "dc-category");
    }
    spec = {}; spec.dcCategory = 11; spec.dcDifference = 2047;
    auto positive = decode(spec);
    Require(positive.Succeeded() && std::to_integer<int>(positive.value->rgba[0]) == 255,
        "DC category=11 上界必须可读且饱和白色");
    spec.dcDifference = -2047;
    auto negative = decode(spec);
    Require(negative.Succeeded() && std::to_integer<int>(negative.value->rgba[0]) == 0,
        "DC category=11 负值必须可读且饱和黑色");
    spec.blocks = 17;
    RequireError(decode(spec), "huffman"); // -34799，禁止 int16 回绕。
    spec.dcDifference = 2047;
    RequireError(decode(spec), "huffman"); // 34799，禁止 int16 回绕。
    spec.blocks = 129; spec.progressive = true; spec.dcAl = 13;
    RequireError(decode(spec), "huffman"); // 旧实现第129块 264063*8192 超出 int32。
    spec = {}; spec.progressive = true; spec.dcCategory = 2; spec.dcDifference = 3; spec.dcAl = 13;
    Require(decode(spec).Succeeded(), "渐进 DC 缩放后 24576 在 int16 范围内");
    spec.blocks = 2;
    RequireError(decode(spec), "huffman"); // 第二块 49152，缩放后检查。
    spec = {}; spec.progressive = true; spec.denseAc = true; spec.acAl = 6;
    RequireError(decode(spec), "huffman"); // 1023*64，AC 同样禁止静默窄化。

    // 原创极端但存储有界的系数：像素(0,0)的 IDCT 浮点结果约149亿，
    // 在 Windows 上先 lround 再 clamp 会超出 long；现在先在 double 域饱和。
    spec = {}; spec.progressive = true; spec.dcCategory = 11; spec.dcDifference = 2047;
    spec.dcAl = 4; spec.denseAc = true; spec.acAl = 5; spec.quant = 65535;
    const auto saturated = decode(spec);
    Require(saturated.Succeeded() && std::to_integer<int>(saturated.value->rgba[0]) == 255,
        "大系数 IDCT 必须在取整前饱和到白色");
}

void TestJpegDimensionsAndWorkingBudget() {
    using namespace War3Map::Image::detail;
    const auto fixture = War3Map::TestFixtures::MakeTinyGrayJpeg(8, 8);
    const auto input = std::as_bytes(std::span(fixture.bytes));
    RequireError(DecodeBlp(WrapBlp(fixture.bytes, 1, 1, 0)), "sof-dimensions");
    JpegDecodeOptions options;
    options.expectedWidth = 8; options.expectedHeight = 8;
    Require(DecodeJpeg(input, options).Succeeded(), "声明尺寸一致必须正常解码");
    options.maxWorkingBytes = 65536 + input.size() + 255;
    RequireError(DecodeJpeg(input, options), "working-budget");
    options.maxWorkingBytes = 65536 + input.size() + 320;
    Require(DecodeJpeg(input, options).Succeeded(), "恰好满足 64组件像素+256RGBA 峰值预算");
    options.callerRetainedBytes = 1;
    RequireError(DecodeJpeg(input, options), "working-budget");
    options.callerRetainedBytes = std::numeric_limits<std::size_t>::max();
    RequireError(DecodeJpeg(input, options), "working-budget");
    options = {}; ++options.maxWorkingBytes;
    RequireError(DecodeJpeg(input, options), "working-budget");

    auto huge = fixture.bytes;
    for (std::size_t offset = 0; offset + 8 < huge.size(); ++offset) {
        if (huge[offset] == 0xff && huge[offset + 1] == 0xc0) {
            huge[offset + 5] = 0x20; huge[offset + 6] = 0; // 8192 行
            huge[offset + 7] = 0x20; huge[offset + 8] = 0; // 8192 列
            break;
        }
    }
    RequireError(DecodeJpeg(std::as_bytes(std::span(huge))), "working-budget");
    RequireError(DecodeBlp(WrapBlp(huge, 1, 1, 0)), "sof-dimensions");
}

// 必须是 main 中首个 DecodeBlp 测试：四个 worker 同时首次进入 IDCT 初始化。
// 使用已有原创非恒定 RGB fixture，避免全零系数掩盖余弦表初始化错误。
void TestConcurrentFirstDecode() {
    constexpr std::size_t workerCount = 4;
    const auto input = WrapBlp(kRgbBaseline, 37, 21, 0);
    std::array<std::vector<std::byte>, workerCount> pixels;
    std::array<std::string, workerCount> errors;
    std::array<std::jthread, workerCount> workers;
    std::latch ready(static_cast<std::ptrdiff_t>(workerCount)), start(1);
    try {
        for (std::size_t index = 0; index < workerCount; ++index) {
            workers[index] = std::jthread([&, index] {
                ready.count_down();
                start.wait();
                try {
                    for (int attempt = 0; attempt < 4; ++attempt) {
                        auto decoded = DecodeBlp(input);
                        Require(decoded.Succeeded() && decoded.value->Valid()
                            && decoded.value->width == 37 && decoded.value->height == 21, "并发解码失败");
                        if (attempt == 0) pixels[index] = std::move(decoded.value->rgba);
                        else Require(decoded.value->rgba == pixels[index], "同线程重复解码像素不一致");
                    }
                } catch (const std::exception& error) { errors[index] = error.what(); }
            });
        }
    } catch (...) {
        // 部分线程创建失败时放行已创建 worker，避免 jthread 析构等待形成死锁。
        start.count_down();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        throw;
    }
    ready.wait();
    start.count_down();
    for (auto& worker : workers) worker.join();
    for (std::size_t index = 0; index < workerCount; ++index) {
        Require(errors[index].empty(), "并发 JPEG worker 失败：" + errors[index]);
        Require(pixels[index] == pixels[0], "并发首次解码像素不一致");
    }
    const auto sample = (4u * 37u + 4u) * 4u;
    Require(std::abs(std::to_integer<int>(pixels[0][sample]) - 195) <= 8
        && std::abs(std::to_integer<int>(pixels[0][sample + 1]) - 43) <= 8
        && std::abs(std::to_integer<int>(pixels[0][sample + 2]) - 38) <= 8, "并发首次解码与原创参考样点不符");
}

} // namespace

int main() {
    try {
        TestConcurrentFirstDecode();
        TestProgressiveMatchesBaseline();
        TestBaselineSamples();
        TestRejectsUnsupported();
        TestStrictEntropyBoundaries();
        TestCoefficientBoundsAndSaturation();
        TestJpegDimensionsAndWorkingBudget();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "BlpImage tests passed\n";
    return 0;
}
