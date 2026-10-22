#pragma once

#include "War3MapCore/EditorSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

namespace War3Map::MapData {

struct TerrainStrokeIntent {
    TerrainBrush brush;
    std::vector<TerrainStrokePoint> points;
    bool followDoodads = false;
};

// 独立目录事务只接受从原 W3E/WPM 重放并逐字节证明的最终地形状态。
// 协议不能提供任意成员 bytes，未证实派生一致性的地形不能密封为可保存覆盖。
class TerrainPatchCodec {
public:
    [[nodiscard]] static Result<TerrainPatch> Seal(const MapDocument& original,
        std::span<const std::byte> terrain, std::span<const std::byte> pathing,
        const WorkspaceSnapshot& source, const MapDataEditSession& current,
        std::span<const TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot = {});
};

} // namespace War3Map::MapData
