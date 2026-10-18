#pragma once

#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/ObjectData/EffectiveObjectCatalog.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Resources/ResourceResolver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace War3Map::Render {

// 原版对象层颜色：MM* 字段优先；useMMColor=0 时尝试模型纹理代表色，模型/贴图
// 缺失时使用 WorldEdit 的固定深绿色 (RGB 0,64,0)。InvalidModel 路径必须来自
// 同一次捕获的 [WorldEditArt] / InvalidModel profile，不使用猜测路径。
enum class ClassicMinimapObjectColorSource : std::uint8_t { CustomFields, ModelTextures, FallbackDarkGreen };

struct ClassicMinimapObjectStyle {
    ObjectData::ObjectKey object;
    bool showInMinimap = false;
    bool useMMColor = false;
    std::array<std::uint8_t, 4> color{0, 64, 0, 255};
    ClassicMinimapObjectColorSource colorSource = ClassicMinimapObjectColorSource::FallbackDarkGreen;
    std::string modelPath;
    std::string pathingTexture;
    std::string replaceableTexture;
    std::uint32_t replaceableId = 0;
};

struct ClassicMinimapObjectPixel {
    std::uint32_t x = 0, y = 0;
    std::array<std::uint8_t, 4> color{};
    std::size_t placementIndex = 0;
};

struct ClassicMinimapObjectLayerLimits {
    std::size_t maxPlacements = 65536;
    std::size_t maxPixelsPerFootprint = 65536;
    std::size_t maxTexturePixels = 1024 * 1024;
    std::size_t maxModelBytes = 16 * 1024 * 1024;
    std::size_t maxResourceBytes = 32 * 1024 * 1024;
    std::size_t maxOutputPixels = 1024 * 1024;
};

struct ClassicMinimapObjectLayerInput {
    std::span<const MapData::DoodadPlacement> placements;
    const ObjectData::StandardObjectCatalog* catalog = nullptr;
    const ObjectData::ObjectFieldSchema* schema = nullptr;
    const ObjectData::NativeObjectDocument* doodadOverrides = nullptr;
    const ObjectData::NativeObjectDocument* destructableOverrides = nullptr;
    const Resources::ResourceResolver* resources = nullptr;
    std::array<float, 2> worldOrigin{};
    std::array<std::uint32_t, 4> sourceRectangle{}; // x0,y0,x1,y1, half-open map cells
    bool highResolution = false;
    // [WorldEditArt] InvalidModel 的实际配置值。对象捕获会把配置文件纳入
    // dependency；独立构造层时未提供它则对缺失模型 fail-closed。
    std::optional<std::string> invalidModelPath;
};

struct ClassicMinimapObjectLayer {
    std::vector<ClassicMinimapObjectPixel> pixels;
    std::vector<ClassicMinimapObjectStyle> styles;
    std::vector<Diagnostic> diagnostics;
    std::size_t shownPlacements = 0;
    std::size_t omittedPlacements = 0;
    std::size_t omittedPixels = 0;
    struct Dependency {
        Resources::ResourceLocation winner;
        std::string encodedSha256;
        std::size_t encodedBytes = 0;
        bool present = true;
        bool mapMemberPresent = false;
    };
    std::vector<Dependency> dependencies;
};

[[nodiscard]] Result<ClassicMinimapObjectLayer> BuildClassicMinimapObjectLayer(
    const ClassicMinimapObjectLayerInput& input,
    const ClassicMinimapObjectLayerLimits& limits = {});

// 正式捕获使用一次只读MPQ/游戏挂载，实际winner与未命中都入依赖；overlays必须是
// 保存任务实际owned成员列表。当前source、game包与overlay变化由生命周期核验拒绝。
[[nodiscard]] Result<ClassicMinimapObjectLayer> CaptureClassicMinimapObjectLayer(
    std::span<const MapData::DoodadPlacement> placements, const MapArchiveSession& source,
    const std::filesystem::path& gameRoot, std::uint8_t mainTileset,
    std::array<float, 2> worldOrigin, std::array<std::uint32_t, 4> rectangle,
    std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {},
    const ClassicMinimapObjectLayerLimits& limits = {});

[[nodiscard]] Status VerifyClassicMinimapObjectLayerDependencies(const ClassicMinimapObjectLayer& layer,
    const MapArchiveSession& source, const std::filesystem::path& gameRoot, std::uint8_t mainTileset,
    std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {},
    bool readback = false, const ClassicMinimapObjectLayerLimits& limits = {});

} // namespace War3Map::Render
