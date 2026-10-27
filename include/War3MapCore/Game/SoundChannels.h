#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 原版声音编辑器用到的两张表，声音编辑器、Agent 工具共用同一份名称；本文件只解析文本，不读文件。
// - 通道：UI\WorldEditData.txt 的 [SoundChannels]，每行“编号=能否播放 3D 声音,显示名键”。
// - EAX：UI\SoundInfo\EAXDefs.slk，EAXLabel 列是写进声音定义的值，DisplayText 列是显示名键。
namespace War3Map::Game {

class WorldEditStrings;

struct SoundChannel {
    std::int32_t id = 0;
    bool supports3d = false;
    std::string nameKey; // 原样的 WESTRING_* 键
    std::string name;    // 经字符串表解析；解析不了时为键本身
};

struct SoundEaxSetting {
    std::string label;   // 写进声音定义的原值，例如 DefaultEAXON
    std::string nameKey;
    std::string name;
};

struct SoundTables {
    std::vector<SoundChannel> channels;
    std::vector<SoundEaxSetting> eaxSettings;
};

// 按编号排序；编号不是非负整数或字段数不对的行跳过，同一编号出现两次时整组去掉，不猜测。strings 为空时 name 即键。
[[nodiscard]] std::vector<SoundChannel> ParseSoundChannels(std::string_view worldEditData, const WorldEditStrings* strings);
// 按表中顺序；缺 EAXLabel/DisplayText 表头时为空，值为空的行跳过，同一标签出现两次时整组去掉。
[[nodiscard]] std::vector<SoundEaxSetting> ParseSoundEaxSettings(std::string_view eaxDefs, const WorldEditStrings* strings);

} // namespace War3Map::Game
