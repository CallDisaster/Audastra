#pragma once

#include "War3MapCore/EditorSession.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/MapData/MapDataPlacementEditing.h"
#include "War3MapCore/Services/MapDataQuery.h"
#include "War3MapCore/Services/ProjectSearch.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainResizeResources.h"
#include "War3MapCore/Services/ObjectCommands.h"
#include "War3MapCore/Services/AuxiliaryTransaction.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"

#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace War3Map::Services { class ObjectTransactionSnapshot; }
namespace War3Map::ObjectData { class EffectiveObjectCatalog; }
namespace War3Map::Trigger { class TriggerTypeSystem; }

namespace War3Map::Mcp {

struct McpObjectWorkspaceState;
struct McpTerrainWorkspaceState;
struct McpTerrainOverlay;

struct McpWorkspaceTerrainContext {
    const MapData::MapDataEditSession* session = nullptr;
    Services::MapDataQueryBinding document;
    bool includesActiveTransactionOverlay = false;
};

struct McpWorkspaceMapDataContext {
    std::shared_ptr<const MapDocument> original;
    std::shared_ptr<const MapDocument> document;
    Services::MapDataQueryBinding binding;
    bool includesActiveTransactionOverlay = false;
    std::shared_ptr<const Services::MapDataQuerySnapshot> preparedSnapshot;
};

// 共享候选仍在隔离副本时检查完整回执；失败不得进入 overlay。
using GraphEditPublishValidator = std::function<Status(
    const Trigger::TriggerDocument& before, const Trigger::TriggerDocument& after,
    const Trigger::TriggerGraphEditReceipt& receipt)>;

// 只能由协议层在完成固定 manifest、handoff 与逐文件 SHA-256 校验后构造。
// McpWorkspace 不接受路径，也不提供任意 custom-JASS 写入口。
struct VerifiedWarVKModule {
    std::string sourceManifestSha256;
    std::string wrapperSha256;
    std::string wrapperText;
};

// MCP 的唯一写入状态。不暴露裸文档写入口；每次成功编辑先由对应领域 Codec
// 生成带来源身份的密封补丁，再进入同一个 EditorSession overlay。
class McpWorkspace {
public:
    McpWorkspace(McpWorkspace&&) noexcept;
    McpWorkspace& operator=(McpWorkspace&&) noexcept;
    ~McpWorkspace();

    McpWorkspace(const McpWorkspace&) = delete;
    McpWorkspace& operator=(const McpWorkspace&) = delete;

    [[nodiscard]] static Result<std::unique_ptr<McpWorkspace>> Open(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& uiDataRoot, const std::filesystem::path& gameRoot = {});

    [[nodiscard]] ProjectRevision Revision() const;
    [[nodiscard]] bool HasActiveTransaction() const noexcept;
    [[nodiscard]] TransactionId ActiveTransactionId() const noexcept;
    [[nodiscard]] const Trigger::TriggerDocument& VisibleDocument() const noexcept;
    [[nodiscard]] const Trigger::UiCatalog& Catalog() const noexcept;
    [[nodiscard]] Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> MapObjectReferences() const;
    [[nodiscard]] EditorSession* Session() noexcept;
    [[nodiscard]] Status EnvironmentStatus() const;
    // 只查询当前已冻结的缓存 / 活动覆盖；不复验磁盘、不准备缺失缓存，返回 owned 页。
    [[nodiscard]] std::string CurrentProjectSearchRevision(std::string_view documentId,
        std::uint64_t documentGeneration) const;
    [[nodiscard]] Result<Services::ProjectSearchPage> SearchCachedProject(std::string_view documentId,
        std::uint64_t documentGeneration, std::string_view expectedContextRevision, std::string_view query,
        const Services::ProjectSearchOptions& options, std::size_t pageLimit,
        const Services::ProjectSearchCursor* cursor = nullptr) const;
    // 显式地图查询已完成 Capture 后交回；只接受当前真正拥有的文档与精确 binding。
    // 该非 const 准备不读取来源、不计算摘要；冷搜索不触发准备。
    [[nodiscard]] Status PrepareProjectSearchMapCache(const Services::MapDataQueryBinding& expected,
        std::shared_ptr<const Services::MapDataQuerySnapshot> snapshot);
    // 只在接受 UI Catalog 后显式准备一次；使用已有 Manifest 准入预算，不读盘、不计算正文摘要。
    // 移动工作区会改变 Catalog owner 地址；旧缓存随即不可借用，查询本身不得重新准备。
    [[nodiscard]] Status PrepareProjectSearchTypesCache(std::size_t maxDefinitions = 32768,
        std::uintmax_t maxSourceBytes = 32ULL * 1024 * 1024);
    [[nodiscard]] const Trigger::TriggerTypeSystem* PreparedProjectSearchTypes() const noexcept;
    [[nodiscard]] const std::filesystem::path& SourceRoot() const noexcept { return projectRoot_; }
    [[nodiscard]] const std::filesystem::path& GameRoot() const noexcept { return gameRoot_; }

    // 同一领域命令在 GUI 与独立 MCP 使用；独立宿主仍只写 EditorSession overlay。
    [[nodiscard]] std::string ObjectRevision() const;
    // 已捕获的只读来源；不会在查询来源信息时读取磁盘或授予写回资格。
    [[nodiscard]] std::shared_ptr<const ObjectData::EffectiveObjectCatalog> EffectiveObjectSources() const noexcept;
    [[nodiscard]] Result<Services::ObjectCommandContext> ObjectContext(ObjectData::ObjectType type);
    [[nodiscard]] Status ApplyObjectPlan(const Services::ObjectCommandPlan& plan);
    [[nodiscard]] Result<McpWorkspaceMapDataContext> MapDataContext(std::string documentId,
        std::uint64_t documentGeneration);
    [[nodiscard]] Result<MapData::MapDataEditSession::EditReceipt> ApplyMapDataPlan(
        const MapData::PreparedMapDataFields& plan, const MapData::MapDataPublishValidator& prepublish = {});
    [[nodiscard]] Result<MapData::MapDataEditSession::EditReceipt> ApplyPlacementPlan(
        const MapData::PreparedMapDataPlacement& plan, const MapData::MapDataPublishValidator& prepublish = {});
    [[nodiscard]] Services::AuxiliaryReferenceContext AuxiliaryReferences() const;
    [[nodiscard]] Result<MapData::MapDataEditSession::EditReceipt> ApplyAuxiliaryPlan(
        const Services::PreparedAuxiliaryEdit& plan, const MapData::MapDataPublishValidator& prepublish = {});
    [[nodiscard]] Result<McpWorkspaceTerrainContext> TerrainContext(std::string documentId,
        std::uint64_t documentGeneration);
    [[nodiscard]] Result<MapData::TerrainPathingResources> CaptureTerrainWaterResources(bool enforceWaterLimits,
        const MapData::TerrainGridSnapshot* prospective = nullptr);
    [[nodiscard]] Result<MapData::TerrainPathingResources> CaptureTerrainBoundsResources();
    [[nodiscard]] Result<MapData::TerrainResizeResources> CaptureTerrainResizeResources();
    [[nodiscard]] Status VerifyTerrainResizeResources(const MapData::TerrainResizeResources& owner) const;
    [[nodiscard]] Status VerifyTerrainWaterResources(const MapData::TerrainPathingResources& owner) const;
    [[nodiscard]] Result<MapData::TerrainStrokeResult> ApplyTerrainPlan(
        const Services::PreparedTerrainEdit& plan,
        const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& prepublish = {});
    [[nodiscard]] Services::ObjectDeletionAssessment AssessObjectDeletion(const ObjectData::ObjectKey& key);

    [[nodiscard]] Result<TransactionId> BeginEdit(
        const ProjectRevision& expectedRevision);
    [[nodiscard]] Result<StableId> CreateTrigger(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        std::int32_t categoryId,
        std::string name);
    [[nodiscard]] Result<StableId> InstallGeneratedWarVKModule(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        std::int32_t categoryId,
        VerifiedWarVKModule module);
    [[nodiscard]] Result<Trigger::EcaAppendReceipt> AppendRootEca(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        StableId triggerId,
        Trigger::UiDefinitionKind kind,
        std::string functionName);
    [[nodiscard]] Result<Trigger::EcaAppendReceipt> AppendArgumentCall(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        StableId argumentId,
        std::string functionName);
    [[nodiscard]] Status SetScalarArgument(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        StableId argumentId,
        std::int32_t valueKind,
        std::string value);
    [[nodiscard]] Result<Trigger::TriggerGraphEditReceipt> ApplyGraphEdit(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        const Trigger::TriggerGraphEditBatch& batch,
        const GraphEditPublishValidator& prepublish = {});
    [[nodiscard]] Result<SemanticDiff> PreviewEdit(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision);
    [[nodiscard]] ValidationReport ValidateEdit(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision);

    // 原目录出口只提交展开目录；归档候选必须使用下方显式来源绑定的保存出口。
    [[nodiscard]] Status ExportCandidate(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        const std::filesystem::path& outputPath);
    // 冻结当前已预览、已验证的完整 GUI 事务，走共用脚本/地图保存链。
    // 成功和失败都保留原目录事务，不把归档候选当作目录的新基准。
    [[nodiscard]] Result<Services::MapEditSaveReceipt> ExportArchiveCandidate(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        Services::MapEditSaveRequest request);
    [[nodiscard]] Status Rollback(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision);

private:
    [[nodiscard]] std::shared_ptr<const Services::MapDataQuerySnapshot> PreparedProjectSearchMapCache(
        const Services::MapDataQueryBinding& expected) const;
    struct ActiveEdit {
        TransactionId id = 0;
        ProjectRevision baseRevision;
        Trigger::TriggerDocument document;
        std::uint64_t generation = 0;
        std::optional<std::uint64_t> previewedGeneration;
        std::optional<std::uint64_t> validatedGeneration;
        std::shared_ptr<const Trigger::MapObjectReferenceSnapshot> usedMapObjectReferences;
        std::array<std::shared_ptr<const ObjectData::LniObjectDocument>, 7> objects;
        std::shared_ptr<const MapDocument> mapData;
        std::shared_ptr<const MapData::MapDataPlacementIntent> placementIntent;
        std::shared_ptr<const Services::AuxiliaryTransactionSnapshot> auxiliary;
        std::shared_ptr<McpTerrainOverlay> terrain;
    };

    McpWorkspace(
        std::filesystem::path projectRoot,
        std::filesystem::path uiDataRoot,
        std::filesystem::path wtgRelativePath,
        std::filesystem::path wctRelativePath,
        Trigger::UiCatalog catalog,
        Trigger::TriggerDocument document,
        EditorSession session);

    [[nodiscard]] Status CheckActive(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision) const;
    [[nodiscard]] Status CheckIdentity(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision) const;
    [[nodiscard]] Status CheckEnvironmentUnchanged() const;
    [[nodiscard]] Status CheckCatalogUnchanged() const;
    [[nodiscard]] Status CheckObjectSourcesUnchanged(std::string_view expectedSourceSha256 = {}) const;
    [[nodiscard]] Status CaptureObjectSources();
    [[nodiscard]] Status CaptureMapDataSources();
    [[nodiscard]] Status SeedMapDataOverlay(ActiveEdit& edit);
    [[nodiscard]] Status ValidateMapDataOverlay() const;
    [[nodiscard]] Status ValidateAuxiliaryOverlay() const;
    [[nodiscard]] Result<Services::MapEditSaveReceipt> ExportAuxiliaryArchiveCandidate(
        TransactionId transactionId, const ProjectRevision& expectedRevision,
        Services::MapEditSaveRequest request);
    [[nodiscard]] Status CaptureTerrainSources();
    [[nodiscard]] Status SeedTerrainOverlay(ActiveEdit& edit);
    [[nodiscard]] Status ValidateTerrainOverlay() const;
    [[nodiscard]] Status CheckTerrainMixingAllowed() const;
    [[nodiscard]] bool HasTerrainOverlay() const noexcept;
    void CommitTerrainOverlay();
    [[nodiscard]] Result<Services::MapEditSaveReceipt> ExportTerrainArchiveCandidate(
        TransactionId transactionId, const ProjectRevision& expectedRevision,
        Services::MapEditSaveRequest request);
    [[nodiscard]] Result<Services::MapEditSaveReceipt> ExportMapDataArchiveCandidate(
        TransactionId transactionId, const ProjectRevision& expectedRevision,
        Services::MapEditSaveRequest request);
    [[nodiscard]] Result<Services::MapEditSaveReceipt> ExportPlacementArchiveCandidate(
        TransactionId transactionId, const ProjectRevision& expectedRevision,
        Services::MapEditSaveRequest request);
    [[nodiscard]] Status ApplyObjectOverlay(ObjectData::ObjectType type,
        const ObjectData::LniObjectDocument& document, TransactionId transactionId);
    [[nodiscard]] Status CheckArchivePublishingIdentity(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        std::uint64_t generation,
        const Trigger::TriggerTransactionSnapshot& snapshot,
        const std::vector<std::shared_ptr<const Services::ObjectTransactionSnapshot>>& objects) const;
    [[nodiscard]] Result<std::vector<std::shared_ptr<const Services::ObjectTransactionSnapshot>>>
        FreezeObjectTransactions(const std::filesystem::path& sourceMap, const std::string& sourceSha256) const;
    [[nodiscard]] Status CheckObjectOverlay(const SemanticDiff& preview,
        const std::vector<std::shared_ptr<const Services::ObjectTransactionSnapshot>>& objects) const;
    [[nodiscard]] Result<std::string> ExpectedArgumentType(
        const Trigger::TriggerDocument& document,
        StableId argumentId) const;
    [[nodiscard]] Status ValidateScalarValue(
        const Trigger::TriggerDocument& document,
        std::string_view expectedType,
        std::int32_t valueKind,
        std::string_view value) const;
    [[nodiscard]] Status SealDocument(
        Trigger::TriggerDocument candidate,
        std::string summary);

    std::filesystem::path projectRoot_;
    std::filesystem::path uiDataRoot_;
    std::filesystem::path gameRoot_;
    mutable std::shared_ptr<const Trigger::MapObjectReferenceSnapshot> mapObjectReferences_;
    std::filesystem::path wtgRelativePath_;
    std::filesystem::path wctRelativePath_;
    Trigger::UiCatalog catalog_;
    std::unique_ptr<const Trigger::TriggerTypeSystem> projectSearchTypes_;
    const Trigger::UiCatalog* projectSearchTypesCatalogOwner_ = nullptr;
    std::string projectSearchTypesCatalogSha_;
    std::uint64_t projectSearchTypesGeneration_ = 0;
    Trigger::WtgWctCodec codec_;
    Trigger::TriggerDocument committedDocument_;
    EditorSession session_;
    std::optional<ActiveEdit> activeEdit_;
    std::unique_ptr<McpObjectWorkspaceState> objectState_;
    std::unique_ptr<DirectoryWorkspace> mapDataSource_;
    std::shared_ptr<McpTerrainWorkspaceState> terrainState_;
    std::shared_ptr<const MapDocument> initialMapData_, committedMapData_;
    std::shared_ptr<const Services::MapDataQuerySnapshot> projectSearchMapCache_;
    std::shared_ptr<const MapData::MapDataPlacementIntent> committedPlacementIntent_;
    std::shared_ptr<const Services::AuxiliaryTransactionSnapshot> committedAuxiliary_;
    std::uint64_t toolEpoch_ = 0;
    bool publishing_ = false;
};

} // namespace War3Map::Mcp
