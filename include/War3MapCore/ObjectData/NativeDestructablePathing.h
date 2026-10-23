#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/TerrainNativeObjectPathing.h"

namespace War3Map::ObjectData {

// 只解释捕获者已持有的原字节，不读取磁盘、不生成通行、不授予保存资格。
// 要求W3B/W3D（缺失也显式提供）、两类SLK和DestructableMetaData五项基础来源。
// 可破坏物只接受明确非walkable的标准对象、原版覆盖或单层标准父继承；普通装饰物
// 接受标准对象、v2 Original覆盖及无父覆盖的一层Custom继承。原生记录缺少第六项实际
// DoodadMetaData时返回TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED，供可信owner
// 精确补捕获一次后完整重验；其它错误不能用于重试。category为明确非C、非_的
// 单可打印ASCII字符且没有原生姿态覆盖时，返回生效非空占地路径、标准fixedRot及2格吸附；
// owner仍须验证真实贴图、摆放注册及旧WPM，不能仅凭此结果允许保存。
[[nodiscard]] Result<MapData::TerrainNativeObjectPathing> InspectNativeDestructablePathing(
    std::span<const std::string_view> placedRawcodes, std::span<const ArchiveExactMember> sources,
    std::size_t maximumWorkingBytes);

// 特殊DOO共用实际DoodadData字段继承；其pathTex与普通对象分开返回，
// 不借用普通占地的category/fixedRot或RGB非零规则。
[[nodiscard]] Result<MapData::TerrainNativeObjectPathing> InspectNativeTerrainObjects(
    std::span<const std::string_view> placedRawcodes, std::span<const std::string_view> specialRawcodes,
    std::span<const ArchiveExactMember> sources, std::size_t maximumWorkingBytes);

// 无占地兼容入口使用同一解析器，仍拒绝任何非空占地；成功值为解析工作预留。
[[nodiscard]] Result<std::size_t> ValidateNativeDestructablePathing(
    std::span<const std::string_view> placedRawcodes, std::span<const ArchiveExactMember> sources,
    std::size_t maximumWorkingBytes);

} // namespace War3Map::ObjectData
