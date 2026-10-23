#pragma once

#include "War3MapCore/MapArchiveSession.h"

namespace War3Map::ObjectData {

// 只解释捕获者已持有的原字节，不读取磁盘、不生成通行、不授予保存资格。
// 要求W3B/W3D（缺失也显式提供）、两类SLK和DestructableMetaData五项基础来源。
// 可破坏物只接受明确非walkable的标准对象、原版覆盖或单层标准父继承；普通装饰物
// 只接受pathTex明确禁用占地的标准对象及v2 Original覆盖。后者缺少第六项实际
// DoodadMetaData时返回TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED，供可信owner
// 精确补捕获一次后完整重验；其它错误不能用于重试。成功值为解析工作预留。
[[nodiscard]] Result<std::size_t> ValidateNativeDestructablePathing(
    std::span<const std::string_view> placedRawcodes, std::span<const ArchiveExactMember> sources,
    std::size_t maximumWorkingBytes);

} // namespace War3Map::ObjectData
