#include "McpWorkspace.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

// 与工作区本体同属纯C++缓存生命周期；不依赖Qt协议适配或工具注册。
namespace War3Map::Mcp {
const Trigger::TriggerTypeSystem* McpWorkspace::PreparedProjectSearchTypes() const noexcept {
    // 先核独立 owner 值，不能从可能引用已移动 Catalog 的类型系统读取 Catalog()。
    if (projectSearchTypesCatalogOwner_ != &catalog_ || projectSearchTypesCatalogSha_ != catalog_.Sha256()) return nullptr;
    return projectSearchTypes_.get();
}

Status McpWorkspace::PrepareProjectSearchTypesCache(std::size_t maxDefinitions, std::uintmax_t maxSourceBytes) {
    projectSearchTypes_.reset();
    projectSearchTypesCatalogOwner_ = nullptr;
    projectSearchTypesCatalogSha_.clear();
    if (projectSearchTypesGeneration_ == std::numeric_limits<std::uint64_t>::max())
        return Status::Failure("MCP.Search", "PROJECT_SEARCH_TYPES_GENERATION_LIMIT", "类型缓存准备代次已耗尽；工作区需要重新打开。");
    ++projectSearchTypesGeneration_;
    const auto& manifest = catalog_.Manifest();
    // Definitions() 会遍历所有生效定义。先用接受目录时已经形成的 Manifest 上界收费，
    // 重复层也收费，不能仅按 Define 子集低估这次准备的工作或字符串大小。
    if (!maxDefinitions || maxDefinitions > 32768 || !maxSourceBytes || maxSourceBytes > 32ULL * 1024 * 1024
        || manifest.empty() || manifest.size() > 1024 || catalog_.Sha256().empty())
        return Status::Failure("MCP.Search", "PROJECT_SEARCH_TYPES_BUDGET", "类型缓存准备的来源或预算无效；类型解析保持未准备。");
    std::size_t definitions = 0;
    std::uintmax_t sourceBytes = 0;
    for (const auto& source : manifest) {
        if (source.definitionCount > maxDefinitions - definitions || source.size > maxSourceBytes - sourceBytes)
            return Status::Failure("MCP.Search", "PROJECT_SEARCH_TYPES_BUDGET", "已接受 UI Catalog 超出一次类型准备的定义或源字节预算；查询不补建。");
        definitions += source.definitionCount;
        sourceBytes += source.size;
    }
    try {
        auto prepared = std::make_unique<const Trigger::TriggerTypeSystem>(catalog_);
        projectSearchTypesCatalogSha_ = catalog_.Sha256();
        projectSearchTypesCatalogOwner_ = &catalog_;
        projectSearchTypes_ = std::move(prepared);
    } catch (const std::exception&) {
        return Status::Failure("MCP.Search", "PROJECT_SEARCH_TYPES_PREPARATION_FAILED", "类型缓存准备失败；保留已接受地图，查询只展示部分覆盖，不重试构建。");
    }
    return Status::Ok();
}

std::shared_ptr<const Services::MapDataQuerySnapshot> McpWorkspace::PreparedProjectSearchMapCache(
    const Services::MapDataQueryBinding& expected) const {
    const auto document = activeEdit_ && activeEdit_->mapData ? activeEdit_->mapData : committedMapData_;
    if (!document || !projectSearchMapCache_ || projectSearchMapCache_->Binding() != expected
        || expected.revision != toolEpoch_ || expected.sourceSha256 != Revision().sourceSha256) return {};
    const auto owner = projectSearchMapCache_->OwnedDocument();
    if (owner.get() != document.get() || owner.owner_before(document) || document.owner_before(owner)) return {};
    return projectSearchMapCache_;
}

Status McpWorkspace::PrepareProjectSearchMapCache(const Services::MapDataQueryBinding& expected,
    std::shared_ptr<const Services::MapDataQuerySnapshot> snapshot) {
    const auto document = activeEdit_ && activeEdit_->mapData ? activeEdit_->mapData : committedMapData_;
    const auto owner = snapshot ? snapshot->OwnedDocument() : std::shared_ptr<const MapDocument>{};
    if (!snapshot || expected.documentId.empty() || !expected.generation || snapshot->Binding() != expected
        || expected.revision != toolEpoch_ || expected.sourceSha256 != Revision().sourceSha256
        || !document || owner.get() != document.get() || owner.owner_before(document) || document.owner_before(owner))
        return Status::Failure("MCP.Search", "PROJECT_SEARCH_CACHE_STALE", "已捕获快照不属于当前地图 owner 或完整 binding。");
    projectSearchMapCache_ = std::move(snapshot);
    return Status::Ok();
}

} // namespace War3Map::Mcp
