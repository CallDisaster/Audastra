#pragma once

#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
// rawcode 与键名区分大小写，和游戏一侧读取一致。键的名称、范围与行的切分只在本头文件定义一次，
// 读表（ParseSurfaceMaterialTable）与原地编辑（SurfaceMaterialTableDocument）共用。
inline constexpr std::string_view kSurfaceMaterialTablePath = "WarVK\\materials.ini";
inline constexpr std::size_t kSurfaceMaterialTableMaxTypes = 256;

enum class SurfaceMaterialKeyKind : std::uint8_t { Image, Number, Choice };

struct SurfaceMaterialKey {
    std::string_view name;  // 表里的键名
    std::string_view label; // 界面中文名
    SurfaceMaterialKeyKind kind = SurfaceMaterialKeyKind::Image;
    float minimum = 0, maximum = 0, defaultValue = 0;                   // Number
    std::array<std::string_view, 2> choices{};                          // Choice：第一个为默认
};

inline constexpr std::array<SurfaceMaterialKey, 14> kSurfaceMaterialKeys{{
    {"shader", "着色方式", SurfaceMaterialKeyKind::Choice, 0, 0, 0, {"surface", "water"}},
    {"normal", "法线", SurfaceMaterialKeyKind::Image},
    {"orm", "ORM（遮蔽/粗糙度/金属）", SurfaceMaterialKeyKind::Image},
    {"detailNormal", "细节法线", SurfaceMaterialKeyKind::Image},
    {"detailOrm", "细节 ORM", SurfaceMaterialKeyKind::Image},
    {"detailAlbedo", "细节颜色（只取亮度）", SurfaceMaterialKeyKind::Image},
    {"normalY", "法线朝向", SurfaceMaterialKeyKind::Choice, 0, 0, 0, {"gl", "dx"}},
    {"normalScale", "法线强度", SurfaceMaterialKeyKind::Number, 0, 8, 1},
    {"roughness", "粗糙度", SurfaceMaterialKeyKind::Number, 0, 4, 1},
    {"metallic", "金属度", SurfaceMaterialKeyKind::Number, 0, 1, 1},
    {"occlusion", "遮蔽", SurfaceMaterialKeyKind::Number, 0, 1, 1},
    {"specular", "高光", SurfaceMaterialKeyKind::Number, 0, 4, 1},
    {"detailScale", "细节尺寸", SurfaceMaterialKeyKind::Number, 8, 65536, 256},
    {"detailStrength", "细节强度", SurfaceMaterialKeyKind::Number, 0, 4, 1},
}};

[[nodiscard]] const SurfaceMaterialKey* FindSurfaceMaterialKey(std::string_view name) noexcept;
// 与 WarVK 相同：可带一个“+”，整段都是有限数且在范围内。
[[nodiscard]] std::optional<float> ParseSurfaceMaterialNumber(std::string_view text, const SurfaceMaterialKey& key);
// 节名：四个可打印 ASCII 字符（不含空白）。
[[nodiscard]] bool SurfaceMaterialRawcode(std::string_view text) noexcept;

// 一行的读法（行内“;”或“#”之后是注释）。value 的位置指向原行，供原地改值。
struct SurfaceMaterialLine {
    enum class Kind : std::uint8_t { Blank, Section, BadSection, KeyValue, Bad };
    Kind kind = Kind::Blank;
    std::string_view rawcode;           // Section
    std::string_view key, value;        // KeyValue：已去空白
    std::size_t valueBegin = 0, valueEnd = 0; // KeyValue：value 在原行里的 [begin, end)；空值时两者都在“=”之后
};
[[nodiscard]] SurfaceMaterialLine ReadSurfaceMaterialLine(std::string_view line) noexcept;

struct SurfaceMaterialTable {
    std::map<std::string, std::shared_ptr<const ModelSurfaceMaterial>, std::less<>> byRawcode;
    std::vector<Diagnostic> warnings;
    [[nodiscard]] std::shared_ptr<const ModelSurfaceMaterial> Find(std::string_view rawcode) const;
};
[[nodiscard]] SurfaceMaterialTable ParseSurfaceMaterialTable(std::string_view text);
}
