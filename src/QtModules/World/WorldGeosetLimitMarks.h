#pragma once

#include "War3MapCore/Render/EditorOverlay.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Modules {

// 场景按模型路径只报一次“几何体超出游戏上限”（Render::kGeosetOverGameLimitCode）；
// 这里把它对回每个摆放请求，模型诊断据此逐个列出，世界视图据此画警示框。只提醒，不改地图。
struct WorldGeosetLimitMarks {
    std::vector<std::uint32_t> requests;        // 命中的请求序号，升序
    std::map<std::string, std::string> details; // 比较键 → 说明（去掉“模型 路径：”前缀）；只收命中过请求的模型
    [[nodiscard]] bool Contains(std::uint32_t request) const noexcept;
    [[nodiscard]] const std::string* Detail(std::string_view modelPath) const;
};

// 模型路径比较键：ASCII 小写，'/' 视作 '\'。
[[nodiscard]] std::string WorldModelPathKey(std::string_view path);
[[nodiscard]] WorldGeosetLimitMarks FindWorldGeosetLimitMarks(const std::vector<Diagnostic>& warnings,
    std::size_t requestCount, const std::function<std::string_view(std::size_t)>& modelPath);

inline constexpr Render::OverlayColor kGeosetLimitBoxColor{1.0f, 0.55f, 0.1f}; // 警示橙
inline constexpr std::size_t kGeosetLimitMaxBoxes = 4096;
using WorldRequestVisible = std::function<bool(std::uint32_t request)>;

// 收集要画框的实例（最多 kGeosetLimitMaxBoxes 个），结果只含包围盒所需的 modelBounds、instances、instanceRequests。
// 静态场景：instances 即全部摆放。
[[nodiscard]] Render::WorldModelScene GeosetLimitInstances(const Render::WorldModelScene& scene,
    const WorldGeosetLimitMarks& marks, const WorldRequestVisible& visible);
// 动画场景：取已接受帧的全部姿势实例（含视野外，框在世界坐标里不随相机变）；姿势被拒的跳过。
[[nodiscard]] Render::WorldModelScene GeosetLimitInstances(const Render::AnimatedModelScene& scene,
    const Render::AnimationFrame& frame, const WorldGeosetLimitMarks& marks, const WorldRequestVisible& visible);
void AppendGeosetLimitBoxes(Render::OverlayLines& lines, const Render::WorldModelScene& marked);

} // namespace War3Map::Modules
