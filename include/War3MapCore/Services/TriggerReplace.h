#pragma once

#include "War3MapCore/Services/ProjectSearch.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>

namespace War3Map::Services {

enum class TriggerReplaceMode : std::uint8_t { ParameterValue, Function, Variable, ObjectReference };
enum class TriggerReplaceMatch : std::uint8_t { Exact, Substring };
enum class TriggerReplaceState : std::uint8_t { Ready, Partial, Unavailable };
enum class TriggerReplaceEligibility : std::uint8_t { PreviewRequired, ReadOnly };

struct TriggerReplaceLimits {
    std::size_t pageLimit = 50; // 1..128；不是全图命中上限。
    std::size_t maxGraphRecords = 524288; // 1..1048576，包括未挂载记录的准入。
    std::size_t maxGraphEdges = 1048576; // 1..2097152，重复边也计费。
    std::size_t maxScanTextBytes = 64 * 1024 * 1024; // 1..128 MiB，全局查询账。
    std::size_t maxResultBytes = 128 * 1024; // 1..256 KiB，单页拥有文本。
    std::size_t maxSelection = 256; // 1..256，跨页合成一个现有图批次。
    std::size_t maxTargetRecords = 32768; // 1..65536，选择列表也先检查完整域。
    std::size_t maxTargetTextBytes = 1024 * 1024; // 1..8 MiB，目录标识/展示账。
    std::size_t maxNameRecords = 65536; // 1..131072，共用 N1 前的完整 WTS 域准入。
    std::size_t maxNameTextBytes = 16 * 1024 * 1024; // 1..64 MiB，非命中 WTS 也计费。
    std::size_t maxPreviewDocumentBytes = 128 * 1024 * 1024; // 1..512 MiB；IR/原文本字节准入。
    std::size_t maxPreviewWorkUnits = 64 * 1024 * 1024; // 1..256 Mi；LegacyKnownWork 估计门，非操作硬上界。
    Trigger::TriggerScalarWorkPolicy scalarWorkPolicy = Trigger::TriggerScalarWorkPolicy::BoundedSourcesV2;
};

// 同步借用，调用时宿主不得修改来源；返回页/预览不借用 span 或控件。
// revision 来自真实会话 generation/revision，并覆盖目录、物编和预放对象来源。
// 不为查询建立引用索引、读文件、序列化 WTG/WCT 或重新加载类型目录。
struct TriggerReplaceContext {
    std::string revision;
    const Trigger::TriggerDocument* document = nullptr;
    const Trigger::UiCatalog* catalog = nullptr;
    const Trigger::TriggerTypeSystem* types = nullptr; // 宿主持有的当前类型目录。
    const Trigger::MapObjectReferenceSnapshot* mapObjects = nullptr;
    const MapData::StringTableDocument* strings = nullptr; // 当前已持有的 WTS overlay，不补读。
    std::span<const ProjectSearchObjectSource> objects;
    const ObjectData::StandardObjectCatalog* standardObjects = nullptr;
    bool standardObjectsReady = false;
};

struct TriggerReplaceQuery {
    TriggerReplaceMode mode = TriggerReplaceMode::ParameterValue;
    TriggerReplaceMatch match = TriggerReplaceMatch::Substring;
    std::string needle; // 严格 UTF8、无 NUL，最多2048字节；可由下方精确身份替代。
    bool caseSensitive = true;
    std::optional<StableId> scopeTriggerId;
    std::optional<StableId> sourceVariableId; // 只在 Variable 模式；真实声明身份。
    std::optional<ObjectData::ObjectKey> sourceObject; // 只在 ObjectReference 模式。
};

// 同一请求所有页使用同一 proof。绑定全部查询/预算/来源、已扫描结构与原值摘要。
// snapshotSha256 是有界查询元数据摘要，不冒已序列化当前 WTG/WCT 的哈希。
struct TriggerReplaceQueryProof {
    std::string contextRevision, requestSha256, snapshotSha256;
    std::string sourceWtgSha256, sourceWctSha256, catalogSha256;
    bool operator==(const TriggerReplaceQueryProof&) const = default;
};
struct TriggerReplaceCursor {
    TriggerReplaceQueryProof proof;
    std::size_t nextMatchOffset = 0;
};
struct TriggerReplaceHit {
    std::string rowKey;
    StableId triggerId = 0, ecaId = 0;
    std::optional<StableId> argumentId;
    std::string triggerName, functionName, oldValue, displayValue, declaredType;
    std::int32_t oldKind = 3;
    TriggerReplaceEligibility eligibility = TriggerReplaceEligibility::ReadOnly;
    std::string reasonCode, reason;
};
struct TriggerReplaceCoverage {
    TriggerReplaceState state = TriggerReplaceState::Unavailable;
    std::size_t triggersExamined = 0, graphRecordsCharged = 0, graphEdgesCharged = 0, textBytesCharged = 0;
    std::size_t matchedInScannedScope = 0, customCodeSlotsExcluded = 0, unresolvedReferenceRows = 0;
    std::size_t nameRecordsCharged = 0, nameTextBytesCharged = 0;
    std::optional<std::size_t> totalMatches; // 仅完整覆盖请求的 GUI 范围时有值。
    bool scanTruncated = false;
    bool displayResolutionPartial = false; // raw-only/缺 WTS/解析预算不足必须明确。
};
struct TriggerReplacePage {
    TriggerReplaceQueryProof proof;
    std::vector<TriggerReplaceHit> rows;
    TriggerReplaceCoverage coverage;
    std::optional<TriggerReplaceCursor> next;
    std::size_t matchOffset = 0;
};

enum class TriggerReplacementKind : std::uint8_t { Variable, ObjectLiteral, PreplacedObject };
struct TriggerReplacement {
    TriggerReplacementKind kind = TriggerReplacementKind::Variable;
    StableId variableId = 0; // 变量名字由当前唯一声明获得，不接收客户端猜测的名字。
    std::optional<ObjectData::ObjectKey> object;
    std::string preplacedSymbol, mapObjectSnapshotIdentity;
    bool operator==(const TriggerReplacement&) const = default;
};
struct TriggerReplacementOption {
    std::string rowKey, display, source, typeName;
    TriggerReplacement replacement;
};
struct TriggerReplacementOptionsPage {
    TriggerReplaceQueryProof proof;
    std::vector<TriggerReplacementOption> rows;
    TriggerReplaceCoverage coverage;
    std::optional<TriggerReplaceCursor> next;
    std::size_t matchOffset = 0;
};

struct TriggerReplacementPreviewRow {
    TriggerReplaceHit hit;
    std::string expectedType, replacementValue;
    std::int32_t replacementKind = 3;
    bool eligible = false;
    std::string reasonCode, reason;
};

struct TriggerReplacePreviewData;
struct TriggerReplacePlanData;
class TriggerReplacePlanner;
// 拥有同步资格准备的封印；不借用 document/catalog/types/WTS/物编 span 或 QObject。
class TriggerReplacePlan final {
public:
    TriggerReplacePlan() = default; // 空值没有完成或发布资格。
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] const TriggerReplaceQueryProof& Proof() const noexcept;
    [[nodiscard]] std::span<const TriggerReplacementPreviewRow> Rows() const noexcept;
private:
    friend class TriggerReplacePlanner;
    explicit TriggerReplacePlan(std::shared_ptr<const TriggerReplacePlanData> data) : data_(std::move(data)) {}
    std::shared_ptr<const TriggerReplacePlanData> data_;
};
struct TriggerReplacePlanResult {
    TriggerReplaceQueryProof proof;
    std::vector<TriggerReplacementPreviewRow> rows;
    std::optional<TriggerReplacePlan> plan;
    std::size_t documentBytesCharged = 0, workUnitsCharged = 0;
};
// 只有共享服务能构造的拥有候选；不允许客户端 DTO 直接取得发布资格。
class TriggerReplacePreview final {
public:
    [[nodiscard]] const TriggerReplaceQueryProof& Proof() const noexcept;
    [[nodiscard]] std::span<const TriggerReplacementPreviewRow> Rows() const noexcept;
    [[nodiscard]] const Trigger::TriggerGraphScalarEditReceipt& ScalarReceipt() const noexcept;
    [[nodiscard]] const Trigger::TriggerGraphEditBatch& Batch() const noexcept;
    [[nodiscard]] const Trigger::TriggerGraphEditReceipt& Receipt() const noexcept;
private:
    friend class TriggerReplacePlanner;
    explicit TriggerReplacePreview(std::shared_ptr<const TriggerReplacePreviewData> data) : data_(std::move(data)) {}
    std::shared_ptr<const TriggerReplacePreviewData> data_;
};
struct TriggerReplacePreviewResult {
    TriggerReplaceQueryProof proof;
    std::vector<TriggerReplacementPreviewRow> rows;
    std::optional<TriggerReplacePreview> preview; // 任一勾选项拒绝时为空；不默默丢项。
    std::size_t documentBytesCharged = 0, workUnitsCharged = 0;
    Trigger::TriggerGraphScalarEditCost cost; // 实际 Core 逻辑细账；不是 RSS。
};

// 有界完整扫描请求范围，每页重跑并核同一 proof；按稳定来源遍历顺序分页。
// Partial 的 totalMatches 保持未知，matchedInScannedScope 不是全图总命中。
[[nodiscard]] Result<TriggerReplacePage> QueryTriggerReplace(const TriggerReplaceContext& context,
    std::string_view expectedRevision, const TriggerReplaceQuery& query,
    const TriggerReplaceLimits& limits = {}, const TriggerReplaceCursor* cursor = nullptr);
// 选择列表只读当前已持有的声明/物编/标准目录；不复制全部字段，不补读文件。
[[nodiscard]] Result<TriggerReplacementOptionsPage> QueryTriggerReplacementOptions(const TriggerReplaceContext& context,
    std::string_view expectedRevision, TriggerReplacementKind kind, std::string_view filter,
    const TriggerReplaceLimits& limits = {}, const TriggerReplaceCursor* cursor = nullptr);
// selectedRowKeys 可以来自多页，但必须同一 proof；空、重复、未知、超预算均拒绝。
// 全部类型与来源校验复用现有图编辑服务，在一份候选执行一个原子批次。
[[nodiscard]] Result<TriggerReplacePreviewResult> PrepareTriggerReplacement(const TriggerReplaceContext& context,
    std::string_view expectedRevision, const TriggerReplaceQuery& query, const TriggerReplaceQueryProof& proof,
    std::span<const std::string> selectedRowKeys, const TriggerReplacement& replacement,
    const TriggerReplaceLimits& limits = {});
// 同步小阶段：仅当前有界 Scanner/target/guards/types/batch 资格，不编码或复制整份 IR。
[[nodiscard]] Result<TriggerReplacePlanResult> PrepareTriggerReplacementPlan(const TriggerReplaceContext& context,
    std::string_view expectedRevision, const TriggerReplaceQuery& query, const TriggerReplaceQueryProof& proof,
    std::span<const std::string> selectedRowKeys, const TriggerReplacement& replacement,
    const TriggerReplaceLimits& limits = {});
// worker 纯值阶段：GUI 一次 clone 后 move 两个独立拥有值；Plan 不保存借用来源。
// wholeGraph 前身份在本阶段建立，发布仍须当前 Core Replay 前/后完整门。
[[nodiscard]] Result<TriggerReplacePreviewResult> CompleteTriggerReplacementPlan(const TriggerReplacePlan& plan,
    Trigger::TriggerDocument ownedDocument, Trigger::UiCatalog ownedCatalog,
    const TriggerReplaceLimits& limits = {}, std::stop_token stop = {});
// 仅当前 query/target/WTS/objects/revision/policy/limits 门；wholeGraph deferredToCore。
// 成功没有授予写入资格，随后必须唯一 Core Replay；强 Verify 公共入口保持全图身份检查。
[[nodiscard]] Status CheckTriggerReplacementPublicationContext(const TriggerReplaceContext& context,
    std::string_view expectedRevision, const TriggerReplacePreview& preview,
    const TriggerReplaceLimits& limits = {});
// 宿主发布前再次调用；随后仍由现有 SnapshotUndoCommand/事务核当前会话再发布。
// 预览成功不是已编辑、已撤销、已保存或原版/游戏验收。
[[nodiscard]] Status VerifyTriggerReplacementPreview(const TriggerReplaceContext& context,
    std::string_view expectedRevision, const TriggerReplacePreview& preview,
    const TriggerReplaceLimits& limits = {});

} // namespace War3Map::Services
