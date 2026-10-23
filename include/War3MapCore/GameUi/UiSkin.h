#pragma once

#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 界面皮肤表：游戏 UI\war3skins.txt 按种族给出贴图、字体等的实际路径；
// 地图 war3mapSkin.txt 的 [CustomSkin] 对所有种族覆盖同名键，[FrameDef] 覆盖界面文字；
// UI\MiscUI.txt 的 [FontHeights] 给出游戏代码创建的文字（多面板等）的字号；队伍颜色来自皮肤指定的贴图。
namespace War3Map::GameUi {

// 一次查找的结果与来源，便于界面显示“这个贴图来自哪里”。
struct SkinLookup {
    std::string value;
    std::string source;      // 例如 "map:customskin"、"game:human"、"game:default"
    std::string matchedKey;  // 实际命中的键（小写），可能带 _v1
};

// 一种队伍颜色：游戏取贴图左上角第一个像素作为颜色，排行榜把贴图本身当作玩家图标。
struct TeamColor {
    std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
    std::string path;  // 实际读到的贴图
};

class UiSkin {
public:
    // 文本由调用方读取并去掉 BOM；节名与键不区分大小写。
    void SetGameSkins(std::string_view war3skinsText);
    void SetMapSkin(std::string_view war3mapSkinText);

    // [Main] Skins= 列出的种族；没有时返回四族默认值。
    [[nodiscard]] std::vector<std::string> Races() const;

    // 查找顺序：地图 [CustomSkin] → 游戏 [种族] → 游戏 [Default]。
    // expansion 为真时同一层先找 key_V1 再找 key，与冰封王座一致。
    [[nodiscard]] std::optional<SkinLookup> Lookup(std::string_view key, std::string_view race, bool expansion = true) const;

    // 地图 [FrameDef] 对界面文字键的覆盖。
    [[nodiscard]] std::optional<std::string> MapFrameString(std::string_view key) const;

    // UI\MiscUI.txt：调用方按资源优先级读取（地图导入的同名文件覆盖游戏文件）。
    void SetMiscUi(std::string_view miscUiText);
    // [FontHeights] 中游戏代码使用的字号，如 LeaderBoard（多面板与排行榜文字）；没有或不是数值时为空。
    [[nodiscard]] std::optional<float> FontHeight(std::string_view key) const;

    // 队伍颜色，按颜色序号（由 ReadTeamColors 读取）。
    void SetTeamColors(std::vector<TeamColor> colors);
    // 玩家默认的队伍颜色：序号 16 及以上取最后一种，其余取 序号 % 颜色数（与游戏一致）；
    // 脚本运行时改过的玩家颜色不在此列。没有读到颜色时为空。
    [[nodiscard]] const TeamColor* PlayerTeamColor(std::uint32_t player) const;

    [[nodiscard]] bool HasGameSkins() const noexcept { return !game_.empty(); }

private:
    Text::ProfileSections game_;
    Text::ProfileSections map_;
    Text::ProfileSections misc_;
    std::vector<TeamColor> teamColors_;
};

// 读皮肤文件用：返回文件内容，文件不存在时为空。
using SkinFileReader = std::function<std::optional<std::vector<std::byte>>(const std::string& path)>;

// 队伍颜色：皮肤键 TeamColors 为种数、TeamColor 为路径前缀（地图 [CustomSkin] 可覆盖），第 i 种是
// “前缀 + 两位序号” 的贴图，先找 .tga 再找 .blp（与游戏一致）。读不到或无法解码的那种颜色记诊断并用白色。
[[nodiscard]] std::vector<TeamColor> ReadTeamColors(const UiSkin& skin, const SkinFileReader& read,
    std::vector<Diagnostic>& diagnostics);

} // namespace War3Map::GameUi
