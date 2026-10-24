#pragma once

#include "War3MapCore/MapData/TerrainStrokeIntent.h"

#include <array>
#include <string_view>

namespace War3Map::MapData {

inline constexpr std::array<std::string_view, 10> TerrainJournalMemberNames{
    "war3map.w3e", "war3map.wpm", "war3map.w3i", "table/w3i.ini",
    "war3mapunits.doo", "war3map.doo", "war3map.w3r", "war3map.w3c", "war3map.w3s", "war3map.wts"};
inline constexpr std::size_t TerrainJournalMaximumBytes = 64u * 1024u * 1024u;

struct TerrainJournalMember {
    std::string name;
    bool present = false;
    std::string sha256;
    std::uint64_t bytes = 0;
};
struct TerrainJournalStroke {
    TerrainStrokeIntent intent;
    // 水算法由intent.waterBrush.algorithm显式决定：FTJ1/2固定v1，FTJ3逐水笔记录v1/v2。
    // 实际水刷/原版悬崖规则owner的依赖摘要；旧普通笔刷为空。不是来源授权。
    std::string rulesSha256;
    // 仅新原版CliffLevel；与digest成对。FTJ1旧悬崖保持旧算法，不能静默升级。
    std::optional<bool> cliffEnforceWaterLimits;
};
struct TerrainStrokeJournal {
    std::string sourceSha256;
    std::vector<TerrainJournalMember> members;
    std::vector<std::byte> baselinePathingLayers;
    std::vector<TerrainJournalStroke> strokes;
};

[[nodiscard]] Status ValidateTerrainStrokeJournal(const TerrainStrokeJournal& journal);
[[nodiscard]] Result<std::vector<std::byte>> EncodeTerrainStrokeJournal(const TerrainStrokeJournal& journal);
[[nodiscard]] Result<TerrainStrokeJournal> DecodeTerrainStrokeJournal(std::span<const std::byte> bytes);

} // namespace War3Map::MapData
