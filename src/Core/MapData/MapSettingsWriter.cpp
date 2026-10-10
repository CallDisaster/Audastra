#include "War3MapCore/MapData/MapSettings.h"
#include "MapDataWriter.h"

#include <algorithm>
#include <new>

namespace War3Map::MapData {
namespace {

constexpr std::size_t kMaxBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxStringBytes = 1024u * 1024u;
constexpr std::size_t kMaxElements = 1024u * 1024u;

void Screen(Detail::Writer& writer, const MapScreenSettings& screen) {
    writer.String(screen.path, kMaxStringBytes);
    writer.String(screen.text, kMaxStringBytes);
    writer.String(screen.title, kMaxStringBytes);
    writer.String(screen.subtitle, kMaxStringBytes);
}

void Color(Detail::Writer& writer, const SettingsColor& color) {
    writer.Byte(color.blue);
    writer.Byte(color.green);
    writer.Byte(color.red);
    writer.Byte(color.alpha);
}

void Custom(Detail::Writer& writer, const MapCustomSettings& custom) {
    writer.Count(custom.upgrades.size(), kMaxElements);
    for (const auto& upgrade : custom.upgrades) {
        writer.U32(upgrade.playerMask);
        writer.Raw(upgrade.id, 4);
        writer.I32(upgrade.level);
        writer.I32(upgrade.availability);
    }
    writer.Count(custom.techRestrictions.size(), kMaxElements);
    for (const auto& tech : custom.techRestrictions) {
        writer.U32(tech.playerMask);
        writer.Raw(tech.id, 4);
    }
    writer.Count(custom.randomUnits.size(), kMaxElements);
    for (const auto& table : custom.randomUnits) {
        writer.I32(table.number);
        writer.String(table.name, kMaxStringBytes);
        writer.Count(table.columnTypes.size(), 1024);
        for (const auto type : table.columnTypes) writer.I32(type);
        writer.Count(table.rows.size(), kMaxElements);
        if (!table.columnTypes.empty() && table.rows.size() > kMaxElements / table.columnTypes.size())
            throw Detail::WriteFailure{"W3I_COUNT_BUDGET", "随机单位表单元格数量超过预算"};
        for (const auto& row : table.rows) {
            if (row.ids.size() != table.columnTypes.size())
                throw Detail::WriteFailure{"W3I_COLUMN_COUNT", "随机单位表某行的列数不匹配"};
            writer.I32(row.chance);
            for (const auto& id : row.ids) writer.Raw(id, 4);
        }
    }
    writer.Count(custom.randomItems.size(), kMaxElements);
    for (const auto& table : custom.randomItems) {
        writer.I32(table.number);
        writer.String(table.name, kMaxStringBytes);
        writer.Count(table.sets.size(), kMaxElements);
        for (const auto& set : table.sets) {
            writer.Count(set.size(), kMaxElements);
            for (const auto& item : set) {
                writer.I32(item.chance);
                writer.Raw(item.id, 4);
            }
        }
    }
}

} // namespace

Result<std::vector<std::byte>> MapSettings::Serialize() const {
    if (formatVersion != 25 && formatVersion != 31)
        return Result<std::vector<std::byte>>::Failure("MapSettings.Write", "W3I_UNSUPPORTED_VERSION",
            "地图设置写出仅支持经典 TFT v25 与 v31");
    const auto source = SourceFormatVersion();
    if ((source && *source != 25 && *source != formatVersion)
        || forceDefaultCameraZoom || forceMaxCameraZoom || forceMinCameraZoom
        || rocCampaignBackground || rocLoadingScreenNumber
        || (formatVersion == 25 && !HasClassicLayout())) {
        return Result<std::vector<std::byte>>::Failure("MapSettings.Write", "W3I_READ_ONLY_LAYOUT",
            "来源版本或扩展字段不适用于目标写出布局；更改版本标签不能授权丢弃字段或格式降级。");
    }
    try {
        Detail::Writer writer(kMaxBytes);
        writer.I32(formatVersion);
        writer.I32(saves);
        writer.I32(editorVersion);
        if (formatVersion == 31) {
            for (const auto part : gameVersion) writer.I32(part);
        }
        writer.String(mapName, kMaxStringBytes);
        writer.String(author, kMaxStringBytes);
        writer.String(description, kMaxStringBytes);
        writer.String(recommendedPlayers, kMaxStringBytes);
        for (const float bound : cameraBounds) writer.F32(bound);
        for (const auto margin : cameraMargins) writer.I32(margin);
        writer.I32(playableWidth);
        writer.I32(playableHeight);
        writer.U32(flags);
        writer.Byte(tileset);
        writer.I32(loadingBackground);
        Screen(writer, loadingScreen);
        writer.I32(gameDataSet);
        Screen(writer, prologueScreen);
        writer.I32(environment.fogStyle);
        writer.F32(environment.fogStart);
        writer.F32(environment.fogEnd);
        writer.F32(environment.fogDensity);
        Color(writer, environment.fogColor);
        writer.Raw(environment.weather, 4);
        writer.String(environment.soundEnvironment, kMaxStringBytes);
        writer.Byte(environment.lightEnvironment);
        Color(writer, environment.waterColor);
        if (formatVersion == 31) {
            writer.I32(scriptLanguage);
            writer.U32(graphicsModes);
            writer.I32(gameDataVersion);
        }
        writer.Count(players.size(), 32);
        for (const auto& player : players) {
            writer.I32(player.number);
            writer.I32(player.controller);
            writer.I32(player.race);
            writer.U32(player.flags);
            writer.String(player.name, kMaxStringBytes);
            writer.F32(player.startX);
            writer.F32(player.startY);
            writer.U32(player.allyLowPriorityMask);
            writer.U32(player.allyHighPriorityMask);
            if (formatVersion == 31) {
                writer.U32(player.enemyLowPriorityMask);
                writer.U32(player.enemyHighPriorityMask);
            }
        }
        writer.Count(forces.size(), 32);
        for (const auto& force : forces) {
            writer.U32(force.flags);
            writer.U32(force.playerMask);
            writer.String(force.name, kMaxStringBytes);
        }
        if (custom) {
            Custom(writer, *custom);
        } else if (unknownTail_ == std::vector<std::byte>{std::byte{0xff}}) {
            writer.Byte(0xff);
        } else {
            return Result<std::vector<std::byte>>::Failure("MapSettings.Write", "W3I_CUSTOM_UNKNOWN",
                "自定义设置表缺失且没有可确认的省略标记，不能写出");
        }
        auto bytes = std::move(writer).Finish();
        if (custom && !unknownTail_.empty()) {
            if (unknownTail_.size() > kMaxBytes - bytes.size())
                return Result<std::vector<std::byte>>::Failure("MapSettings.Write", "W3I_FILE_BUDGET",
                    "地图设置尾部超过输出预算");
            bytes.insert(bytes.end(), unknownTail_.begin(), unknownTail_.end());
        }
        const auto reparsed = Parse(bytes);
        if (!reparsed.Succeeded() || !reparsed.value || reparsed.value->unknownTail_ != unknownTail_)
            return Result<std::vector<std::byte>>::Failure("MapSettings.Write", "W3I_OUTPUT_INVALID",
                "写出的地图设置不能按原有结构重新读取");
        if (bytes == originalBytes_) return Result<std::vector<std::byte>>::Success(originalBytes_);
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    } catch (const Detail::WriteFailure& error) {
        return Result<std::vector<std::byte>>::Failure("MapSettings.Write", error.code, error.message);
    } catch (const std::bad_alloc&) {
        return Result<std::vector<std::byte>>::Failure("MapSettings.Write", "W3I_ALLOCATION_FAILED",
            "地图设置输出内存分配失败");
    } catch (const std::length_error&) {
        return Result<std::vector<std::byte>>::Failure("MapSettings.Write", "W3I_CONTAINER_LIMIT",
            "地图设置输出超过容器限制");
    }
}

} // namespace War3Map::MapData
