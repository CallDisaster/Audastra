#pragma once

#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/RenderPluginModelLayers.h"
#include "War3MapCore/Services/BuildingSplatService.h"

namespace War3Map::Render {

// 仅供预览的进程内 owned DTO；不修改物编或地图，不宣称原版投影/GE4/pass/bias一致。
struct BuildingSplatPreviewLimits {
    std::size_t maxAssetBytes = 64 * 1024 * 1024, maxFrameBytes = 64 * 1024 * 1024;
    std::size_t maxIssueBytes = 64 * 1024, maxIssues = 128;
    float maximumCellSize = 128; // 只能收紧；网格不是原版精确地形裁剪。
    float previewHeightOffset = 0; // 显式预览策略，不能称原版depth bias；不叠加dooZ/usca/ussc。
};
struct BuildingSplatPreviewTexture {
    std::string path, sourceLayer, digest;
    std::shared_ptr<const ModelTextureRgba> image; // 来源读取与DecodeModelTexture的owned输出。
};
struct BuildingSplatPreviewBinding {
    Services::WorldSelectionVersion version;
    std::string catalogIdentity, terrainIdentity;
    std::uint64_t terrainRevision = 0;
    [[nodiscard]] bool operator==(const BuildingSplatPreviewBinding&) const = default;
};
struct BuildingSplatPreviewInstance {
    Services::BuildingSplatRow row; // QueryBuildingSplatFootprints的实际owned行，含原字段来源。
    std::uint64_t stablePlacementId = 0; // 当前宿主registry的实例身份；不等同PlacementId.index。
};
struct BuildingSplatPreviewIssue {
    std::optional<MapData::PlacementId> owner;
    std::uint64_t stablePlacementId = 0;
    std::string splatKey, code, reason;
    bool omitted = false;
};
struct BuildingSplatPreviewAssets {
    struct Grid {
        std::string key;
        std::uint32_t firstVertex = 0, firstIndex = 0, indexCount = 0, vertexCount = 0;
        std::uint32_t segmentsX = 0, segmentsY = 0, texture = 0;
        float fullWidth = 0, fullHeight = 0;
    };
    std::shared_ptr<const Services::BuildingSplatCatalog> catalog;
    std::shared_ptr<const AnimatedModelScene> scene; // 按key常驻原创UV网格与真实RGBA；无MDX动画。
    std::vector<Grid> grids;
    std::vector<BuildingSplatPreviewTexture> sources;
    std::vector<BuildingSplatPreviewIssue> issues;
    std::size_t omittedIssues = 0, logicalBytes = 0;
    float maximumCellSize = 128;
    bool previewApproximate = true;
};
struct BuildingSplatPreviewOwner {
    MapData::PlacementId owner;
    std::uint64_t stablePlacementId = 0, inputIndex = 0;
    std::uint32_t renderedInstance = 0, firstGroup = 0, groupCount = 0;
    std::string splatKey;
    std::vector<Services::BuildingSplatFieldOrigin> origins;
};
struct BuildingSplatPreviewFrame {
    std::uint32_t renderLayerRole = WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW;
    std::shared_ptr<const AnimatedModelScene> scene;
    std::shared_ptr<const AnimationFrame> frame;
    BuildingSplatPreviewBinding binding;
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain;
    std::vector<BuildingSplatPreviewOwner> owners; // 与frame.instances平行；不进入单位精确点选。
    std::vector<BuildingSplatPreviewIssue> issues;
    std::size_t requested = 0, displayed = 0, absent = 0, unavailable = 0, omittedBudget = 0;
    std::size_t omittedIssues = 0, logicalBytes = 0;
    bool previewApproximate = true;
};

// 只使用调用方实际提供的纹理，不读磁盘、不猜低层资源。缺纹理/坏纹理逐key报告并省略。
// 默认值为硬预算，调用方只能收紧。目录/来源身份错误整体失败，无部分候选。
[[nodiscard]] Result<BuildingSplatPreviewAssets> BuildBuildingSplatPreviewAssets(
    std::shared_ptr<const Services::BuildingSplatCatalog> catalog,
    std::span<const BuildingSplatPreviewTexture> textures, const AnimationSession& session,
    const BuildingSplatPreviewLimits& limits = {});

// 显示terrain须为同一冻结ApplyRampHeights输出。完全覆盖footprint的水/边界/未支持悬崖
// 明确省略，不夹XY、不回退Z0。height只由SampleTerrainHeight给出；源dooZ不参与。
// binding是当前地图/目录/地形身份；scene/documentRevision属于辅助资产自己的编号空间。
// 同布局previous复用静态scene，更新仅产生owned组/alpha。新布局只重交这一聚合层。
// parent epoch/CAS/真正完成帧由既有generic辅助层桥绑定，本函数的成功仅为CPU准备成功。
[[nodiscard]] Result<BuildingSplatPreviewFrame> BuildBuildingSplatPreviewFrame(
    const BuildingSplatPreviewAssets& assets, std::span<const BuildingSplatPreviewInstance> instances,
    const BuildingSplatPreviewBinding& binding,
    std::shared_ptr<const MapData::TerrainGridSnapshot> displayedTerrain,
    std::uint64_t contentVersion, const BuildingSplatPreviewLimits& limits = {},
    const BuildingSplatPreviewFrame* previous = nullptr);

} // namespace War3Map::Render
