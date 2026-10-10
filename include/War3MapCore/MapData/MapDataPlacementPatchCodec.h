#pragma once

#include "War3MapCore/EditorSession.h"
#include "War3MapCore/MapData/MapDataPlacementEditing.h"

namespace War3Map::MapData {

// 原意图完整重放后才可密封一个既有摆放成员；恢复原值也写入原 bytes。
class MapDataPlacementPatchCodec {
public:
    [[nodiscard]] static Result<MapDataPatch> Seal(const MapDocument& original,
        const MapDocument& current, const WorkspaceSnapshot& source,
        const MapDataPlacementIntent& originalIntent);
};

} // namespace War3Map::MapData
