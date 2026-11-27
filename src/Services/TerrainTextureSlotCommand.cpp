#include "War3MapCore/Services/TerrainTextureSlotCommand.h"
#include <algorithm>

namespace War3Map::Services {
Result<MapData::TerrainTextureTable> ParseTerrainTextureTable(std::string_view value) {
    if (value == "ground") return Result<MapData::TerrainTextureTable>::Success(MapData::TerrainTextureTable::Ground);
    if (value == "cliff") return Result<MapData::TerrainTextureTable>::Success(MapData::TerrainTextureTable::Cliff);
    return Result<MapData::TerrainTextureTable>::Failure("Terrain.TextureSlot", "TERRAIN_TILESET_TABLE", "贴图表只接受ground或cliff。");
}

Result<MapData::TerrainPaletteEditIntent> NormalizeTerrainTextureSlotCommand(
    const TerrainTextureSlotCommand& command, const MapData::TerrainHeader* current) {
    using Output = Result<MapData::TerrainPaletteEditIntent>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure("Terrain.TextureSlot", code, message); };
    if (command.table != MapData::TerrainTextureTable::Ground && command.table != MapData::TerrainTextureTable::Cliff)
        return fail("TERRAIN_TILESET_TABLE", "贴图表只接受ground或cliff。");
    const auto rawcode = [](std::string_view value) { return value.size() == 4
        && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c <= 126; }); };
    const bool append = command.operation == TerrainTextureSlotOperation::Append;
    const bool remap = command.operation == TerrainTextureSlotOperation::Remap;
    if (command.operation != TerrainTextureSlotOperation::Replace && !append && !remap)
        return fail("TERRAIN_TEXTURE_SLOT_OPERATION", "贴图槽仅接受替换、表尾追加或地面重映射。");
    if (remap && command.table != MapData::TerrainTextureTable::Ground)
        return fail("TERRAIN_TEXTURE_REMAP_TABLE", "贴图重映射只用于地面表。");
    if (remap && command.expectedRawcode == command.rawcode)
        return fail("TERRAIN_TILESET_REMAP_SAME", "原贴图和目标贴图相同。");
    const auto maximum = command.table == MapData::TerrainTextureTable::Ground ? 16u : 15u;
    if (command.slot >= maximum || (append && command.slot == 0))
        return fail("TERRAIN_TILESET_SLOT", "槽号从0起；地面最多16槽、悬崖最多15槽，追加须绑定非空表的当前长度。");
    if (!rawcode(command.rawcode) || (append ? !command.expectedRawcode.empty() : !rawcode(command.expectedRawcode)))
        return fail("TERRAIN_TEXTURE_SLOT_RAWCODE", "rawcode须为四个可显示ASCII字符；替换必须给旧值，追加不接受旧值。");
    if (current) {
        const auto& table = command.table == MapData::TerrainTextureTable::Ground ? current->groundTilesets : current->cliffTilesets;
        if (table.empty() || table.size() > maximum)
            return fail("TERRAIN_TILESET_SLOT", "当前贴图表不在受支持的非空经典表范围内。");
        if (append ? command.slot != table.size() : command.slot >= table.size() || table[command.slot] != command.expectedRawcode)
            return fail("TERRAIN_JOURNAL_PALETTE_DRIFT", "执行到本条时的旧rawcode或表长与期望不符，未发布任何前缀。");
        if (remap && std::find(table.begin(), table.end(), command.rawcode) == table.end())
            return fail("TERRAIN_TILESET_REMAP_TARGET_MISSING", "目标贴图不在地面贴图表里；不在表里时应直接替换槽位。");
    }
    return Output::Success({command.table, command.slot, command.expectedRawcode, command.rawcode,
        std::uint8_t(remap ? 4 : append ? (command.table == MapData::TerrainTextureTable::Ground ? 2 : 3) : 1)});
}
} // namespace War3Map::Services
