#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace LniMapSettingsFixtures {
// 原创确定性样例；不包含原版资源或私有地图内容。
inline std::string Complete() {
    std::string text = R"LNI([map]
file_version = 25
map_version = 7
we_version = 6052
map_name = "LNI fixture"
author_name = "FORGE tests"
map_desc = [=[
Line one
Line two]=]
player_desc = "1 player"
[camera]
camera_bound = {-512, -512, 512, 512, -512, 512, 512, -512}
camera_complement = {1, 2, 3, 4}
[map_info]
map_width = 32
map_height = 64
map_main_ground = "L"
[config]
game_data_setting = 1
)LNI";
    constexpr std::string_view flags[] = {"disable_preview", "custom_ally", "melee_map", "large_map",
        "masked_area_show_terrain", "fix_force_setting", "custom_force", "custom_techtree", "custom_ability",
        "custom_upgrade", "map_menu_mark", "show_wave_on_cliff", "show_wave_on_rolling", "unknown_1", "unknown_2",
        "unknown_3", "unknown_4", "unknown_5", "unknown_6", "unknown_7", "unknown_8", "unknown_9"};
    for (const auto flag : flags) text += std::string(flag) + " = 1\n";
    text += R"LNI([loading_screen]
id = -1
path = "Loading\\screen.mdx"
text = "Loading\ntext"
title = "Title"
subtitle = "Subtitle"
[prologue]
path = ""
text = ""
title = ""
subtitle = ""
[fog]
type = 1
start_z = 32.5
end_z = 2048
density = 0.2
color = {11, 22, 33, 44}
[environment]
weather = "\0\0\0\0"
sound = "Default"
light = "L"
water_color = {1, 2, 3, 255}
[player]
player_count = 1
[player1]
player = 3
type = 1
race = 2
fix_start_position = 3
name = "Player four"
start_position = {128, -256}
ally_low_flag = {1, 32}
ally_high_flag = {2}
[force]
force_count = 1
[force1]
ally = 1
ally_win = 1
share_visible = 1
share_control = 1
share_advance = 1
player_list = {4}
force_name = "Team one"
[upgrade1]
player_list = {4}
ID = "R001"
level = 2
available = 1
[tech1]
player_list = {4}
ID = "A001"
[random_group1]
random_group_name = "Two columns"
position_type = {0, 2}
setting = {{chance = 60, ID = {"u001", "i001"}}, {chance = 40, ID = {"u002", "i002"}}}
[random_item1]
random_item_name = "Two sets"
setting = {
1 = {{chance = 100, ID = "i001"}},
2 = {{chance = 75, ID = "i002"}, {chance = 25, ID = "i003"}},
}
)LNI";
    return text;
}
inline std::vector<std::byte> Bytes(std::string_view text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    return {p, p + text.size()};
}
inline std::string Replace(std::string text, std::string_view before, std::string_view after) {
    const auto position = text.find(before);
    if (position == text.npos) throw std::runtime_error("fixture replacement not found");
    text.replace(position, before.size(), after);
    return text;
}
} // namespace LniMapSettingsFixtures
