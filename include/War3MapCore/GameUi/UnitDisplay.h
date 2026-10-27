#pragma once

#include <cstdint>
#include <string>

// 团队框架与目标框架共用的单位显示选项：名字来源、登记的增减益与头像。
namespace War3Map::GameUi {

enum class UnitNameSource : std::uint8_t {
    Player,  // 所属玩家的名字
    Unit,    // 英雄用英雄名，其他单位用单位名
};

// 登记的增减益：增减益码与图标路径。
struct UnitBuff {
    std::string buff;
    std::string icon;
};

// 登记的头像：单位类型与头像图标路径。
struct UnitPortrait {
    std::string unit;
    std::string icon;
};

} // namespace War3Map::GameUi
