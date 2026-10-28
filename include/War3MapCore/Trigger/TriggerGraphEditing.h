#pragma once

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <map>
#include <memory>
#include <stop_token>
#include <variant>

namespace War3Map::Trigger {

class MapObjectReferenceSnapshot;

enum class GraphRecordKind : std::uint8_t { Category, Variable, Trigger, Eca, Argument };
// id 与 clientKey 恰有一个非空；clientKey 只在本批次内解析。
struct GraphRecordRef {
    GraphRecordKind kind = GraphRecordKind::Trigger;
    StableId id = 0;
    std::string clientKey;
};
enum class GraphParentKind : std::uint8_t { Categories, Variables, CategoryTriggers, TriggerEcas, EcaChildren };
struct GraphParentExpectation {
    GraphParentKind kind = GraphParentKind::Categories;
    GraphRecordRef owner;
    // 完整当前顺序，不是局部邻居猜测；新增对象使用已成功创建的 clientKey。
    std::vector<GraphRecordRef> orderedChildren;
    std::optional<std::int32_t> childBranch;
};
struct GraphCategoryValue { std::string name; bool comment = false; };
struct GraphVariableValue {
    std::string name, typeName;
    bool isArray = false;
    std::int32_t arraySize = 1;
    bool initialized = false;
    std::string initialValue;
};
struct GraphTriggerValue {
    std::string name, description;
    bool enabled = true, initiallyOn = true, runOnInit = false, comment = false;
};
struct GraphScalarValue {
    std::int32_t kind = 3; // 0:真实预设，1:声明变量，3:字面值；函数使用递归 DTO。
    std::string value;
    bool operator==(const GraphScalarValue&) const = default;
};
struct GraphEcaValue;
struct GraphArgumentValue {
    std::string clientKey;
    GraphScalarValue scalar;
    std::shared_ptr<const GraphEcaValue> call;
    std::shared_ptr<const GraphArgumentValue> index;
};
struct GraphEcaValue {
    std::string clientKey, function;
    UiDefinitionKind kind = UiDefinitionKind::Action;
    bool enabled = true;
    std::optional<std::int32_t> childBranch;
    std::vector<GraphArgumentValue> arguments;
    std::vector<std::shared_ptr<const GraphEcaValue>> children;
    std::string origin; // 紧凑写法里的行列与函数名，只用于诊断定位
};
struct GraphCreate {
    std::string clientKey;
    std::variant<GraphCategoryValue, GraphVariableValue, GraphTriggerValue> value;
    GraphParentExpectation parent;
    std::optional<GraphRecordRef> before;
};
enum class GraphField : std::uint8_t {
    Comment, Description, Enabled, InitiallyOn, RunOnInit,
    VariableType, VariableArray, VariableArraySize, VariableInitialized, VariableInitialValue,
    ArgumentScalar,
};
using GraphFieldValue = std::variant<bool, std::int32_t, std::string, GraphScalarValue>;
struct GraphSet {
    GraphRecordRef target;
    GraphField field = GraphField::Enabled;
    GraphFieldValue expected, value;
};
struct GraphRename {
    GraphRecordRef target;
    RawText expectedName; // 编码、原字节与解码值均核对。
    std::string name;
    std::vector<AtomicRenameCodeAudit> codeAudits;
};
struct GraphMove {
    GraphRecordRef target;
    GraphParentExpectation from, to;
    std::optional<GraphRecordRef> before;
};
struct GraphRemove {
    GraphRecordRef target;
    std::string expectedName;
    GraphParentExpectation parent;
};
struct GraphInsertSubtree {
    GraphParentExpectation parent;
    std::optional<GraphRecordRef> before;
    std::shared_ptr<const GraphEcaValue> root;
};
// 由共用查询生成；绑定直接参数槽、实际类型与完整原字节子树，不能由客户端猜测。
struct GraphArgumentReplacementGuard {
    std::uint32_t schemaVersion = 1;
    StableId ownerEcaId = 0, sourceTriggerId = 0;
    std::uint32_t argumentOrdinal = 0;
    std::string uiSignature, expectedType, subtreeSha256;
    bool operator==(const GraphArgumentReplacementGuard&) const = default;
};
struct GraphReplaceArgument {
    GraphRecordRef target;
    GraphArgumentReplacementGuard expected;
    GraphArgumentValue value;
};
struct GraphArgumentReplacementReceipt {
    StableId argumentId = 0;
    std::string beforeSubtreeSha256, afterSubtreeSha256;
    std::size_t removedEcas = 0, removedArguments = 0;
};
// 紧凑 T 写法（TriggerTextNotation.h）：一段文本解析成若干完整子树，按顺序插到同一父列表（before 之前或末尾）。
struct GraphInsertText {
    GraphParentExpectation parent;
    std::optional<GraphRecordRef> before;
    std::string text;
};
// 用紧凑写法整体替换触发器的全部根 ECA；新鲜度由批次绑定保证。
struct GraphReplaceTriggerText {
    GraphRecordRef trigger;
    std::string text;
};
// 功能写法（TriggerFeatureText.h）：类别按名字找（没有就新建）；变量按名字新建，已有时只更新初值与数组长度
// （类型与是否数组须一致）；触发器按名字在该类别内整体替换内容或新建。未提到的记录不动，不删除任何记录。
struct GraphApplyFeatureText {
    std::string category;
    std::string text;
    std::string clientKey; // 新建记录的 clientKey 前缀；空时为 "feature"
    // 新建触发器依次放在该触发器之后（须在同一类别）；空时放到类别末尾。已有触发器不移动。
    std::optional<GraphRecordRef> after;
};
// 局部替换：按 trigger_text 导出的行号替换第 fromLine..toLine 行（toLine = fromLine - 1 表示插在 fromLine 之前），
// expected 须与当前导出中这几行逐字相同，防止按过期行号改错位置；替换后整段按紧凑 T 重新写入（同整体替换）。
struct GraphPatchTriggerText {
    GraphRecordRef trigger;
    std::size_t fromLine = 1, toLine = 0;
    std::string expected, text;
};
// 轻量移动：把触发器或类别放到 anchor 紧前/紧后，父列表取自 anchor 当前所在位置（触发器可借此换类别），
// 不需要完整兄弟顺序；以两个记录的身份为前提，不校验其他兄弟。ECA 用 patch_trigger_text 改写。
struct GraphPlace {
    GraphRecordRef target, anchor;
    bool after = true;
};
class PreparedTriggerCodeIdentityChange;
struct GraphCodeIdentityChange { std::shared_ptr<const PreparedTriggerCodeIdentityChange> prepared; };
using GraphEditOperation = std::variant<GraphCreate, GraphSet, GraphRename, GraphMove, GraphRemove, GraphInsertSubtree,
    GraphReplaceArgument, GraphInsertText, GraphReplaceTriggerText, GraphApplyFeatureText, GraphPatchTriggerText, GraphPlace,
    GraphCodeIdentityChange>;
struct TriggerGraphBinding {
    std::string sourceWtgSha256, sourceWctSha256, uiCatalogSha256;
    std::string currentWtgSha256, currentWctSha256;
    std::string mapObjectReferencesSha256;
};
struct TriggerGraphEditBatch {
    TriggerGraphBinding expected;
    std::vector<GraphEditOperation> operations;
};
enum class TriggerScalarWorkPolicy : std::uint8_t { BoundedSourcesV2, LegacyKnownWork };
struct TriggerGraphEditLimits {
    std::size_t maxOperations = 256;
    std::size_t maxAddedRecords = 16384;
    std::size_t maxDepth = 128;
    std::size_t maxRequestTextBytes = 4 * 1024 * 1024;
    // 仅紧凑标量准备使用；旧一般图 Apply 的预算及语义不改变。
    std::size_t maxScalarOwnedBytes = 128 * 1024 * 1024;
    std::size_t maxScalarWorkUnits = 64 * 1024 * 1024; // 仅 LegacyKnownWork 比较估计账；不是操作硬上界。
    std::size_t scalarAdditionalWorkUnits = 0; // 调用者已计费的批量 guard/类型路径。
    std::size_t scalarAdditionalOwnedBytes = 0; // Service 已新拥有的预览/请求/guard 临时量。
    TriggerScalarWorkPolicy scalarWorkPolicy = TriggerScalarWorkPolicy::BoundedSourcesV2;
    std::size_t maxScalarRecords = 524288, maxScalarEdges = 1048576;
};
struct TriggerGraphEditReceipt {
    TriggerGraphBinding before, after;
    std::size_t appliedOperations = 0;
    std::map<std::string, GraphRecordRef> created;
    std::vector<AtomicRenameReceipt> renames;
    std::vector<GraphArgumentReplacementReceipt> argumentReplacements;
    std::size_t codeIdentityChanges = 0;
    // 编辑收据不是撤销栈、外部引用证明、保存或运行证明。
    bool externalDependenciesVerified = false;
};

// 新拥有数据的逻辑账；只有显式 encodedCapacity 字段读取实际 vector capacity，其他不是 RSS。
// 已知工作估计 coverageIncomplete；结构/字节准入与实测耗时分别验收。
struct TriggerGraphScalarEditCost {
    std::size_t sourceDocumentBytes = 0, candidateGrowthBytes = 0, candidateOwnedBytes = 0;
    std::size_t compactOwnedBytes = 0, publicationScratchBytes = 0;
    std::size_t guardScratchBytes = 0, additionalOwnedBytes = 0;
    std::size_t lookupScratchBytes = 0, operationScratchBytes = 0, validationScratchBytes = 0;
    std::size_t typePreparationScratchBytes = 0, mountIndexScratchBytes = 0, catalogBytesCharged = 0;
    std::size_t codeProtectionScratchBytes = 0;
    std::size_t encodedPairBytes = 0, encodedPairCapacityBytes = 0, identityScratchBytes = 0, peakOwnedBytes = 0;
    std::size_t copiedTextBytes = 0, codecEncodedBytesCharged = 0, identityBytesCharged = 0;
    std::size_t recordVisitsCharged = 0, edgeVisitsCharged = 0, additionalWorkUnits = 0, workUnitsCharged = 0;
    std::size_t identityCaptures = 0, candidateCopies = 0, bulkGuardCaptures = 0, candidateValidations = 0;
    TriggerScalarWorkPolicy workPolicy = TriggerScalarWorkPolicy::BoundedSourcesV2;
    bool workCoverageComplete = false; // 已知费用估计遗漏底层重复类型/locals工作，不是操作硬上界。
    bool ownedInputConsumed = false; // owned入口内没有再次复制整份文档；调用者的一次快照仍计入逻辑持有。
};
namespace detail { struct GraphScalarEditState; }
// 仅共用 Core 准备能创建；客户端不能提供可改写的节点、图身份或授权标记。
class TriggerGraphScalarEditReceipt final {
public:
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] const TriggerGraphEditReceipt& Receipt() const noexcept;
    [[nodiscard]] const TriggerGraphScalarEditCost& Cost() const noexcept;
    [[nodiscard]] std::string_view CurrentWtgSha256(bool afterExpected) const noexcept;
    [[nodiscard]] std::string_view CurrentWctSha256(bool afterExpected) const noexcept;
private:
    std::shared_ptr<const detail::GraphScalarEditState> state_;
    friend Result<TriggerGraphScalarEditReceipt> PrepareTriggerGraphScalarEditBatch(
        const TriggerDocument&, const UiCatalog&, const TriggerGraphEditBatch&,
        const TriggerGraphEditLimits&, const MapObjectReferenceSnapshot*);
    friend Result<TriggerGraphScalarEditReceipt> PrepareOwnedTriggerGraphScalarEditBatch(
        TriggerDocument, const UiCatalog&, const TriggerGraphEditBatch&,
        const TriggerGraphEditLimits&, const MapObjectReferenceSnapshot*, std::stop_token);
    friend Status VerifyTriggerGraphScalarReplay(const TriggerDocument&, const UiCatalog&,
        const TriggerGraphScalarEditReceipt&, bool);
    friend Status ReplayTriggerGraphScalarEdit(TriggerDocument&, const UiCatalog&,
        const TriggerGraphScalarEditReceipt&, bool);
};
// 同一个 Editor/ValidateTouched/codec/WCT 门，仅 Core 内一次 private candidate。
// 成功后仅拥有 <=256 个完整 ArgumentNode 前后值，保留来源 seal 和所有稳定身份。
[[nodiscard]] Result<TriggerGraphScalarEditReceipt> PrepareTriggerGraphScalarEditBatch(
    const TriggerDocument& document, const UiCatalog& catalog, const TriggerGraphEditBatch& batch,
    const TriggerGraphEditLimits& limits = {}, const MapObjectReferenceSnapshot* mapObjects = nullptr);
// 只消耗调用者已独立拥有的一次快照。失败丢弃该私有值，不发布、不写作者源。
// 和 const 入口共用 Editor/ValidateTouched/完整前后身份；stop 只在阶段边界协作检查。
[[nodiscard]] Result<TriggerGraphScalarEditReceipt> PrepareOwnedTriggerGraphScalarEditBatch(
    TriggerDocument ownedDocument, const UiCatalog& catalog, const TriggerGraphEditBatch& batch,
    const TriggerGraphEditLimits& limits = {}, const MapObjectReferenceSnapshot* mapObjects = nullptr,
    std::stop_token stop = {});
// 仅已有 IR 长度/数量元数据；不序列化、hash、建类型/引用索引或复制整图。
[[nodiscard]] Result<TriggerGraphScalarEditCost> InspectTriggerGraphScalarSourceCost(
    const TriggerDocument& document, const TriggerGraphEditLimits& limits = {});
[[nodiscard]] Status VerifyTriggerGraphScalarReplay(const TriggerDocument& document, const UiCatalog& catalog,
    const TriggerGraphScalarEditReceipt& receipt, bool afterExpected);
// 所有分配/原值门在前；整批 noexcept 交换，终态失败 RAII 逆交换；不执行事件回调。
[[nodiscard]] Status ReplayTriggerGraphScalarEdit(TriggerDocument& document, const UiCatalog& catalog,
    const TriggerGraphScalarEditReceipt& receipt, bool forward);

[[nodiscard]] Result<TriggerGraphBinding> CaptureTriggerGraphBinding(
    const TriggerDocument& document, const UiCatalog& catalog,
    const MapObjectReferenceSnapshot* mapObjects = nullptr);
// 最小合同只接受已有 ECA 的直接参数槽；旧根参数有数组下标或不透明代码时拒绝。
[[nodiscard]] Result<GraphArgumentReplacementGuard> CaptureArgumentReplacementGuard(
    const TriggerDocument& document, const UiCatalog& catalog, StableId argumentId,
    const TriggerGraphEditLimits& limits = {}, const MapObjectReferenceSnapshot* mapObjects = nullptr);
struct ScalarArgumentReplacementGuard {
    StableId argumentId = 0;
    std::optional<GraphArgumentReplacementGuard> guard;
    std::vector<Diagnostic> diagnostics; // 局部拒绝仍保留该请求，不能静默丢选中项。
};
// 一次 codec.Validate、挂载索引、类型目录，每触发器一次 locals；只准入旧标量槽。
// 逐项复用上面单项检查/相同指纹，复杂 call/index 不取得该批量标量资格。
// 调用者仍须先做完整 IR/目录字节与工作预算准入，再用同一 GraphSet 批次发布。
[[nodiscard]] Result<std::vector<ScalarArgumentReplacementGuard>> CaptureScalarArgumentReplacementGuards(
    const TriggerDocument& document, const UiCatalog& catalog, std::span<const StableId> argumentIds,
    const TriggerGraphEditLimits& limits = {}, const MapObjectReferenceSnapshot* mapObjects = nullptr);
// 整批在副本内执行；所有前值、类型、配对格式验证通过后才移动发布文档。
// 不接收原始 IR/WTG/WCT，不创建自定义代码，不转换 GUI，旧代码逐字节保持。
[[nodiscard]] Result<TriggerGraphEditReceipt> ApplyTriggerGraphEditBatch(
    TriggerDocument& document, const UiCatalog& catalog,
    const TriggerGraphEditBatch& batch, const TriggerGraphEditLimits& limits = {},
    const MapObjectReferenceSnapshot* mapObjects = nullptr);

} // namespace War3Map::Trigger
