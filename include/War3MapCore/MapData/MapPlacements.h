#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace War3Map::MapData {

struct PlacementReadLimits {
    std::size_t maxBytes = 128 * 1024 * 1024;
    std::size_t maxPlacements = 1000000;
    std::size_t maxNestedRecords = 1000000;
};

enum class PlacementRecordLayout { Classic, SkinId, Undetermined };

struct PlacementTransform {
    std::string rawcode; // 保留原始四字节，不假定所有 rawcode 都是可打印字符。
    std::optional<std::string> skinId; // 新布局在缩放后插入 ID4；经典布局不含此字段。
    std::int32_t variation = 0;
    std::array<float, 3> position{};
    float rotation = 0; // 弧度。
    std::array<float, 3> scale{};
};

struct PlacementItemChoice {
    std::string rawcode;
    std::int32_t chance = 0;
};

struct PlacementItemSet {
    std::vector<PlacementItemChoice> items;
};

struct PlacementInventoryItem {
    std::int32_t slot = 0;
    std::string rawcode;
};

struct PlacementAbility {
    std::string rawcode;
    bool autocast = false;
    std::int32_t level = 0;
};

struct RandomAnyUnit {
    std::int32_t level = 0; // 文件中为有符号小端 24 位，-1 保留为 -1。
    std::uint8_t itemClass = 0;
};

struct RandomGlobalTable {
    std::int32_t tableId = 0;
    std::int32_t column = 0;
};

struct RandomCustomTable {
    std::vector<PlacementItemChoice> choices;
};

// monostate 对应文件值 -1；其余三个分支分别对应 0、1、2。
using RandomUnitConfig = std::variant<std::monostate, RandomAnyUnit, RandomGlobalTable, RandomCustomTable>;

struct UnitPlacement : PlacementTransform {
    std::uint8_t flags = 0;
    std::int32_t owner = 0;
    std::array<std::uint8_t, 2> unknownOwnerBytes{};
    std::int32_t hitPoints = -1;
    std::int32_t mana = -1;
    std::int32_t droppedItemTable = -1;
    std::vector<PlacementItemSet> itemSets;
    std::int32_t gold = 0;
    float targetAcquisition = 0;
    std::int32_t heroLevel = 0;
    std::int32_t heroStrength = 0;
    std::int32_t heroAgility = 0;
    std::int32_t heroIntelligence = 0;
    std::vector<PlacementInventoryItem> inventory;
    std::vector<PlacementAbility> abilities;
    RandomUnitConfig random;
    std::int32_t teamColor = -1;
    std::int32_t waygateRegionId = -1;
    std::int32_t creationId = 0;
};

struct DoodadPlacement : PlacementTransform {
    std::uint8_t flags = 0;
    std::uint8_t lifePercent = 100;
    std::int32_t droppedItemTable = -1;
    std::vector<PlacementItemSet> itemSets;
    std::int32_t creationId = 0;
};

struct SpecialDoodadPlacement {
    std::string rawcode;
    std::int32_t variation = 0;
    std::array<std::int32_t, 2> position{}; // 地形网格坐标，不是普通实例的世界坐标。
};

class UnitPlacements;
class DoodadPlacements;

struct PlacementWireProjection {
    std::size_t byteLength = 0;
    std::size_t placementCount = 0;
    std::size_t nestedRecordCount = 0;
};

// 不展开或序列化候选；按经典 writer 的字段流计量原表及每次追加的完整源记录。
// 装饰物总数包含特殊尾；追加来源仅允许普通行，嵌套数量按 reader 的全局规则累计。
[[nodiscard]] Result<PlacementWireProjection> MeasureClassicPlacementWire(
    const UnitPlacements& placements, std::span<const std::uint32_t> appendedSourceRows = {},
    const PlacementReadLimits& limits = {});
[[nodiscard]] Result<PlacementWireProjection> MeasureClassicPlacementWire(
    const DoodadPlacements& placements, std::span<const std::uint32_t> appendedSourceRows = {},
    const PlacementReadLimits& limits = {});
// 新建记录沿用同一 writer 字段流；先计量原表与完整新记录，禁止复制后才检查嵌套/wire 预算。
[[nodiscard]] Result<PlacementWireProjection> MeasureClassicPlacementAppend(
    const UnitPlacements& placements, std::span<const UnitPlacement> appended,
    const PlacementReadLimits& limits = {});
[[nodiscard]] Result<PlacementWireProjection> MeasureClassicPlacementAppend(
    const DoodadPlacements& placements, std::span<const DoodadPlacement> appended,
    const PlacementReadLimits& limits = {});

// 经典后端读写的实例格式：TFT v8/subversion 11，以及混乱之治 v7/subversion 9
// （单位少掉落物品表编号与英雄三围，装饰物少掉落物品字段；写出时这些字段须为默认值）。
[[nodiscard]] constexpr bool IsClassicPlacementVersion(std::int32_t version, std::int32_t subversion) noexcept {
    return (version == 8 && subversion == 11) || (version == 7 && subversion == 9);
}

// 可编辑入口只识别上述经典格式的无皮肤布局。
// 带 SkinId 的 8/11 布局仅由下方版本化读取入口识别，不能经 Serialize 写出。
// 失败不返回半成品；调用方继续保留原始归档成员。
[[nodiscard]] Result<UnitPlacements> ParseUnitPlacements(
    std::span<const std::byte> bytes, const PlacementReadLimits& limits = {});
[[nodiscard]] Result<DoodadPlacements> ParseDoodadPlacements(
    std::span<const std::byte> bytes, const PlacementReadLimits& limits = {});
// 两种 8/11 记录布局独立解析至 EOF；无普通记录时布局未定，只允许原字节保留。
// 非空且两种布局均成立时明确报歧义。
[[nodiscard]] Result<UnitPlacements> ReadUnitPlacementsVersioned(
    std::span<const std::byte> bytes, const PlacementReadLimits& limits = {});
[[nodiscard]] Result<DoodadPlacements> ReadDoodadPlacementsVersioned(
    std::span<const std::byte> bytes, const PlacementReadLimits& limits = {});

class UnitPlacements {
public:
    std::int32_t version = 8;
    std::int32_t subversion = 11;
    PlacementRecordLayout recordLayout = PlacementRecordLayout::Classic;
    std::vector<UnitPlacement> units;

    [[nodiscard]] const std::vector<std::byte>& OriginalBytes() const noexcept { return original_; }
    // 只返回输入原字节，不将可读取字段冒充成已实现可编辑序列化器。
    [[nodiscard]] const std::vector<std::byte>& SerializeOriginal() const noexcept { return original_; }
    [[nodiscard]] Result<std::vector<std::byte>> Serialize(const PlacementReadLimits& limits = {}) const;

private:
    std::vector<std::byte> original_;
    friend Result<UnitPlacements> ParseUnitPlacements(std::span<const std::byte>, const PlacementReadLimits&);
    friend Result<UnitPlacements> ReadUnitPlacementsVersioned(std::span<const std::byte>, const PlacementReadLimits&);
};

class DoodadPlacements {
public:
    std::int32_t version = 8;
    std::int32_t subversion = 11;
    PlacementRecordLayout recordLayout = PlacementRecordLayout::Classic;
    std::int32_t specialVersion = 0;
    std::vector<DoodadPlacement> doodads;
    std::vector<SpecialDoodadPlacement> specialDoodads;

    [[nodiscard]] const std::vector<std::byte>& OriginalBytes() const noexcept { return original_; }
    [[nodiscard]] const std::vector<std::byte>& SerializeOriginal() const noexcept { return original_; }
    [[nodiscard]] Result<std::vector<std::byte>> Serialize(const PlacementReadLimits& limits = {}) const;

private:
    std::vector<std::byte> original_;
    friend Result<DoodadPlacements> ParseDoodadPlacements(std::span<const std::byte>, const PlacementReadLimits&);
    friend Result<DoodadPlacements> ReadDoodadPlacementsVersioned(std::span<const std::byte>, const PlacementReadLimits&);
};

} // namespace War3Map::MapData
