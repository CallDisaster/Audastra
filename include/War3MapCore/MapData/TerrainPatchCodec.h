#pragma once

#include "War3MapCore/EditorSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

namespace War3Map::Services { class TerrainPathingSave; }

namespace War3Map::MapData {

class TerrainSavePlan;

struct TerrainStrokeIntent {
    TerrainBrush brush;
    std::vector<TerrainStrokePoint> points;
    bool followDoodads = false;
    // 只有选项；anchor始终从本笔当前地形的第一点重新捕获。
    std::optional<TerrainWaterStrokeOptions> waterBrush;
};

// 独立目录事务只接受从原 W3E/WPM 重放并逐字节证明的最终地形状态。
// 协议不能提供任意成员 bytes，未证实派生一致性的地形不能密封为可保存覆盖。
class TerrainPatchCodec {
public:
    [[nodiscard]] static Result<TerrainPatch> Seal(const MapDocument& original,
        std::span<const std::byte> terrain, std::span<const std::byte> pathing,
        const WorkspaceSnapshot& source, const MapDataEditSession& current,
        std::span<const TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot = {});
private:
    friend class Services::TerrainPathingSave;
    // 仅同一Services保存适配器能传入计划；仍完整重放并核会话/来源/最终字节。
    [[nodiscard]] static Result<TerrainPatch> SealWithPlan(const MapDocument& original,
        std::span<const std::byte> terrain, std::span<const std::byte> pathing,
        const WorkspaceSnapshot& source, const MapDataEditSession& current,
        std::span<const TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot,
        const TerrainSavePlan* prepared, const MapDataEditSession* trustedReplay = nullptr);
};

} // namespace War3Map::MapData
