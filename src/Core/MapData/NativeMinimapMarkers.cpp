#include "War3MapCore/MapData/NativeMinimapMarkers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
template<class T> Result<T> Failure(const char* code, const char* message) {
    return Result<T>::Failure("Map.NativeMinimapMarkers", code, message);
}
}

Result<std::array<std::uint32_t, 4>> NativeMinimapPlayableRectangle(
    const TerrainGridSnapshot& terrain, const MapSettings& settings,
    const std::optional<std::array<double, 4>>& cameraMargins) {
    using Rectangle = std::array<std::uint32_t, 4>;
    if (terrain.header.version != 11 || terrain.header.width < 2 || terrain.header.height < 2
        || !settings.FullyParsed() || settings.formatVersion != 25)
        return Failure<Rectangle>("NATIVE_MMP_PLAYABLE_LAYOUT", "可玩区需要完整经典v11 W3E与v25 W3I。");
    const auto width = terrain.header.width - 1, height = terrain.header.height - 1;
    std::array<double, 4> rectangle{};
    if ((settings.flags & 0x400u) != 0) {
        rectangle = {double(settings.cameraMargins[0]), double(settings.cameraMargins[2]),
            double(width) - settings.cameraMargins[1], double(height) - settings.cameraMargins[3]};
    } else {
        if (!cameraMargins)
            return Failure<Rectangle>("NATIVE_MMP_CAMERA_CONFIG", "自定义相机边界缺少实际CameraMargins。");
        for (const auto value : *cameraMargins) if (!std::isfinite(value) || value < 0 || value > 65536)
            return Failure<Rectangle>("NATIVE_MMP_CAMERA_CONFIG", "CameraMargins必须为有界非负数。");
        rectangle = {(double(settings.cameraBounds[0]) - terrain.header.offset[0]) / 128.0 - (*cameraMargins)[0],
            (double(settings.cameraBounds[1]) - terrain.header.offset[1]) / 128.0 - (*cameraMargins)[2],
            (double(settings.cameraBounds[2]) - terrain.header.offset[0]) / 128.0 + (*cameraMargins)[1],
            (double(settings.cameraBounds[3]) - terrain.header.offset[1]) / 128.0 + (*cameraMargins)[3]};
    }
    Rectangle output{};
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (!std::isfinite(rectangle[i]) || !std::isfinite(terrain.header.offset[i % 2]))
            return Failure<Rectangle>("NATIVE_MMP_CAMERA_BOUNDS", "相机或地形边界含非有限值。");
        output[i] = static_cast<std::uint32_t>(std::clamp(rectangle[i], 0.0, double(i % 2 ? height : width)) + 0.5);
    }
    if (output[0] >= output[2] || output[1] >= output[3])
        return Failure<Rectangle>("NATIVE_MMP_PLAYABLE_EMPTY", "实际可玩区为空，未生成部分小地图。");
    return Result<Rectangle>::Success(output);
}

Result<NativeMinimapMarkerFrame> NativeMinimapFrame(const TerrainGridSnapshot& terrain,
    const std::array<std::uint32_t, 4>& playable) {
    if (terrain.header.width < 2 || terrain.header.height < 2 || playable[0] >= playable[2]
        || playable[1] >= playable[3] || playable[2] >= terrain.header.width || playable[3] >= terrain.header.height
        || !std::isfinite(terrain.header.offset[0]) || !std::isfinite(terrain.header.offset[1]))
        return Failure<NativeMinimapMarkerFrame>("NATIVE_MMP_FRAME", "MMP可玩区或地形offset无效。");
    NativeMinimapMarkerFrame frame;
    for (std::size_t i = 0; i < playable.size(); ++i)
        frame.worldRectangle[i] = static_cast<float>(double(playable[i]) * 128.0 + terrain.header.offset[i % 2]);
    const auto width = playable[2] - playable[0], height = playable[3] - playable[1];
    if (width > height) {
        const auto margin = (256 - static_cast<std::int32_t>(double(height) / width * 256.0)) / 2;
        frame.pixelRectangle[1] = margin; frame.pixelRectangle[3] = 256 - margin;
    } else if (height > width) {
        const auto margin = (256 - static_cast<std::int32_t>(double(width) / height * 256.0)) / 2;
        frame.pixelRectangle[0] = margin; frame.pixelRectangle[2] = 256 - margin;
    }
    return Result<NativeMinimapMarkerFrame>::Success(frame);
}

Result<MinimapMarkersSnapshot> BuildNativeMinimapMarkers(const NativeMinimapMarkerFrame& frame,
    std::span<const NativeMinimapMarkerCandidate> candidates, const MinimapMarkerLimits& limits) {
    const MinimapMarkerLimits hard;
    if (limits.maxBytes > hard.maxBytes || limits.maxMarkers > hard.maxMarkers)
        return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_LIMITS", "MMP计算预算只能收紧硬上限。");
    if (candidates.size() > limits.maxMarkers || limits.maxBytes < 8
        || candidates.size() > (limits.maxBytes - 8) / 16)
        return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_BUDGET", "MMP候选数量或输出字节超过预算。");
    const auto& w = frame.worldRectangle;
    const auto& p = frame.pixelRectangle;
    for (const auto coordinate : w) if (!std::isfinite(coordinate))
        return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_WORLD_RECTANGLE", "MMP世界矩形包含非有限值。");
    if (!(w[0] < w[2]) || !(w[1] < w[3]))
        return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_WORLD_RECTANGLE", "MMP世界矩形为空或反向。");
    for (const auto coordinate : p) if (coordinate < 0 || coordinate > 256)
        return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_PIXEL_RECTANGLE", "MMP像素矩形超过0..256。");
    if (p[0] >= p[2] || p[1] >= p[3])
        return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_PIXEL_RECTANGLE", "MMP像素矩形为空或反向。");
    try {
        MinimapMarkersSnapshot output;
        output.records.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            if (candidate.type < 0 || candidate.type > 2)
                return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_TYPE", "正式保存只生成矿点、商店和开始位置三类图标。");
            if (!std::isfinite(candidate.position[0]) || !std::isfinite(candidate.position[1]))
                return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_POSITION", "MMP单位二维位置包含非有限值。");
            const auto x = std::trunc((double(candidate.position[0]) - w[0]) / (double(w[2]) - w[0]) * (p[2] - p[0]));
            const auto y = std::trunc((double(candidate.position[1]) - w[1]) / (double(w[3]) - w[1]) * (p[3] - p[1]));
            const auto mappedX = double(p[0]) + x, mappedY = double(p[3]) - y;
            constexpr auto low = double(std::numeric_limits<std::int32_t>::min());
            constexpr auto high = double(std::numeric_limits<std::int32_t>::max());
            if (!std::isfinite(mappedX) || !std::isfinite(mappedY)
                || mappedX < low || mappedX > high || mappedY < low || mappedY > high)
                return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_POSITION_OVERFLOW", "MMP单位映射超出安全I32范围，未夹回边缘。");
            output.records.push_back({candidate.type,
                {static_cast<std::int32_t>(mappedX), static_cast<std::int32_t>(mappedY)}, candidate.colorBgra});
        }
        return Result<MinimapMarkersSnapshot>::Success(std::move(output));
    } catch (const std::bad_alloc&) {
        return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_ALLOCATION", "MMP计算分配失败，未返回部分记录。");
    } catch (const std::length_error&) {
        return Failure<MinimapMarkersSnapshot>("NATIVE_MMP_ALLOCATION", "MMP计算容量不足，未返回部分记录。");
    }
}

Result<std::array<std::uint8_t, 4>> AverageNativeMinimapTeamColor(std::span<const std::byte> rgba,
    std::size_t maximumPixels) {
    using Color = std::array<std::uint8_t, 4>;
    if (!maximumPixels || maximumPixels > 1024u * 1024u)
        return Failure<Color>("NATIVE_MMP_COLOR_LIMITS", "TeamColor像素预算只能收紧非零硬上限。");
    if (rgba.empty() || rgba.size() % 4 || rgba.size() / 4 > maximumPixels)
        return Failure<Color>("NATIVE_MMP_COLOR_IMAGE", "TeamColor需要完整非空、有界RGBA像素。");
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t pixel = 0; pixel < rgba.size(); pixel += 4)
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
            sums[channel] += std::to_integer<std::uint8_t>(rgba[pixel + channel]);
    const auto count = rgba.size() / 4;
    return Result<Color>::Success({static_cast<std::uint8_t>(sums[2] / count),
        static_cast<std::uint8_t>(sums[1] / count), static_cast<std::uint8_t>(sums[0] / count), 255});
}

} // namespace War3Map::MapData
