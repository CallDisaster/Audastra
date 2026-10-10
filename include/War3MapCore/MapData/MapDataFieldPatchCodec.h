#pragma once

#include "War3MapCore/EditorSession.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"

namespace War3Map::MapData {

// 纯字段冻结转为既有目录事务密封；requestedKinds 也包含恢复原值的当前成员。
// 不能从协议提供原生替换字节，不能增加、删除或改写其它地图成员。
class MapDataFieldPatchCodec {
public:
    [[nodiscard]] static Result<MapDataPatch> Seal(const MapDocument& original,
        const MapDocument& current, const WorkspaceSnapshot& source,
        std::span<const MapDataKind> requestedKinds);
};

} // namespace War3Map::MapData
