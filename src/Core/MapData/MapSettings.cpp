#include "War3MapCore/MapData/MapSettings.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>

namespace War3Map::MapData {

const MapSettingsLayout* FindMapSettingsLayout(std::int32_t version) noexcept {
    static constexpr MapSettingsLayout classic{}, lua{true, true},
        reforged{true, true, true, true}, zoom{true, true, true, true, true},
        minimumZoom{true, true, true, true, true, true}, roc{.roc = true};
    switch (version) {
    case 18: return &roc;
    case 25: return &classic;
    case 28: return &lua;
    case 31: return &reforged;
    case 32: return &zoom;
    case 33: return &minimumZoom;
    default: return nullptr;
    }
}

std::optional<std::int32_t> MapSettings::SourceFormatVersion() const noexcept {
    if (originalBytes_.size() < 4) return {};
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index)
        value |= std::to_integer<std::uint32_t>(originalBytes_[index]) << (index * 8);
    return std::bit_cast<std::int32_t>(value);
}

bool MapSettings::HasClassicLayout() const noexcept {
    const auto source = SourceFormatVersion();
    return formatVersion == 25 && (!source || *source == 25)
        && gameVersion == std::array<std::int32_t, 4>{} && scriptLanguage == 0
        && graphicsModes == 0 && gameDataVersion == 0 && !forceDefaultCameraZoom
        && !forceMaxCameraZoom && !forceMinCameraZoom && !rocCampaignBackground && !rocLoadingScreenNumber
        && std::none_of(players.begin(), players.end(), [](const auto& player) {
            return player.enemyLowPriorityMask != 0 || player.enemyHighPriorityMask != 0;
        });
}

namespace {

constexpr std::size_t MaxFileBytes = 64 * 1024 * 1024;
constexpr std::size_t MaxStringBytes = 1024 * 1024;
constexpr std::size_t MaxElements = 1024 * 1024;

// 布局按公开格式说明与 War3Net 的版本分支交叉核对；读取器和原创夹具为本项目实现。
// 字段证据等级与未覆盖的特殊布局见 docs/research/LUA_AND_NEW_FORMATS_2026-09-29.md §3。
// https://github.com/SimonMossmyr/w3x-spec/blob/master/w3i.md
// https://github.com/Drake53/War3Net/blob/master/src/War3Net.Build.Core/Serialization/Binary/Info/MapInfo.cs

struct ParseFailure {
    std::string code;
    std::string message;
    std::size_t offset;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    std::size_t Position() const noexcept { return position_; }
    std::size_t Remaining() const noexcept { return bytes_.size() - position_; }
    std::uint8_t Peek() const {
        Need(1);
        return std::to_integer<std::uint8_t>(bytes_[position_]);
    }
    std::uint8_t Byte() {
        const auto value = Peek();
        ++position_;
        return value;
    }
    std::uint32_t UInt() {
        Need(4);
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(Byte()) << shift;
        }
        return value;
    }
    std::int32_t Int() { return std::bit_cast<std::int32_t>(UInt()); }
    float Float() {
        const auto offset = position_;
        const auto value = std::bit_cast<float>(UInt());
        if (!std::isfinite(value)) {
            throw ParseFailure{"W3I_NONFINITE", "地图设置包含非有限浮点数。", offset};
        }
        return value;
    }
    std::string String() {
        const auto start = position_;
        while (Remaining() != 0 && bytes_[position_] != std::byte{0}) {
            if (position_ - start >= MaxStringBytes) {
                throw ParseFailure{"W3I_STRING_BUDGET", "地图设置字符串超过 1 MiB 读取预算。", start};
            }
            ++position_;
        }
        Need(1);
        const auto count = position_ - start;
        ++position_;
        return std::string(reinterpret_cast<const char*>(bytes_.data() + start), count);
    }
    SettingsRawcode Rawcode() {
        Need(4);
        SettingsRawcode code;
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), 4, code.begin());
        position_ += 4;
        return code;
    }
    SettingsColor Color() { return {Byte(), Byte(), Byte(), Byte()}; }
    std::size_t Count(std::size_t minimumEntryBytes, std::size_t maximum = MaxElements) {
        const auto offset = position_;
        const auto count = Int();
        if (count < 0 || static_cast<std::size_t>(count) > maximum) {
            throw ParseFailure{"W3I_COUNT_BUDGET", "地图设置记录数量非法或超过读取预算。", offset};
        }
        const auto size = static_cast<std::size_t>(count);
        if (minimumEntryBytes != 0 && size > Remaining() / minimumEntryBytes) {
            throw ParseFailure{"W3I_TRUNCATED", "地图设置记录数量超过剩余数据边界。", offset};
        }
        Charge(size);
        return size;
    }
    void Charge(std::size_t count) {
        if (count > elementBudget_) {
            throw ParseFailure{"W3I_COUNT_BUDGET", "地图设置累计元素超过读取预算。", position_};
        }
        elementBudget_ -= count;
    }

private:
    void Need(std::size_t count) const {
        if (count > Remaining()) {
            throw ParseFailure{"W3I_TRUNCATED", "地图设置在字段结束前已截断。", position_};
        }
    }
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
    std::size_t elementBudget_ = MaxElements;
};

Diagnostic MakeDiagnostic(DiagnosticSeverity severity, std::string code,
    std::string message, std::size_t offset, std::size_t length = 0) {
    return {severity, "MapSettings", std::move(code), std::move(message),
        OriginSpan{"war3map.w3i", offset, length, {}}};
}

MapScreenSettings ReadScreen(Reader& reader, bool hasPath) {
    return {hasPath ? reader.String() : std::string{}, reader.String(), reader.String(), reader.String()};
}

void ReadPlayersAndForces(Reader& reader, MapSettings& settings) {
    const auto playerCount = reader.Count(33, 32);
    settings.players.reserve(playerCount);
    for (std::size_t index = 0; index < playerCount; ++index) {
        MapPlayerSettings player;
        player.number = reader.Int();
        player.controller = reader.Int();
        player.race = reader.Int();
        player.flags = reader.UInt();
        player.name = reader.String();
        player.startX = reader.Float();
        player.startY = reader.Float();
        player.allyLowPriorityMask = reader.UInt();
        player.allyHighPriorityMask = reader.UInt();
        if (settings.Layout()->enemyPriorities) {
            player.enemyLowPriorityMask = reader.UInt();
            player.enemyHighPriorityMask = reader.UInt();
        }
        settings.players.push_back(std::move(player));
    }
    const auto forceCount = reader.Count(9, 32);
    settings.forces.reserve(forceCount);
    for (std::size_t index = 0; index < forceCount; ++index) {
        settings.forces.push_back({reader.UInt(), reader.UInt(), reader.String()});
    }
}

MapCustomSettings ReadCustom(Reader& reader, bool hasRandomItems) {
    MapCustomSettings custom;
    const auto upgradeCount = reader.Count(16);
    custom.upgrades.reserve(upgradeCount);
    for (std::size_t index = 0; index < upgradeCount; ++index) {
        custom.upgrades.push_back({reader.UInt(), reader.Rawcode(), reader.Int(), reader.Int()});
    }
    const auto techCount = reader.Count(8);
    custom.techRestrictions.reserve(techCount);
    for (std::size_t index = 0; index < techCount; ++index) {
        custom.techRestrictions.push_back({reader.UInt(), reader.Rawcode()});
    }
    const auto unitTableCount = reader.Count(13);
    custom.randomUnits.reserve(unitTableCount);
    for (std::size_t index = 0; index < unitTableCount; ++index) {
        MapRandomUnitTable table;
        table.number = reader.Int();
        table.name = reader.String();
        const auto columns = reader.Count(4, 1024);
        table.columnTypes.reserve(columns);
        for (std::size_t column = 0; column < columns; ++column) {
            table.columnTypes.push_back(reader.Int());
        }
        const auto rows = reader.Count(4 + columns * 4);
        // 列、行与每个 rawcode 分别计入预算，避免二维表放大内存分配。
        if (columns != 0 && rows > MaxElements / columns) {
            throw ParseFailure{"W3I_COUNT_BUDGET", "随机单位表的单元格数量超过读取预算。", reader.Position()};
        }
        reader.Charge(columns * rows);
        table.rows.reserve(rows);
        for (std::size_t rowIndex = 0; rowIndex < rows; ++rowIndex) {
            MapRandomUnitRow row;
            row.chance = reader.Int();
            row.ids.reserve(columns);
            for (std::size_t column = 0; column < columns; ++column) {
                row.ids.push_back(reader.Rawcode());
            }
            table.rows.push_back(std::move(row));
        }
        custom.randomUnits.push_back(std::move(table));
    }
    if (!hasRandomItems) return custom;
    const auto itemTableCount = reader.Count(9);
    custom.randomItems.reserve(itemTableCount);
    for (std::size_t index = 0; index < itemTableCount; ++index) {
        MapRandomItemTable table;
        table.number = reader.Int();
        table.name = reader.String();
        const auto sets = reader.Count(4);
        table.sets.reserve(sets);
        for (std::size_t setIndex = 0; setIndex < sets; ++setIndex) {
            const auto items = reader.Count(8);
            std::vector<MapRandomItem> set;
            set.reserve(items);
            for (std::size_t itemIndex = 0; itemIndex < items; ++itemIndex) {
                set.push_back({reader.Int(), reader.Rawcode()});
            }
            table.sets.push_back(std::move(set));
        }
        custom.randomItems.push_back(std::move(table));
    }
    return custom;
}

} // namespace

Result<MapSettings> MapSettings::Parse(std::span<const std::byte> data) {
    Result<MapSettings> result;
    if (data.size() > MaxFileBytes) {
        result.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Error,
            "W3I_FILE_BUDGET", "地图设置文件超过 64 MiB 读取预算。", 0, data.size()));
        return result;
    }
    try {
        Reader reader(data);
        MapSettings settings;
        settings.formatVersion = reader.Int();
        const auto* layout = settings.Layout();
        if (!layout) {
            result.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Error,
                "W3I_UNSUPPORTED_VERSION", "地图设置完整读取支持 v18/25/28/31/32/33；未推断过渡版或未知未来布局。", 0, 4));
            return result;
        }
        settings.saves = reader.Int();
        settings.editorVersion = reader.Int();
        if (layout->gameVersion) {
            for (auto& part : settings.gameVersion) part = reader.Int();
        }
        settings.mapName = reader.String();
        settings.author = reader.String();
        settings.description = reader.String();
        settings.recommendedPlayers = reader.String();
        for (auto& bound : settings.cameraBounds) bound = reader.Float();
        for (auto& margin : settings.cameraMargins) margin = reader.Int();
        settings.playableWidth = reader.Int();
        settings.playableHeight = reader.Int();
        settings.flags = reader.UInt();
        settings.tileset = reader.Byte();
        if (layout->roc) settings.rocCampaignBackground = reader.Int();
        else settings.loadingBackground = reader.Int();
        settings.loadingScreen = ReadScreen(reader, !layout->roc);
        if (layout->roc) settings.rocLoadingScreenNumber = reader.Int();
        else settings.gameDataSet = reader.Int();
        settings.prologueScreen = ReadScreen(reader, !layout->roc);
        if (!layout->roc) {
            auto& environment = settings.environment;
            environment.fogStyle = reader.Int();
            environment.fogStart = reader.Float();
            environment.fogEnd = reader.Float();
            environment.fogDensity = reader.Float();
            environment.fogColor = reader.Color();
            environment.weather = reader.Rawcode();
            environment.soundEnvironment = reader.String();
            environment.lightEnvironment = reader.Byte();
            environment.waterColor = reader.Color();
        }
        if (layout->scriptLanguage) settings.scriptLanguage = reader.Int();
        if (layout->graphicsModes) {
            settings.graphicsModes = reader.UInt();
            settings.gameDataVersion = reader.Int();
        }
        if (layout->cameraZoom) {
            settings.forceDefaultCameraZoom = reader.Int();
            settings.forceMaxCameraZoom = reader.Int();
        }
        if (layout->minimumCameraZoom) settings.forceMinCameraZoom = reader.Int();
        ReadPlayersAndForces(reader, settings);
        // 只接受独立单字节 0xff 作为省略标记，避免把低字节为 255 的合法计数误判。
        if (reader.Remaining() == 1 && reader.Peek() == 0xff) {
            settings.unknownTail_.push_back(std::byte{0xff});
            result.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning,
                "W3I_CUSTOM_TABLES_OMITTED", "该地图用 0xff 省略自定义设置表；未将缺失表解释为空表。", reader.Position(), 1));
        } else {
            settings.custom = ReadCustom(reader, !layout->roc);
            if (reader.Remaining() != 0) {
                settings.unknownTail_.assign(data.begin() + static_cast<std::ptrdiff_t>(reader.Position()), data.end());
                result.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning,
                    "W3I_UNKNOWN_TAIL", "地图设置已知字段之后含有未解释的尾部；已完整保留，不能声明初始化信息完整。",
                    reader.Position(), reader.Remaining()));
            }
        }
        settings.originalBytes_.assign(data.begin(), data.end());
        result.value = std::move(settings);
    } catch (const ParseFailure& failure) {
        result.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Error,
            failure.code, failure.message, failure.offset));
    } catch (const std::bad_alloc&) {
        result.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Error,
            "W3I_ALLOCATION_FAILED", "地图设置读取内存分配失败。", 0));
    }
    return result;
}

} // namespace War3Map::MapData
