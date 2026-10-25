#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Workspace.h"

namespace War3Map::Services {
class TerrainPathingSave;

// 可选冷历史的保存前来源捕获；不写地图、不重建Undo栈、不扩大正式保存资格。
class TerrainHistorySave {
public:
    using Lease = MapData::MapDataEditSession::TerrainHistorySaveLease;
    [[nodiscard]] static bool NeedsSourceShadowBinding(const Lease& lease) noexcept;
    // 必须在源地图被覆盖前调用。W3E与SHD在同一实际来源读取中核验；
    // 返回新不可变lease，失败保留原lease与活动会话，宿主可警告后仅保留raw草稿。
    [[nodiscard]] static Result<Lease> BindSourceShadow(const MapArchiveSession& source,
        const Lease& lease, std::size_t maximumBytes = MapData::TerrainJournalMaximumBytes);
    [[nodiscard]] static Result<Lease> BindSourceShadow(const WorkspaceSnapshot& source,
        const Lease& lease, std::size_t maximumBytes = MapData::TerrainJournalMaximumBytes);

private:
    friend class TerrainPathingSave;
    using MemberReader = std::function<Result<std::vector<ArchiveExactMember>>(
        std::span<const std::string>, const ArchiveExactMemberLimits&)>;
    // 仅同一Services恢复链可传入已逐笔核验的旧历史reader，公共调用不接受reader或验证位。
    [[nodiscard]] static Result<Lease> BindSourceShadow(const MemberReader& reader,
        const Lease& lease, std::size_t maximumBytes);
};

} // namespace War3Map::Services
