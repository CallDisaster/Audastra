#include "War3MapCore/Render/ClassicMinimapObjectLayer.h"

#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ClassicObjectTexture.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <unordered_map>
#include <string_view>

namespace War3Map::Render {
namespace {

constexpr char kSubsystem[] = "ClassicMinimapObjectLayer";
constexpr std::array<std::uint8_t, 4> kFallback{0, 64, 0, 255};

Result<ClassicMinimapObjectLayer> Fail(const char* code, const char* message) {
    return Result<ClassicMinimapObjectLayer>::Failure(kSubsystem, code, message);
}

std::string Lower(std::string_view value) {
    std::string output(value);
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return output;
}

std::optional<bool> Boolean(std::string_view value) {
    const auto text = Lower(value);
    if (text == "1" || text == "true") return true;
    if (text == "0" || text == "false") return false;
    return std::nullopt;
}

std::optional<int> Integer(std::string_view value) {
    if (value.empty()) return std::nullopt;
    int sign = 1; std::size_t at = 0;
    if (value.front() == '+' || value.front() == '-') { sign = value.front() == '-' ? -1 : 1; at = 1; }
    int result = 0;
    for (; at < value.size(); ++at) {
        const auto c = value[at]; if (c < '0' || c > '9') return std::nullopt;
        if (result > (std::numeric_limits<int>::max() - (c - '0')) / 10) return std::nullopt;
        result = result * 10 + (c - '0');
    }
    return result * sign;
}

void AddDiagnostic(ClassicMinimapObjectLayer& output, const char* code, std::string message) {
    output.diagnostics.push_back({DiagnosticSeverity::Warning, kSubsystem, code, std::move(message), {}});
}

std::optional<ObjectData::StandardObjectEntry> FindObject(const ClassicMinimapObjectLayerInput& input,
    std::string_view rawcode, ObjectData::ObjectType& type, bool& missing) {
    missing = false;
    const ObjectData::NativeObjectDocument* overrides = nullptr;
    const ObjectData::ObjectRecord* record = nullptr;
    const ObjectData::StandardObjectEntry* standard = nullptr;
    for (const auto candidate : {ObjectData::ObjectType::Doodad, ObjectData::ObjectType::Destructable}) {
        const auto* document = candidate == ObjectData::ObjectType::Doodad ? input.doodadOverrides : input.destructableOverrides;
        const auto* found = document ? document->FindObject({candidate, std::string(rawcode)}) : nullptr;
        const auto* catalogEntry = input.catalog->Find(candidate, rawcode);
        if (found || catalogEntry) {
            if (overrides || standard || record) return std::nullopt; // 同rawcode跨两种目录有歧义，不能猜。
            type = candidate; overrides = document; record = found; standard = catalogEntry;
        }
    }
    // 只有两种实际目录及其原生覆盖均查无此rawcode才属于取色能力缺失。
    // 下方的父链、类型歧义与opaque字段仍返回未知，不能借此取得保存回退资格。
    if (!record && !standard) { missing = true; return std::nullopt; }
    std::vector<const ObjectData::ObjectRecord*> chain;
    std::set<std::string> visited;
    auto current = record;
    while (current) {
        if (!visited.insert(current->key.rawcode).second || chain.size() >= 32) return std::nullopt;
        chain.push_back(current);
        if (!current->parentRawcode) break;
        standard = input.catalog->Find(type, *current->parentRawcode);
        current = overrides->FindObject({type, *current->parentRawcode});
        if (!current && !standard) return std::nullopt;
    }
    if (!standard) return std::nullopt;
    auto result = *standard; result.key = {type, std::string(rawcode)};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) for (const auto& modification : (*it)->modifications) {
        const auto* definition = input.schema ? input.schema->FindNative(type, modification.key.fieldId) : nullptr;
        if (!definition) continue;
        auto key = Lower(definition->sourceField);
        const bool minimapField = key == "showinmm" || key == "usemmcolor" || key == "mmred" || key == "mmgreen" || key == "mmblue"
            || key == "file" || key == "pathtex" || key == "numvar" || key == "texfile" || key == "texid";
        if (minimapField) {
            if (modification.valueWasOpaque || std::holds_alternative<ObjectData::UnknownBytesValue>(modification.value))
                return std::nullopt;
            result.fields[key] = ObjectData::ValueToDisplayString(modification.value);
        }
    }
    return result;
}

void CaptureDependency(const Resources::ResolvedResource& resource, ClassicMinimapObjectLayer& output) {
    const auto duplicate = std::find_if(output.dependencies.begin(), output.dependencies.end(), [&](const auto& item) {
        return item.winner.canonicalPath == resource.location.canonicalPath;
    });
    if (duplicate != output.dependencies.end()) return;
    const auto sha = Sha256(resource.bytes);
    if (sha.Succeeded() && sha.value) output.dependencies.push_back({resource.location, *sha.value, resource.bytes.size()});
}

bool ColorFields(const ObjectData::StandardObjectEntry& entry, std::array<std::uint8_t, 4>& color) {
    const auto red = Integer(entry.Field("mmred"));
    const auto green = Integer(entry.Field("mmgreen"));
    const auto blue = Integer(entry.Field("mmblue"));
    if (!red || !green || !blue || *red < 0 || *red > 255 || *green < 0 || *green > 255 || *blue < 0 || *blue > 255)
        return false;
    color = {static_cast<std::uint8_t>(*red), static_cast<std::uint8_t>(*green), static_cast<std::uint8_t>(*blue), 255};
    return true;
}

std::array<std::uint8_t, 4> Average(const Image::DecodedImage& image, bool& hasPixels) {
    std::uint64_t channels[3]{}; std::uint64_t count = 0;
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        channels[0] += std::to_integer<std::uint8_t>(image.rgba[i]);
        channels[1] += std::to_integer<std::uint8_t>(image.rgba[i + 1]);
        channels[2] += std::to_integer<std::uint8_t>(image.rgba[i + 2]); ++count;
    }
    if (!count) { hasPixels = false; return kFallback; }
    hasPixels = true;
    return {static_cast<std::uint8_t>(channels[0] / count), static_cast<std::uint8_t>(channels[1] / count),
        static_cast<std::uint8_t>(channels[2] / count), 255};
}

Result<std::array<std::uint8_t, 4>> ModelColor(const ClassicMinimapObjectStyle& style,
    const Resources::ResourceResolver& resources, std::size_t maxModelBytes,
    ClassicMinimapObjectLayer& output, bool& success) {
    success = false;
    if (style.modelPath.empty()) return Result<std::array<std::uint8_t, 4>>::Success(kFallback);
    auto modelBytes = resources.Read(style.modelPath);
    if (!modelBytes.Succeeded() || !modelBytes.value) {
        if (!Resources::ClassicObjectTextureMissing(modelBytes.diagnostics)) return {std::nullopt, std::move(modelBytes.diagnostics)};
        AddDiagnostic(output, "CLASSIC_MINIMAP_OBJECT_MODEL", "对象模型明确缺失，使用原版固定回退色。");
        return Result<std::array<std::uint8_t, 4>>::Success(kFallback);
    }
    if (modelBytes.value->bytes.size() > maxModelBytes) return Result<std::array<std::uint8_t, 4>>::Failure(
        kSubsystem, "CLASSIC_MINIMAP_OBJECT_MODEL_BUDGET", "对象模型超过读取预算。");
    CaptureDependency(*modelBytes.value, output);
    auto model = ParseMdx(modelBytes.value->bytes);
    if (!model.Succeeded() || !model.value) {
        return {std::nullopt, std::move(model.diagnostics)};
    }
    std::uint64_t channels[3]{}; std::uint64_t count = 0;
    for (const auto& material : model.value->materials) for (const auto& layer : material.layers) {
        if (layer.textureId >= model.value->textures.size()) continue;
        const auto& texture = model.value->textures[layer.textureId];
        auto texturePath = texture.path;
        if (texture.replaceableId && texture.replaceableId == style.replaceableId && !style.replaceableTexture.empty())
            texturePath = style.replaceableTexture;
        else if (texture.replaceableId != 0) continue;
        if (texturePath.empty()) continue;
        auto resource = resources.Read(texturePath);
        if (!resource.Succeeded() || !resource.value) {
            if (Resources::ClassicObjectTextureMissing(resource.diagnostics)) continue;
            return {std::nullopt, std::move(resource.diagnostics)};
        }
        CaptureDependency(*resource.value, output);
        auto image = Image::DecodeTexture(texturePath, resource.value->bytes);
        if (!image.Succeeded() || !image.value) return {std::nullopt, std::move(image.diagnostics)};
        bool hasPixels = false; const auto average = Average(*image.value, hasPixels);
        if (!hasPixels) continue;
        // 原版先对每个材质纹理做整图平均，再等权平均这些代表色；不会按贴图面积加权。
        channels[0] += average[0]; channels[1] += average[1]; channels[2] += average[2]; ++count;
    }
    if (!count) { AddDiagnostic(output, "CLASSIC_MINIMAP_OBJECT_MODEL_COLOR", "对象模型未枚举到有效纹理，使用原版默认白色。");
        return Result<std::array<std::uint8_t, 4>>::Success({255, 255, 255, 255}); }
    success = true;
    return Result<std::array<std::uint8_t, 4>>::Success({static_cast<std::uint8_t>(channels[0] / count),
        static_cast<std::uint8_t>(channels[1] / count), static_cast<std::uint8_t>(channels[2] / count), 255});
}

struct Mask { std::uint32_t width = 0, height = 0; std::vector<std::uint8_t> cells; };

Result<std::optional<Mask>> ReadMask(const ClassicMinimapObjectStyle& style,
    const Resources::ResourceResolver& resources, std::size_t maxPixels,
    ClassicMinimapObjectLayer& output) {
    if (style.pathingTexture.empty()) return Result<std::optional<Mask>>::Success(std::nullopt);
    auto resource = Resources::ReadClassicObjectTexture<Resources::ResolvedResource>(style.pathingTexture,
        [&](std::string_view name) { return resources.Read(name); });
    if (!resource.Succeeded() || !resource.value) {
        const bool missing = Resources::ClassicObjectTextureMissing(resource.diagnostics);
        if (missing) {
            AddDiagnostic(output, "CLASSIC_MINIMAP_OBJECT_PATHING_MISSING", "对象通行纹理明确缺失，按原版单格回退。");
            return Result<std::optional<Mask>>::Success(std::nullopt);
        }
        return {std::nullopt, std::move(resource.diagnostics)};
    }
    CaptureDependency(resource.value->resource, output);
    auto image = Image::DecodeTexture(resource.value->path, resource.value->resource.bytes, maxPixels);
    if (!image.Succeeded() || !image.value || !image.value->Valid())
        return {std::nullopt, std::move(image.diagnostics)};
    if (static_cast<std::size_t>(image.value->width) * image.value->height > maxPixels) {
        return Result<std::optional<Mask>>::Failure(kSubsystem, "CLASSIC_MINIMAP_OBJECT_PATHING_BUDGET", "对象通行纹理超过像素预算。");
    }
    Mask mask{image.value->width, image.value->height, std::vector<std::uint8_t>(image.value->width * image.value->height)};
    for (std::size_t i = 0; i < mask.cells.size(); ++i)
        mask.cells[i] = std::to_integer<std::uint8_t>(image.value->rgba[i * 4])
            || std::to_integer<std::uint8_t>(image.value->rgba[i * 4 + 1])
            || std::to_integer<std::uint8_t>(image.value->rgba[i * 4 + 2]);
    return Result<std::optional<Mask>>::Success(std::move(mask));
}

} // namespace

Result<ClassicMinimapObjectLayer> BuildClassicMinimapObjectLayer(
    const ClassicMinimapObjectLayerInput& input, const ClassicMinimapObjectLayerLimits& limits) {
    if (!input.catalog || !input.resources || input.placements.size() > limits.maxPlacements
        || limits.maxPlacements == 0 || limits.maxPixelsPerFootprint == 0 || limits.maxTexturePixels == 0)
        return Fail("CLASSIC_MINIMAP_OBJECT_INPUT", "对象层缺少同源物编目录/资源来源或超过预算。");
    const auto x0 = input.sourceRectangle[0], y0 = input.sourceRectangle[1];
    const auto x1 = input.sourceRectangle[2], y1 = input.sourceRectangle[3];
    if (x1 <= x0 || y1 <= y0) return Fail("CLASSIC_MINIMAP_OBJECT_RECTANGLE", "对象层输出矩形为空。");
    ClassicMinimapObjectLayer output;
    output.styles.reserve(input.placements.size());
    std::map<std::string, ClassicMinimapObjectStyle, std::less<>> styleCache;
    std::map<std::string, std::optional<Mask>, std::less<>> maskCache;
    for (std::size_t index = 0; index < input.placements.size(); ++index) {
        const auto& placement = input.placements[index];
        ObjectData::ObjectType type = ObjectData::ObjectType::Doodad;
        bool missing = false;
        const auto entry = FindObject(input, placement.rawcode, type, missing);
        if (!entry) return missing
            ? Fail("CLASSIC_MINIMAP_OBJECT_MISSING", "对象rawcode在同源的装饰物、可破坏物目录及原生覆盖中均不存在，暂不能为其生成小地图。")
            : Fail("CLASSIC_MINIMAP_OBJECT_UNKNOWN", "对象父链、类型或小地图字段有歧义，不能确定有效取色内容。");
        const auto show = Boolean(entry->Field("showinmm"));
        if (!show || !*show) { ++output.omittedPlacements; continue; }
        ClassicMinimapObjectStyle style;
        const auto styleKey = placement.rawcode + ":" + std::to_string(placement.variation);
        if (const auto cached = styleCache.find(styleKey); cached != styleCache.end()) style = cached->second;
        else {
        style.object = {type, placement.rawcode}; style.showInMinimap = true;
        const auto variations = std::max(1, Integer(entry->Field("numvar")).value_or(1));
        if (placement.variation < 0) return Fail("CLASSIC_MINIMAP_OBJECT_VARIATION", "对象变体编号不能为负。");
        const auto modelField = entry->Field("file");
        if (modelField.empty()) {
            if (!input.invalidModelPath || input.invalidModelPath->empty())
                return Fail("CLASSIC_MINIMAP_OBJECT_INVALID_MODEL_CONFIG",
                    "对象模型路径为空，但未提供同一次捕获的 WorldEditArt/InvalidModel 配置。");
            style.modelPath = *input.invalidModelPath;
        } else {
            style.modelPath = MdxModelFile(modelField, static_cast<std::uint32_t>(placement.variation),
                static_cast<std::uint32_t>(variations));
        }
        style.pathingTexture = std::string(entry->Field("pathtex"));
        if (style.pathingTexture == "_") style.pathingTexture.clear();
        style.replaceableTexture = std::string(entry->Field("texfile"));
        style.replaceableId = static_cast<std::uint32_t>(std::max(0, Integer(entry->Field("texid")).value_or(0)));
        style.useMMColor = Boolean(entry->Field("usemmcolor")).value_or(false);
        if (style.useMMColor && ColorFields(*entry, style.color)) style.colorSource = ClassicMinimapObjectColorSource::CustomFields;
        else if (!style.useMMColor) {
            bool modelColor = false;
            auto color = ModelColor(style, *input.resources, limits.maxModelBytes, output, modelColor);
            if (!color.Succeeded() || !color.value) return {std::nullopt, std::move(color.diagnostics)};
            style.color = *color.value;
            style.colorSource = modelColor ? ClassicMinimapObjectColorSource::ModelTextures : ClassicMinimapObjectColorSource::FallbackDarkGreen;
        } else {
            return Fail("CLASSIC_MINIMAP_OBJECT_COLOR", "对象声明useMMColor但MM颜色字段无效，不能猜测颜色。");
        }
        styleCache.emplace(styleKey, style);
        }
        output.styles.push_back(style); ++output.shownPlacements;
        if (!maskCache.contains(style.pathingTexture)) {
            auto loaded = ReadMask(style, *input.resources, limits.maxTexturePixels, output);
            if (!loaded.Succeeded() || !loaded.value) return {std::nullopt, std::move(loaded.diagnostics)};
            maskCache.emplace(style.pathingTexture, std::move(*loaded.value));
            output.diagnostics.insert(output.diagnostics.end(), loaded.diagnostics.begin(), loaded.diagnostics.end());
        }
        const auto& mask = maskCache.at(style.pathingTexture);
        const auto width = mask ? mask->width : 1u, height = mask ? mask->height : 1u;
        if (!std::isfinite(placement.rotation) || !std::isfinite(placement.position[0]) || !std::isfinite(placement.position[1]))
            return Fail("CLASSIC_MINIMAP_OBJECT_COORDINATE", "对象坐标或角度不是有限数。");
        constexpr auto pi = std::numbers::pi_v<double>;
        auto rotation = std::fmod(static_cast<double>(placement.rotation), pi * 2.0);
        if (rotation < 0) rotation += pi * 2.0;
        const auto quadrant = static_cast<unsigned>(std::floor(rotation / (pi * 0.5) + 0.5)) % 4u;
        // WorldEdit 的 pathing loader 保存 top-down 源纹理。q0/q2 交换宽高，
        // q1/q3 不交换；这与模型空间角度的通常约定不同，不能换成任意角度投影。
        const auto rotatedWidth = (quadrant & 1u) ? width : height, rotatedHeight = (quadrant & 1u) ? height : width;
        if (static_cast<std::size_t>(width) * height > limits.maxPixelsPerFootprint)
            return Fail("CLASSIC_MINIMAP_OBJECT_FOOTPRINT_BUDGET", "对象通行覆盖超过单实例预算，不能静默漏画。");
        const auto sourceScale = input.highResolution ? 1u : 4u;
        const auto left = static_cast<std::int64_t>(std::trunc((placement.position[0] - input.worldOrigin[0]
            - static_cast<float>(rotatedWidth * 32) * 0.5f + (rotatedWidth % 2 == 0 ? 16.0f : 0.0f)) / 32.0f));
        const auto bottom = static_cast<std::int64_t>(std::trunc((placement.position[1] - input.worldOrigin[1]
            - static_cast<float>(rotatedHeight * 32) * 0.5f + (rotatedHeight % 2 == 0 ? 16.0f : 0.0f)) / 32.0f));
        if (!mask) {
            const auto step = input.highResolution ? 32.0 : 128.0;
            const auto cellX = static_cast<std::int64_t>(std::floor((placement.position[0] - input.worldOrigin[0]) / step));
            const auto cellY = static_cast<std::int64_t>(std::floor((placement.position[1] - input.worldOrigin[1]) / step));
            if (cellX >= x0 && cellX < x1 && cellY >= y0 && cellY < y1)
                output.pixels.push_back({static_cast<std::uint32_t>(cellX), static_cast<std::uint32_t>(cellY), style.color, index});
            if (output.pixels.size() > limits.maxOutputPixels) return Fail("CLASSIC_MINIMAP_OBJECT_PIXEL_BUDGET", "对象层覆盖像素超过累计预算。");
            continue;
        }
        for (std::uint32_t v = 0; v < rotatedHeight; ++v) for (std::uint32_t u = 0; u < rotatedWidth; ++u) {
            std::uint32_t sx = v, sy = u;
            if (quadrant == 1) { sx = width - 1 - u; sy = v; }
            else if (quadrant == 2) { sx = width - 1 - v; sy = height - 1 - u; }
            else if (quadrant == 3) { sx = u; sy = height - 1 - v; }
            if (mask && !mask->cells[static_cast<std::size_t>(sy) * width + sx]) continue;
            const auto px = left + static_cast<std::int64_t>(u);
            const auto py = bottom + static_cast<std::int64_t>(v);
            const auto cellX = px / sourceScale, cellY = py / sourceScale;
            if (cellX < x0 || cellX >= x1 || cellY < y0 || cellY >= y1) continue;
            output.pixels.push_back({static_cast<std::uint32_t>(cellX), static_cast<std::uint32_t>(cellY), style.color, index});
            if (output.pixels.size() > limits.maxOutputPixels) return Fail("CLASSIC_MINIMAP_OBJECT_PIXEL_BUDGET", "对象层覆盖像素超过累计预算。");
        }
    }
    return Result<ClassicMinimapObjectLayer>::Success(std::move(output));
}

} // namespace War3Map::Render
