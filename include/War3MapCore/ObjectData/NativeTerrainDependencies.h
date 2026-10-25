#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace War3Map::ObjectData {

enum class NativeTerrainDependencyContribution {
    Dynamic,
    StaticRgb,
    WalkableBridge,
    Special,
};

// 只描述同次实际读取的静态通行输入；不是保存资格或外部授权。
// 类别保持原字符串，不在probe中解释启动目录或本地化值。
struct NativeTerrainDependencyType {
    std::string rawcode;
    std::string parentRawcode;
    bool special = false;
    NativeTerrainDependencyContribution contribution = NativeTerrainDependencyContribution::Dynamic;
    std::string pathTexture;
    std::optional<float> fixedRotationDegrees;
    std::string category;
    // 未知字段按原始wire散列。两侧不一致不能视为无关；空串表示没有未知字段。
    std::string unknownFieldsSha256;

    [[nodiscard]] bool operator==(const NativeTerrainDependencyType&) const noexcept = default;
};

struct NativeTerrainDependencyDescription {
    std::size_t workBytes = 0;
    std::vector<NativeTerrainDependencyType> objects;
};

// sources必须显式提供W3B/W3D、DestructableData、Doodads及两类metadata六项，缺失也提供。
// 只读实际通行字段，已知外观覆盖不必满足完整原生重建资格；未知变动由两侧调用者拒绝。
// 不解码占地图、不重现原WPM；完整重建仍调用严格InspectNativeTerrainObjects。
[[nodiscard]] Result<NativeTerrainDependencyDescription> InspectNativeTerrainDependencies(
    std::span<const std::string_view> placedRawcodes, std::span<const std::string_view> specialRawcodes,
    std::span<const ArchiveExactMember> sources, std::size_t maximumWorkingBytes);

} // namespace War3Map::ObjectData
