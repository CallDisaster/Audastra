#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <span>

namespace War3Map::Campaign {

// Classic war3campaign.w3f v1。身份只属于编辑会话，不写入原生文件。
struct MenuButton {
    StableId id = 0;
    std::int32_t visible = 0; // 保留非标准整数；只有用户切换可见性时才写 0/1。
    std::string chapterTitle;
    std::string mapTitle;
    std::string mapPath;
    bool operator==(const MenuButton&) const = default;
};

struct MapEntry {
    StableId id = 0;
    std::string unknown; // 原格式的前置字符串含义未确认，原样保留，不作为标题猜测。
    std::string path;
    bool operator==(const MapEntry&) const = default;
};

struct CampaignInfo {
    std::int32_t formatVersion = 1;
    std::int32_t saves = 0;
    std::int32_t editorVersion = 0;
    std::string name, difficulty, author, description;
    std::uint32_t flags = 0;
    std::int32_t background = -1;
    std::string backgroundPath, minimapPath;
    std::int32_t ambientSound = -1;
    std::string ambientSoundPath;
    std::int32_t fogStyle = 0;
    // 原始 IEEE-754 位模式使 NaN 和负零也能逐字节往返。
    std::array<std::uint32_t, 3> fogBits{}; // 起点、终点、密度。
    std::array<std::uint8_t, 4> fogColor{}; // 原始四字节，不重新解释或归一化。
    std::int32_t cursor = 0;
    std::vector<MenuButton> buttons;
    std::vector<MapEntry> maps;
    std::vector<std::byte> trailing;
    bool operator==(const CampaignInfo&) const = default;
};

inline constexpr std::size_t kMaximumCampaignInfoBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaximumCampaignEntries = 4096;
[[nodiscard]] Result<CampaignInfo> ParseInfo(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<std::byte>> SerializeInfo(const CampaignInfo& info);

} // namespace War3Map::Campaign
