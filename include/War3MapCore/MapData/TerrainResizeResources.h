#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/TerrainResizeEditing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/Workspace.h"

#include <functional>
#include <memory>

namespace War3Map::Services { class TerrainResizeEditService; class TerrainPathingSave; }

namespace War3Map::MapData {

class MapDataEditSession;

// 真实来源与当前会话共同建立的尺寸规则/空间预检owner。调用方只能读取，
// 不能用自报尺寸限制、摘要、已验证布尔或任意SHD取得命令资格。
class TerrainResizeResources {
public:
    [[nodiscard]] const TerrainResizeDimensionRules& DimensionRules() const noexcept;
    [[nodiscard]] const std::array<float, 4>& CameraMargins() const noexcept;
    // 物理来源的固定成员及明确absence。后续resize的当前SHD由会话typed状态给出，
    // 不能把这里的旧物理SHD当成当前尺寸数据。
    [[nodiscard]] const ArchiveExactMember& SourceShadow() const noexcept;
    [[nodiscard]] std::span<const TerrainWaterSpecialDoodadShape> SpecialShapes() const noexcept;
    [[nodiscard]] const std::string& DependencyFingerprint() const noexcept;
    [[nodiscard]] std::size_t CaptureBudgetBytes() const noexcept;
    [[nodiscard]] std::size_t OwnedCapacityBytes() const noexcept;
    // 捕获时实际累计的工作计数。批量重放须累计每个owner的计数，
    // 不能用内存容量代替，也不能让每笔单独享用整批工作上限。
    [[nodiscard]] std::size_t CaptureWork() const noexcept;
    // 同一session/修订/原始W3E与当前W3E、完整空间成员逐项绑定；按intent的
    // 新外矩形核查实际位置/覆盖，不移动、裁短或删除普通对象、区域、镜头和起点。
    [[nodiscard]] Status VerifyInput(const MapDataEditSession& session,
        const TerrainResizeIntent& intent, std::size_t maximumBytes, std::size_t maximumWork) const;
    // 宿主在发布前重新读取真实来源；I/O、来源变化与预算错误不降级。
    [[nodiscard]] Status VerifySources(const MapArchiveSession& source, const MapDataEditSession& session,
        const TerrainResizeLimits& limits = {}) const;
    [[nodiscard]] Status VerifySources(const WorkspaceSnapshot& source, const MapDataEditSession& session,
        const TerrainResizeLimits& limits = {}) const;
    // 序列重放结束时复核捕获时的实际来源；编辑CAS仍由VerifyInput独立检查。
    [[nodiscard]] Status VerifySourceInputs(const MapArchiveSession& source,
        const TerrainResizeLimits& limits = {}) const;
    [[nodiscard]] Status VerifySourceInputs(const WorkspaceSnapshot& source,
        const TerrainResizeLimits& limits = {}) const;

private:
    struct State;
    explicit TerrainResizeResources(std::shared_ptr<const State> state) : state_(std::move(state)) {}
    using MemberReader = std::function<Result<std::vector<ArchiveExactMember>>(
        std::span<const std::string>, const ArchiveExactMemberLimits&)>;
    [[nodiscard]] static Result<TerrainResizeResources> CaptureInputs(const MemberReader& reader,
        const MapDataEditSession& session, const std::filesystem::path& gameRoot,
        const TerrainResizeLimits& limits);
    [[nodiscard]] Status VerifySources(const MemberReader& reader, const MapDataEditSession& session,
        const TerrainResizeLimits& limits) const;
    [[nodiscard]] Status VerifySourceInputs(const MemberReader& reader, const TerrainResizeLimits& limits) const;
    std::shared_ptr<const State> state_;
    friend class Services::TerrainResizeEditService;
    friend class Services::TerrainPathingSave;
};

} // namespace War3Map::MapData
