#pragma once

#include "War3MapCore/MapData/TerrainStrokeIntent.h"
#include <string_view>

namespace War3Map::Services {

enum class TerrainTextureSlotOperation { Replace, Append };
struct TerrainTextureSlotCommand {
    TerrainTextureSlotOperation operation = TerrainTextureSlotOperation::Replace;
    MapData::TerrainTextureTable table = MapData::TerrainTextureTable::Ground;
    // Replace的零基槽号；Append执行到本条时的期望表长，也即新槽号。
    std::uint32_t slot = 0;
    std::string expectedRawcode;
    std::string rawcode;
};

[[nodiscard]] Result<MapData::TerrainTextureTable> ParseTerrainTextureTable(std::string_view value);
// 协议共用形状校验/algorithm归一化。current存在时再核对执行到本条的CAS；
// 它不授予资源或写入资格，编辑/保存仍须取得实际来源owner。
[[nodiscard]] Result<MapData::TerrainPaletteEditIntent> NormalizeTerrainTextureSlotCommand(
    const TerrainTextureSlotCommand& command, const MapData::TerrainHeader* current = nullptr);

} // namespace War3Map::Services
