#pragma once

#include "War3MapCore/MapData/TerrainShadowRebuild.h"

#include <string_view>

namespace War3Map::MapData {

struct TerrainShadowCasterModelInput {
    std::string_view path;
    std::span<const std::byte> bytes;
};

struct TerrainShadowCasterInstanceInput {
    std::size_t modelIndex = 0;
    // 宿主 ModelInstanceTransform 的行主序 3×4；不是原版内存中列主序
    // 48 字节的直接副本。来源捕获者须证明与实际摆放世界变换相同。
    std::array<float, 12> nativeMatrix{};
    // 实际DOO scale * 实际标准表defaultScale，各轴F32；原版球半径用
    // 三轴普通max。正式来源owner提供，不能从舍入后的矩阵反推。
    std::array<float, 3> actualScale{1, 1, 1};
};

struct TerrainShadowCasterFilterInputs {
    // 同Light startup owner实际捕获的[Terrain] MaxHeight/MinHeight。
    float maximumTerrainHeight = 0, minimumTerrainHeight = 0;
};

struct TerrainShadowCasterLimits : TerrainShadowRebuildLimits {
    std::size_t maximumModels = 4096;
    std::size_t maximumInstances = 65536;
    std::size_t maximumModelBytes = 128u * 1024u * 1024u;
    std::size_t maximumTriangles = 1024u * 1024u;
    // 自有局部及单实例世界三角形缓存，不含借用字节与 ParseMdx 内部存储。
    // ParseMdx 另遵守其固定输入/顶点/节点硬上限；编译每次只持有一个模型。
    std::size_t maximumGeometryBytes = 128u * 1024u * 1024u;
    std::size_t maximumCompiledBytes = 32u * 1024u * 1024u;
};

struct TerrainShadowCasterProjection {
    std::uint32_t width = 0, height = 0;
    // 仅模型射线命中的逻辑持久 bit0，值为0/1。560A20@560CD4置同一
    // bit0；562750 writer只消费bit0并写00/FF。正式写出须显式转换为FF，
    // 无损 SHD codec 不应更改其任意原字节往返合同。
    std::vector<std::uint8_t> casterShadowBits;
    std::size_t work = 0;
    std::size_t triangles = 0; // 各不同输入模型可见三角形总数，不乘实例数。
};

// 纯 CPU 静态模型射线内核。新鲜 ParseMdx + CompileMdxModel 后必须具备
// UsesNoControllerStatic资格；原selectionFlags必须0，静态GEOA alpha仅0/1。
// 每模型保留全部命中的最小有符号交距，再判断>0，不能提前丢弃负交距。
// worldOrigin是实际W3E西南偏移；receiver中心为origin+(x/y)*32+16。
//
// 本接口不证明原版边界球、区块候选筛选及资源集合完整性，不单独授予
// native等价烘焙或保存资格。调用者须在正式入口对这些证据缺口fail-closed。
// 不读写文件/地图，失败、取消或预算耗尽均不返回部分结果。
[[nodiscard]] Result<TerrainShadowCasterProjection> ProjectStaticTerrainShadowCasters(
    const TerrainShadowHeightSamples& samples, const std::array<float, 2>& worldOrigin,
    std::span<const TerrainShadowCasterModelInput> models,
    std::span<const TerrainShadowCasterInstanceInput> instances,
    const TerrainShadowCasterLimits& limits = {}, std::stop_token cancellation = {});

// 正式静态子集：额外重现45F3B0 MODL/首SEQS sphere、46F7F0有限线段
// 球筛选，以及560E60/560FB0的512区块/128 anchor halo候选筛选。
// 原版Y终点使用receiver blockX + directionY；此处保持该真实行为。
// 只接受有限正scale与已证明无控制器模型；资源集合、变换和profile的
// 实际来源完整性仍须由Services owner在保存前后校验。
[[nodiscard]] Result<TerrainShadowCasterProjection> ProjectFilteredStaticTerrainShadowCasters(
    const TerrainShadowHeightSamples& samples, const std::array<float, 2>& worldOrigin,
    std::span<const TerrainShadowCasterModelInput> models,
    std::span<const TerrainShadowCasterInstanceInput> instances,
    const TerrainShadowCasterFilterInputs& filters,
    const TerrainShadowCasterLimits& limits = {}, std::stop_token cancellation = {});

} // namespace War3Map::MapData
