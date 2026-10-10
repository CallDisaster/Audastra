#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace War3Map {

// war3map.w3i 地图信息解析/编辑。
// 解析头部字段（版本/名称/作者/描述/推荐玩家），其余数据原样保留确保往返保真。
struct MapInfo {
    std::int32_t formatVersion = 0;   // 文件格式版本（25=TFT 1.27+）
    std::int32_t saves = 0;           // 保存次数（地图版本）
    std::int32_t editorVersion = 0;   // 编辑器版本
    std::string mapName;              // 地图名称
    std::string author;               // 作者
    std::string description;          // 描述
    std::string recommendedPlayers;   // 推荐玩家数
    std::vector<std::byte> trailing;  // 头部之后的全部原始数据（相机/玩家/力量等）

    // 解析 w3i 字节。失败返回 Failure。
    [[nodiscard]] static Result<MapInfo> Parse(std::span<const std::byte> data);

    // 序列化为 w3i 字节（头部字段 + trailing 原样拼接）。
    [[nodiscard]] std::vector<std::byte> Serialize() const;
};

} // namespace War3Map
