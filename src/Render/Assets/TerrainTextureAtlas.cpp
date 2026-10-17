#include "War3MapCore/Render/TerrainTextureAtlas.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/BlpMipChain.h"

#include <new>
#include <stdexcept>
#include <string_view>

namespace War3Map::Render {
namespace {

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

void ValidateLimits(const TerrainAtlasLimits& limits) {
    const TerrainAtlasLimits hard;
    if (limits.maxInputBytes > hard.maxInputBytes || limits.maxDimension > hard.maxDimension
        || limits.maxRgbaBytes > hard.maxRgbaBytes || limits.maxRetainedRgbaBytes > hard.maxRetainedRgbaBytes
        || limits.maxTotalMipPixels > hard.maxTotalMipPixels
        || limits.maxWorkingBytes > hard.maxWorkingBytes || limits.maxJpegSegments > hard.maxJpegSegments
        || limits.maxJpegScans > hard.maxJpegScans)
        Fail("TERRAIN_ATLAS_LIMITS", "Atlas 限额只能收紧，不能放宽硬上限。");
}

Image::BlpMipChainLimits ChainLimits(const TerrainAtlasLimits& limits) {
    return {limits.maxInputBytes, limits.maxDimension, limits.maxRgbaBytes, limits.maxRetainedRgbaBytes,
        limits.maxTotalMipPixels, limits.maxWorkingBytes, limits.maxJpegSegments, limits.maxJpegScans};
}

// 共享层保留停止事实；Atlas仍沿用原来的代码、文本和后级静默截断合同。
std::vector<Diagnostic> AtlasDiagnostics(std::vector<Diagnostic> diagnostics) {
    for (auto& diagnostic : diagnostics) {
        if (diagnostic.subsystem != "Image.BlpMipChain") continue;
        diagnostic.subsystem = kSubsystem;
        constexpr std::string_view prefix = "BLP_MIP_";
        if (diagnostic.code.starts_with(prefix))
            diagnostic.code.replace(0, prefix.size(), "TERRAIN_ATLAS_");
        constexpr std::string_view subject = "BLP mip 链";
        const auto at = diagnostic.message.find(subject);
        if (at != std::string::npos) diagnostic.message.replace(at, subject.size(), "Atlas");
    }
    return diagnostics;
}

template <typename T>
T TakeChain(Result<T> result) {
    if (!result.Succeeded()) throw Failure{AtlasDiagnostics(std::move(result.diagnostics))};
    return std::move(*result.value);
}

} // namespace

Result<Snapshot> DecodeTerrainTextureAtlas(const Resources::ResolvedResource& resource, const TerrainAtlasLimits& limits) {
    try {
        ValidateLimits(limits);
        const auto chainLimits = ChainLimits(limits);
        // 保留input→format→dimension→Atlas layout→RGBA/alpha/mip的原拒绝顺序。
        const auto dimensions = TakeChain(Image::ReadBlpMipChainDimensions(resource.bytes, chainLimits));
        if (dimensions.height % 4 != 0
            || (dimensions.width != dimensions.height && dimensions.width != dimensions.height * 2))
            Fail("TERRAIN_ATLAS_LAYOUT", "Atlas 不是 4x4 或 8x4 方形单元格布局。");
        auto decoded = Image::DecodeBlpMipChain(resource.bytes, chainLimits);
        auto diagnostics = AtlasDiagnostics(std::move(decoded.diagnostics));
        if (!decoded.Succeeded()) throw Failure{std::move(diagnostics)};
        auto snapshot = std::make_shared<TerrainAtlasSnapshot>();
        snapshot->source = resource.location;
        snapshot->sourceBytes = resource.bytes.size();
        snapshot->sourceWidth = dimensions.width; snapshot->sourceHeight = dimensions.height;
        snapshot->columns = dimensions.width == dimensions.height ? 4 : 8;
        snapshot->image = std::move(decoded.value->image);
        snapshot->additionalMips = std::move(decoded.value->additionalMips);
        snapshot->validatedMipCount = decoded.value->validatedMipCount;
        const auto size = dimensions.height / TerrainAtlasSnapshot::rows;
        for (std::uint32_t row = 0; row < TerrainAtlasSnapshot::rows; ++row)
            for (std::uint32_t column = 0; column < snapshot->columns; ++column)
                snapshot->cells.push_back({column, row, column * size, row * size, size, size});
        snapshot->sourceSha256 = Take(Sha256(resource.bytes));
        for (auto& diagnostic : diagnostics)
            diagnostic.message = resource.location.canonicalPath + "：" + diagnostic.message;
        auto result = Result<Snapshot>::Success(std::move(snapshot));
        result.diagnostics = std::move(diagnostics);
        return result;
    } catch (Failure& failure) {
        Result<Snapshot> result;
        result.diagnostics = std::move(failure.diagnostics);
        for (auto& diagnostic : result.diagnostics)
            if (diagnostic.code != "TERRAIN_ATLAS_ALLOCATION")
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
