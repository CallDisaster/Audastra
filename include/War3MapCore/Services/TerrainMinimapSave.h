#pragma once

#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/Render/ClassicMinimapObjectAssets.h"
#include "War3MapCore/Render/TerrainMinimapAssets.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/TerrainMinimapSaveChoice.h"

namespace War3Map::Services {

struct TerrainMinimapSaveLimits {
    // 只可收紧。计本单元拥有的容器、重叠task替换副本和有界临时工作；
    // 不含预存archive/edits、MPQ挂载、分配器/CNG provider内部内存或进程RSS。
    std::size_t maxWorkingBytes = 128 * 1024 * 1024;
    MapData::TerrainSavePlanLimits plan;
    Render::TerrainMinimapAssetCaptureLimits assets;
    Render::TerrainMinimapLimits candidate;
    Render::ClassicMinimapObjectAssetLimits objects;
    Render::ClassicMinimapCandidateLimits classic;
};

// 单次小地图保存资格。构造器封闭；输入必须是FreezeArchive后的真实任务，
// 不接caller像素、BLP、SHA或候选。Automatic用于普通保存/另存；保留旧
// missing-only策略只用于既有回归。任何策略都不得放宽地形与通行保存门。
// 调用方串行排除edits写入；成功不等于native编辑器/游戏验证。
class TerrainMinimapSave final {
public:
    [[nodiscard]] static Result<std::shared_ptr<const TerrainMinimapSave>> Prepare(
        const MapSaveTask& actualTask, const MapData::MapDataEditSession& edits,
        const std::filesystem::path& gameRoot,
        TerrainMinimapSaveChoice choice = TerrainMinimapSaveChoice::Automatic,
        const TerrainMinimapSaveLimits& limits = {});

    // 仅查看真实冻结来源及当前最终成员列表。已有 TGA（包括空成员）一律
    // 不补写 BLP；否则在地图缺少 BLP、地形或对象净变化时自动重建。
    [[nodiscard]] static Result<bool> RequiresAutomaticGeneration(const MapSaveTask& actualTask);

    // Preserve、已有TGA或Automatic判定未变化时返回null。非null时只append这一份MapBLP，
    // 并以Plan替换旧Preserve计划；W3E/WPM已在任务里，不能再次append整份计划。
    [[nodiscard]] const std::shared_ptr<const MapData::TerrainSavePlan>& Plan() const noexcept { return plan_; }
    [[nodiscard]] const MemberReplacement& MinimapReplacement() const noexcept { return plan_->Replacements().back(); }
    [[nodiscard]] const Render::TerrainMinimapManifest& Manifest() const noexcept { return manifest_; }
    [[nodiscard]] const Render::ClassicMinimapManifest* ClassicManifest() const noexcept {
        return classicManifest_ ? &*classicManifest_ : nullptr;
    }
    // 只描述实际encoded bytes经唯一decoder所得mip0；量化时不使用原渲染RGBA摘要。
    [[nodiscard]] const Render::TerrainMinimapMipFacts& DecodedMip0() const noexcept { return decodedMip0_; }

    [[nodiscard]] Status VerifyCurrent(const MapSaveTask& task, const MapData::MapDataEditSession& edits,
        const std::filesystem::path& gameRoot) const;
    // 在脚本/native模板最终列表形成后、发布前调用；重新核game winner与源/目标身份。
    // 各次来源guard返回即释放，仅证明本次检查时点；不宣称连续game租约。
    [[nodiscard]] Status VerifyFrozenOutput(const MapSaveTask& task) const;
    [[nodiscard]] Status VerifyReadback(const MapSaveTask& actualTask, const MapArchiveSession& actualSaved) const;

private:
    TerrainMinimapSave() = default;
    [[nodiscard]] static Result<std::shared_ptr<const TerrainMinimapSave>> PrepareAutomatic(
        const MapSaveTask& task, const MapData::MapDataEditSession& edits,
        const std::filesystem::path& gameRoot, const TerrainMinimapSaveLimits& limits);
    [[nodiscard]] Status VerifyAutomaticCurrent(const MapSaveTask& task, const MapData::MapDataEditSession& edits,
        const std::filesystem::path& gameRoot) const;
    [[nodiscard]] Status VerifyAutomaticTaskIdentity(const MapSaveTask& task) const;
    [[nodiscard]] Status VerifyAutomaticFrozenOutput(const MapSaveTask& task) const;
    [[nodiscard]] Status VerifyAutomaticReadback(const MapSaveTask& task, const MapArchiveSession& saved) const;
    [[nodiscard]] Status VerifyTask(const MapSaveTask& task, bool beforeAdmission) const;
    [[nodiscard]] std::size_t RetainedCapacityBytes() const;
    std::unique_ptr<const Render::TerrainMinimapAssetSnapshot> assets_;
    std::unique_ptr<const Render::ClassicMinimapObjectAssets> classicAssets_;
    std::optional<Render::ClassicMinimapManifest> classicManifest_;
    std::shared_ptr<const MapData::TerrainSavePlan> plan_;
    Render::TerrainMinimapManifest manifest_;
    Render::TerrainMinimapMipFacts decodedMip0_;
    TerrainMinimapSaveLimits limits_;
    bool automatic_ = false;
    std::filesystem::path sourcePath_, targetPath_;
    std::size_t taskReplacementCapacityCeiling_ = 0;
    const MapSaveTask* taskIdentity_ = nullptr; // 仅比较身份，不借用其中输入。
    const MapArchiveSession* archiveIdentity_ = nullptr;
};

} // namespace War3Map::Services
