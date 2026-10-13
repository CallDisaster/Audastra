#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace War3Map::MapData {

// rawcode 始终保留文件中的四个字节；禁止用宿主整数端序推断文本 ID。
using SettingsRawcode = std::array<std::byte, 4>;

struct SettingsColor {
    std::uint8_t blue = 0;
    std::uint8_t green = 0;
    std::uint8_t red = 0;
    std::uint8_t alpha = 0;
};

struct MapScreenSettings {
    std::string path;
    std::string text;
    std::string title;
    std::string subtitle;
};

struct MapEnvironmentSettings {
    std::int32_t fogStyle = 0;
    float fogStart = 0;
    float fogEnd = 0;
    float fogDensity = 0;
    SettingsColor fogColor;
    SettingsRawcode weather{};
    std::string soundEnvironment;
    std::uint8_t lightEnvironment = 0;
    SettingsColor waterColor;
};

struct MapPlayerSettings {
    std::int32_t number = 0;
    std::int32_t controller = 0;
    std::int32_t race = 0;
    std::uint32_t flags = 0;
    std::string name;
    float startX = 0;
    float startY = 0;
    std::uint32_t allyLowPriorityMask = 0;
    std::uint32_t allyHighPriorityMask = 0;
    // W3I v31 增加；v25 没有这两个字段，不能借用盟友优先级。
    std::uint32_t enemyLowPriorityMask = 0;
    std::uint32_t enemyHighPriorityMask = 0;
};

// W3I v25 地图选项“使用地形迷雾”：唯一的地形雾启用条件（第 15 位属于物品分类，不能控制雾）。
inline constexpr std::uint32_t kMapFlagTerrainFog = 0x2000u;

// W3I 玩家控制者：1 用户、2 电脑、3 中立、4 可营救；种族：0 可选择、1 人类、2 兽人、3 亡灵、4 暗夜精灵。
// 范围外的值来源不明：读取时原样保留，编辑时不能新写入，生成脚本时不能退回默认值。
[[nodiscard]] constexpr bool KnownPlayerController(std::int32_t value) { return value >= 1 && value <= 4; }
[[nodiscard]] constexpr bool KnownPlayerRace(std::int32_t value) { return value >= 0 && value <= 4; }

struct MapForceSettings {
    std::uint32_t flags = 0;
    std::uint32_t playerMask = 0;
    std::string name;
};

struct MapUpgradeAvailability {
    std::uint32_t playerMask = 0;
    SettingsRawcode id{};
    std::int32_t level = 0; // 文件中的零基等级。
    std::int32_t availability = 0;
};

struct MapTechRestriction {
    std::uint32_t playerMask = 0;
    SettingsRawcode id{}; // 表中列出的单位、物品或技能不可用。
};

struct MapRandomUnitRow {
    std::int32_t chance = 0;
    std::vector<SettingsRawcode> ids;
};

struct MapRandomUnitTable {
    std::int32_t number = 0;
    std::string name;
    std::vector<std::int32_t> columnTypes; // 0 单位，1 建筑，2 物品。
    std::vector<MapRandomUnitRow> rows;
};

struct MapRandomItem {
    std::int32_t chance = 0;
    SettingsRawcode id{};
};

struct MapRandomItemTable {
    std::int32_t number = 0;
    std::string name;
    std::vector<std::vector<MapRandomItem>> sets;
};

struct MapCustomSettings {
    std::vector<MapUpgradeAvailability> upgrades;
    std::vector<MapTechRestriction> techRestrictions;
    std::vector<MapRandomUnitTable> randomUnits;
    std::vector<MapRandomItemTable> randomItems;
};

// 已实现的完整布局白名单；字段存在性与是否允许写出分开，不外推未知未来版本。
struct MapSettingsLayout {
    bool gameVersion = false;
    bool scriptLanguage = false;
    bool graphicsModes = false;
    bool enemyPriorities = false;
    bool cameraZoom = false;
    bool minimumCameraZoom = false;
    bool roc = false; // v18：两个旧编号，无 TFT 载入路径/环境块/随机物品表。
};
[[nodiscard]] const MapSettingsLayout* FindMapSettingsLayout(std::int32_t version) noexcept;

// 所有字符串保留原编码字节（含 TRIGSTR 引用）。v18/v28/v32/v33 仅只读。
// v31 的现有解析/序列化不授权地图脚本或完整归档保存；后者须另行验证。
struct MapSettings {
    std::int32_t formatVersion = 0;
    std::int32_t saves = 0;
    std::int32_t editorVersion = 0;
    std::array<std::int32_t, 4> gameVersion{}; // 有字段的布局：major, minor, build, revision；原值保留。
    std::string mapName;
    std::string author;
    std::string description;
    std::string recommendedPlayers;
    std::array<float, 8> cameraBounds{};
    std::array<std::int32_t, 4> cameraMargins{};
    std::int32_t playableWidth = 0;
    std::int32_t playableHeight = 0;
    std::uint32_t flags = 0;
    std::uint8_t tileset = 0;
    std::int32_t loadingBackground = 0;
    std::optional<std::int32_t> rocCampaignBackground;
    std::optional<std::int32_t> rocLoadingScreenNumber;
    MapScreenSettings loadingScreen;
    std::int32_t gameDataSet = 0;
    MapScreenSettings prologueScreen;
    MapEnvironmentSettings environment;
    std::int32_t scriptLanguage = 0; // v28 起：0 JASS, 1 Lua；未知值保留原始整数。
    std::uint32_t graphicsModes = 0; // v31: 原始 U32 位掩码，不在此处猜测或清位。
    std::int32_t gameDataVersion = 0; // v31: 原始版本选择值。
    std::optional<std::int32_t> forceDefaultCameraZoom; // v32 起；未知哨兵原样保留，不能按 F32 解码。
    std::optional<std::int32_t> forceMaxCameraZoom;
    std::optional<std::int32_t> forceMinCameraZoom; // v33 起。
    std::vector<MapPlayerSettings> players;
    std::vector<MapForceSettings> forces;
    // 某些文件用 0xff 省略自定义表；nullopt 表示未提供，不能解释成空表。
    std::optional<MapCustomSettings> custom;

    [[nodiscard]] static Result<MapSettings> Parse(std::span<const std::byte> data);
    [[nodiscard]] Result<std::vector<std::byte>> Serialize() const;
    [[nodiscard]] const std::vector<std::byte>& OriginalBytes() const noexcept { return originalBytes_; }
    [[nodiscard]] const std::vector<std::byte>& UnknownTail() const noexcept { return unknownTail_; }
    [[nodiscard]] bool FullyParsed() const noexcept { return custom.has_value() && unknownTail_.empty(); }
    [[nodiscard]] const MapSettingsLayout* Layout() const noexcept { return FindMapSettingsLayout(formatVersion); }
    [[nodiscard]] std::optional<std::int32_t> SourceFormatVersion() const noexcept;
    // 仅格式资格，不证明初始化引用、脚本或完整地图可用。
    [[nodiscard]] bool HasClassicLayout() const noexcept;
    // 混乱之治 v18：没有 TFT 之后的扩展字段，读到的就是全部内容。
    [[nodiscard]] bool HasRocLayout() const noexcept;

private:
    std::vector<std::byte> originalBytes_;
    std::vector<std::byte> unknownTail_;
};

} // namespace War3Map::MapData
