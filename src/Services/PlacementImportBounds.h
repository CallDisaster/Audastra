#pragma once

#include "War3MapCore/Services/PlacementImport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace War3Map::Services::Detail {

// 一个已规划的装饰物摆放：与写入的记录同一位置（贴地时为地面高度）、同一弧度朝向与三轴缩放。
struct PlacedDoodad {
    std::size_t instance = 0;
    std::uint32_t type = 0;
    std::array<double, 3> position{};
    float rotation = 0;
    std::array<float, 3> scale{1, 1, 1};
};

// 先校验选项（观察点在地图内、余量有限为正、加宽须给观察点），再按模型写明范围逐个摆放检查，
// 填写 plan.report.bounds、各类型计数与提醒；要求加宽时改写有问题的模型并放进 plan.fileRewrites。
// 选项无效或需要加宽的模型不能安全改写时整份拒绝。
[[nodiscard]] Status CheckPlacementImportBounds(const MapData::PlacementManifest& manifest,
    const PlacementImportInputs& inputs, const std::vector<PlacedDoodad>& placed, PlacementImportPlan& plan);

} // namespace War3Map::Services::Detail
