#pragma once

#include "War3MapCore/MapData/TerrainUniformDoodadFootprint.h"
#include "War3MapCore/MapData/TerrainNativeObjectPathing.h"

#include <memory>
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

// 普通对象的有界扩展入口：type2仍零拷贝借用bytes，type10持有恰好decodedBytes的
// 解包像素。结果只能移动，移动后footprint的span仍有效；结果存活期间不得释放
// decodedPixels，type2还须保持原bytes有效。特殊地形和旧同色入口仍只接受type2。
struct TerrainDoodadFootprintImage {
    TerrainDoodadFootprint footprint;
    std::unique_ptr<std::byte[]> decodedPixels;
    std::size_t decodedBytes = 0;
    std::vector<std::byte> decodedBlpPixels;
};

// maximumDecodedBytes只约束新增像素存储；调用者另计仍并存的原始TGA与其它工作。
// 分配前核对额度、完整包流及附加区；失败不返回部分图像。
[[nodiscard]] Result<TerrainDoodadFootprintImage> DecodeDoodadFootprintTgaBudgeted(
    std::span<const std::byte> bytes, std::size_t maximumDecodedBytes);

// BLP的mip0经公共解码器读取，不重采样、不翻行。maximumWorkingBytes约束新增
// 解码峰值，输入bytes由调用者另计；decodedBytes返回该峰值而非仅最终像素容量。
// decodedBlpPixels持有转换后的BGRA，移动结果不会改变footprint所借用的地址。
[[nodiscard]] Result<TerrainDoodadFootprintImage> DecodeDoodadFootprintBlpBudgeted(
    std::span<const std::byte> bytes, std::size_t maximumWorkingBytes);

// fixedRotationDegrees必须来自实际生效fixedRot字段；负值表示不强制角度。
// 缺失字段只允许旋转不影响结果的同色正方形。先按wire角度吸附，之后才按
// fixedRot决定像素方向；不能用最终角度反推此前的XY吸附。临界角度/坐标拒绝。
// 其余坐标域、原子失败、只读浮点环境与既有同色接口一致。
// WalkableTerrainSuppression将非零RGB属性映射为内部0x40；不写RGB低位，
// alpha仍参与原版完整同色判定。此纯投影接口不授予对象或保存资格。
[[nodiscard]] Status AddDoodadFootprint(std::span<std::uint8_t> staticAttributes,
    const TerrainHeader& terrain, const DoodadPlacement& placement,
    const TerrainDoodadFootprint& footprint, std::uint8_t snapCells,
    std::optional<float> fixedRotationDegrees,
    TerrainNativeFootprintContribution contribution = TerrainNativeFootprintContribution::StaticRgbAttributes);

} // namespace War3Map::MapData
