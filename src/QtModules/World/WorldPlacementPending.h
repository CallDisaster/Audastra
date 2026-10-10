#pragma once

#include "War3MapCore/Services/WorldInstanceRegistry.h"
#include "War3MapCore/Render/EditorOverlay.h"
#include "WorldInstanceTiming.h"

namespace War3Map::Modules {

// 正式地图新增记录的临时呈现。创建命令身份与渲染实例身份是两个编号空间；
// 只允许已验证谱系的registry完成映射。线框仅表示等待模型，不表示模型尺寸/占地。
class WorldPlacementPending {
public:
    struct Entry {
        std::uint64_t editSession = 0, command = 0, createdRevision = 0;
        MapDataKind member = MapDataKind::Units;
        std::string memberSha;
        MapData::PlacementId createdRow;
        std::optional<War3RenderCore::InstanceIdentity> renderIdentity;
        std::optional<MapData::PlacementId> currentRow;
        Render::ModelInstanceRequest request;
        WorldPlacementCommandTiming timing;
    };
    [[nodiscard]] Status Remember(const Services::WorldInstanceBinding& binding,
        const MapData::MapDataEditSession::EditReceipt& receipt, const MapDocument& after,
        std::span<const MapData::PlacementId> rows, std::span<const Render::ModelInstanceRequest> requests);
    // registry必须是当前正式地图修订的候选；首次映射须紧跟对应创建命令。
    [[nodiscard]] Status Reconcile(std::shared_ptr<const Services::WorldInstanceRegistry> registry,
        const MapDocument* current = nullptr);
    // completed仅可来自真正完成帧或已完成roster票据，不能用刚提交的CPU目标签收。
    void AppendLines(Render::OverlayLines& lines, const Services::WorldInstanceBinding& current,
        const Render::AnimatedModelScene* completedScene = nullptr, const Render::AnimationFrame* completed = nullptr,
        const std::function<bool(const MapData::PlacementId&)>& visible = {}) const;
    void Clear() noexcept;
    [[nodiscard]] const std::vector<Entry>& Entries() const noexcept { return entries_; }
    [[nodiscard]] std::vector<WorldPlacementCommandTiming> TimingCommands(
        std::uint64_t afterRevision, std::uint64_t throughRevision) const;
private:
    friend class WorldModule;
    void RecordCreatedTiming(std::uint64_t command, std::uint64_t inputNs, std::uint64_t transactionNs);
    void RecordCaptureTiming(const WorldInstanceTiming& timing);
    // Qt事务适配器专用：rows必须是刚完成Add/Copy的返回值，不能接Agent任意行请求。
    [[nodiscard]] Status RememberCreated(const Services::WorldInstanceBinding& binding,
        const MapData::MapDataEditSession::EditReceipt& receipt, std::span<const MapData::PlacementId> rows,
        std::span<const Render::ModelInstanceRequest> requests);
    [[nodiscard]] Status RememberImpl(const Services::WorldInstanceBinding& binding,
        const MapData::MapDataEditSession::EditReceipt& receipt, const MapDocument* after,
        std::span<const MapData::PlacementId> rows, std::span<const Render::ModelInstanceRequest> requests);
    std::optional<Services::WorldInstanceBinding> binding_;
    std::vector<Entry> entries_;
};

// 缓存摆放预览只移动已接受姿势，不重新读取资产/求骨架。只接受平移；
// 相机billboard/粒子依赖仍由后台Prepare更新，此快路径不伪造新的求值回执。
[[nodiscard]] Result<std::shared_ptr<const Render::AnimationFrame>> TranslatePlacementPreviewFrame(
    const Render::AnimatedModelScene& scene, const Render::AnimationFrame& previous,
    std::span<const std::array<float, 12>> transforms, std::uint64_t contentVersion);

} // namespace War3Map::Modules
