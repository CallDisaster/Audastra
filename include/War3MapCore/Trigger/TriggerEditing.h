#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Trigger {

enum class EcaAppendPlacement : std::uint8_t {
    TriggerRoot,
    ArgumentCall,
};

// 收据包含撤销/重做所需的最小尾部图，不复制整份大型 WTG 文档。
struct EcaAppendReceipt {
    EcaAppendPlacement placement = EcaAppendPlacement::TriggerRoot;
    StableId rootEcaId = 0;
    StableId triggerId = 0;
    StableId argumentId = 0;
    std::size_t row = 0;
    std::size_t previousEcaCount = 0;
    std::size_t previousArgumentCount = 0;
    std::vector<EcaNode> appendedEcas;
    std::vector<ArgumentNode> appendedArguments;
    std::optional<ArgumentNode> argumentBefore;
    std::optional<ArgumentNode> argumentAfter;
};

enum class AtomicRenameCodeKind : std::uint8_t {
    GlobalCustomCode,
    TriggerCustomCode,
    ArgumentCode,
    VariableInitialValue,
};

// 这是调用方对不改动的代码块作出的显式“本次改名不影响它”审计，不是词法分析结论。
// 身份绑定当前槽位原字节、编码、目录和本次两个生成符号；不能复用为通用放行开关。
struct AtomicRenameCodeAudit {
    AtomicRenameCodeKind kind = AtomicRenameCodeKind::GlobalCustomCode;
    StableId id = 0; // 全局自定义代码为 0；其余为所属记录/参数 ID。
    TextEncoding encoding = TextEncoding::Utf8;
    std::string sourceSha256;
    std::string uiCatalogSha256;
    std::string oldName;
    std::string newName;
    std::string oldSymbol;
    std::string newSymbol;
    std::string reason;
};

enum class AtomicRenameCodeCoverage : std::uint8_t {
    NoOpaqueCode,
    CallerAssertedNotRuntimeProven,
};

struct AtomicRenameReceipt {
    StableId declarationId = 0;
    std::string oldName, newName;
    std::string oldSymbol, newSymbol;
    std::vector<StableId> changedArgumentIds;
    std::vector<AtomicRenameCodeAudit> externallyAuditedCode;
    AtomicRenameCodeCoverage codeCoverage = AtomicRenameCodeCoverage::NoOpaqueCode;
    // 只覆盖该 WTG/WCT 文档；外部库、include 和动态字符串分发未经本接口证明。
    bool externalDependenciesVerified = false;
};

class TriggerDocumentEditor {
public:
    // insertRow 为空时追加到触发器根末尾；否则插到指定行（越界自动夹紧）。
    [[nodiscard]] static Result<EcaAppendReceipt> AppendRootEca(
        TriggerDocument& document,
        const UiCatalog& catalog,
        StableId triggerId,
        UiDefinitionKind kind,
        std::string_view functionName,
        std::optional<std::size_t> insertRow = std::nullopt);

    [[nodiscard]] static Result<EcaAppendReceipt> AppendArgumentCall(
        TriggerDocument& document,
        const UiCatalog& catalog,
        StableId argumentId,
        std::string_view expectedReturnType,
        std::string_view functionName);

    [[nodiscard]] static Status UndoAppend(
        TriggerDocument& document,
        const EcaAppendReceipt& receipt);
    [[nodiscard]] static Status RedoAppend(
        TriggerDocument& document,
        const EcaAppendReceipt& receipt);

    [[nodiscard]] static Status SetScalarArgument(
        TriggerDocument& document,
        StableId argumentId,
        std::int32_t valueKind,
        std::string value);

    [[nodiscard]] static EcaNode* FindEca(
        TriggerDocument& document,
        StableId ecaId) noexcept;
    [[nodiscard]] static ArgumentNode* FindArgument(
        TriggerDocument& document,
        StableId argumentId) noexcept;
    [[nodiscard]] static TriggerRecord* FindTrigger(
        TriggerDocument& document,
        StableId triggerId) noexcept;

    // 触发器元数据（启用/初始关闭/初始化运行/注释）。用于 WE 式属性区编辑。
    [[nodiscard]] static Status SetTriggerEnabled(
        TriggerDocument& document,
        StableId triggerId,
        bool enabled);
    [[nodiscard]] static Status SetTriggerInitiallyOn(
        TriggerDocument& document,
        StableId triggerId,
        bool initiallyOn);
    [[nodiscard]] static Status SetTriggerRunOnInit(
        TriggerDocument& document,
        StableId triggerId,
        bool runOnInit);
    [[nodiscard]] static Status SetTriggerComment(
        TriggerDocument& document,
        StableId triggerId,
        std::string comment);

    // 启用/禁用单个 ECA 节点（禁用后在 WE 中显示为灰字 + 红齿轮，不编译进脚本）。
    [[nodiscard]] static Status SetEcaEnabled(
        TriggerDocument& document,
        StableId ecaId,
        bool enabled);

    // 自定义文本使用原槽编码，不能表示/含 NUL 时拒绝。普通修改不隐式删除 GUI 树。
    [[nodiscard]] static Status SetCustomText(TriggerDocument& document, StableId triggerId, std::string text);
    [[nodiscard]] static Status SetGlobalCustomCode(TriggerDocument& document, std::string text);
    // 显式转换才移除 GUI 闭包；调用方必须先核对完整原树，整个操作进入同一编辑事务。
    [[nodiscard]] static Status ConvertToCustomText(TriggerDocument& document, StableId triggerId, std::string text);

    // 删除 ECA 子树的完整回执：压实重排所需的旧索引与撤销重挂所需的全部数据。
    struct EcaRemoveReceipt {
        StableId ecaId = 0;
        StableId parentTriggerId = 0; // 非 0：顶层节点，挂在触发器 ecas
        StableId parentEcaId = 0;     // 非 0：嵌套子节点，挂在父节点 children
        std::size_t row = 0;
        std::uint32_t rootOldIndex = 0; // 根节点删除前的旧索引
        // 子树全部节点/参数（按删除前旧索引升序，旧索引向量与其一一对齐）。
        std::vector<EcaNode> removedEcas;
        std::vector<std::uint32_t> removedEcaOldIndices;
        std::vector<ArgumentNode> removedArguments;
        std::vector<std::uint32_t> removedArgumentOldIndices;
    };

    // 删除 ECA 子树并压实节点表：闭包遍历 children/arguments/callEca/indexArgument，
    // 重建索引引用，文档保持可序列化（无孤儿）。
    [[nodiscard]] static Result<EcaRemoveReceipt> RemoveEcaSubtree(
        TriggerDocument& document,
        StableId ecaId);

    // 参数当前由函数提供：摘除该函数子树（压实节点表），参数成为空的直接值，
    // 调用方随后写入新的来源（预设 / 变量 / 值 / 另一个函数）。撤销按原索引精确补回。
    struct ArgumentCallDetachReceipt {
        StableId argumentId = 0;
        StableId rootEcaId = 0;
        ArgumentNode argumentBefore;
        std::uint32_t rootOldIndex = 0;
        std::vector<EcaNode> removedEcas;
        std::vector<std::uint32_t> removedEcaOldIndices;
        std::vector<ArgumentNode> removedArguments;
        std::vector<std::uint32_t> removedArgumentOldIndices;
    };
    [[nodiscard]] static Result<ArgumentCallDetachReceipt> DetachArgumentCall(
        TriggerDocument& document,
        StableId argumentId);
    [[nodiscard]] static Status UndoDetachArgumentCall(
        TriggerDocument& document,
        const ArgumentCallDetachReceipt& receipt);
    [[nodiscard]] static Status RedoDetachArgumentCall(
        TriggerDocument& document,
        const ArgumentCallDetachReceipt& receipt);

    // 撤销 RemoveEcaSubtree：按回执把子树补回原索引并重挂到原挂载点。
    [[nodiscard]] static Status UndoRemoveEcaSubtree(
        TriggerDocument& document,
        const EcaRemoveReceipt& receipt);

    // 新建空触发器（无 ECA；WCT 自定义文本为空槽）。返回新触发器稳定 ID。
    [[nodiscard]] static Result<StableId> AddTrigger(
        TriggerDocument& document,
        std::int32_t categoryId,
        std::string name);

    // 重命名触发器（空名/NUL 拒绝）。
    [[nodiscard]] static Status RenameTrigger(
        TriggerDocument& document,
        StableId triggerId,
        std::string name);

    // 声明和全部明确 type=1 引用原子改名。数组索引和嵌套调用复用同一参数池。
    // 非空自定义/未解析代码默认拒绝；显式逐块 audit 只保留代码，不改字符串或 WCT。
    // 旧 RenameTrigger 的 UI 行为保持独立，调用方须显式选择此能力。
    [[nodiscard]] static Result<AtomicRenameReceipt> RenameTriggerAtomic(
        TriggerDocument& document, const UiCatalog& catalog,
        StableId triggerId, std::string name,
        std::span<const AtomicRenameCodeAudit> codeAudits = {});

    // 删除整个触发器的回执（含其全部 ECA 子树快照）。
    struct TriggerRemoveReceipt {
        TriggerRecord trigger;
        std::size_t row = 0;
        std::vector<EcaNode> removedEcas;
        std::vector<std::uint32_t> removedEcaOldIndices;
        std::vector<ArgumentNode> removedArguments;
        std::vector<std::uint32_t> removedArgumentOldIndices;
    };

    // 删除整个触发器并压实节点表（无孤儿）。
    [[nodiscard]] static Result<TriggerRemoveReceipt> RemoveTrigger(
        TriggerDocument& document,
        StableId triggerId);

    // 撤销 RemoveTrigger：触发器与其全部子树按回执恢复。
    [[nodiscard]] static Status UndoRemoveTrigger(
        TriggerDocument& document,
        const TriggerRemoveReceipt& receipt);

    // ECA 节点在图中的挂载点。
    struct EcaPlacement {
        StableId parentTriggerId = 0; // 非 0：顶层节点，挂在触发器 ecas
        StableId parentEcaId = 0;     // 非 0：嵌套子节点，挂在父节点 children
        std::size_t row = 0;
        bool found = false;
    };
    [[nodiscard]] static EcaPlacement LocateEcaPlacement(
        const TriggerDocument& document,
        StableId ecaId) noexcept;

    // 剪贴板快照：子树全部节点与参数（含内部引用的旧索引）。
    struct EcaSubtreeSnapshot {
        std::uint32_t rootOldIndex = 0; // 根节点在快照生成前的索引
        std::vector<EcaNode> ecas;
        std::vector<std::uint32_t> ecaOldIndices;
        std::vector<ArgumentNode> arguments;
        std::vector<std::uint32_t> argumentOldIndices;
    };

    // 收集子树为可粘贴快照（只读）。
    [[nodiscard]] static Result<EcaSubtreeSnapshot> CopyEcaSubtree(
        const TriggerDocument& document,
        StableId ecaId);

    struct EcaPasteReceipt {
        StableId rootEcaId = 0;
        StableId parentTriggerId = 0;
        StableId parentEcaId = 0;
        std::size_t row = 0;
    };

    // 粘贴：克隆子树（分配新稳定 ID，重映射内部引用），挂到指定挂载点。
    // parentTriggerId 非 0 挂触发器根，否则挂 parentEcaId 的 children 的 row 处；
    // rootChildId 为粘贴根节点的 childId（兄弟语义，通常取参照节点的分支号）。
    [[nodiscard]] static Result<EcaPasteReceipt> PasteEcaSubtree(
        TriggerDocument& document,
        const UiCatalog& catalog,
        const EcaSubtreeSnapshot& snapshot,
        StableId parentTriggerId,
        StableId parentEcaId,
        std::size_t row,
        std::optional<std::int32_t> rootChildId = std::nullopt);

    // 移动 ECA 子树（只改挂载关系，不复制、不压实节点表）：同一触发器内重排或跨分支移动。
    // 目标为触发器根（targetTriggerId 非 0，childId 必须为空）或某个 ECA 的子块（targetEcaId 非 0，
    // childId 为分支号）。beforeEcaId 非 0 时插在该兄弟之前，否则放在目标分支末尾。
    // 不能移入自身子树，不能跨触发器。
    struct EcaMoveReceipt {
        StableId ecaId = 0;
        StableId fromTriggerId = 0;
        StableId fromEcaId = 0;
        std::size_t fromRow = 0;
        std::optional<std::int32_t> fromChildId;
        StableId toTriggerId = 0;
        StableId toEcaId = 0;
        std::size_t toRow = 0;
        std::optional<std::int32_t> toChildId;
    };
    [[nodiscard]] static Result<EcaMoveReceipt> MoveEca(
        TriggerDocument& document,
        StableId ecaId,
        StableId targetTriggerId,
        StableId targetEcaId,
        std::optional<std::int32_t> childId,
        StableId beforeEcaId = 0);
    [[nodiscard]] static Status UndoMoveEca(TriggerDocument& document, const EcaMoveReceipt& receipt);
    [[nodiscard]] static Status RedoMoveEca(TriggerDocument& document, const EcaMoveReceipt& receipt);

    // 全局变量管理。
    [[nodiscard]] static Status AddVariable(
        TriggerDocument& document,
        std::string_view name,
        std::string_view typeName,
        bool isArray = false);
    [[nodiscard]] static Status RenameVariable(
        TriggerDocument& document,
        std::string_view oldName,
        std::string_view newName);
    [[nodiscard]] static Result<AtomicRenameReceipt> RenameVariableAtomic(
        TriggerDocument& document, const UiCatalog& catalog,
        StableId variableId, std::string name,
        std::span<const AtomicRenameCodeAudit> codeAudits = {});
    [[nodiscard]] static Status RemoveVariable(
        TriggerDocument& document,
        std::string_view name);
};

} // namespace War3Map::Trigger
