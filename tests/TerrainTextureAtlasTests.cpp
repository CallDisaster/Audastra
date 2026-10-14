#include "War3MapCore/Render/TerrainTextureAtlas.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "fixtures/TinyJpegFixture.h"
#include "fixtures/TerrainAtlasProgressive.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <type_traits>

namespace {

using namespace War3Map;
using namespace War3Map::Render;
using namespace War3Map::Resources;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

template <typename T>
void Rejected(const Result<T>& result, std::string_view code = {}) {
    if (result.Succeeded() || result.value || (!code.empty() && !std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const Diagnostic& diagnostic) { return diagnostic.code == code; }))) {
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        throw std::runtime_error("Atlas 没有按预期拒绝：" + std::string(code));
    }
}

void Put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 255);
}

ResolvedResource Resource(std::vector<std::byte> bytes) {
    return {{"TERRAIN\\ORIGINAL.BLP", ResourceLayer::MapImport, ResourcePriority::MapArchive,
        "original-fixture", "Terrain\\Original.blp"}, std::move(bytes)};
}

ResolvedResource JpegBlp(std::uint32_t width, std::uint32_t height,
    const std::vector<std::vector<unsigned char>>& mips, std::size_t sharedBytes = 0) {
    std::vector<std::byte> bytes(160 + sharedBytes);
    bytes[0] = std::byte{'B'}; bytes[1] = std::byte{'L'}; bytes[2] = std::byte{'P'}; bytes[3] = std::byte{'1'};
    Put32(bytes, 12, width); Put32(bytes, 16, height); Put32(bytes, 156, static_cast<std::uint32_t>(sharedBytes));
    if (sharedBytes != 0) {
        Require(!mips.empty() && sharedBytes <= mips.front().size(), "共享 JPEG 头 fixture 范围无效");
        for (std::size_t index = 0; index < sharedBytes; ++index) bytes[160 + index] = static_cast<std::byte>(mips.front()[index]);
    }
    for (std::size_t level = 0; level < mips.size(); ++level) {
        Require(sharedBytes <= mips[level].size(), "JPEG fixture mip 过短");
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(bytes.size()));
        Put32(bytes, 92 + level * 4, static_cast<std::uint32_t>(mips[level].size() - sharedBytes));
        for (std::size_t index = sharedBytes; index < mips[level].size(); ++index) bytes.push_back(static_cast<std::byte>(mips[level][index]));
    }
    return Resource(std::move(bytes));
}

struct ColoredFixture {
    Image::DecodedImage original;
    ResolvedResource resource;

    explicit ColoredFixture(std::uint32_t width) {
        original.width = width; original.height = 8;
        for (std::uint32_t y = 0; y < original.height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                // 切片内保持同色，易检查方向与边界；包含半透明像素。
                original.rgba.push_back(static_cast<std::byte>((x / 2) * 23));
                original.rgba.push_back(static_cast<std::byte>((y / 2) * 41));
                original.rgba.push_back(static_cast<std::byte>(213 - (x / 2) * 13));
                original.rgba.push_back(static_cast<std::byte>(31 + (y / 2) * 53));
            }
        }
        const auto encoded = Image::EncodeBlpPaletted(original);
        Require(encoded.Succeeded(), "原创调色板 fixture 编码失败");
        resource = Resource(*encoded.value);
    }
};

void TestPixelsCellsAndOwnership() {
    for (const auto width : {8u, 16u}) {
        ColoredFixture fixture(width);
        const auto sourceHash = Sha256(fixture.resource.bytes);
        const auto result = DecodeTerrainTextureAtlas(fixture.resource);
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        Require(result.Succeeded(), "合法原创调色板 atlas 被拒绝");
        const auto snapshot = *result.value;
        static_assert(std::is_const_v<std::remove_reference_t<decltype(*snapshot)>>);
        Require(snapshot->image.rgba == fixture.original.rgba, "BGRA/RGBA、alpha 或栅格方向错误");
        Require(snapshot->additionalMips.size() + 1 == snapshot->validatedMipCount,
            "已验证的原始 mip 没有完整保留");
        std::size_t retained = snapshot->image.rgba.size();
        for (std::size_t level = 0; level < snapshot->additionalMips.size(); ++level) {
            const auto& mip = snapshot->additionalMips[level];
            Require(mip.Valid() && mip.width == std::max(1u, width >> (level + 1))
                && mip.height == std::max(1u, 8u >> (level + 1)), "保留 mip 尺寸或字节无效");
            retained += mip.rgba.size();
        }
        Require(retained == snapshot->RgbaByteCount() && retained > snapshot->image.rgba.size(), "mip 保留总量遗漏");
        Require(snapshot->sourceWidth == width && snapshot->sourceHeight == 8 && snapshot->columns == width / 2
            && snapshot->cells.size() == width * 2 && snapshot->validatedMipCount >= 4, "atlas 布局或 mip 数错误");
        Require(sourceHash.Succeeded() && snapshot->sourceSha256 == *sourceHash.value
            && snapshot->source.sourceId == "original-fixture" && snapshot->source.sourcePath == "Terrain\\Original.blp",
            "实际输入 SHA 或资源来源丢失");
        const auto* last = snapshot->Cell(snapshot->columns - 1, 3);
        Require(last && last->x == width - 2 && last->y == 6 && last->width == 2 && last->height == 2,
            "row-major 切片矩形不正确");
        Require(snapshot->Cell(snapshot->columns, 0) == nullptr && snapshot->Cell(0, 4) == nullptr, "越界切片未拒绝");
        fixture.resource.bytes.clear();
        Require(snapshot->image.rgba == fixture.original.rgba, "快照仍引用可变源缓冲");
    }
}

void TestJpegMipValidation() {
    const auto base = TestFixtures::MakeTinyGrayJpeg(16, 16);
    const auto small = TestFixtures::MakeTinyGrayJpeg(8, 8);
    auto resource = JpegBlp(16, 16, {base.bytes, small.bytes}, 71); // SOI + 共同 DQT 段
    auto result = DecodeTerrainTextureAtlas(resource);
    Require(result.Succeeded() && (*result.value)->validatedMipCount == 2, "共享 JPEG 头与两级 mip 应通过");
    Require((*result.value)->additionalMips.size() == 1 && (*result.value)->additionalMips[0].rgba.size() == 8 * 8 * 4,
        "JPEG mip1 未保留");
    for (std::size_t pixel = 0; pixel < (*result.value)->image.rgba.size(); pixel += 4)
        Require((*result.value)->image.rgba[pixel] == std::byte{128} && (*result.value)->image.rgba[pixel + 3] == std::byte{255},
            "原创 JPEG 常量灰度或不透明度错误");
    const std::vector<unsigned char> progressive(std::begin(kTerrainAtlasProgressive), std::end(kTerrainAtlasProgressive));
    result = DecodeTerrainTextureAtlas(JpegBlp(16, 16, {progressive}));
    Require(result.Succeeded(), "原创渐进式 atlas 应通过");
    auto broken = small.bytes;
    broken.erase(broken.begin() + static_cast<std::ptrdiff_t>(small.entropyOffset)); // EOI 仍完整，只有熵数据缺失
    // 后续层损坏或尺寸不符（常见工具把最后一层写成上一层的副本）：从坏层截断，mip0 照常使用。
    result = DecodeTerrainTextureAtlas(JpegBlp(16, 16, {base.bytes, broken}));
    Require(result.Succeeded() && (*result.value)->validatedMipCount == 1 && (*result.value)->additionalMips.empty(), "坏 mip1 应截断");
    result = DecodeTerrainTextureAtlas(JpegBlp(16, 16, {base.bytes, small.bytes, small.bytes}));
    Require(result.Succeeded() && (*result.value)->validatedMipCount == 2, "重复上一层的 mip2 应截断");
    // JPEG 头部 alpha 位数只决定用不用第四组件：9 这类非常规值不拒绝；无第四组件时 4 位按不透明处理。
    for (const auto alpha : {4u, 9u}) {
        auto odd = JpegBlp(16, 16, {base.bytes});
        Put32(odd.bytes, 8, alpha);
        result = DecodeTerrainTextureAtlas(odd);
        if (alpha == 4) Require(result.Succeeded() && (*result.value)->image.rgba[3] == std::byte{255}, "JPEG 4 位 alpha 应按不透明解码");
        else Rejected(result, "TERRAIN_ATLAS_JPEG");
    }
    Rejected(DecodeTerrainTextureAtlas(JpegBlp(16, 16, {small.bytes})), "TERRAIN_ATLAS_JPEG");
    broken = base.bytes; broken.pop_back();
    Rejected(DecodeTerrainTextureAtlas(JpegBlp(16, 16, {broken})), "TERRAIN_ATLAS_JPEG");
    broken = base.bytes; broken.insert(broken.end(), 0);
    Rejected(DecodeTerrainTextureAtlas(JpegBlp(16, 16, {broken})), "TERRAIN_ATLAS_JPEG");
    const auto restart = TestFixtures::MakeTinyGrayJpeg(16, 16, true);
    Require(DecodeTerrainTextureAtlas(JpegBlp(16, 16, {restart.bytes})).Succeeded(), "含 RST 的 atlas 应通过");
    broken = restart.bytes; broken[restart.entropyOffset + 2] = 0xd2;
    Rejected(DecodeTerrainTextureAtlas(JpegBlp(16, 16, {broken})), "TERRAIN_ATLAS_DECODE");
}

void TestRejectedFormatsAndBudgets() {
    ColoredFixture fixture(8);
    const auto original = fixture.resource;
    const auto mip0End = static_cast<std::size_t>(std::to_integer<unsigned>(original.bytes[28]) | std::to_integer<unsigned>(original.bytes[29]) << 8)
        + (std::to_integer<unsigned>(original.bytes[92]) | std::to_integer<unsigned>(original.bytes[93]) << 8);
    for (std::size_t size = 0; size < original.bytes.size(); ++size) {
        fixture.resource.bytes.assign(original.bytes.begin(), original.bytes.begin() + static_cast<std::ptrdiff_t>(size));
        // 截断在 mip0 内拒绝；只截掉后续层时保留完整的前几层。
        if (size < mip0End) Rejected(DecodeTerrainTextureAtlas(fixture.resource));
        else {
            const auto truncated = DecodeTerrainTextureAtlas(fixture.resource);
            Require(truncated.Succeeded() && (*truncated.value)->validatedMipCount < 4, "截断后续 mip 应保留前几层");
        }
    }
    fixture.resource = original;
    for (const auto alpha : {1u, 4u}) {
        Put32(fixture.resource.bytes, 8, alpha);
        Require(DecodeTerrainTextureAtlas(fixture.resource).Succeeded(), "1/4 位 alpha 调色板应可解码");
    }
    Put32(fixture.resource.bytes, 8, 16);
    Rejected(DecodeTerrainTextureAtlas(fixture.resource), "TERRAIN_ATLAS_ALPHA_UNSUPPORTED");
    fixture.resource = original; fixture.resource.bytes[3] = std::byte{'2'};
    Rejected(DecodeTerrainTextureAtlas(fixture.resource), "TERRAIN_ATLAS_FORMAT");
    fixture.resource = original; Put32(fixture.resource.bytes, 28, 0xfffffff0);
    Rejected(DecodeTerrainTextureAtlas(fixture.resource), "TERRAIN_ATLAS_MIP");
    for (const bool missing : {false, true}) {
        fixture.resource = original;
        if (missing) { Put32(fixture.resource.bytes, 32, 0); Put32(fixture.resource.bytes, 96, 0); }
        else Put32(fixture.resource.bytes, 32, 0xfffffff0);
        const auto truncated = DecodeTerrainTextureAtlas(fixture.resource);
        Require(truncated.Succeeded() && (*truncated.value)->validatedMipCount == 1, "mip1 缺失或越界应只用 mip0");
    }
    fixture.resource = original; Put32(fixture.resource.bytes, 12, 12);
    Rejected(DecodeTerrainTextureAtlas(fixture.resource), "TERRAIN_ATLAS_LAYOUT");
    fixture.resource = original;
    TerrainAtlasLimits limits;
    limits.maxInputBytes = 1; Rejected(DecodeTerrainTextureAtlas(fixture.resource, limits), "TERRAIN_ATLAS_INPUT_BUDGET");
    limits = {}; limits.maxDimension = 4; Rejected(DecodeTerrainTextureAtlas(fixture.resource, limits), "TERRAIN_ATLAS_DIMENSION");
    limits = {}; limits.maxRgbaBytes = 255; Rejected(DecodeTerrainTextureAtlas(fixture.resource, limits), "TERRAIN_ATLAS_RGBA_BUDGET");
    limits = {}; limits.maxRetainedRgbaBytes = 252;
    Rejected(DecodeTerrainTextureAtlas(fixture.resource, limits), "TERRAIN_ATLAS_RETAINED_BUDGET");
    limits = {}; limits.maxRetainedRgbaBytes = 340;
    Require(DecodeTerrainTextureAtlas(fixture.resource, limits).Succeeded(), "完整 mip 链的精确字节限额被拒绝");
    limits.maxRetainedRgbaBytes = 339; // 超额的最后一层被截掉
    auto partial = DecodeTerrainTextureAtlas(fixture.resource, limits);
    Require(partial.Succeeded() && (*partial.value)->validatedMipCount == 3, "保留限额应截断末层");
    limits = {}; limits.maxTotalMipPixels = 63; Rejected(DecodeTerrainTextureAtlas(fixture.resource, limits), "TERRAIN_ATLAS_MIP_BUDGET");
    limits = {}; limits.maxTotalMipPixels = 65; partial = DecodeTerrainTextureAtlas(fixture.resource, limits);
    Require(partial.Succeeded() && (*partial.value)->validatedMipCount == 1, "像素限额应截断后续层");
    limits = {}; limits.maxWorkingBytes = 1; Rejected(DecodeTerrainTextureAtlas(fixture.resource, limits), "TERRAIN_ATLAS_WORKING_BUDGET");
    limits = {}; ++limits.maxDimension; Rejected(DecodeTerrainTextureAtlas(fixture.resource, limits), "TERRAIN_ATLAS_LIMITS");
    const auto jpeg = JpegBlp(16, 16, {TestFixtures::MakeTinyGrayJpeg(16, 16).bytes});
    limits = {}; limits.maxJpegSegments = 1; Rejected(DecodeTerrainTextureAtlas(jpeg, limits), "TERRAIN_ATLAS_JPEG_BUDGET");
    limits = {}; limits.maxJpegScans = 0; Rejected(DecodeTerrainTextureAtlas(jpeg, limits), "TERRAIN_ATLAS_JPEG_BUDGET");
}

void TestResolverFailures() {
    ColoredFixture base(8), map(16);
    bool broken = false;
    int baseReads = 0;
    ResourceResolver resolver;
    ResourceSource low{"game", ResourceLayer::Game, ResourcePriority::GameBase,
        [](std::string_view path) { return Result<ResourceProbe>::Success({ResourceProbeState::Found, std::string(path)}); },
        [&](std::string_view) { ++baseReads; return Result<std::vector<std::byte>>::Success(base.resource.bytes); }};
    Require(resolver.AddSource(std::move(low)).Succeeded(), "基础资源注册失败");
    ResourceSource high{"map", ResourceLayer::MapImport, ResourcePriority::MapArchive,
        [](std::string_view path) { return Result<ResourceProbe>::Success({ResourceProbeState::Found, std::string(path)}); },
        [&](std::string_view) {
            if (broken) return Result<std::vector<std::byte>>::Failure("Fixture", "BROKEN_MAP", "合成读取故障");
            return Result<std::vector<std::byte>>::Success(map.resource.bytes);
        }};
    Require(resolver.AddSource(std::move(high)).Succeeded(), "地图资源注册失败");
    const auto result = LoadTerrainTextureAtlas(resolver, "Terrain/Original.blp");
    Require(result.Succeeded() && (*result.value)->columns == 8 && (*result.value)->source.sourceId == "map"
        && baseReads == 0, "地图覆盖层或来源身份错误");
    broken = true;
    Rejected(LoadTerrainTextureAtlas(resolver, "Terrain/Original.blp"), "BROKEN_MAP");
    Require(baseReads == 0, "高层失败不得回退基础资源");
    Rejected(LoadTerrainTextureAtlas(resolver, "../escape.blp"), "RESOURCE_PATH_UNSAFE");
}

void InspectGameResource(const std::filesystem::path& root, std::string_view path) {
    auto game = Game::GameDataSource::Mount(root);
    Require(game.Succeeded(), "游戏资源只读挂载失败");
    ResourceResolver resolver;
    ResourceSource source;
    source.id = "game-mount-patch-local-expansion-base";
    source.layer = ResourceLayer::Game;
    source.priority = ResourcePriority::GameBase;
    source.probe = [&](std::string_view name) {
        return Result<ResourceProbe>::Success(game.value->HasFile(std::string(name))
            ? ResourceProbe{ResourceProbeState::Found, std::string(name)} : ResourceProbe{});
    };
    source.read = [&](std::string_view name) { return game.value->ReadFile(std::string(name)); };
    Require(resolver.AddSource(std::move(source)).Succeeded(), "资源来源注册失败");
    const auto result = LoadTerrainTextureAtlas(resolver, path);
    for (const auto& diagnostic : result.diagnostics) std::cout << diagnostic.code << ": " << diagnostic.message << '\n';
    Require(result.Succeeded(), "真实 atlas 未通过严格解码");
    const auto& snapshot = **result.value;
    std::uint8_t minimumAlpha = 255, maximumAlpha = 0;
    for (std::size_t index = 3; index < snapshot.image.rgba.size(); index += 4) {
        const auto alpha = std::to_integer<std::uint8_t>(snapshot.image.rgba[index]);
        minimumAlpha = std::min(minimumAlpha, alpha); maximumAlpha = std::max(maximumAlpha, alpha);
    }
    std::cout << "atlas CPU-only path=" << snapshot.source.canonicalPath << " sourceSha=" << snapshot.sourceSha256
        << " width=" << snapshot.image.width << " height=" << snapshot.image.height << " cells=" << snapshot.columns << "x4"
        << " mips=" << snapshot.validatedMipCount << " rgbaBytes=" << snapshot.image.rgba.size()
        << " alphaRange=" << static_cast<unsigned>(minimumAlpha) << ',' << static_cast<unsigned>(maximumAlpha) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || argc == 3, "用法：terrain-atlas-tests [游戏目录 资源虚拟路径]");
        TestPixelsCellsAndOwnership();
        TestJpegMipValidation();
        TestRejectedFormatsAndBudgets();
        TestResolverFailures();
        if (argc == 3) InspectGameResource(std::filesystem::path(argv[1]), argv[2]);
        std::cout << "TerrainTextureAtlas tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
