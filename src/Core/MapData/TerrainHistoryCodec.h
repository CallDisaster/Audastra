#pragma once
#include "War3MapCore/MapData/TerrainStrokeJournal.h"

namespace War3Map::MapData::detail {
// 与旧FTJ同一validator，仅借只读view；history预检不能在收紧预算之前复制大delta。
[[nodiscard]] Status ValidateTerrainJournalParts(std::string_view sourceSha256,
    std::span<const TerrainJournalMember> members, std::span<const std::byte> baselinePathingLayers,
    std::span<const TerrainJournalStroke> strokes);
[[nodiscard]] Status ValidateTerrainHistoryJournal(const TerrainStrokeJournal& journal);
[[nodiscard]] Result<std::vector<std::byte>> EncodeTerrainHistoryJournal(const TerrainStrokeJournal& journal);
[[nodiscard]] Result<TerrainStrokeJournal> DecodeTerrainHistoryJournal(std::span<const std::byte> bytes);
} // namespace War3Map::MapData::detail
