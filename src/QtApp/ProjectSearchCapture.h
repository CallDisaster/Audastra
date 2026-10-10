#pragma once

#include "EditorProject.h"
#include "War3MapCore/Services/ProjectSearch.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace War3Map::QtApp {

struct ProjectSearchCaptureOptions {
    std::size_t maxAssetInputRows = 8192; // 源成员+全部edit+pending；硬帽32768，不接受前缀准入。
    std::size_t maxAssetPathBytes = 1024 * 1024; // 总UTF8路径；硬帽1MiB，单路径2048B。
    std::size_t maxCatalogSourceRows = 256; // 完整manifest/document数；硬帽1024。
    std::size_t maxCatalogDefinitions = 32768; // manifest保守定义账；硬帽65536。
    std::size_t maxCatalogBytes = 8 * 1024 * 1024; // 原始文档+manifest元数据；硬帽8MiB。
    std::size_t maxMapBytes = 16 * 1024 * 1024; // 当前七成员Snapshot原始输入字节账；硬帽64MiB，不是RSS帽。
    [[nodiscard]] bool operator==(const ProjectSearchCaptureOptions&) const = default;
};

struct ProjectSearchCaptureStats {
    std::uint64_t prepareCalls = 0, typePreparations = 0, typeCacheHits = 0;
    std::uint64_t assetPreparations = 0, assetCacheHits = 0, assetMergeCalls = 0;
    std::uint64_t mapPreparations = 0, mapCacheHits = 0, mapSnapshotCalls = 0;
    std::uint64_t objectPreparations = 0, objectCacheHits = 0;
    std::size_t assetInputRows = 0, assetPathBytes = 0, ambiguousAssets = 0;
    std::size_t catalogSourceRows = 0, catalogDefinitions = 0, catalogBytes = 0;
    // Snapshot内部先核当前覆盖字节再复制；单位/装饰物附带W3I重复输入也收费。
    // FromMembers解析结构和临时复制另有开销；此数不是实际进程峰值内存。
    std::size_t mapSnapshotBytesCharged = 0, mapUniqueBytes = 0;
    bool typesReady = false;
    Services::ProjectSearchState assetState = Services::ProjectSearchState::Unavailable;
    Services::ProjectSearchState mapState = Services::ProjectSearchState::Unavailable;
    std::string typeReason, assetReason, mapReason, catalogManifestSha256;
};

// 文档所属线程内同步Prepare→Borrow→Search；借用结果/Assets不得跨修改、异步或下次Prepare保存。
// 仅Prepare首次/真实域身份变化时做有界准备。Borrow不打开、读盘、rescan、散列或自动准备。
// 固定预算是准备上限，与每页用户的SearchOptions独立；超预算域如实Unavailable，其他域保留。
class ProjectSearchCapture final {
public:
    ProjectSearchCapture();
    ~ProjectSearchCapture();
    ProjectSearchCapture(const ProjectSearchCapture&) = delete;
    ProjectSearchCapture& operator=(const ProjectSearchCapture&) = delete;
    [[nodiscard]] Status Prepare(const QtProject::EditorProject& project, std::string projectIdentity,
        std::uint64_t openedGeneration, std::string contextRevision,
        const ProjectSearchCaptureOptions& options = {});
    [[nodiscard]] Result<Services::ProjectSearchContext> Borrow(const QtProject::EditorProject& project,
        std::string_view expectedRevision) const;
    [[nodiscard]] std::span<const QtProject::AssetEntry> Assets() const;
    [[nodiscard]] const ProjectSearchCaptureStats& Stats() const;
    void Reset();
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace War3Map::QtApp
