#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/TerrainNativeObjectPathing.h"

namespace War3Map::ObjectData {

// 只解释捕获者已持有的原字节，不读取磁盘、不生成通行、不授予保存资格。
// 要求W3B/W3D（缺失也显式提供）、两类SLK和DestructableMetaData五项基础来源。
// 非walkable可破坏物接受标准对象、原版覆盖或单层标准父继承；walkable桥接受唯一标准
// 对象、v2 Original及无父覆盖的一层Custom。原生记录只接受空记录或精确walkable/pathTex/
// fixedRot覆盖；覆盖字段须按实际DestructableMetaData唯一绑定到零索引标量并保持wire类型。
// Custom实际使用的walkable/覆盖字段不能带对象选择器，未覆盖字段才回退标准父；网格始终
// 按DOO新ID第二字节而非父ID解释。实例及完整DOO
// 冻结、实际占地和旧WPM仍由owner核实。普通装饰物
// 接受标准对象、v2 Original覆盖及无父覆盖的一层Custom继承。原生记录缺少第六项实际
// DoodadMetaData时返回TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED，供可信owner
// 精确补捕获一次后完整重验；其它错误不能用于重试。
// 已有vertR/G/B颜色记录可只读保留：实际唯一DoodadData/Int32、repeat=10/index=0、
// variation=1..10及第二索引0；仍完整核对codec、全部覆盖与原始元数据别名，
// 不开放通用物编变体写回，也不允许未知重复字段取得占地资格。普通category覆盖须绑定实际唯一
// String标量元数据，无覆盖沿标准父；缺格/空串或首字节'_'按DOO新ID第二字节回退，
// 其它值只取原始首字节；标准WESTRING_类别因未捕获本地化来源而拒绝。
// 支持可打印ASCII首字节：非C请求2格，C请求Core捕获实际
// fresh-startup类别目录后选择2/4格，不将字面C当成已证4格类别身份。
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
