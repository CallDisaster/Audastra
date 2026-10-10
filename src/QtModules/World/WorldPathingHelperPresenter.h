#pragma once

#include "WorldPlacementModels.h"
#include "WorldTerrainAssets.h"
#include "WorldInstanceTiming.h"
#include "War3MapCore/Services/WorldInstanceRegistry.h"

#include <map>

namespace War3Map::Modules {

// 所有方法由既有资源worker调用；无QObject、第二套线程或额外渲染/保存通道。
struct WorldPathingHelperCache {
    std::string resourceIdentity;
    std::shared_ptr<WorldResourceReader> sourceOwner; // 当前worker读取闭包的guard，不累积整图reader。
    std::map<std::string, Result<Resources::ResolvedResource>> resources;
    std::map<std::string, std::shared_ptr<const Render::OwnedStaticModelAsset>> assets;
    std::size_t resourceBytes = 0;
};
struct WorldPathingHelperBinding {
    std::uint32_t requestIndex = 0;
    std::shared_ptr<const Services::PlacementPresentationRow> row; // 原捕获事实，绝不重贴owner/version/inputIdentity。
    std::optional<MapData::PlacementId> currentOwner; // 由当前registry的已验证谱系重映射。
};
struct WorldPathingHelperFact {
    ObjectData::ObjectKey object;
    std::shared_ptr<const Services::PlacementPresentationFields> fields;
    std::shared_ptr<const Services::PlacementPresentationRow> row;
    std::shared_ptr<const Render::OwnedStaticModelAsset> asset;
    std::string assetKey;
    std::vector<Diagnostic> diagnostics;
    bool helper = false, unknownFootprint = false, unavailableClassification = false;
};
struct WorldPathingHelperRecord {
    std::uint32_t requestIndex = 0;
    std::optional<MapData::PlacementId> currentOwner;
    std::shared_ptr<const WorldPathingHelperFact> fact;
};
struct WorldPathingHelperPrepared {
    std::shared_ptr<const WorldPathingHelperCache> cache;
    std::vector<WorldPathingHelperBinding> helpers;
    std::vector<std::uint8_t> helperRequests; // 原请求索引；显隐为O(1)，不是实例总数上限。
    std::vector<Diagnostic> diagnostics;
    std::size_t unknownFootprints = 0, unavailableClassifications = 0;
    WorldPathingHelperTimings timings;
    Services::WorldSelectionVersion version; // 本次绑定，不改变所引用事实的捕获身份。
    std::vector<WorldPathingHelperRecord> records; // 包含普通/不可用分类，增量不会重查未变实例。
};

// 身份覆盖完整owned来源层序和有效目录版本，不是长期磁盘租约。
[[nodiscard]] Result<std::string> WorldPathingHelperResourceIdentity(
    const WorldTerrainSources& sources, std::uint64_t catalogRevision);
[[nodiscard]] Services::PlacementPresentationResourceReader WorldPathingHelperResourceReader(
    std::shared_ptr<WorldResourceReader> source);
[[nodiscard]] Result<std::string> WorldPathingHelperCandidateIdentity(
    std::span<const WorldPlacementPresentationCandidate> candidates,
    const Services::WorldSelectionVersion& version, std::string_view resourceIdentity);
// context必须绑定真实当前版本/来源；reader可由原创fixture注入。仅已证辅助分类生成checker。
// requests仍保留原modelPath；修改的是渲染请求，无原图/placement字段写入。
[[nodiscard]] Result<WorldPathingHelperPrepared> PrepareWorldPathingHelpers(
    std::vector<Render::ModelInstanceRequest>& requests,
    std::span<const WorldPlacementPresentationCandidate> candidates,
    const Services::PlacementPresentationContext& context,
    std::shared_ptr<WorldResourceReader> sourceOwner = {},
    std::shared_ptr<const WorldPathingHelperCache> previous = {});

// before/after必须来自同一编辑谱系；来源、目录或失败查找不能证明未变时走完整查询。
// 旧row仍是原捕获证据，只有当前owner/请求索引由registry映射；无独立保存/渲染通道。
[[nodiscard]] Result<Services::WorldSelectionVersion> BindWorldPathingHelperVersion(
    const Services::WorldSelectionVersion& captured,
    const Services::WorldInstanceRegistry& before, const Services::WorldInstanceRegistry& after);
[[nodiscard]] Result<WorldPathingHelperPrepared> PrepareWorldPathingHelperDelta(
    std::vector<Render::ModelInstanceRequest>& requests,
    std::span<const WorldPlacementPresentationCandidate> candidates,
    const Services::PlacementPresentationContext& context,
    std::shared_ptr<WorldResourceReader> sourceOwner,
    std::shared_ptr<const WorldPathingHelperPrepared> previous,
    const Services::WorldInstanceRegistry& before, const Services::WorldInstanceRegistry& after,
    std::span<const War3RenderCore::InstanceIdentity> renderIdentities);

} // namespace War3Map::Modules
