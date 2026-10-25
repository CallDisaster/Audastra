#pragma once

#include "War3MapCore/MapData/TerrainStrokeIntent.h"
#include "War3MapCore/MapData/TerrainDoodadsJournal.h"
#include "War3MapCore/MapData/TerrainUnitsJournal.h"
#include "War3MapCore/MapData/TerrainStartLocationsJournal.h"

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
    // FTJ7独立普通DOO记录；intent必须完全中性，不进入公共笔刷请求域。
    std::optional<TerrainDoodadsJournalEdit> ordinaryDoodads;
    // 仅UI冻结的真实历史别名，后台Capture物化前不能编码、解码或恢复。
    std::optional<FrozenTerrainDoodadsJournalEdit> frozenDoodads;
    // FTJ9完整经典Units，保持与水刷/普通DOO相同的真实命令顺序。
    std::optional<TerrainUnitsJournalEdit> units;
    std::optional<FrozenTerrainUnitsJournalEdit> frozenUnits;
    // FTJA仅承载真实PrepareUnits产生的Units+W3I起始位置复合命令。
    std::optional<TerrainStartLocationsJournalEdit> startLocations;
    std::optional<FrozenTerrainStartLocationsJournalEdit> frozenStartLocations;
    // FTJ6新增独立palette变体，复用实际owner的完整规则/来源摘要；不携带验证位。
    // 水算法由intent.waterBrush.algorithm显式决定：FTJ1/2固定v1，FTJ3/4逐水笔记录v1/v2。
    // FTJ4沿用FTJ3正文布局，新增独立CornerRamp=13，必需单点/实际规则与完整DOO摘要。
    // FTJ8沿用FTJ7变体，在CornerRamp的kind后写algorithm字节；旧FTJ4–7固定algorithm1。
    // FTJB新增StraightRamp=15固定v1三点模式；旧Ramp与FTJ1–A不能升级为含水直坡。
    // FTJC在每个StraightRamp的kind后记录独立/连接算法；仅含Connected时选择新版本。
    // FTJD新增block variant6：算法、源地格矩形、目标顶点、实际源块SHA、follow和规则SHA。
    // 不携带raw载荷或验证位；源块在每笔恢复的before中重新读取并核对。
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
// Services后台补来源身份后调用；累计预收codec/扫描/中段容量，成功后释放全部冻结别名。
[[nodiscard]] Status MaterializeTerrainJournalPlacements(TerrainStrokeJournal& journal);
[[nodiscard]] Result<std::vector<std::byte>> EncodeTerrainStrokeJournal(const TerrainStrokeJournal& journal);
[[nodiscard]] Result<TerrainStrokeJournal> DecodeTerrainStrokeJournal(std::span<const std::byte> bytes);

} // namespace War3Map::MapData
