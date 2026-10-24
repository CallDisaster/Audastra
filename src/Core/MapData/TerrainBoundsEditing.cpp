#include "War3MapCore/MapData/TerrainBoundsEditing.h"

#include <cmath>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr std::size_t kMaximumSettingsBytes = 1024u * 1024u;
constexpr std::size_t kMaximumWork = 2048u * 2048u;

template<class T> Result<T> Failure(const char* code, const char* message) {
    return Result<T>::Failure("Map.TerrainBounds", code, message);
}

bool RectangleValid(const TerrainHeader& header, const TerrainPlayableRectangle& rectangle) {
    return header.version == 11 && header.width >= 2 && header.height >= 2
        && header.width <= 2049 && header.height <= 2049
        && rectangle.west < rectangle.east && rectangle.south < rectangle.north
        && rectangle.east < header.width && rectangle.north < header.height;
}

bool ConfigurationValid(const std::array<float, 4>& margins) {
    for (const auto margin : margins)
        if (!std::isfinite(margin) || margin < 0 || margin > 65536 || std::trunc(margin) != margin) return false;
    return true;
}
}

Result<TerrainNativePlayableBounds> ComputeTerrainNativePlayableBounds(const TerrainHeader& terrain,
    const MapSettings& settings, const std::array<float, 4>& cameraMargins) {
    if (terrain.version != 11 || terrain.width < 2 || terrain.height < 2
        || !settings.FullyParsed() || settings.formatVersion != 25)
        return Failure<TerrainNativePlayableBounds>("TERRAIN_NATIVE_PLAYABLE_BOUNDS", "原版世界可玩范围需要完整经典W3E和W3I。");
    if (!ConfigurationValid(cameraMargins))
        return Failure<TerrainNativePlayableBounds>("TERRAIN_NATIVE_CAMERA_CONFIG", "CameraMargins必须为有界非负整数。");
    const auto world = [&](std::size_t i) { return cameraMargins[i] * 128.0; };
    const auto low = [&](double base, std::size_t i) {
        return static_cast<float>(static_cast<double>(static_cast<float>(base + world(i))) - world(i));
    };
    const auto high = [&](double base, std::size_t i) {
        return static_cast<float>(static_cast<double>(static_cast<float>(base - world(i))) + world(i));
    };
    TerrainNativePlayableBounds bounds;
    if ((settings.flags & 0x400u) != 0) {
        for (const auto margin : settings.cameraMargins) if (margin < 0)
            return Failure<TerrainNativePlayableBounds>("TERRAIN_NATIVE_PLAYABLE_BOUNDS", "W3I可玩边距不能为负。");
        bounds = {low(terrain.offset[0] + settings.cameraMargins[0] * 128.0, 0),
            low(terrain.offset[1] + settings.cameraMargins[2] * 128.0, 2),
            high(terrain.offset[0] + (static_cast<double>(terrain.width - 1) - settings.cameraMargins[1]) * 128.0, 1),
            high(terrain.offset[1] + (static_cast<double>(terrain.height - 1) - settings.cameraMargins[3]) * 128.0, 3)};
    } else {
        bounds = {static_cast<float>(settings.cameraBounds[0] - world(0)),
            static_cast<float>(settings.cameraBounds[1] - world(2)),
            static_cast<float>(settings.cameraBounds[2] + world(1)),
            static_cast<float>(settings.cameraBounds[3] + world(3))};
    }
    if (!std::isfinite(bounds.left) || !std::isfinite(bounds.bottom) || !std::isfinite(bounds.right)
        || !std::isfinite(bounds.top) || bounds.left >= bounds.right || bounds.bottom >= bounds.top)
        return Failure<TerrainNativePlayableBounds>("TERRAIN_NATIVE_PLAYABLE_BOUNDS", "W3I实际世界可玩矩形无效或为空。");
    return Result<TerrainNativePlayableBounds>::Success(bounds);
}

Result<MapSettings> ProjectTerrainPlayableBounds(const TerrainGridSnapshot& terrain, const MapSettings& settings,
    const std::array<float, 4>& cameraMargins, const TerrainBoundsEditIntent& intent, std::size_t maximumSettingsBytes) {
    if (maximumSettingsBytes > kMaximumSettingsBytes)
        return Failure<MapSettings>("TERRAIN_BOUNDS_LIMITS", "地图范围设置预算只能收紧硬上限。");
    if (intent.algorithm != 1 || !RectangleValid(terrain.header, intent.playable)
        || !settings.FullyParsed() || settings.formatVersion != 25)
        return Failure<MapSettings>("TERRAIN_BOUNDS_LAYOUT", "地图范围需要完整经典格式和地形内非空整数矩形。");
    if (!ConfigurationValid(cameraMargins))
        return Failure<MapSettings>("TERRAIN_BOUNDS_CAMERA_CONFIG", "地图范围缺少有效的CameraMargins数值。");
    const auto& rectangle = intent.playable;
    if (double(rectangle.east - rectangle.west) - cameraMargins[0] - cameraMargins[1] < 1
        || double(rectangle.north - rectangle.south) - cameraMargins[2] - cameraMargins[3] < 1)
        return Failure<MapSettings>("TERRAIN_BOUNDS_CAMERA_EMPTY", "扣除游戏镜头边距后，每轴至少需要一个地块。");
    for (std::size_t i = 0; i < 2; ++i) {
        const double low = terrain.header.offset[i];
        const double high = low + double(i ? terrain.header.height - 1 : terrain.header.width - 1) * 128;
        if (!std::isfinite(low) || std::fmod(low, 128.0) != 0 || std::abs(low) > 16777216.0
            || !std::isfinite(high) || std::abs(high) > 16777216.0)
            return Failure<MapSettings>("TERRAIN_BOUNDS_COORDINATE", "地图范围编辑需要128对齐且可精确表示的经典世界坐标。");
    }
    try {
        const auto original = settings.Serialize();
        if (!original.Succeeded() || !original.value) return {{}, original.diagnostics};
        if (original.value->size() > maximumSettingsBytes)
            return Failure<MapSettings>("TERRAIN_BOUNDS_BUDGET", "地图设置超出范围编辑预算。");
        auto output = settings;
        const float west = static_cast<float>(terrain.header.offset[0] + (rectangle.west + double(cameraMargins[0])) * 128);
        const float south = static_cast<float>(terrain.header.offset[1] + (rectangle.south + double(cameraMargins[2])) * 128);
        const float east = static_cast<float>(terrain.header.offset[0] + (rectangle.east - double(cameraMargins[1])) * 128);
        const float north = static_cast<float>(terrain.header.offset[1] + (rectangle.north - double(cameraMargins[3])) * 128);
        output.cameraBounds = {west, south, east, north, west, north, east, south};
        output.cameraMargins = {static_cast<std::int32_t>(rectangle.west),
            static_cast<std::int32_t>(terrain.header.width - 1 - rectangle.east),
            static_cast<std::int32_t>(rectangle.south),
            static_cast<std::int32_t>(terrain.header.height - 1 - rectangle.north)};
        output.playableWidth = static_cast<std::int32_t>(rectangle.east - rectangle.west);
        output.playableHeight = static_cast<std::int32_t>(rectangle.north - rectangle.south);
        output.flags |= 0x400u;
        return Result<MapSettings>::Success(std::move(output));
    } catch (const std::bad_alloc&) {
        return Failure<MapSettings>("TERRAIN_BOUNDS_BUDGET", "地图范围候选内存不足，未修改会话。");
    } catch (const std::length_error&) {
        return Failure<MapSettings>("TERRAIN_BOUNDS_BUDGET", "地图范围候选容量超限，未修改会话。");
    }
}

Result<std::size_t> ApplyTerrainPlayableEdges(TerrainGridSnapshot& terrain,
    const TerrainPlayableRectangle& playable, std::size_t maximumWork) {
    if (maximumWork > kMaximumWork)
        return Failure<std::size_t>("TERRAIN_BOUNDS_LIMITS", "地图外缘工作预算只能收紧硬上限。");
    const auto& header = terrain.header;
    if (!RectangleValid(header, playable)
        || terrain.vertices.size() != static_cast<std::size_t>(header.width) * header.height)
        return Failure<std::size_t>("TERRAIN_BOUNDS_LAYOUT", "地图外缘需要完整经典顶点和有效可玩矩形。");
    const auto work = static_cast<std::size_t>(header.width - 1) * (header.height - 1);
    if (work > maximumWork)
        return Failure<std::size_t>("TERRAIN_BOUNDS_BUDGET", "地图外缘超出逐格工作预算，未修改顶点。");
    for (const auto& vertex : terrain.vertices) if (vertex.extendedTextureLayout)
        return Failure<std::size_t>("TERRAIN_BOUNDS_LAYOUT", "经典地图外缘不能混入扩展顶点布局。");
    std::size_t changed = 0;
    for (std::uint32_t y = 0; y + 1 < header.height; ++y)
        for (std::uint32_t x = 0; x + 1 < header.width; ++x) {
            auto& raw = terrain.vertices[static_cast<std::size_t>(y) * header.width + x].waterHeightAndFlagsRaw;
            const bool edge = x < playable.west || x >= playable.east || y < playable.south || y >= playable.north;
            const auto next = static_cast<std::uint16_t>((raw & ~0x4000u) | (edge ? 0x4000u : 0u));
            changed += next != raw;
            raw = next;
        }
    return Result<std::size_t>::Success(changed);
}

} // namespace War3Map::MapData
