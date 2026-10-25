#pragma once
#include "War3MapCore/MapData/TerrainStrokeJournal.h"

namespace War3Map::MapData::detail {
[[nodiscard]] Status ValidateTerrainHistoryJournal(const TerrainStrokeJournal& journal);
[[nodiscard]] Result<std::vector<std::byte>> EncodeTerrainHistoryJournal(const TerrainStrokeJournal& journal);
[[nodiscard]] Result<TerrainStrokeJournal> DecodeTerrainHistoryJournal(std::span<const std::byte> bytes);
} // namespace War3Map::MapData::detail
