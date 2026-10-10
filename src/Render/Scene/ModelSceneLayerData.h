#pragma once
#include "ModelSceneData.h"
#include "../Particles/ParticleInput.h"
#include "War3MapCore/RenderPluginModelLayers.h"
#include <memory>
#include <vector>

namespace War3Map::Render {
struct ModelSceneLayerData {
    std::uint64_t id = 0, parentModelEpoch = 0, revision = 0;
    std::uint32_t role = WAR3_RENDER_MODEL_LAYER_MODEL; // 随资产和事务拥有，动态更新不能修改。
    std::shared_ptr<const ModelScene> models;
    std::shared_ptr<const SkinPose> pose;
    std::shared_ptr<const ParticleSceneData> particles;
    std::shared_ptr<const ParticleFrameData> particleFrame;
};
using ModelSceneLayers = std::vector<std::shared_ptr<const ModelSceneLayerData>>;
}
