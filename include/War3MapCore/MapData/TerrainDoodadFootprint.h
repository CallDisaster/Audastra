#pragma once

#include "War3MapCore/MapData/TerrainUniformDoodadFootprint.h"

#include <optional>

namespace War3Map::MapData {

// 借用已捕获的TGA像素，不分配解码或旋转副本；调用期间原字节必须保持有效。
// topOrigin表示文件首行已是原版loader的首行，底原点TGA读取时反转行号。
struct TerrainDoodadFootprint {
    std::uint32_t width = 0, height = 0;
    std::span<const std::byte> pixels;
    std::uint8_t pixelBytes = 0;
    bool topOrigin = false;
    bool uniform = false; // 完整BGRA相同，不能只比较通行位或忽略alpha。
    std::uint8_t uniformAttributes = 0;
};

// 与同色占地共用格式校验，允许1..256的矩形和不同像素。
[[nodiscard]] Result<TerrainDoodadFootprint> DecodeDoodadFootprintTga(std::span<const std::byte> bytes);

// fixedRotationDegrees必须来自实际生效fixedRot字段；负值表示不强制角度。
// 缺失字段只允许旋转不影响结果的同色正方形。先按wire角度吸附，之后才按
// fixedRot决定像素方向；不能用最终角度反推此前的XY吸附。临界角度/坐标拒绝。
// 其余坐标域、原子失败、只读浮点环境与既有同色接口一致。
[[nodiscard]] Status AddDoodadFootprint(std::span<std::uint8_t> staticAttributes,
    const TerrainHeader& terrain, const DoodadPlacement& placement,
    const TerrainDoodadFootprint& footprint, std::uint8_t snapCells,
    std::optional<float> fixedRotationDegrees);

} // namespace War3Map::MapData
