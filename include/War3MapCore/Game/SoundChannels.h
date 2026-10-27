#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 原版声音通道表：UI\WorldEditData.txt 的 [SoundChannels]，每行“编号=能否播放 3D 声音,显示名键”。
// 声音编辑器、Agent 工具共用同一份名称；本文件只解析文本，不读文件。
namespace War3Map::Game {

class WorldEditStrings;

struct SoundChannel {
    std::int32_t id = 0;
    bool supports3d = false;
    std::string nameKey; // 原样的 WESTRING_* 键
    std::string name;    // 经字符串表解析；解析不了时为键本身
};

// 按编号排序；编号不是非负整数或字段数不对的行跳过，不猜测。strings 为空时 name 即键。
[[nodiscard]] std::vector<SoundChannel> ParseSoundChannels(std::string_view worldEditData, const WorldEditStrings* strings);

} // namespace War3Map::Game
