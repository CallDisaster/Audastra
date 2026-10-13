#include "War3MapCore/MapData/MapPlacements.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <utility>

namespace War3Map::MapData {
namespace {

// 仅参考公开格式字段顺序，解析边界、预算和错误处理为本工程独立实现。
// 格式对照：War3Net 的 Widget/UnitData、MapDoodads 及 mdx-m3-viewer 的 unitsdoo、doo。
struct PlacementError {
    Diagnostic diagnostic;
};

class PlacementReader {
public:
    PlacementReader(std::span<const std::byte> bytes, const PlacementReadLimits& limits,
        const char* member) : bytes_(bytes), limits_(limits), member_(member) {
        if (bytes.size() > limits.maxBytes) Fail("PLACEMENT_BYTE_LIMIT", "实例成员超过读取字节预算");
    }

    [[noreturn]] void Fail(const char* code, std::string message) const {
        throw PlacementError{{DiagnosticSeverity::Error, "MapPlacements", code, std::move(message),
            OriginSpan{member_, offset_, offset_ < bytes_.size() ? 1u : 0u, {}}}};
    }

    std::uint8_t Byte() {
        Need(1);
        return std::to_integer<std::uint8_t>(bytes_[offset_++]);
    }

    std::uint32_t UInt() {
        Need(4);
        std::uint32_t result = 0;
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            result |= static_cast<std::uint32_t>(Byte()) << shift;
        }
        return result;
    }

    std::int32_t Int() { return std::bit_cast<std::int32_t>(UInt()); }

    float Float() {
        const auto result = std::bit_cast<float>(UInt());
        if (!std::isfinite(result)) Fail("PLACEMENT_NONFINITE", "实例浮点字段不是有限数值");
        return result;
    }

    std::string Rawcode() {
        Need(4);
        std::string result(4, '\0');
        for (auto& value : result) value = static_cast<char>(Byte());
        return result;
    }

    std::size_t Count(std::size_t minimumWidth, bool nested = true) {
        const auto raw = Int();
        if (raw < 0) Fail("PLACEMENT_NEGATIVE_COUNT", "实例或子记录数量不能为负数");
        const auto count = static_cast<std::size_t>(raw);
        auto& consumed = nested ? nestedCount_ : placementCount_;
        const auto limit = nested ? limits_.maxNestedRecords : limits_.maxPlacements;
        if (consumed > limit || count > limit - consumed) {
            Fail("PLACEMENT_RECORD_LIMIT", "实例或嵌套记录总数超过读取预算");
        }
        if (count > (bytes_.size() - offset_) / minimumWidth) {
            Fail("PLACEMENT_TRUNCATED", "实例数量超出剩余成员字节能够容纳的记录数");
        }
        consumed += count;
        return count;
    }

    // 返回是否为混乱之治格式 v7/subversion 9（只有经典布局，没有皮肤扩展）。
    bool Header(std::int32_t& version, std::int32_t& subversion, PlacementRecordLayout layout) {
        if (Rawcode() != "W3do") Fail("PLACEMENT_MAGIC", "实例成员签名不是 W3do");
        version = Int();
        subversion = Int();
        const bool roc = version == 7 && subversion == 9;
        if (!IsClassicPlacementVersion(version, subversion) || (roc && layout != PlacementRecordLayout::Classic)) {
            Fail("PLACEMENT_VERSION_UNSUPPORTED",
                "当前只支持经典 TFT 实例格式 v8/subversion 11 与混乱之治 v7/subversion 9；原成员保持不变");
        }
        return roc;
    }

    std::uint8_t Flags() {
        const auto flags = Byte();
        // Reforged 在此位置插入 skin rawcode，但不更改版本号。本实现只接受经典
        // 布局，不把可打印 skin 字节误作经典 flags 后继续错位读取。
        if (flags >= 0x20) {
            Fail("PLACEMENT_LAYOUT_UNSUPPORTED", "实例 flags 不属于当前经典布局，可能包含皮肤扩展；原成员保持不变");
        }
        return flags;
    }

    void End() const {
        if (offset_ != bytes_.size()) {
            Fail("PLACEMENT_TRAILING_DATA", "实例结构之后仍有未识别字节，不能确认该成员格式");
        }
    }

private:
    void Need(std::size_t length) const {
        if (length > bytes_.size() - offset_) Fail("PLACEMENT_TRUNCATED", "实例成员在字段中间截断");
    }

    std::span<const std::byte> bytes_;
    const PlacementReadLimits& limits_;
    const char* member_;
    std::size_t offset_ = 0;
    std::size_t placementCount_ = 0;
    std::size_t nestedCount_ = 0;
};

void ReadTransform(PlacementReader& reader, PlacementTransform& result) {
    result.rawcode = reader.Rawcode();
    result.variation = reader.Int();
    for (auto& coordinate : result.position) coordinate = reader.Float();
    result.rotation = reader.Float();
    for (auto& scale : result.scale) scale = reader.Float();
}

std::vector<PlacementItemChoice> ReadChoices(PlacementReader& reader) {
    const auto count = reader.Count(8);
    std::vector<PlacementItemChoice> choices;
    choices.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        PlacementItemChoice choice;
        choice.rawcode = reader.Rawcode();
        choice.chance = reader.Int();
        choices.push_back(std::move(choice));
    }
    return choices;
}

std::vector<PlacementItemSet> ReadItemSets(PlacementReader& reader) {
    const auto count = reader.Count(4);
    std::vector<PlacementItemSet> sets;
    sets.reserve(count);
    for (std::size_t index = 0; index < count; ++index) sets.push_back({ReadChoices(reader)});
    return sets;
}

RandomUnitConfig ReadRandom(PlacementReader& reader) {
    const auto mode = reader.Int();
    switch (mode) {
    case -1:
        return std::monostate{};
    case 0: {
        std::uint32_t level = 0;
        for (unsigned int shift = 0; shift < 24; shift += 8) {
            level |= static_cast<std::uint32_t>(reader.Byte()) << shift;
        }
        if ((level & 0x800000u) != 0) level |= 0xff000000u;
        return RandomAnyUnit{std::bit_cast<std::int32_t>(level), reader.Byte()};
    }
    case 1: {
        RandomGlobalTable table;
        table.tableId = reader.Int();
        table.column = reader.Int();
        return table;
    }
    case 2:
        return RandomCustomTable{ReadChoices(reader)};
    default:
        reader.Fail("PLACEMENT_RANDOM_MODE", "随机单位配置分支未被识别，不能确定后续字段边界");
    }
}

// 混乱之治（v7）单位没有掉落物品表编号和英雄三围；装饰物没有掉落物品表与物品组。
// 依据：游戏自带 45 张 .w3m 的实例文件按此布局逐字节读到结尾，其他组合均不成立。
UnitPlacement ReadUnit(PlacementReader& reader, PlacementRecordLayout layout, bool roc) {
    UnitPlacement result;
    ReadTransform(reader, result);
    if (layout == PlacementRecordLayout::SkinId) result.skinId = reader.Rawcode();
    result.flags = reader.Flags();
    result.owner = reader.Int();
    for (auto& byte : result.unknownOwnerBytes) byte = reader.Byte();
    result.hitPoints = reader.Int();
    result.mana = reader.Int();
    if (!roc) result.droppedItemTable = reader.Int();
    result.itemSets = ReadItemSets(reader);
    result.gold = reader.Int();
    result.targetAcquisition = reader.Float();
    result.heroLevel = reader.Int();
    if (!roc) {
        result.heroStrength = reader.Int();
        result.heroAgility = reader.Int();
        result.heroIntelligence = reader.Int();
    }
    const auto inventoryCount = reader.Count(8);
    result.inventory.reserve(inventoryCount);
    for (std::size_t index = 0; index < inventoryCount; ++index) {
        PlacementInventoryItem item;
        item.slot = reader.Int();
        item.rawcode = reader.Rawcode();
        result.inventory.push_back(std::move(item));
    }
    const auto abilityCount = reader.Count(12);
    result.abilities.reserve(abilityCount);
    for (std::size_t index = 0; index < abilityCount; ++index) {
        PlacementAbility ability;
        ability.rawcode = reader.Rawcode();
        const auto autocast = reader.Int();
        if (autocast != 0 && autocast != 1) {
            reader.Fail("PLACEMENT_INVALID_BOOLEAN", "技能自动施放标记必须为 0 或 1");
        }
        ability.autocast = autocast != 0;
        ability.level = reader.Int();
        result.abilities.push_back(std::move(ability));
    }
    result.random = ReadRandom(reader);
    result.teamColor = reader.Int();
    result.waygateRegionId = reader.Int();
    result.creationId = reader.Int();
    return result;
}

template <typename T>
Result<T> Failed(PlacementError error) {
    Result<T> result;
    result.diagnostics.push_back(std::move(error.diagnostic));
    return result;
}

template <typename T>
Result<T> LayoutFailure(std::span<const std::byte> bytes, const char* member,
    const char* code, std::string message) {
    Result<T> result;
    result.diagnostics.push_back({DiagnosticSeverity::Error, "MapPlacements", code, std::move(message),
        OriginSpan{member, std::min<std::size_t>(16, bytes.size()), bytes.size() > 16 ? 1u : 0u, {}}});
    return result;
}

Result<UnitPlacements> ParseUnitsLayout(std::span<const std::byte> bytes,
    const PlacementReadLimits& limits, PlacementRecordLayout layout) {
    try {
        PlacementReader reader(bytes, limits, "war3mapUnits.doo");
        UnitPlacements document;
        document.recordLayout = layout;
        const bool roc = reader.Header(document.version, document.subversion, layout);
        const auto count = reader.Count(layout == PlacementRecordLayout::SkinId ? 111 : roc ? 91 : 107, false);
        document.units.reserve(count);
        for (std::size_t index = 0; index < count; ++index) document.units.push_back(ReadUnit(reader, layout, roc));
        reader.End();
        return Result<UnitPlacements>::Success(std::move(document));
    } catch (PlacementError& error) {
        return Failed<UnitPlacements>(std::move(error));
    } catch (const std::bad_alloc&) {
        return Result<UnitPlacements>::Failure("MapPlacements", "PLACEMENT_MEMORY_LIMIT", "单位实例读取无法分配所需内存");
    }
}

Result<DoodadPlacements> ParseDoodadsLayout(std::span<const std::byte> bytes,
    const PlacementReadLimits& limits, PlacementRecordLayout layout) {
    try {
        PlacementReader reader(bytes, limits, "war3map.doo");
        DoodadPlacements document;
        document.recordLayout = layout;
        const bool roc = reader.Header(document.version, document.subversion, layout);
        const auto count = reader.Count(layout == PlacementRecordLayout::SkinId ? 54 : roc ? 42 : 50, false);
        document.doodads.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            DoodadPlacement doodad;
            ReadTransform(reader, doodad);
            if (layout == PlacementRecordLayout::SkinId) doodad.skinId = reader.Rawcode();
            doodad.flags = reader.Flags();
            doodad.lifePercent = reader.Byte();
            if (!roc) {
                doodad.droppedItemTable = reader.Int();
                doodad.itemSets = ReadItemSets(reader);
            }
            doodad.creationId = reader.Int();
            document.doodads.push_back(std::move(doodad));
        }
        document.specialVersion = reader.Int();
        if (document.specialVersion != 0) {
            reader.Fail("PLACEMENT_SPECIAL_VERSION", "特殊装饰物格式版本未被支持，原成员保持不变");
        }
        const auto specialCount = reader.Count(16, false);
        document.specialDoodads.reserve(specialCount);
        for (std::size_t index = 0; index < specialCount; ++index) {
            SpecialDoodadPlacement doodad;
            doodad.rawcode = reader.Rawcode();
            doodad.variation = reader.Int();
            for (auto& coordinate : doodad.position) coordinate = reader.Int();
            document.specialDoodads.push_back(std::move(doodad));
        }
        reader.End();
        return Result<DoodadPlacements>::Success(std::move(document));
    } catch (PlacementError& error) {
        return Failed<DoodadPlacements>(std::move(error));
    } catch (const std::bad_alloc&) {
        return Result<DoodadPlacements>::Failure("MapPlacements", "PLACEMENT_MEMORY_LIMIT", "装饰物实例读取无法分配所需内存");
    }
}

} // namespace

Result<UnitPlacements> ParseUnitPlacements(std::span<const std::byte> bytes, const PlacementReadLimits& limits) {
    auto classic = ParseUnitsLayout(bytes, limits, PlacementRecordLayout::Classic);
    if (!classic.Succeeded()) return classic;
    if (!classic.value->units.empty()
        && ParseUnitsLayout(bytes, limits, PlacementRecordLayout::SkinId).Succeeded()) {
        return LayoutFailure<UnitPlacements>(bytes, "war3mapUnits.doo", "WIDGET_LAYOUT_AMBIGUOUS",
            "单位实例 8/11 的旧布局和 SkinId 布局都能完整读取；不能选择可编辑布局");
    }
    classic.value->original_.assign(bytes.begin(), bytes.end());
    return classic;
}

Result<DoodadPlacements> ParseDoodadPlacements(std::span<const std::byte> bytes, const PlacementReadLimits& limits) {
    auto classic = ParseDoodadsLayout(bytes, limits, PlacementRecordLayout::Classic);
    if (!classic.Succeeded()) return classic;
    if (!classic.value->doodads.empty()
        && ParseDoodadsLayout(bytes, limits, PlacementRecordLayout::SkinId).Succeeded()) {
        return LayoutFailure<DoodadPlacements>(bytes, "war3map.doo", "WIDGET_LAYOUT_AMBIGUOUS",
            "装饰物实例 8/11 的旧布局和 SkinId 布局都能完整读取；不能选择可编辑布局");
    }
    classic.value->original_.assign(bytes.begin(), bytes.end());
    return classic;
}

Result<UnitPlacements> ReadUnitPlacementsVersioned(std::span<const std::byte> bytes,
    const PlacementReadLimits& limits) {
    auto classic = ParseUnitsLayout(bytes, limits, PlacementRecordLayout::Classic);
    auto skin = ParseUnitsLayout(bytes, limits, PlacementRecordLayout::SkinId);
    if (classic.Succeeded() && skin.Succeeded()) {
        if (classic.value->units.empty()) {
            classic.value->recordLayout = PlacementRecordLayout::Undetermined;
            classic.value->original_.assign(bytes.begin(), bytes.end());
            return classic;
        }
        return LayoutFailure<UnitPlacements>(bytes, "war3mapUnits.doo", "WIDGET_LAYOUT_AMBIGUOUS",
            "单位实例 8/11 的旧布局和 SkinId 布局都能完整读取；原成员保持不变");
    }
    if (classic.Succeeded()) {
        classic.value->original_.assign(bytes.begin(), bytes.end());
        return classic;
    }
    if (skin.Succeeded()) {
        skin.value->original_.assign(bytes.begin(), bytes.end());
        return skin;
    }
    if (!classic.diagnostics.empty() && !skin.diagnostics.empty()
        && classic.diagnostics.front().code != skin.diagnostics.front().code) {
        return LayoutFailure<UnitPlacements>(bytes, "war3mapUnits.doo", "WIDGET_LAYOUT_UNRESOLVED",
            "单位实例两种 8/11 布局均未完整读取：旧布局 " + classic.diagnostics.front().code
                + "，SkinId 布局 " + skin.diagnostics.front().code);
    }
    return classic;
}

Result<DoodadPlacements> ReadDoodadPlacementsVersioned(std::span<const std::byte> bytes,
    const PlacementReadLimits& limits) {
    auto classic = ParseDoodadsLayout(bytes, limits, PlacementRecordLayout::Classic);
    auto skin = ParseDoodadsLayout(bytes, limits, PlacementRecordLayout::SkinId);
    if (classic.Succeeded() && skin.Succeeded()) {
        if (classic.value->doodads.empty()) {
            classic.value->recordLayout = PlacementRecordLayout::Undetermined;
            classic.value->original_.assign(bytes.begin(), bytes.end());
            return classic;
        }
        return LayoutFailure<DoodadPlacements>(bytes, "war3map.doo", "WIDGET_LAYOUT_AMBIGUOUS",
            "装饰物实例 8/11 的旧布局和 SkinId 布局都能完整读取；原成员保持不变");
    }
    if (classic.Succeeded()) {
        classic.value->original_.assign(bytes.begin(), bytes.end());
        return classic;
    }
    if (skin.Succeeded()) {
        skin.value->original_.assign(bytes.begin(), bytes.end());
        return skin;
    }
    if (!classic.diagnostics.empty() && !skin.diagnostics.empty()
        && classic.diagnostics.front().code != skin.diagnostics.front().code) {
        return LayoutFailure<DoodadPlacements>(bytes, "war3map.doo", "WIDGET_LAYOUT_UNRESOLVED",
            "装饰物实例两种 8/11 布局均未完整读取：旧布局 " + classic.diagnostics.front().code
                + "，SkinId 布局 " + skin.diagnostics.front().code);
    }
    return classic;
}

} // namespace War3Map::MapData
