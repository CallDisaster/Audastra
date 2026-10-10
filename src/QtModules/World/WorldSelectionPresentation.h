#pragma once

#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/Render/ViewRay.h"
#include "War3MapCore/MapData/MapTerrain.h"

namespace War3Map::Modules {

// 真正的MiscData选取圈纹理；原创四顶点显示模型，不借用单位AABB或回旋圈特效。
// 资产只加载一次。全部选择实例共用一个附加层；布局变化重提交此小层，
// 布局不变时仅更新矩阵。当前主世界roster合同不适用于附加层。
struct WorldSelectionCircleAssets {
    struct Band { float maximumSize = 0; std::uint32_t model = 0; };
    struct Source { std::string path, layer, digest; };
    std::shared_ptr<const Render::AnimatedModelScene> scene;
    std::vector<Band> bands;
    std::vector<Source> sources;
    float scaleFactor = 0, baseHeight = 0, alpha = 1;
    std::array<float, 3> tint{0, 1, 0};
};
struct WorldSelectionCircleFrame {
    std::shared_ptr<const Render::AnimationFrame> frame;
    std::vector<std::uint32_t> requestIndices; // 与排序后的frame实例平行，不进入精确点选。
    std::size_t unknownScale = 0, terrainUnavailable = 0, waterSemanticsPending = 0;
    bool previewApproximate = true; // 尺寸单位/水面/可通行地物尚未逐帧原版对照。
};
[[nodiscard]] Result<WorldSelectionCircleAssets> LoadWorldSelectionCircleAssets(
    Render::AnimationSession session, const Render::AnimationResourceReader& read);
// sourceScene的修订是附加显示资产自己的编号空间；主地图修订由宿主完成帧绑定。
// 请求position已经是显示位置；用fly偏移恢复rawZ，仅选取圈消费selZ，绝不改源地图。
// 有显示地表采样时，圈使用max(rawZ,terrainCenter)的预览近似锚点；水面/可通行地物规则未原版签收。
[[nodiscard]] Result<WorldSelectionCircleFrame> BuildWorldSelectionCircleFrame(
    const WorldSelectionCircleAssets& assets, std::span<const Render::ModelInstanceRequest> requests,
    const MapData::TerrainGridSnapshot* terrain, std::uint64_t contentVersion,
    const std::function<std::optional<float>(double, double)>& waterHeight = {});
// 只读完成帧矩阵，将逻辑像素矩形变成既有renderer overlay的四条边；
// 内部不绘制任何Qt backingstore，因此原生D3D内容始终由同一次Draw/Present呈现。
// 仅截取边框的绘制区域；框选命中仍使用原始手势坐标与修饰键。
[[nodiscard]] Result<std::vector<War3RenderOverlayLineV2>> BuildWorldSelectionOutline(
    const Render::ViewProjectionFrame& completed, const Render::ViewLogicalMetrics& metrics,
    Render::LogicalViewPoint anchor, Render::LogicalViewPoint cursor);

} // namespace War3Map::Modules
