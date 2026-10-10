#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Workspace.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::Services {

// revision 是宿主原修订令牌，不转换为数字；无独立资产修订时 assetRevision 使用同一令牌。
struct ImportInventoryBinding {
    std::string documentId;
    std::uint64_t generation = 0;
    std::string revision;
    std::string sourceSha256;
    std::string assetRevision;
    std::string contentScope = "committed-source";
    [[nodiscard]] bool operator==(const ImportInventoryBinding&) const = default;
};

struct ImportPathEdit { std::string from, to; }; // to 为空表示待删除，均相对原来源。
struct ImportPendingMetadata {
    std::string archivePath;
    std::optional<std::uint64_t> reportedBytes; // 队列声明值，不是冻结内容身份。
};
struct ImportInventoryOverlay {
    bool includesUnsavedChanges = false;
    std::vector<ImportPathEdit> pathEdits;
    std::vector<ImportPendingMetadata> pending;
};

struct ImportInventoryRow {
    std::string importId;
    std::string kind; // imp-record / physical-member / queued-unfrozen
    std::string rawPath; // IMP 原始字节或来源原名字；适配器必须无损表示。
    std::optional<std::uint8_t> flags;
    std::optional<std::string> sourcePath; // 唯一解析出的原来源成员；歧义不选择。
    std::optional<std::string> archivePath; // 叠加现有资产改名后的目标路径；删除为 null。
    std::string sourceLayer;
    std::string state; // present / missing / unresolved / queued-unfrozen
    std::string overlayState = "original"; // original / moved / removed / queued
    std::optional<std::uint64_t> bytes;
    std::optional<std::string> sha256;
    std::optional<std::uint64_t> reportedBytes;
    // 歧义时保留两个已核验候选。仅身份，不含正文；所有载荷一律 opaque。
    std::vector<ArchiveExactMemberIdentity> candidates;
    std::vector<Diagnostic> diagnostics;
};

struct ImportInventoryDescription {
    ImportInventoryBinding binding;
    std::string inventorySha256;
    std::string sourceKind; // archive / directory-literal-paths
    std::string impState; // present / missing
    std::optional<std::string> impSourcePath;
    std::optional<std::string> impSha256;
    std::uint64_t impRecordCount = 0, physicalMemberCount = 0, queuedCount = 0;
    // 仅计本输入快照中显式出现后跳过的保留名；不声称枚举归档内部metadata。
    std::uint64_t excludedArchiveMetadataCount = 0;
    bool hasOpaqueArchiveMembers = false;
    bool includesUnsavedChanges = false;
};

struct ImportInventoryCursor {
    ImportInventoryBinding binding;
    std::string inventorySha256;
    std::string query;
    std::uint64_t nextIndex = 0;
    [[nodiscard]] bool operator==(const ImportInventoryCursor&) const = default;
};

struct ImportInventoryPage {
    ImportInventoryDescription description;
    std::vector<ImportInventoryRow> rows;
    std::optional<ImportInventoryCursor> next;
    std::optional<std::uint64_t> matchedTotal;
    std::uint64_t knownRowCount = 0;
    std::size_t examined = 0;
    bool scanBudgetExhausted = false;
};

class ImportInventorySnapshot {
public:
    struct Impl;
    explicit ImportInventorySnapshot(std::shared_ptr<const Impl> impl);
    [[nodiscard]] const ImportInventoryDescription& Description() const noexcept;
    [[nodiscard]] Status VerifyCurrent() const;
    [[nodiscard]] Result<ImportInventoryPage> Query(const ImportInventoryBinding& expected,
        std::string query = {}, const std::optional<ImportInventoryCursor>& cursor = {},
        std::size_t limit = 64, std::size_t maxExamined = 4096) const;
    [[nodiscard]] Result<ImportInventoryRow> Get(const ImportInventoryBinding& expected,
        std::string_view expectedInventorySha256, std::string_view importId) const;
private:
    std::shared_ptr<const Impl> impl_;
};

// 仅解析 IMP；导入载荷只交既有 exact 身份服务散列，不解析/导出正文。归档引用必须
// 活到最后一次 Query/Get/VerifyCurrent。目录快照和路径身份由本服务持有，不借用临时数组。
// 初始 metadata 捕获最多262144行/32MiB、IMP最多8MiB；页谓词最多4096，最多128结果。
// 每次页/get的归档身份读取沿原 <=256名字/64MiB 总预算。目录采用原 SHA 快照和
// VerifyUnchanged，不把 root/map 的 IMP 来源别名扩展成其它目录/资源路径的猜测。
[[nodiscard]] Result<std::shared_ptr<const ImportInventorySnapshot>> CaptureImportInventory(
    const MapArchiveSession& source, ImportInventoryBinding binding,
    ImportInventoryOverlay overlay = {});
[[nodiscard]] Result<std::shared_ptr<const ImportInventorySnapshot>> CaptureImportInventory(
    const DirectoryWorkspace& source, ImportInventoryBinding binding,
    ImportInventoryOverlay overlay = {});

} // namespace War3Map::Services
