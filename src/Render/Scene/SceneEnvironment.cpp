#include "War3MapCore/Render/SceneEnvironment.h"
#include "War3MapCore/MapData/MapShadow.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace War3Map::Render {
namespace {

constexpr std::string_view kSubsystem = "Render.SceneEnvironment";
constexpr std::uint32_t kShadowCellsPerTile = 4;
constexpr float kShadowCellSize = 32.0f;

void Note(SceneEnvironment& output, const char* code, std::string message) {
    output.notes.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem), code, std::move(message), std::nullopt});
}

void CopyColor(float (&target)[3], const std::array<float, 3>& source, float maximum) {
    for (std::size_t i = 0; i < 3; ++i) target[i] = std::isfinite(source[i]) ? std::clamp(source[i], 0.0f, maximum) : 0.0f;
}

} // namespace

TerrainLighting TerrainLightingFrom(const SceneLight* terrainLight) {
    TerrainLighting lighting;
    if (!terrainLight || !(terrainLight->towardLight[2] > 0.0f)) return lighting;
    lighting.towardLight = terrainLight->towardLight;
    for (std::size_t i = 0; i < 3; ++i) {
        lighting.ambient[i] = std::clamp(terrainLight->ambient[i], 0.0f, 16.0f);
        lighting.diffuse[i] = std::clamp(terrainLight->diffuse[i], 0.0f, 16.0f);
    }
    return lighting;
}

void ApplySceneFog(SceneEnvironment& environment, const MapData::MapEnvironmentSettings* settings, bool terrainFog) {
    std::erase_if(environment.notes, [](const Diagnostic& note) { return note.code.starts_with("SCENE_FOG_"); });
    auto& v = environment.values;
    v.fogMode = WAR3_RENDER_FOG_NONE;
    v.fogStart = v.fogEnd = 0.0f;
    std::fill(std::begin(v.fogColor), std::end(v.fogColor), 0.0f);
    if (!settings || !terrainFog) return;
    if (settings->fogStyle < 0 || settings->fogStyle > 2) {
        Note(environment, "SCENE_FOG_STYLE", "地图雾类型 " + std::to_string(settings->fogStyle) + " 未知，未显示雾。");
    } else if (!std::isfinite(settings->fogStart) || !std::isfinite(settings->fogEnd) || !(settings->fogStart < settings->fogEnd)
        || std::abs(settings->fogStart) > 1.0e7f || std::abs(settings->fogEnd) > 1.0e7f) {
        Note(environment, "SCENE_FOG_RANGE", "地图雾的起止距离无效，未显示雾。");
    } else {
        v.fogMode = WAR3_RENDER_FOG_LINEAR;
        v.fogStart = settings->fogStart;
        v.fogEnd = settings->fogEnd;
        v.fogColor[0] = settings->fogColor.red / 255.0f;
        v.fogColor[1] = settings->fogColor.green / 255.0f;
        v.fogColor[2] = settings->fogColor.blue / 255.0f;
        if (settings->fogStyle != 0) Note(environment, "SCENE_FOG_APPROXIMATE", "地图使用指数雾，编辑器暂按起止距离线性近似显示。");
    }
}

SceneEnvironment BuildSceneEnvironment(const SceneLight* unitLight, const MapData::MapEnvironmentSettings* settings,
    bool terrainFog, const MapData::TerrainGridSnapshot& grid, std::optional<std::span<const std::byte>> shadowMap) {
    SceneEnvironment output;
    auto& v = output.values;
    v.structSize = sizeof(War3RenderEnvironmentV2);
    // 默认中性光与地表默认光一致。
    const TerrainLighting neutral;
    const auto toward = unitLight ? unitLight->towardLight : neutral.towardLight;
    CopyColor(v.lightColor, unitLight ? unitLight->diffuse : neutral.diffuse, 16.0f);
    CopyColor(v.ambientColor, unitLight ? unitLight->ambient : neutral.ambient, 16.0f);
    const auto length = std::sqrt(toward[0] * toward[0] + toward[1] * toward[1] + toward[2] * toward[2]);
    const auto& direction = std::isfinite(length) && length > 1e-6f ? toward : neutral.towardLight;
    std::copy(direction.begin(), direction.end(), v.towardLight);

    ApplySceneFog(output, settings, terrainFog);

    if (shadowMap) {
        output.shadowInput.sourceBytes = shadowMap->size();
        const auto columns = static_cast<std::uint64_t>(grid.header.width > 0 ? grid.header.width - 1 : 0) * kShadowCellsPerTile;
        const auto rows = static_cast<std::uint64_t>(grid.header.height > 0 ? grid.header.height - 1 : 0) * kShadowCellsPerTile;
        // W3E维度先扩展为u64，超出u32时用codec的零维度拒绝，不截断成另一布局。
        const auto dimension = [](std::uint64_t value) {
            return value <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(value) : 0u;
        };
        auto parsed = MapData::ParseMapShadow(*shadowMap, dimension(columns), dimension(rows));
        if (!parsed.Succeeded()) {
            output.shadowInput.state = shadowMap->empty() ? SceneShadowInputState::PresentEmpty : SceneShadowInputState::Rejected;
            output.shadowInput.diagnostics = std::move(parsed.diagnostics);
            Note(output, "SCENE_SHADOW_SIZE", "war3map.shd 无法用于当前预览网格（" + std::to_string(columns) + "×"
                + std::to_string(rows) + " 字节），未显示静态阴影；原诊断见阴影详情。");
        } else {
            output.shadowInput.state = SceneShadowInputState::Parsed;
            // move唯一codec输出后原位归一化，不同时保留raw/parsed/preview三份格数组。
            output.shadow = std::move(parsed.value->intensities);
            std::transform(output.shadow.begin(), output.shadow.end(), output.shadow.begin(),
                [](std::uint8_t value) { return value != 0 ? std::uint8_t{255} : std::uint8_t{0}; });
            v.shadowWidth = static_cast<std::uint32_t>(columns);
            v.shadowHeight = static_cast<std::uint32_t>(rows);
            v.shadowOriginX = grid.header.offset[0];
            v.shadowOriginY = grid.header.offset[1];
            v.shadowCellSize = kShadowCellSize;
            v.shadowDarkness = kStaticShadowDarkness;
        }
    }
    return output;
}

War3RenderEnvironmentV2 DescribeSceneEnvironment(const SceneEnvironment& environment) {
    auto values = environment.values;
    values.structSize = sizeof(War3RenderEnvironmentV2);
    values.shadow = environment.shadow.empty() ? nullptr : environment.shadow.data();
    if (environment.shadow.empty()) values.shadowWidth = values.shadowHeight = 0;
    return values;
}

} // namespace War3Map::Render
