#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"

namespace War3Map::MapData {

// 仅真实保存lease的私有桥能构造；wire历史不能制造来源读取资格。
class TerrainHistorySourceShadow {
    friend class MapDataEditSession;
    friend class MapDataEditSession::TerrainHistorySaveLease;
    friend Status MaterializeTerrainJournalHistory(TerrainStrokeJournal& journal);
    explicit TerrainHistorySourceShadow(ArchiveExactMember member) : member_(std::move(member)) {}
    ArchiveExactMember member_;
};

} // namespace War3Map::MapData
