#include "War3MapCore/MapData/MapPlacements.h"
#include "MapDataWriter.h"

#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace War3Map::MapData {
namespace {

// 与输出器共用字段遍历，不分配原表、追加记录或候选字节 buffer。
class PlacementWireSink {
public:
    explicit PlacementWireSink(const PlacementReadLimits& limits) : limits_(limits) {}

    void Byte(std::uint8_t) { AddBytes(1); }
    void I32(std::int32_t) { AddBytes(4); }

    void F32(float value) {
        if (!std::isfinite(value))
            throw Detail::WriteFailure{"MAP_WRITE_NONFINITE", "浮点字段不是有限数值"};
        AddBytes(4);
    }

    void Raw(std::string_view value, std::size_t expected) {
        if (value.size() != expected)
            throw Detail::WriteFailure{"MAP_WRITE_RAWCODE", "原始代码长度与格式不符"};
        AddBytes(value.size());
    }

    void Count(std::size_t value, std::size_t maxCount) {
        if (value > maxCount || value > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()))
            throw Detail::WriteFailure{"MAP_WRITE_COUNT", "记录数量超过格式或解析预算"};
        AddBytes(4);
    }

    void AddRecords(std::size_t count, bool nested) {
        auto& consumed = nested ? projection_.nestedRecordCount : projection_.placementCount;
        const auto limit = nested ? limits_.maxNestedRecords : limits_.maxPlacements;
        if (consumed > limit || count > limit - consumed)
            throw Detail::WriteFailure{"PLACEMENT_RECORD_LIMIT", "实例或嵌套记录总数超过读取预算"};
        consumed += count;
    }

    [[nodiscard]] PlacementWireProjection Finish() const noexcept { return projection_; }

private:
    void AddBytes(std::size_t count) {
        if (projection_.byteLength > limits_.maxBytes || count > limits_.maxBytes - projection_.byteLength)
            throw Detail::WriteFailure{"MAP_WRITE_BUDGET", "地图成员超过输出字节预算"};
        projection_.byteLength += count;
    }

    const PlacementReadLimits& limits_;
    PlacementWireProjection projection_;
};

void CountRecords(Detail::Writer& writer, std::size_t count, std::size_t maxCount, bool) {
    writer.Count(count, maxCount);
}

void CountRecords(PlacementWireSink& writer, std::size_t count, std::size_t maxCount, bool nested) {
    writer.Count(count, maxCount);
    writer.AddRecords(count, nested);
}

void RequireClassicLayout(PlacementRecordLayout layout) {
    if (layout != PlacementRecordLayout::Classic)
        throw Detail::WriteFailure{"PLACEMENT_SKIN_WRITE_UNSUPPORTED", "未证明是经典布局，当前只允许原字节保留"};
}

std::size_t AppendedPlacementCount(std::size_t originalCount, std::size_t appendedCount) {
    if (appendedCount > (std::numeric_limits<std::size_t>::max)() - originalCount)
        throw Detail::WriteFailure{"MAP_WRITE_COUNT", "追加实例数量超过计量容量"};
    return originalCount + appendedCount;
}

template <typename Sink>
void Header(Sink& writer, std::int32_t version, std::int32_t subversion,
    std::size_t count, const PlacementReadLimits& limits) {
    if (!IsClassicPlacementVersion(version, subversion))
        throw Detail::WriteFailure{"PLACEMENT_VERSION_UNSUPPORTED", "仅支持经典 TFT v8/subversion 11 与混乱之治 v7/subversion 9 写出"};
    writer.Raw("W3do", 4);
    writer.I32(version);
    writer.I32(subversion);
    CountRecords(writer, count, limits.maxPlacements, false);
}

template <typename Sink>
void Transform(Sink& writer, const PlacementTransform& value) {
    writer.Raw(value.rawcode, 4);
    writer.I32(value.variation);
    for (const auto coordinate : value.position) writer.F32(coordinate);
    writer.F32(value.rotation);
    for (const auto coordinate : value.scale) writer.F32(coordinate);
}

template <typename Sink>
void ItemChoices(Sink& writer, const std::vector<PlacementItemChoice>& choices,
    const PlacementReadLimits& limits) {
    CountRecords(writer, choices.size(), limits.maxNestedRecords, true);
    for (const auto& choice : choices) {
        writer.Raw(choice.rawcode, 4);
        writer.I32(choice.chance);
    }
}

template <typename Sink>
void ItemSets(Sink& writer, const std::vector<PlacementItemSet>& sets,
    const PlacementReadLimits& limits) {
    CountRecords(writer, sets.size(), limits.maxNestedRecords, true);
    for (const auto& set : sets) ItemChoices(writer, set.items, limits);
}

template <typename Sink>
void Random(Sink& writer, const RandomUnitConfig& random,
    const PlacementReadLimits& limits) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            writer.I32(-1);
        } else if constexpr (std::is_same_v<T, RandomAnyUnit>) {
            if (value.level < -8388608 || value.level > 8388607)
                throw Detail::WriteFailure{"PLACEMENT_RANDOM_LEVEL", "随机单位等级超出有符号 24 位格式范围"};
            writer.I32(0);
            const auto bits = std::bit_cast<std::uint32_t>(value.level);
            for (unsigned shift = 0; shift < 24; shift += 8)
                writer.Byte(static_cast<std::uint8_t>((bits >> shift) & 0xffu));
            writer.Byte(value.itemClass);
        } else if constexpr (std::is_same_v<T, RandomGlobalTable>) {
            writer.I32(1);
            writer.I32(value.tableId);
            writer.I32(value.column);
        } else {
            writer.I32(2);
            ItemChoices(writer, value.choices, limits);
        }
    }, random);
}

// 混乱之治格式（v7）写不下掉落物品表编号与英雄三围：非默认值明确拒绝，不悄悄丢弃。
bool Roc(std::int32_t version) { return version == 7; }

template <typename Sink>
void Unit(Sink& writer, const UnitPlacement& value,
    const PlacementReadLimits& limits, bool roc) {
    if (value.skinId)
        throw Detail::WriteFailure{"PLACEMENT_SKIN_WRITE_UNSUPPORTED", "不能以经典布局写出带 SkinId 的单位"};
    Transform(writer, value);
    if (value.flags >= 0x20)
        throw Detail::WriteFailure{"PLACEMENT_LAYOUT_UNSUPPORTED", "单位 flags 不属于当前经典布局"};
    writer.Byte(value.flags);
    writer.I32(value.owner);
    for (const auto byte : value.unknownOwnerBytes) writer.Byte(byte);
    writer.I32(value.hitPoints);
    writer.I32(value.mana);
    if (roc && (value.droppedItemTable != -1 || value.heroStrength != 0 || value.heroAgility != 0 || value.heroIntelligence != 0))
        throw Detail::WriteFailure{"PLACEMENT_ROC_FIELD", "混乱之治格式的单位没有掉落物品表和英雄力量/敏捷/智力字段"};
    if (!roc) writer.I32(value.droppedItemTable);
    ItemSets(writer, value.itemSets, limits);
    writer.I32(value.gold);
    writer.F32(value.targetAcquisition);
    writer.I32(value.heroLevel);
    if (!roc) {
        writer.I32(value.heroStrength);
        writer.I32(value.heroAgility);
        writer.I32(value.heroIntelligence);
    }
    CountRecords(writer, value.inventory.size(), limits.maxNestedRecords, true);
    for (const auto& item : value.inventory) {
        writer.I32(item.slot);
        writer.Raw(item.rawcode, 4);
    }
    CountRecords(writer, value.abilities.size(), limits.maxNestedRecords, true);
    for (const auto& ability : value.abilities) {
        writer.Raw(ability.rawcode, 4);
        writer.I32(ability.autocast ? 1 : 0);
        writer.I32(ability.level);
    }
    Random(writer, value.random, limits);
    writer.I32(value.teamColor);
    writer.I32(value.waygateRegionId);
    writer.I32(value.creationId);
}

template <typename Sink>
void Doodad(Sink& writer, const DoodadPlacement& value,
    const PlacementReadLimits& limits, bool roc) {
    if (value.skinId)
        throw Detail::WriteFailure{"PLACEMENT_SKIN_WRITE_UNSUPPORTED", "不能以经典布局写出带 SkinId 的装饰物"};
    Transform(writer, value);
    if (value.flags >= 0x20)
        throw Detail::WriteFailure{"PLACEMENT_LAYOUT_UNSUPPORTED", "装饰物 flags 不属于当前经典布局"};
    writer.Byte(value.flags);
    writer.Byte(value.lifePercent);
    if (roc && (value.droppedItemTable != -1 || !value.itemSets.empty()))
        throw Detail::WriteFailure{"PLACEMENT_ROC_FIELD", "混乱之治格式的装饰物没有掉落物品字段"};
    if (!roc) {
        writer.I32(value.droppedItemTable);
        ItemSets(writer, value.itemSets, limits);
    }
    writer.I32(value.creationId);
}

template <typename Sink>
void SpecialDoodads(Sink& writer, const DoodadPlacements& placements,
    const PlacementReadLimits& limits) {
    if (placements.specialVersion != 0)
        throw Detail::WriteFailure{"PLACEMENT_SPECIAL_VERSION", "不支持该特殊装饰物格式版本"};
    writer.I32(placements.specialVersion);
    CountRecords(writer, placements.specialDoodads.size(), limits.maxPlacements, false);
    for (const auto& special : placements.specialDoodads) {
        writer.Raw(special.rawcode, 4);
        writer.I32(special.variation);
        for (const auto coordinate : special.position) writer.I32(coordinate);
    }
}

template <typename T>
Result<T> Failure(std::string_view member, const Detail::WriteFailure& error) {
    return Result<T>::Failure("MapPlacements.Write", error.code,
        std::string(member) + "：" + error.message);
}

template<class Table, class Record>
Result<PlacementWireProjection> MeasureAppend(const Table& placements, std::span<const Record> appended,
    const PlacementReadLimits& limits) {
    constexpr bool units = std::is_same_v<Record, UnitPlacement>;
    constexpr auto member = units ? "war3mapUnits.doo" : "war3map.doo";
    try {
        RequireClassicLayout(placements.recordLayout);
        PlacementWireSink writer(limits);
        if constexpr (units) {
            Header(writer, placements.version, placements.subversion,
                AppendedPlacementCount(placements.units.size(), appended.size()), limits);
            for (const auto& record : placements.units) Unit(writer, record, limits, Roc(placements.version));
            for (const auto& record : appended) Unit(writer, record, limits, Roc(placements.version));
        } else {
            Header(writer, placements.version, placements.subversion,
                AppendedPlacementCount(placements.doodads.size(), appended.size()), limits);
            for (const auto& record : placements.doodads) Doodad(writer, record, limits, Roc(placements.version));
            for (const auto& record : appended) Doodad(writer, record, limits, Roc(placements.version));
            SpecialDoodads(writer, placements, limits);
        }
        return Result<PlacementWireProjection>::Success(writer.Finish());
    } catch (const Detail::WriteFailure& error) {
        return Failure<PlacementWireProjection>(member, error);
    } catch (const std::bad_alloc&) {
        return Result<PlacementWireProjection>::Failure("MapPlacements.Write", "PLACEMENT_ALLOCATION_FAILED",
            "新增摆放计量诊断内存分配失败");
    } catch (const std::length_error&) {
        return Result<PlacementWireProjection>::Failure("MapPlacements.Write", "PLACEMENT_CONTAINER_LIMIT",
            "新增摆放计量诊断超过容器容量");
    }
}

} // namespace

Result<PlacementWireProjection> MeasureClassicPlacementAppend(const UnitPlacements& placements,
    std::span<const UnitPlacement> appended, const PlacementReadLimits& limits) {
    return MeasureAppend(placements, appended, limits);
}
Result<PlacementWireProjection> MeasureClassicPlacementAppend(const DoodadPlacements& placements,
    std::span<const DoodadPlacement> appended, const PlacementReadLimits& limits) {
    return MeasureAppend(placements, appended, limits);
}

Result<PlacementWireProjection> MeasureClassicPlacementWire(const UnitPlacements& placements,
    std::span<const std::uint32_t> appendedSourceRows, const PlacementReadLimits& limits) {
    try {
        RequireClassicLayout(placements.recordLayout);
        for (const auto row : appendedSourceRows) {
            if (static_cast<std::size_t>(row) >= placements.units.size())
                throw Detail::WriteFailure{"PLACEMENT_PROJECTION_SOURCE_RANGE", "追加单位来源行不在原表范围内"};
        }
        PlacementWireSink writer(limits);
        Header(writer, placements.version, placements.subversion,
            AppendedPlacementCount(placements.units.size(), appendedSourceRows.size()), limits);
        for (const auto& unit : placements.units) Unit(writer, unit, limits, Roc(placements.version));
        for (const auto row : appendedSourceRows) Unit(writer, placements.units[row], limits, Roc(placements.version));
        return Result<PlacementWireProjection>::Success(writer.Finish());
    } catch (const Detail::WriteFailure& error) {
        return Failure<PlacementWireProjection>("war3mapUnits.doo", error);
    } catch (const std::bad_alloc&) {
        return Result<PlacementWireProjection>::Failure("MapPlacements.Write", "PLACEMENT_ALLOCATION_FAILED",
            "单位实例计量诊断内存分配失败");
    } catch (const std::length_error&) {
        return Result<PlacementWireProjection>::Failure("MapPlacements.Write", "PLACEMENT_CONTAINER_LIMIT",
            "单位实例计量诊断超过容器容量");
    }
}

Result<PlacementWireProjection> MeasureClassicPlacementWire(const DoodadPlacements& placements,
    std::span<const std::uint32_t> appendedSourceRows, const PlacementReadLimits& limits) {
    try {
        RequireClassicLayout(placements.recordLayout);
        for (const auto row : appendedSourceRows) {
            if (static_cast<std::size_t>(row) >= placements.doodads.size()) {
                if (static_cast<std::size_t>(row) - placements.doodads.size() < placements.specialDoodads.size())
                    throw Detail::WriteFailure{"PLACEMENT_SPECIAL_SOURCE_UNSUPPORTED", "不能将特殊装饰物作为普通追加来源"};
                throw Detail::WriteFailure{"PLACEMENT_PROJECTION_SOURCE_RANGE", "追加装饰物来源行不在原表范围内"};
            }
        }
        PlacementWireSink writer(limits);
        Header(writer, placements.version, placements.subversion,
            AppendedPlacementCount(placements.doodads.size(), appendedSourceRows.size()), limits);
        for (const auto& doodad : placements.doodads) Doodad(writer, doodad, limits, Roc(placements.version));
        for (const auto row : appendedSourceRows) Doodad(writer, placements.doodads[row], limits, Roc(placements.version));
        SpecialDoodads(writer, placements, limits);
        return Result<PlacementWireProjection>::Success(writer.Finish());
    } catch (const Detail::WriteFailure& error) {
        return Failure<PlacementWireProjection>("war3map.doo", error);
    } catch (const std::bad_alloc&) {
        return Result<PlacementWireProjection>::Failure("MapPlacements.Write", "PLACEMENT_ALLOCATION_FAILED",
            "装饰物实例计量诊断内存分配失败");
    } catch (const std::length_error&) {
        return Result<PlacementWireProjection>::Failure("MapPlacements.Write", "PLACEMENT_CONTAINER_LIMIT",
            "装饰物实例计量诊断超过容器容量");
    }
}

Result<std::vector<std::byte>> UnitPlacements::Serialize(const PlacementReadLimits& limits) const {
    try {
        RequireClassicLayout(recordLayout);
        Detail::Writer writer(limits.maxBytes);
        Header(writer, version, subversion, units.size(), limits);
        for (const auto& unit : units) Unit(writer, unit, limits, Roc(version));
        auto output = std::move(writer).Finish();
        const auto reparsed = ParseUnitPlacements(output, limits);
        if (!reparsed.Succeeded())
            return Result<std::vector<std::byte>>::Failure("MapPlacements.Write", "PLACEMENT_OUTPUT_INVALID",
                "单位实例写出后不能按同一经典布局读取");
        if (output == original_) return Result<std::vector<std::byte>>::Success(original_);
        return Result<std::vector<std::byte>>::Success(std::move(output));
    } catch (const Detail::WriteFailure& error) {
        return Failure<std::vector<std::byte>>("war3mapUnits.doo", error);
    } catch (const std::bad_alloc&) {
        return Result<std::vector<std::byte>>::Failure("MapPlacements.Write", "PLACEMENT_ALLOCATION_FAILED",
            "单位实例写出内存分配失败");
    } catch (const std::length_error&) {
        return Result<std::vector<std::byte>>::Failure("MapPlacements.Write", "PLACEMENT_CONTAINER_LIMIT",
            "单位实例写出超过容器容量");
    }
}

Result<std::vector<std::byte>> DoodadPlacements::Serialize(const PlacementReadLimits& limits) const {
    try {
        RequireClassicLayout(recordLayout);
        Detail::Writer writer(limits.maxBytes);
        Header(writer, version, subversion, doodads.size(), limits);
        for (const auto& doodad : doodads) Doodad(writer, doodad, limits, Roc(version));
        SpecialDoodads(writer, *this, limits);
        auto output = std::move(writer).Finish();
        const auto reparsed = ParseDoodadPlacements(output, limits);
        if (!reparsed.Succeeded())
            return Result<std::vector<std::byte>>::Failure("MapPlacements.Write", "PLACEMENT_OUTPUT_INVALID",
                "装饰物写出后不能按同一经典布局读取");
        if (output == original_) return Result<std::vector<std::byte>>::Success(original_);
        return Result<std::vector<std::byte>>::Success(std::move(output));
    } catch (const Detail::WriteFailure& error) {
        return Failure<std::vector<std::byte>>("war3map.doo", error);
    } catch (const std::bad_alloc&) {
        return Result<std::vector<std::byte>>::Failure("MapPlacements.Write", "PLACEMENT_ALLOCATION_FAILED",
            "装饰物写出内存分配失败");
    } catch (const std::length_error&) {
        return Result<std::vector<std::byte>>::Failure("MapPlacements.Write", "PLACEMENT_CONTAINER_LIMIT",
            "装饰物写出超过容器容量");
    }
}

} // namespace War3Map::MapData
