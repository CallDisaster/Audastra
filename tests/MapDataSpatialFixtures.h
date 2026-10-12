#pragma once

#include "War3MapCore/MapDocument.h"

#include <array>
#include <stdexcept>

// 原创世界坐标样例；多个测试宿主读取同一组 native 字节，不读本机地图。
namespace MapDataSpatialFixtures {

inline std::vector<War3Map::MapArchiveMember> Members(bool includeSpecialDoodad = false) {
    using namespace War3Map;
    using namespace War3Map::MapData;
    std::vector<MapArchiveMember> members;
    const auto append = [&](const char* name, const auto& table) {
        auto bytes = table.Serialize();
        if (!bytes.Succeeded() || !bytes.value) throw std::runtime_error("original spatial fixture serialization");
        members.push_back({name, std::move(*bytes.value)});
    };
    UnitPlacements units;
    constexpr std::array<std::array<float, 3>, 7> positions{{
        {-32, -16, 0}, {0, 0, 1}, {0, 0, 2}, {32, 16, 3}, {32.25f, 16, 4}, {0, 17, 5}, {0, 0, 6}}};
    for (std::size_t i = 0; i < positions.size(); ++i) {
        UnitPlacement unit;
        unit.rawcode = i == 2 ? "Hfoo" : "hfoo"; unit.owner = i == 1 ? 1 : 0;
        unit.position = positions[i]; unit.scale = {1, 1, 1}; unit.creationId = static_cast<std::int32_t>(i + 10);
        units.units.push_back(std::move(unit));
    }
    append("war3mapUnits.doo", units);
    DoodadPlacements doodads;
    constexpr std::array<std::array<float, 3>, 4> doodadPositions{{{-32, -16, 0}, {32, 16, 0}, {0, 0, 0}, {48, 0, 0}}};
    for (std::size_t i = 0; i < doodadPositions.size(); ++i) {
        DoodadPlacement doodad;
        doodad.rawcode = i == 2 ? "LTbr" : "LTlt"; doodad.position = doodadPositions[i]; doodad.scale = {1, 1, 1};
        doodad.creationId = static_cast<std::int32_t>(i + 20); doodads.doodads.push_back(std::move(doodad));
    }
    if (includeSpecialDoodad) doodads.specialDoodads.push_back({"YTfb", 0, {0, 0}});
    append("war3map.doo", doodads);
    RegionsDocument regions;
    constexpr std::array<std::array<float, 4>, 5> bounds{{
        {-64, -32, -32, -16}, {-8, -8, 8, 8}, {32, 16, 48, 32}, {32.25f, -16, 48, 16}, {-64, -64, 64, 64}}};
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        RegionRecord region{};
        region.left = bounds[i][0]; region.bottom = bounds[i][1]; region.right = bounds[i][2]; region.top = bounds[i][3];
        region.name = "Spatial Region " + std::to_string(i); region.creationNumber = static_cast<std::int32_t>(i + 30);
        regions.entries.push_back(std::move(region));
    }
    append("war3map.w3r", regions);
    CamerasDocument cameras;
    for (const auto& target : std::array<std::array<float, 2>, 3>{{{32, 16}, {0, 0}, {48, 0}}}) {
        CameraRecord camera{};
        camera.targetX = target[0]; camera.targetY = target[1];
        camera.targetDistance = 1600; camera.fieldOfView = 70; camera.farClippingPlane = 5000; camera.nearClippingPlane = 100;
        // 短名称保证无 W3I 的原创样例也只有经典布局，不能误把名称前 12 字节当扩展转角。
        camera.name = "Camera " + std::to_string(cameras.entries.size()); cameras.entries.push_back(std::move(camera));
    }
    append("war3map.w3c", cameras);
    return members;
}

} // namespace MapDataSpatialFixtures
