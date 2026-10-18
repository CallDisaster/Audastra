#pragma once

#include "War3MapCore/MapData/TerrainSavePlan.h"
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
};

// 单次显式missing-only资格。构造器封闭；输入必须是FreezeArchive后的真实任务，
// 不接caller像素、BLP、SHA或候选。生成域仍与原干地height保存门求交。
// 调用方串行排除edits写入；成功不等于native编辑器/游戏验证。
class TerrainMinimapSave final {
public:
    [[nodiscard]] static Result<std::shared_ptr<const TerrainMinimapSave>> Prepare(
        const MapSaveTask& actualTask, const MapData::MapDataEditSession& edits,
        const std::filesystem::path& gameRoot,
        TerrainMinimapSaveChoice choice = TerrainMinimapSaveChoice::Preserve,
        const TerrainMinimapSaveLimits& limits = {});

    // Preserve成功返回null，不捕获资源、不生成。非null时调用者只append这一份MapBLP，
    // 并以Plan替换旧Preserve计划；W3E/WPM已在任务里，不能再次append整份计划。
    [[nodiscard]] const std::shared_ptr<const MapData::TerrainSavePlan>& Plan() const noexcept { return plan_; }
    [[nodiscard]] const MemberReplacement& MinimapReplacement() const noexcept { return plan_->Replacements().back(); }
    [[nodiscard]] const Render::TerrainMinimapManifest& Manifest() const noexcept { return manifest_; }
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
    [[nodiscard]] Status VerifyTask(const MapSaveTask& task, bool beforeAdmission) const;
    [[nodiscard]] std::size_t RetainedCapacityBytes() const;
    std::unique_ptr<const Render::TerrainMinimapAssetSnapshot> assets_;
    std::shared_ptr<const MapData::TerrainSavePlan> plan_;
    Render::TerrainMinimapManifest manifest_;
    Render::TerrainMinimapMipFacts decodedMip0_;
    TerrainMinimapSaveLimits limits_;
    std::filesystem::path sourcePath_, targetPath_;
    std::size_t taskReplacementCapacityCeiling_ = 0;
    const MapSaveTask* taskIdentity_ = nullptr; // 仅比较身份，不借用其中输入。
    const MapArchiveSession* archiveIdentity_ = nullptr;
};

} // namespace War3Map::Services
