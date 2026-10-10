#pragma once

#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/Render/ModelAnimation.h"

#include <deque>

namespace War3Map::Services {

struct WorldInstanceBinding {
    std::uint64_t sourceGeneration = 0, mapRevision = 0, editSessionIdentity = 0, editRevision = 0;
    std::string sourceIdentity;
    bool operator==(const WorldInstanceBinding&) const noexcept = default;
};
struct WorldRegisteredPlacement {
    MapData::PlacementId row;
    War3RenderCore::InstanceIdentity identity;
};
struct WorldInstanceHistoryLimits {
    std::size_t maximumCommands = 128;
    std::size_t maximumBytes = 64ull * 1024 * 1024; // 只限制可恢复撤销记录，不限制地图实例。
};
class WorldInstanceRegistry;
struct WorldInstanceRegistryChange {
    std::shared_ptr<const WorldInstanceRegistry> registry;
    Render::AnimationInstanceDelta delta;
};

// UI/Agent共同使用的Qt-free只读绑定。Prepare不修改本对象；宿主只在同一修订CAS
// 和CPU/GPU实例事务都成功后发布返回的registry。无谱系必须完整重建，禁止猜测身份。
class WorldInstanceRegistry {
public:
    [[nodiscard]] static Result<std::shared_ptr<const WorldInstanceRegistry>> Create(
        const MapDocument& document, WorldInstanceBinding binding,
        std::span<const Render::ModelInstanceRequest> requests, std::span<const MapData::PlacementId> rows,
        const Render::AnimationInstanceSpec& defaults = {}, WorldInstanceHistoryLimits limits = {});
    [[nodiscard]] Result<WorldInstanceRegistryChange> Prepare(
        const MapData::MapDataEditSession::EditReceipt& receipt, const WorldInstanceBinding& expected,
        std::uint64_t nextMapRevision, const MapDocument& nextDocument,
        std::span<const Render::ModelInstanceRequest> requests, std::span<const MapData::PlacementId> rows,
        const Render::AnimationInstanceSpec& newInstanceDefaults = {}) const;
    [[nodiscard]] const WorldInstanceBinding& Binding() const noexcept { return binding_; }
    [[nodiscard]] const std::vector<WorldRegisteredPlacement>& Placements(MapData::PlacementFile file) const noexcept;
    [[nodiscard]] const std::vector<Render::AnimationInstanceSpec>& CapturedSpecs() const noexcept { return captured_; }
    [[nodiscard]] std::optional<War3RenderCore::InstanceIdentity> Find(const MapData::PlacementId& row) const noexcept;
    // 两个已验证谱系快照的差集，合并连续正式提交；身份来自谱系，绝不按模型/位置匹配。
    [[nodiscard]] Result<Render::AnimationInstanceDelta> DeltaTo(const WorldInstanceRegistry& next) const;
private:
    struct SnapshotIdentity {};
    // 只复用由Prepare完整核验出的相邻差集；令牌不持有前驱整棵地图/历史。
    // 跳过中间修订或来自另一注册器时，DeltaTo仍执行完整身份比较。
    std::shared_ptr<const SnapshotIdentity> snapshotIdentity_ = std::make_shared<const SnapshotIdentity>();
    std::shared_ptr<const SnapshotIdentity> previousSnapshotIdentity_;
    std::shared_ptr<const Render::AnimationInstanceDelta> previousDelta_;
    struct RememberedRow { std::uint32_t row; War3RenderCore::InstanceIdentity identity; };
    struct FileMemory { MapDataKind kind; std::vector<RememberedRow> removed, added; };
    struct CommandMemory {
        std::uint64_t identity = 0;
        bool forward = true;
        std::vector<FileMemory> files;
        [[nodiscard]] std::size_t Bytes() const noexcept;
    };
    [[nodiscard]] Status Capture(std::span<const Render::ModelInstanceRequest> requests,
        std::span<const MapData::PlacementId> rows, const Render::AnimationInstanceSpec& defaults);
    WorldInstanceBinding binding_;
    WorldInstanceHistoryLimits limits_;
    std::array<std::vector<WorldRegisteredPlacement>,2> rows_;
    std::array<std::string,2> memberHashes_;
    std::vector<Render::AnimationInstanceSpec> captured_;
    std::deque<std::shared_ptr<const CommandMemory>> history_;
};

} // namespace War3Map::Services
