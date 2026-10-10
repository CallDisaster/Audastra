#pragma once

#include <array>
#include <string_view>

namespace War3Map::Game {

// 原版 World Editor 的七张声音元数据表。脚本标签与资源目录使用同一清单顺序。
inline constexpr std::array<std::string_view, 7> kSoundInfoTables{
    "AbilitySounds", "AmbienceSounds", "AnimSounds", "DialogSounds",
    "UISounds", "UnitAckSounds", "UnitCombatSounds"};

} // namespace War3Map::Game
