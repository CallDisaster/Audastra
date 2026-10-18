#pragma once

#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Render/DayNightLighting.h"
#include "War3MapCore/Render/TerrainGroundMesh.h"
#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/Types.h"

#include <optional>
#include <span>
#include <vector>

namespace War3Map::Render {

enum class SceneShadowInputState { NotProvided, Parsed, PresentEmpty, Rejected };

// 仅描述传入raw的codec结果；NotProvided不证明地图缺失该成员。
struct SceneShadowInputFacts {
    SceneShadowInputState state = SceneShadowInputState::NotProvided;
    std::size_t sourceBytes = 0;
    std::vector<Diagnostic> diagnostics; // 唯一MapShadow codec原诊断，含origin；不改写为通用缺失。
};

// 提交给渲染插件 setEnvironment 的场景环境：模型光照、雾、地表静态阴影。
struct SceneEnvironment {
    War3RenderEnvironmentV2 values{}; // shadow 指针由 DescribeSceneEnvironment 填写
    std::vector<std::uint8_t> shadow;
    std::vector<Diagnostic> notes;    // 近似显示或数据被忽略的原因（不阻止显示）
    SceneShadowInputFacts shadowInput;
};

// 静态阴影处地表亮度的保留比例之补（原版叠加强度未核实，按半透明显示）。
constexpr float kStaticShadowDarkness = 0.5f;

// unitLight 为空时用与地表默认 TerrainLighting 一致的中性灰光。
// 雾：只由 terrainFog（W3I 选项“使用地形迷雾”，kMapFlagTerrainFog）启用，与生成脚本是否调用 SetTerrainFogEx 一致；
// fogStyle 即 SetTerrainFogEx 的样式：0 线性、1 指数、2 指数平方；指数两种暂按起止距离线性近似并记入 notes。
// 阴影：war3map.shd 每格 4×4 像素、每像素 32 世界单位，首行在南，非 0 为阴影；尺寸须为 (宽−1)·4 × (高−1)·4，
// 原字交唯一MapShadow codec，失败原诊断保留在shadowInput，兼容说明仍记入notes；
// Parsed只证明当前预览尺寸布局，不证明原版烘焙/强度语义。归一化仅用于预览，不回写raw。
[[nodiscard]] SceneEnvironment BuildSceneEnvironment(const SceneLight* unitLight,
    const MapData::MapEnvironmentSettings* settings, bool terrainFog, const MapData::TerrainGridSnapshot& grid,
    std::optional<std::span<const std::byte>> shadowMap);

// 只按地图设置重算雾（替换此前的雾说明），光照与阴影保持不变；地图设置修改后无需重读地形。
void ApplySceneFog(SceneEnvironment& environment, const MapData::MapEnvironmentSettings* settings, bool terrainFog);

// 地表光照：DNC 地形光（为空时保持默认中性光）。
[[nodiscard]] TerrainLighting TerrainLightingFrom(const SceneLight* terrainLight);

// 返回的结构借用 environment.shadow，须在其生命周期内提交。
[[nodiscard]] War3RenderEnvironmentV2 DescribeSceneEnvironment(const SceneEnvironment& environment);

} // namespace War3Map::Render
