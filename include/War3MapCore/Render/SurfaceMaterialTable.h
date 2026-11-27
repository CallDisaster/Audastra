#pragma once

#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Types.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Render {
// 地图的物体类型表面材质表（地图归档里的 WarVK\materials.ini）。格式与 WarVK 的同名表相同：
// 每节一个四字符 rawcode，键 normal/orm/detailNormal/detailOrm/detailAlbedo 是地图归档内的图片路径，
// normalY = gl|dx，normalScale 0..8、roughness 0..4、metallic 0..1、occlusion 0..1、specular 0..4、
// detailScale 8..65536、detailStrength 0..4、shader = surface|water（默认 surface）。“;”或“#”之后是注释。
// 同名节合并，最多 256 节。未知键、坏数值、坏节名逐行给出警告并跳过（坏数值保留原值）；
// surface 节没有任何图片时丢弃并警告；water 节不需要图片。
// rawcode 与键名区分大小写，和游戏一侧读取一致。
inline constexpr std::string_view kSurfaceMaterialTablePath = "WarVK\\materials.ini";
inline constexpr std::size_t kSurfaceMaterialTableMaxTypes = 256;

struct SurfaceMaterialTable {
    std::map<std::string, std::shared_ptr<const ModelSurfaceMaterial>, std::less<>> byRawcode;
    std::vector<Diagnostic> warnings;
    [[nodiscard]] std::shared_ptr<const ModelSurfaceMaterial> Find(std::string_view rawcode) const;
};
[[nodiscard]] SurfaceMaterialTable ParseSurfaceMaterialTable(std::string_view text);
}
