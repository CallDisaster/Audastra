#pragma once

#include "War3MapCore/MapDocument.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 地图数据字段目录：UI、CLI、MCP 与 Agent 共用的“类别 / 行 / 字段”寻址方式。
// 新增可编辑字段只在目录里登记一项（读、写、取值约束），不再为每个字段新建请求结构、
// 命令行参数或 MCP 模式；保存服务按同一套前值绑定与回读核对处理所有字段。
namespace War3Map::MapData {

// 成员类别的稳定英文标识：settings/units/doodads/regions/cameras/sounds/strings。
[[nodiscard]] std::string_view MapDataKindId(MapDataKind kind) noexcept;
[[nodiscard]] std::optional<MapDataKind> ParseMapDataKindId(std::string_view text) noexcept;

enum class MapFieldType : std::uint8_t {
    Int32,
    UInt32,
    Float32,      // 按 float 原始位型绑定
    AngleRadians, // 原件为弧度 float；自然输入与显示用角度
    Bytes,        // 原始字节，可能是 TRIGSTR 引用或非 UTF-8 文本
};

[[nodiscard]] std::string_view MapFieldTypeId(MapFieldType type) noexcept;

// 字段值。整数看 integer；Float32/AngleRadians 只看 floatBits（逐位比较）；Bytes 看 bytes。
struct MapFieldValue {
    MapFieldType type = MapFieldType::Int32;
    std::int64_t integer = 0;
    std::uint32_t floatBits = 0;
    std::string bytes;

    [[nodiscard]] bool operator==(const MapFieldValue&) const = default;
};

enum class MapFieldQuantity : std::uint8_t { None, PercentOfMaximum, Absolute };

// writable 表示产品已开放的字段；原版保存和游戏验收状态另见回执，不能由此标志推定。
struct MapFieldDescriptor {
    MapDataKind kind = MapDataKind::Settings;
    std::string_view id;         // 稳定英文标识
    std::string_view label;      // 中文显示名
    MapFieldType type = MapFieldType::Int32;
    bool writable = false;
    std::string_view constraint; // 写入约束说明；只读字段为空
    MapFieldQuantity quantity = MapFieldQuantity::None;
};

[[nodiscard]] std::span<const MapFieldDescriptor> MapFieldCatalog();
[[nodiscard]] std::span<const MapFieldDescriptor> MapFieldCatalog(MapDataKind kind);
[[nodiscard]] const MapFieldDescriptor* FindMapField(MapDataKind kind, std::string_view id);
// 校验目录中可写字段的类型和取值；不修改文档，也不替代保存时的行身份/前值绑定。
[[nodiscard]] Status ValidateMapFieldValue(MapDataKind kind, std::string_view fieldId,
    const MapFieldValue& value);
// 区域四边必须是目录允许的坐标，且左 < 右、下 < 上；可在整行修改完成后调用。
[[nodiscard]] Status ValidateMapFieldRegionBounds(float left, float bottom, float right, float top);

// 精确原值文本（前值绑定用）：整数为十进制；浮点与角度为 "f32:" 加 8 位小写十六进制位型；
// 字节为 "hex:" 加小写十六进制。
[[nodiscard]] std::string FormatMapFieldRaw(const MapFieldValue& value);
[[nodiscard]] Result<MapFieldValue> ParseMapFieldRaw(MapFieldType type, std::string_view text);
// 新值文本：接受精确原值文本，或按字段类型的自然值（整数、十进制小数、角度、UTF-8 字面量）。
[[nodiscard]] Result<MapFieldValue> ParseMapFieldInput(const MapFieldDescriptor& field, std::string_view text);
// 编辑界面的数量输入：无后缀为绝对值，% 为比例，默认/-1 保留继承。maximum 必须来自
// 已知物编上限；缺失时拒绝需要换算的输入。绝对 mana 可保留原值并报告运行期夹取，
// 不是已预测的游戏数值。HP 按原格式取整到百分之一，mana 取整到整数。
struct MapFieldQuantityInput {
    std::optional<double> maximum;
};
[[nodiscard]] Result<MapFieldValue> ParseMapFieldInput(const MapFieldDescriptor& field,
    std::string_view text, const MapFieldQuantityInput& quantity);
// 显示用数值：整数、有限浮点、角度（度）；字节或非有限值为空。
[[nodiscard]] std::optional<double> MapFieldNumber(const MapFieldValue& value);
// 声音 W3S 的 -1 / f32:4f800000 是默认哨兵；raw 仍须保留以供前值绑定。
[[nodiscard]] bool IsMapFieldDefault(const MapFieldDescriptor& field, const MapFieldValue& value) noexcept;
// Agent 数值展示：默认哨兵为空；精确前值仍由 FormatMapFieldRaw 提供。
[[nodiscard]] std::optional<double> MapFieldDisplayNumber(const MapFieldDescriptor& field,
    const MapFieldValue& value);

// 行号只在当前成员字节内有效。行身份令牌是 ASCII 字符串，由行内身份字段组成，
// 写入请求须原样回传，用于发现“行号对了但指向的不是同一行”的过期请求。
[[nodiscard]] Result<std::size_t> MapFieldRowCount(const MapDocument& document, MapDataKind kind);
[[nodiscard]] Result<std::string> MapFieldRowKey(const MapDocument& document, MapDataKind kind, std::size_t row);
[[nodiscard]] Result<MapFieldValue> ReadMapField(const MapDocument& document, MapDataKind kind,
    std::size_t row, const MapFieldDescriptor& field);

// 只检查目录能力与实际成员布局；不替代成员状态、前值与完整保存资格检查。
[[nodiscard]] bool MapFieldLayoutAllowsWrite(const MapDocument& document, const MapFieldDescriptor& field);
// 按实际行核对写能力：单位 XYZ 仅允许 Ready 的经典 8/11 普通单位；起点 sloc
// 的位置须经 W3I 关联编辑，不能由单成员字段命令改写。其他字段保留原目录能力。
[[nodiscard]] bool MapFieldRowAllowsWrite(const MapDocument& document, std::size_t row,
    const MapFieldDescriptor& field);

// 可写表副本，只含本次请求涉及的类别。
struct MapFieldTables {
    std::optional<MapSettings> settings;
    std::optional<UnitPlacements> units;
    std::optional<RegionsDocument> regions;
    std::optional<CamerasDocument> cameras;
    std::optional<SoundsDocument> sounds;
    std::optional<StringTableDocument> strings;
};

[[nodiscard]] Result<MapFieldValue> ReadMapField(const MapFieldTables& tables, MapDataKind kind,
    std::size_t row, const MapFieldDescriptor& field);
// 按成员原生格式序列化 / 解析单个类别，供保存前后逐字节核对。
[[nodiscard]] Result<std::vector<std::byte>> SerializeMapFieldTable(const MapDocument& document, MapDataKind kind);
[[nodiscard]] Result<std::vector<std::byte>> SerializeMapFieldTable(const MapFieldTables& tables, MapDataKind kind);
[[nodiscard]] Result<MapFieldTables> ParseMapFieldTable(MapDataKind kind, std::span<const std::byte> bytes);

struct MapFieldEdit {
    MapDataKind kind = MapDataKind::Settings;
    std::size_t row = 0;
    std::string expectedRowKey;
    std::string field;
    MapFieldValue expected;
    MapFieldValue value;
};

inline constexpr std::size_t kMaxMapFieldEdits = 32;

// 从源文档复制涉及的表并按顺序施加。逐项核对字段存在且可写、行在范围内且行身份一致、
// 前值逐位一致、新值满足字段约束且不同于前值、同一行同一字段不重复；全部施加后再检查
// 行级约束（区域须 left<right、bottom<top）。任一失败整体拒绝，不产生部分结果。
[[nodiscard]] Result<MapFieldTables> ApplyMapFieldEdits(const MapDocument& source,
    std::span<const MapFieldEdit> edits, std::size_t maximumEdits = kMaxMapFieldEdits);

} // namespace War3Map::MapData
