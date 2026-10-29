#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerRemovalAudit.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerCodeIdentityChange.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerFeatureText.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerReal.h"
#include "War3MapCore/Trigger/TriggerValueSafety.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"
#include "TriggerArgumentReplacement.h"
#include "TriggerIdAllocation.h"
#include "TriggerCodecDefinitionLookupMemo.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <new>
#include <set>
#include <tuple>
#include <type_traits>

namespace War3Map::Trigger {
namespace {
constexpr const char* kDomain = "Trigger.GraphEdit";
Status Fail(std::string code, std::string message) { return Status::Failure(kDomain, std::move(code), std::move(message)); }
template<class T> Result<T> Failed(const Status& status) { Result<T> out; out.diagnostics = status.diagnostics; return out; }
template<class T, class U> Result<T> Failed(const Result<U>& result) { Result<T> out; out.diagnostics = result.diagnostics; return out; }
bool SameBinding(const TriggerGraphBinding& a, const TriggerGraphBinding& b) {
    return a.sourceWtgSha256 == b.sourceWtgSha256 && a.sourceWctSha256 == b.sourceWctSha256
        && a.uiCatalogSha256 == b.uiCatalogSha256 && a.currentWtgSha256 == b.currentWtgSha256
        && a.currentWctSha256 == b.currentWctSha256 && a.mapObjectReferencesSha256 == b.mapObjectReferencesSha256;
}
template<class T> T* Find(std::vector<T>& values, StableId id) {
    const auto at = std::find_if(values.begin(), values.end(), [id](const auto& value) { return value.id == id; });
    return at == values.end() ? nullptr : &*at;
}
Status SetText(RawText& text, const std::string& value) {
    RawText replacement{value, text.encoding, {}};
    const auto encoded = replacement.ToBytes();
    if (!encoded.Succeeded()) return {encoded.diagnostics};
    text = std::move(replacement); return Status::Ok();
}
bool SameRaw(const RawText& left, const RawText& right) {
    const auto a = left.ToBytes(), b = right.ToBytes();
    return a.Succeeded() && b.Succeeded() && left.text == right.text && left.encoding == right.encoding && *a.value == *b.value;
}
std::size_t RecordCount(const TriggerDocument& doc) {
    return doc.categories.size() + doc.variables.size() + doc.triggers.size() + doc.ecaNodes.size() + doc.argumentNodes.size();
}

// 预算约束请求及新增工作，不限制既有地图总节点数。
struct RequestBudget {
    const TriggerGraphEditLimits& limits;
    std::size_t text = 0, records = 0;
    bool AddText(std::string_view value) {
        if (value.size() > limits.maxRequestTextBytes - text || value.find('\0') != std::string_view::npos) return false;
        text += value.size(); return true;
    }
    bool Ref(const GraphRecordRef& ref) { return AddText(ref.clientKey); }
    bool Parent(const GraphParentExpectation& parent) {
        if (parent.orderedChildren.size() > limits.maxRequestTextBytes / sizeof(GraphRecordRef) || !Ref(parent.owner)) return false;
        for (const auto& ref : parent.orderedChildren) if (!Ref(ref)) return false;
        return true;
    }
    bool Arg(const GraphArgumentValue& argument, std::size_t depth, bool existingRoot = false) {
        if (depth > limits.maxDepth || (!existingRoot && records++ >= limits.maxAddedRecords)
            || !AddText(argument.clientKey) || !AddText(argument.scalar.value) || !AddText(argument.origin)) return false;
        return (!argument.call || Eca(*argument.call, depth + 1)) && (!argument.index || Arg(*argument.index, depth + 1));
    }
    bool Eca(const GraphEcaValue& eca, std::size_t depth) {
        if (depth > limits.maxDepth || records++ >= limits.maxAddedRecords
            || !AddText(eca.clientKey) || !AddText(eca.function) || !AddText(eca.origin)) return false;
        for (const auto& arg : eca.arguments) if (!Arg(arg, depth + 1)) return false;
        for (const auto& child : eca.children) if (!child || !Eca(*child, depth + 1)) return false;
        return true;
    }
    bool Field(const GraphFieldValue& value) {
        return std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::string>) return AddText(item);
            else if constexpr (std::is_same_v<T, GraphScalarValue>) return AddText(item.value);
            else return true;
        }, value);
    }
    bool Operation(const GraphEditOperation& op) {
        return std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, GraphCreate>) {
                if (records++ >= limits.maxAddedRecords || !AddText(item.clientKey) || !Parent(item.parent)
                    || (item.before && !Ref(*item.before))) return false;
                return std::visit([&](const auto& value) {
                    using V = std::decay_t<decltype(value)>;
                    if (!AddText(value.name)) return false;
                    if constexpr (std::is_same_v<V, GraphVariableValue>) return AddText(value.typeName) && AddText(value.initialValue);
                    else if constexpr (std::is_same_v<V, GraphTriggerValue>) return AddText(value.description);
                    else return true;
                }, item.value);
            } else if constexpr (std::is_same_v<T, GraphSet>) {
                return Ref(item.target) && Field(item.expected) && Field(item.value);
            } else if constexpr (std::is_same_v<T, GraphRename>) {
                if (!Ref(item.target) || !AddText(item.expectedName.text) || !AddText(item.name)
                    || item.expectedName.originalBytes.size() > limits.maxRequestTextBytes - text) return false;
                text += item.expectedName.originalBytes.size();
                for (const auto& audit : item.codeAudits) {
                    for (const auto* value : {&audit.sourceSha256, &audit.uiCatalogSha256, &audit.oldName,
                             &audit.newName, &audit.oldSymbol, &audit.newSymbol, &audit.reason})
                        if (!AddText(*value)) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, GraphMove>) {
                return Ref(item.target) && Parent(item.from) && Parent(item.to) && (!item.before || Ref(*item.before));
            } else if constexpr (std::is_same_v<T, GraphRemove>) {
                if (!Ref(item.target) || !AddText(item.expectedName) || !Parent(item.parent) || item.codeAudits.size() > 16384) return false;
                std::size_t blocks = 0;
                for (const auto& audit : item.codeAudits) {
                    if (!AddText(audit.reviewSha256) || !AddText(audit.decision) || !AddText(audit.reason)) return false;
                    if (audit.blocks.size() > 16384 - blocks) return false;
                    blocks += audit.blocks.size();
                    for (const auto& block : audit.blocks)
                        if (!AddText(block.sourceSha256) || !AddText(block.evidenceSha256)) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, GraphReplaceArgument>) {
                return Ref(item.target) && AddText(item.expected.uiSignature) && AddText(item.expected.expectedType)
                    && AddText(item.expected.subtreeSha256) && Arg(item.value, 0, true);
            } else if constexpr (std::is_same_v<T, GraphInsertText>) {
                return Parent(item.parent) && (!item.before || Ref(*item.before)) && AddText(item.text);
            } else if constexpr (std::is_same_v<T, GraphReplaceTriggerText>) {
                return Ref(item.trigger) && AddText(item.text);
            } else if constexpr (std::is_same_v<T, GraphApplyFeatureText>) {
                return AddText(item.category) && AddText(item.text) && AddText(item.clientKey) && (!item.after || Ref(*item.after));
            } else if constexpr (std::is_same_v<T, GraphPatchTriggerText>) {
                return Ref(item.trigger) && AddText(item.expected) && AddText(item.text);
            } else if constexpr (std::is_same_v<T, GraphPlace>) {
                return Ref(item.target) && Ref(item.anchor);
            } else if constexpr (std::is_same_v<T, GraphCodeIdentityChange>) {
                return static_cast<bool>(item.prepared);
            } else {
                return Parent(item.parent) && (!item.before || Ref(*item.before)) && item.root && Eca(*item.root, 0);
            }
        }, op);
    }
};

struct Editor {
    TriggerDocument& doc;
    const UiCatalog& catalog;
    TriggerTypeSystem types;
    const TriggerGraphEditLimits& limits;
    const MapObjectReferenceSnapshot* mapObjects;
    TriggerGraphEditReceipt receipt;
    std::set<StableId> typedEcas, typedArguments, typedVariables;
    std::uint64_t nextId;
    std::set<StableId> changedVariableTypes;
    std::map<StableId, std::string> origins; // 紧凑写法节点的行列，诊断定位用
    // 整体替换时目标触发器原有的值，按所属触发器记录，只能留在原触发器里。
    std::set<std::pair<StableId, std::string>> preservedCode; // (触发器, 代码类参数值)
    std::set<std::tuple<StableId, std::string, std::string>> preservedLiterals; // (触发器, 期望类型, 固定值)

    void TouchEcaTree(std::uint32_t index, std::size_t depth = 0) {
        if (index >= doc.ecaNodes.size() || depth > limits.maxDepth) return;
        const auto& node = doc.ecaNodes[index]; typedEcas.insert(node.id);
        for (const auto arg : node.arguments) if (arg < doc.argumentNodes.size() && doc.argumentNodes[arg].callEca)
            TouchEcaTree(*doc.argumentNodes[arg].callEca, depth + 1);
        for (const auto arg : node.arguments) if (arg < doc.argumentNodes.size()) {
            auto index = doc.argumentNodes[arg].indexArgument;
            for (std::size_t indexDepth = depth; index && *index < doc.argumentNodes.size() && indexDepth <= limits.maxDepth; ++indexDepth) {
                const auto& nested = doc.argumentNodes[*index];
                if (nested.callEca) TouchEcaTree(*nested.callEca, indexDepth + 1);
                index = nested.indexArgument;
            }
        }
        for (const auto child : node.children) TouchEcaTree(child, depth + 1);
    }

    Result<StableId> NewId() {
        const auto id = detail::AllocateId(nextId);
        return id ? Result<StableId>::Success(id) : Result<StableId>::Failure(kDomain, "TRIGGER_GRAPH_ID_EXHAUSTED", "批次稳定 ID 空间已耗尽。");
    }
    // 只给现有 Builder 的新尾部记录分配批内单调身份，不重写索引或复制构造/撤销算法。
    Status FreshAppend(EcaAppendReceipt& append) {
        const auto oldRootId = append.rootEcaId;
        for (std::size_t i = append.previousEcaCount; i < doc.ecaNodes.size(); ++i) {
            const auto id = NewId(); if (!id.Succeeded()) return {id.diagnostics};
            auto& row = doc.ecaNodes[i];
            if (row.id == oldRootId) append.rootEcaId = *id.value;
            row.id = *id.value;
        }
        for (std::size_t i = append.previousArgumentCount; i < doc.argumentNodes.size(); ++i) {
            const auto id = NewId(); if (!id.Succeeded()) return {id.diagnostics};
            doc.argumentNodes[i].id = *id.value;
        }
        return Status::Ok();
    }

    bool Exists(GraphRecordKind kind, StableId id) {
        switch (kind) {
        case GraphRecordKind::Category: return Find(doc.categories, id) != nullptr;
        case GraphRecordKind::Variable: return Find(doc.variables, id) != nullptr;
        case GraphRecordKind::Trigger: return Find(doc.triggers, id) != nullptr;
        case GraphRecordKind::Eca: return Find(doc.ecaNodes, id) != nullptr;
        case GraphRecordKind::Argument: return Find(doc.argumentNodes, id) != nullptr;
        }
        return false;
    }
    // 引用须用查询结果各行 ref 里的稳定编号；分类行另有 WTG 分类号 category_id，常被误用，单独指出。
    std::string MissingMessage(GraphRecordKind kind, StableId id) const {
        static constexpr const char* names[] = {"category", "variable", "trigger", "eca", "argument"};
        const auto index = static_cast<std::size_t>(kind);
        std::string message = std::string("目标对象不在当前文档中：") + (index < std::size(names) ? names[index] : "unknown") +
            " " + std::to_string(id) + "。引用请用查询结果中该行 ref 的 id。";
        const auto category = std::find_if(doc.categories.begin(), doc.categories.end(),
            [&](const auto& row) { return static_cast<StableId>(row.categoryId) == id; });
        if (kind == GraphRecordKind::Category && category != doc.categories.end())
            message += "该数字是分类“" + category->name.text + "”的 WTG 分类号 category_id，它的 ref.id 是 " +
                std::to_string(category->id) + "。";
        return message;
    }
    Result<StableId> Resolve(const GraphRecordRef& ref) {
        if ((ref.id != 0) == !ref.clientKey.empty())
            return Result<StableId>::Failure(kDomain, "TRIGGER_GRAPH_REF_INVALID", "对象引用须恰有 stableId 或批内 clientKey。");
        StableId id = ref.id;
        if (!ref.clientKey.empty()) {
            const auto found = receipt.created.find(ref.clientKey);
            if (found == receipt.created.end() || found->second.kind != ref.kind)
                return Result<StableId>::Failure(kDomain, "TRIGGER_GRAPH_LOCAL_KEY_MISSING", "批内对象尚未创建或记录种类不符：" + ref.clientKey);
            id = found->second.id;
        }
        if (!Exists(ref.kind, id)) return Result<StableId>::Failure(kDomain, "TRIGGER_GRAPH_RECORD_MISSING", MissingMessage(ref.kind, id));
        return Result<StableId>::Success(id);
    }
    Status Bind(std::string_view key, GraphRecordKind kind, StableId id, bool required = false) {
        if (key.empty()) return required ? Fail("TRIGGER_GRAPH_LOCAL_KEY_INVALID", "创建记录必须有非空 clientKey。") : Status::Ok();
        if (key.size() > 128 || !receipt.created.emplace(std::string(key), GraphRecordRef{kind, id, {}}).second)
            return Fail("TRIGGER_GRAPH_LOCAL_KEY_DUPLICATE", "批内 clientKey 重复或超长：" + std::string(key));
        return Status::Ok();
    }
    Result<std::vector<StableId>> Children(const GraphParentExpectation& parent) {
        std::vector<StableId> ids;
        if (parent.kind == GraphParentKind::Categories || parent.kind == GraphParentKind::Variables) {
            if (parent.owner.id != 0 || !parent.owner.clientKey.empty() || parent.childBranch)
                return Result<std::vector<StableId>>::Failure(kDomain, "TRIGGER_GRAPH_PARENT_INVALID", "文档根不能带 owner 或分支。");
            if (parent.kind == GraphParentKind::Categories) for (const auto& row : doc.categories) ids.push_back(row.id);
            else for (const auto& row : doc.variables) ids.push_back(row.id);
        } else {
            const auto owner = Resolve(parent.owner); if (!owner.Succeeded()) return Failed<std::vector<StableId>>(owner);
            const auto append = [&](const std::vector<std::uint32_t>& indices) {
                for (const auto index : indices) { if (index >= doc.ecaNodes.size()) return false; ids.push_back(doc.ecaNodes[index].id); }
                return true;
            };
            if (parent.kind == GraphParentKind::CategoryTriggers && parent.owner.kind == GraphRecordKind::Category && !parent.childBranch) {
                const auto category = Find(doc.categories, *owner.value)->categoryId;
                for (const auto& row : doc.triggers) if (row.categoryId == category) ids.push_back(row.id);
            } else if (parent.kind == GraphParentKind::TriggerEcas && parent.owner.kind == GraphRecordKind::Trigger && !parent.childBranch) {
                if (!append(Find(doc.triggers, *owner.value)->ecas)) return Result<std::vector<StableId>>::Failure(kDomain, "TRIGGER_GRAPH_PARENT_INVALID", "根节点索引无效。");
            } else if (parent.kind == GraphParentKind::EcaChildren && parent.owner.kind == GraphRecordKind::Eca && parent.childBranch && *parent.childBranch >= 0) {
                // guard 包含全部分支顺序，childBranch 只决定新节点的挂载分支。
                if (!append(Find(doc.ecaNodes, *owner.value)->children)) return Result<std::vector<StableId>>::Failure(kDomain, "TRIGGER_GRAPH_PARENT_INVALID", "子节点索引无效。");
            } else return Result<std::vector<StableId>>::Failure(kDomain, "TRIGGER_GRAPH_PARENT_INVALID", "父种类、owner 和分支不匹配。");
        }
        return Result<std::vector<StableId>>::Success(std::move(ids));
    }
    GraphRecordKind ChildKind(GraphParentKind kind) const {
        switch (kind) {
        case GraphParentKind::Categories: return GraphRecordKind::Category;
        case GraphParentKind::Variables: return GraphRecordKind::Variable;
        case GraphParentKind::CategoryTriggers: return GraphRecordKind::Trigger;
        default: return GraphRecordKind::Eca;
        }
    }
    Status Guard(const GraphParentExpectation& parent, std::optional<StableId> member = {}) {
        const auto children = Children(parent); if (!children.Succeeded()) return {children.diagnostics};
        if (children.value->size() != parent.orderedChildren.size()) return Fail("TRIGGER_GRAPH_ORDER_STALE", "父列表数量已变化。");
        for (std::size_t i = 0; i < children.value->size(); ++i) {
            if (parent.orderedChildren[i].kind != ChildKind(parent.kind)) return Fail("TRIGGER_GRAPH_PARENT_INVALID", "顺序 guard 的记录种类不符。");
            const auto id = Resolve(parent.orderedChildren[i]); if (!id.Succeeded()) return {id.diagnostics};
            if (*id.value != (*children.value)[i]) return Fail("TRIGGER_GRAPH_ORDER_STALE", "父列表顺序已变化。");
        }
        if (member && std::find(children.value->begin(), children.value->end(), *member) == children.value->end())
            return Fail("TRIGGER_GRAPH_MOUNT_STALE", "目标不属于声明的父列表。");
        if (member && parent.kind == GraphParentKind::EcaChildren && Find(doc.ecaNodes, *member)->childId != parent.childBranch)
            return Fail("TRIGGER_GRAPH_BRANCH_MISMATCH", "目标的原分支号已变化。");
        return Status::Ok();
    }
    Result<StableId> Before(const GraphParentExpectation& parent, const std::optional<GraphRecordRef>& before) {
        if (!before) return Result<StableId>::Success(0);
        if (before->kind != ChildKind(parent.kind)) return Result<StableId>::Failure(kDomain, "TRIGGER_GRAPH_PARENT_INVALID", "before 对象种类不符。");
        const auto id = Resolve(*before); if (!id.Succeeded()) return id;
        const auto children = Children(parent); if (!children.Succeeded()) return Failed<StableId>(children);
        if (std::find(children.value->begin(), children.value->end(), *id.value) == children.value->end())
            return Result<StableId>::Failure(kDomain, "TRIGGER_GRAPH_BEFORE_MISSING", "before 对象不在目标父列表中，拒绝静默追加。");
        if (parent.kind == GraphParentKind::EcaChildren && Find(doc.ecaNodes, *id.value)->childId != parent.childBranch)
            return Result<StableId>::Failure(kDomain, "TRIGGER_GRAPH_BRANCH_MISMATCH", "before 对象属于其他分支。");
        return id;
    }
    Status Position(GraphRecordKind kind, StableId id, const GraphParentExpectation& parent, StableId before) {
        if (kind != ChildKind(parent.kind)) return Fail("TRIGGER_GRAPH_PARENT_INVALID", "记录不能放入该父列表。");
        if (kind == GraphRecordKind::Category) return TriggerDirectoryEditor::MoveCategory(doc, Find(doc.categories, id)->categoryId,
            before ? std::optional(Find(doc.categories, before)->categoryId) : std::nullopt);
        if (kind == GraphRecordKind::Trigger) {
            const auto owner = Resolve(parent.owner); if (!owner.Succeeded()) return {owner.diagnostics};
            return TriggerDirectoryEditor::MoveTrigger(doc, id, Find(doc.categories, *owner.value)->categoryId, before);
        }
        if (kind == GraphRecordKind::Eca) {
            const auto owner = Resolve(parent.owner); if (!owner.Succeeded()) return {owner.diagnostics};
            auto moved = TriggerDocumentEditor::MoveEca(doc, id,
                parent.kind == GraphParentKind::TriggerEcas ? *owner.value : 0,
                parent.kind == GraphParentKind::EcaChildren ? *owner.value : 0, parent.childBranch, before);
            return {std::move(moved.diagnostics)};
        }
        return Fail("TRIGGER_GRAPH_MOVE_UNSUPPORTED", "变量顺序不影响此批次；尚未开放变量重排。");
    }
    Status ValidateVariable(const VariableRecord& variable) {
        const auto* type = types.Find(variable.typeName.text);
        if (!type || !type->canBeGlobal) return Fail("TRIGGER_GRAPH_VARIABLE_TYPE", "变量类型不在真实可声明全局类型目录中：" + variable.typeName.text);
        if (variable.size <= 0 || variable.size > 8192 || (variable.isArray == 0 && variable.size != 1))
            return Fail("TRIGGER_GRAPH_VARIABLE_ARRAY", "数组长度须为 1..8192；标量长度须为 1。");
        if (variable.hasDefault != 0) {
            if (types.LiteralKind(variable.typeName.text) == TriggerTypeSystem::Literal::Script)
                return Fail("TRIGGER_GRAPH_CODE_UNSUPPORTED", "通用 T 批次不接受代码初值。");
            if (const auto preset = FindPreset(catalog, variable.value.text)) {
                if (!types.Accepts(variable.typeName.text, preset->type)) return Fail("TRIGGER_GRAPH_TYPE_MISMATCH", "变量初值预设类型不符。");
            } else {
                if (types.LiteralKind(variable.typeName.text) == TriggerTypeSystem::Literal::Other && variable.value.text != "null")
                    return Fail("TRIGGER_GRAPH_INITIAL_VALUE_UNSUPPORTED", "该变量初值须为真实预设或明确 null，不能注入表达式。");
                return types.ValidateLiteral(variable.typeName.text, variable.value.text);
            }
        }
        return Status::Ok();
    }
    Status Apply(const GraphCreate& op) {
        if (auto status = Guard(op.parent); !status.Succeeded()) return status;
        const auto before = Before(op.parent, op.before); if (!before.Succeeded()) return {before.diagnostics};
        return std::visit([&](const auto& value) -> Status {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, GraphCategoryValue>) {
                if (op.parent.kind != GraphParentKind::Categories) return Fail("TRIGGER_GRAPH_PARENT_INVALID", "类别须位于文档类别列表。");
                if (auto valid = TriggerDirectoryEditor::ValidateCategoryName(doc, value.name); !valid.Succeeded()) return valid;
                const auto added = TriggerDirectoryEditor::AddCategory(doc, value.name); if (!added.Succeeded()) return {added.diagnostics};
                auto* row = &doc.categories.back(); row->comment = value.comment ? 1 : 0;
                const auto identity = NewId(); if (!identity.Succeeded()) return {identity.diagnostics}; row->id = *identity.value;
                if (auto status = Bind(op.clientKey, GraphRecordKind::Category, row->id, true); !status.Succeeded()) return status;
                return Position(GraphRecordKind::Category, row->id, op.parent, *before.value);
            } else if constexpr (std::is_same_v<T, GraphVariableValue>) {
                if (op.parent.kind != GraphParentKind::Variables || op.before) return Fail("TRIGGER_GRAPH_PARENT_INVALID", "变量创建仅按完整原列表追加。");
                if (auto status = TriggerDocumentEditor::AddVariable(doc, value.name, value.typeName, value.isArray); !status.Succeeded()) return status;
                auto& row = doc.variables.back(); row.size = value.arraySize; row.hasDefault = value.initialized ? 1 : 0;
                const auto identity = NewId(); if (!identity.Succeeded()) return {identity.diagnostics}; row.id = *identity.value;
                if (auto status = SetText(row.value, value.initialValue); !status.Succeeded()) return status;
                // 同名 no-op 调用复用原子编辑器的标识符规则；不修改已有代码。
                const auto checked = TriggerDocumentEditor::RenameVariableAtomic(doc, catalog, row.id, value.name);
                if (!checked.Succeeded()) return {checked.diagnostics};
                if (auto valid = ValidateVariable(row); !valid.Succeeded()) return valid;
                typedVariables.insert(row.id); return Bind(op.clientKey, GraphRecordKind::Variable, row.id, true);
            } else {
                if (op.parent.kind != GraphParentKind::CategoryTriggers) return Fail("TRIGGER_GRAPH_PARENT_INVALID", "触发器须位于真实类别。");
                const auto owner = Resolve(op.parent.owner); if (!owner.Succeeded()) return {owner.diagnostics};
                if (auto status = TriggerDirectoryEditor::ValidateTriggerName(doc, value.name); !status.Succeeded()) return status;
                const auto added = TriggerDocumentEditor::AddTrigger(doc, Find(doc.categories, *owner.value)->categoryId, value.name);
                if (!added.Succeeded()) return {added.diagnostics};
                auto* row = Find(doc.triggers, *added.value);
                const auto identity = NewId(); if (!identity.Succeeded()) return {identity.diagnostics}; row->id = *identity.value;
                row->enabled = value.enabled ? 1 : 0; row->initiallyClosed = value.initiallyOn ? 0 : 1;
                row->runOnInit = value.runOnInit ? 1 : 0; row->type = value.comment ? 1 : 0;
                if (auto status = TriggerDocumentEditor::SetTriggerComment(doc, row->id, value.description); !status.Succeeded()) return status;
                if (auto status = Bind(op.clientKey, GraphRecordKind::Trigger, row->id, true); !status.Succeeded()) return status;
                return Position(GraphRecordKind::Trigger, row->id, op.parent, *before.value);
            }
        }, op.value);
    }
    Result<GraphFieldValue> Field(GraphRecordKind kind, StableId id, GraphField field) {
        if (kind == GraphRecordKind::Category && field == GraphField::Comment) return Result<GraphFieldValue>::Success(Find(doc.categories, id)->comment != 0);
        if (kind == GraphRecordKind::Trigger) {
            const auto* row = Find(doc.triggers, id);
            switch (field) {
            case GraphField::Comment: return Result<GraphFieldValue>::Success(row->type != 0);
            case GraphField::Description: return Result<GraphFieldValue>::Success(row->description.text);
            case GraphField::Enabled: return Result<GraphFieldValue>::Success(row->enabled != 0);
            case GraphField::InitiallyOn: return Result<GraphFieldValue>::Success(row->initiallyClosed == 0);
            case GraphField::RunOnInit: return Result<GraphFieldValue>::Success(row->runOnInit != 0);
            default: break;
            }
        }
        if (kind == GraphRecordKind::Variable) {
            const auto* row = Find(doc.variables, id);
            switch (field) {
            case GraphField::VariableType: return Result<GraphFieldValue>::Success(row->typeName.text);
            case GraphField::VariableArray: return Result<GraphFieldValue>::Success(row->isArray != 0);
            case GraphField::VariableArraySize: return Result<GraphFieldValue>::Success(row->size);
            case GraphField::VariableInitialized: return Result<GraphFieldValue>::Success(row->hasDefault != 0);
            case GraphField::VariableInitialValue: return Result<GraphFieldValue>::Success(row->value.text);
            default: break;
            }
        }
        if (kind == GraphRecordKind::Eca && field == GraphField::Enabled) return Result<GraphFieldValue>::Success(Find(doc.ecaNodes, id)->enabled != 0);
        if (kind == GraphRecordKind::Argument && field == GraphField::ArgumentScalar) {
            const auto* row = Find(doc.argumentNodes, id);
            if (row->callEca || row->indexArgument) return Result<GraphFieldValue>::Failure(kDomain, "TRIGGER_GRAPH_ARGUMENT_COMPLEX", "复杂参数使用有 guard 的移除/插入完整子树，不丢弃 call/index。");
            return Result<GraphFieldValue>::Success(GraphScalarValue{row->type, row->value.text});
        }
        return Result<GraphFieldValue>::Failure(kDomain, "TRIGGER_GRAPH_FIELD_UNSUPPORTED", "该字段不适用或属于尚未开放的代码字段。");
    }
    Status Apply(const GraphSet& op) {
        const auto id = Resolve(op.target); if (!id.Succeeded()) return {id.diagnostics};
        const auto current = Field(op.target.kind, *id.value, op.field); if (!current.Succeeded()) return {current.diagnostics};
        if (*current.value != op.expected) return Fail("TRIGGER_GRAPH_FIELD_STALE", "字段前值已变化。");
        if (op.value.index() != current.value->index()) return Fail("TRIGGER_GRAPH_FIELD_TYPE", "字段新值类型不符，禁止字符串转布尔/整数。");
        if (op.target.kind == GraphRecordKind::Trigger) {
            auto* row = Find(doc.triggers, *id.value);
            if (row->customTextFlag) return Fail("TRIGGER_GRAPH_CUSTOM_READ_ONLY", "完整 T 批次不编辑既有自定义脚本触发器。");
            switch (op.field) {
            case GraphField::Comment: row->type = std::get<bool>(op.value) ? 1 : 0; return Status::Ok();
            case GraphField::Description: return TriggerDocumentEditor::SetTriggerComment(doc, row->id, std::get<std::string>(op.value));
            case GraphField::Enabled:
                if (std::get<bool>(op.value)) for (const auto root : row->ecas) TouchEcaTree(root);
                return TriggerDocumentEditor::SetTriggerEnabled(doc, row->id, std::get<bool>(op.value));
            case GraphField::InitiallyOn: return TriggerDocumentEditor::SetTriggerInitiallyOn(doc, row->id, std::get<bool>(op.value));
            case GraphField::RunOnInit: return TriggerDocumentEditor::SetTriggerRunOnInit(doc, row->id, std::get<bool>(op.value));
            default: break;
            }
        } else if (op.target.kind == GraphRecordKind::Category) {
            Find(doc.categories, *id.value)->comment = std::get<bool>(op.value) ? 1 : 0; return Status::Ok();
        } else if (op.target.kind == GraphRecordKind::Variable) {
            auto* row = Find(doc.variables, *id.value); typedVariables.insert(row->id);
            switch (op.field) {
            case GraphField::VariableType:
                changedVariableTypes.insert(row->id);
                return SetText(row->typeName, std::get<std::string>(op.value));
            case GraphField::VariableArray: row->isArray = std::get<bool>(op.value) ? 1 : 0; break;
            case GraphField::VariableArraySize: row->size = std::get<std::int32_t>(op.value); break;
            case GraphField::VariableInitialized: row->hasDefault = std::get<bool>(op.value) ? 1 : 0; break;
            case GraphField::VariableInitialValue: return SetText(row->value, std::get<std::string>(op.value));
            default: break;
            }
            return Status::Ok();
        } else if (op.target.kind == GraphRecordKind::Eca) {
            if (std::get<bool>(op.value)) {
                const auto* node = Find(doc.ecaNodes, *id.value);
                if (node) TouchEcaTree(static_cast<std::uint32_t>(node - doc.ecaNodes.data()));
            }
            return TriggerDocumentEditor::SetEcaEnabled(doc, *id.value, std::get<bool>(op.value));
        } else {
            const auto& scalar = std::get<GraphScalarValue>(op.value); typedArguments.insert(*id.value);
            return TriggerDocumentEditor::SetScalarArgument(doc, *id.value, scalar.kind, scalar.value);
        }
        return Fail("TRIGGER_GRAPH_FIELD_UNSUPPORTED", "字段不适用。");
    }
    Status Apply(const GraphCodeIdentityChange& op) {
        if (!op.prepared) return Fail("TRIGGER_CODE_SOURCE_STALE", "没有核定的代码身份计划。");
        const auto applied = op.prepared->ApplyTo(doc, catalog);
        if (applied.Succeeded()) {
            ++receipt.codeIdentityChanges;
            nextId = detail::NextGeneratedCounter(doc);
        }
        return applied;
    }
    Status Apply(const GraphRename& op) {
        const auto id = Resolve(op.target); if (!id.Succeeded()) return {id.diagnostics};
        RawText* name = nullptr;
        if (op.target.kind == GraphRecordKind::Category) name = &Find(doc.categories, *id.value)->name;
        if (op.target.kind == GraphRecordKind::Variable) name = &Find(doc.variables, *id.value)->name;
        if (op.target.kind == GraphRecordKind::Trigger) {
            const auto* row = Find(doc.triggers, *id.value);
            if (row->customTextFlag) return Fail("TRIGGER_GRAPH_CUSTOM_READ_ONLY", "完整 T 批次不重命名既有自定义脚本触发器。");
            name = &Find(doc.triggers, *id.value)->name;
        }
        if (!name) return Fail("TRIGGER_GRAPH_RENAME_UNSUPPORTED", "只有类别、变量和触发器声明可重命名。");
        if (!SameRaw(*name, op.expectedName)) return Fail("TRIGGER_GRAPH_NAME_STALE", "名称原字节、编码或解码值已变化。");
        if (op.target.kind == GraphRecordKind::Category) {
            if (!op.codeAudits.empty()) return Fail("TRIGGER_GRAPH_RENAME_AUDIT_INVALID", "类别改名不接受符号代码审计。");
            const auto category = Find(doc.categories, *id.value)->categoryId;
            if (auto status = TriggerDirectoryEditor::ValidateCategoryName(doc, op.name, category); !status.Succeeded()) return status;
            return TriggerDirectoryEditor::RenameCategory(doc, category, op.name);
        }
        auto renamed = op.target.kind == GraphRecordKind::Variable
            ? TriggerDocumentEditor::RenameVariableAtomic(doc, catalog, *id.value, op.name, op.codeAudits)
            : TriggerDocumentEditor::RenameTriggerAtomic(doc, catalog, *id.value, op.name, op.codeAudits);
        if (!renamed.Succeeded()) return {renamed.diagnostics};
        receipt.renames.push_back(std::move(*renamed.value)); return Status::Ok();
    }
    Status Apply(const GraphMove& op) {
        const auto id = Resolve(op.target); if (!id.Succeeded()) return {id.diagnostics};
        if (auto status = Guard(op.from, *id.value); !status.Succeeded()) return status;
        if (auto status = Guard(op.to); !status.Succeeded()) return status;
        const auto before = Before(op.to, op.before); if (!before.Succeeded()) return {before.diagnostics};
        return Position(op.target.kind, *id.value, op.to, *before.value);
    }
    Status Apply(const GraphPlace& op) {
        if (op.target.kind != op.anchor.kind || (op.target.kind != GraphRecordKind::Trigger && op.target.kind != GraphRecordKind::Category))
            return Fail("TRIGGER_GRAPH_PLACE_UNSUPPORTED", "轻量移动只支持触发器或类别，且 target 与 anchor 种类相同；ECA 用 patch_trigger_text 改写。");
        const auto id = Resolve(op.target); if (!id.Succeeded()) return {id.diagnostics};
        const auto anchor = Resolve(op.anchor); if (!anchor.Succeeded()) return {anchor.diagnostics};
        if (*id.value == *anchor.value) return Fail("TRIGGER_GRAPH_PLACE_SELF", "target 与 anchor 是同一个记录。");
        GraphParentExpectation parent{GraphParentKind::Categories, {}, {}, std::nullopt};
        if (op.target.kind == GraphRecordKind::Trigger) {
            const auto categoryId = Find(doc.triggers, *anchor.value)->categoryId;
            const auto category = std::find_if(doc.categories.begin(), doc.categories.end(), [&](const auto& row) { return row.categoryId == categoryId; });
            if (category == doc.categories.end()) return Fail("TRIGGER_GRAPH_PARENT_INVALID", "anchor 所在类别不存在。");
            parent = {GraphParentKind::CategoryTriggers, {GraphRecordKind::Category, category->id, {}}, {}, std::nullopt};
        }
        const auto children = Children(parent); if (!children.Succeeded()) return {children.diagnostics};
        auto at = std::find(children.value->begin(), children.value->end(), *anchor.value);
        StableId before = *anchor.value;
        if (op.after) {
            before = 0;
            for (++at; at != children.value->end(); ++at) if (*at != *id.value) { before = *at; break; }
        }
        return Position(op.target.kind, *id.value, parent, before);
    }
    Status Apply(const GraphRemove& op) {
        const auto id = Resolve(op.target); if (!id.Succeeded()) return {id.diagnostics};
        if (auto status = Guard(op.parent, *id.value); !status.Succeeded()) return status;
        if (op.target.kind != ChildKind(op.parent.kind)) return Fail("TRIGGER_GRAPH_PARENT_INVALID", "移除对象的父种类不符。");
        std::string name;
        switch (op.target.kind) {
        case GraphRecordKind::Category: name = Find(doc.categories, *id.value)->name.text; break;
        case GraphRecordKind::Variable: name = Find(doc.variables, *id.value)->name.text; break;
        case GraphRecordKind::Trigger: name = Find(doc.triggers, *id.value)->name.text; break;
        case GraphRecordKind::Eca: name = Find(doc.ecaNodes, *id.value)->name.text; break;
        default: return Fail("TRIGGER_GRAPH_REMOVE_UNSUPPORTED", "不能直接移除参数槽。");
        }
        if (name != op.expectedName) return Fail("TRIGGER_GRAPH_NAME_STALE", "移除目标名称前值已变化。");
        if (op.target.kind == GraphRecordKind::Category) return TriggerDirectoryEditor::RemoveEmptyCategory(doc, Find(doc.categories, *id.value)->categoryId);
        if (op.target.kind == GraphRecordKind::Eca) {
            auto removed = TriggerDocumentEditor::RemoveEcaSubtree(doc, *id.value); return {std::move(removed.diagnostics)};
        }
        auto resolved = op; resolved.target.id = *id.value; resolved.target.clientKey.clear();
        if (auto checked = CheckDeclarationRemovalAudit(doc, catalog, resolved, mapObjects); !checked.Succeeded()) return checked;
        receipt.removalAudits.insert(receipt.removalAudits.end(), op.codeAudits.begin(), op.codeAudits.end());
        if (op.target.kind == GraphRecordKind::Variable) return TriggerDocumentEditor::RemoveVariable(doc, name);
        if (Find(doc.triggers, *id.value)->customTextFlag) return Fail("TRIGGER_GRAPH_CUSTOM_READ_ONLY", "不能删除既有自定义代码槽。");
        auto removed = TriggerDocumentEditor::RemoveTrigger(doc, *id.value); return {std::move(removed.diagnostics)};
    }
    Status ClearDefaultCall(StableId argumentId) {
        if (!Find(doc.argumentNodes, argumentId)->callEca) return Status::Ok();
        const auto detached = TriggerDocumentEditor::DetachArgumentCall(doc, argumentId); return {detached.diagnostics};
    }
    Status ConfigureArgument(StableId id, const GraphArgumentValue& spec, const std::string& expected, std::size_t depth) {
        if (depth > limits.maxDepth) return Fail("TRIGGER_GRAPH_BUDGET", "参数递归超过预算。");
        if (auto status = Bind(spec.clientKey, GraphRecordKind::Argument, id); !status.Succeeded()) return status;
        if (!spec.origin.empty()) origins[id] = spec.origin;
        if (auto status = ClearDefaultCall(id); !status.Succeeded()) return status;
        typedArguments.insert(id);
        if (spec.call) {
            // 函数槽（eventcall/boolexpr/boolcall/code）可填对应的事件/条件/动作；其余只能是 Call。
            const auto slot = TriggerTypeSystem::FunctionSlotKind(expected);
            if (spec.index || spec.scalar.kind != 3 || !spec.scalar.value.empty()
                || (spec.call->kind != UiDefinitionKind::Call && spec.call->kind != slot)
                || spec.call->childBranch || !spec.call->children.empty())
                return Fail("TRIGGER_GRAPH_CALL_INVALID", slot
                    ? "参数类型 " + expected + " 的函数须为 " + std::string(ToString(*slot)) + " 或返回该类型的 call，无子分支或数组下标。"
                    : "参数函数须为真实 Call，无子分支或数组下标。");
            auto appended = TriggerDocumentEditor::AppendArgumentCall(doc, catalog, id, expected, spec.call->function, spec.call->kind);
            if (!appended.Succeeded()) return {appended.diagnostics};
            if (auto status = FreshAppend(*appended.value); !status.Succeeded()) return status;
            return ConfigureEca(appended.value->rootEcaId, *spec.call, depth + 1);
        }
        if (spec.scalar.kind != 0 && spec.scalar.kind != 1 && spec.scalar.kind != 3 && spec.scalar.kind != -1)
            return Fail("TRIGGER_GRAPH_ARGUMENT_KIND", "参数种类须为预设、变量、字面值、未设置或显式递归函数。");
        if (auto status = TriggerDocumentEditor::SetScalarArgument(doc, id, spec.scalar.kind, spec.scalar.value); !status.Succeeded()) return status;
        if (spec.index) {
            if (spec.scalar.kind != 1) return Fail("TRIGGER_GRAPH_INDEX_INVALID", "只有真实数组变量可携带下标。");
            const auto reference = ResolveVariableReference(doc, spec.scalar.value, mapObjects);
            if (!reference.Succeeded()) return {reference.diagnostics};
            if (!reference.value->isArray) return Fail("TRIGGER_GRAPH_INDEX_INVALID", "下标来源不是当前已声明数组。");
            if (doc.argumentNodes.size() >= kInvalidNodeIndex) return Fail("TRIGGER_GRAPH_BUDGET", "参数索引空间已耗尽。");
            const auto identity = NewId(); if (!identity.Succeeded()) return {identity.diagnostics};
            ArgumentNode index; index.id = *identity.value; index.type = 3;
            index.value = RawText{"0", Find(doc.argumentNodes, id)->value.encoding, {}};
            const auto childId = index.id; const auto childIndex = static_cast<std::uint32_t>(doc.argumentNodes.size());
            doc.argumentNodes.push_back(std::move(index));
            auto* argument = Find(doc.argumentNodes, id); argument->insertIndex = 1; argument->indexArgument = childIndex;
            return ConfigureArgument(childId, *spec.index, "integer", depth + 1);
        }
        return Status::Ok();
    }
    // 最深一层带位置的失败节点给首条诊断补上位置；外层不重复追加。
    // 代码类参数（scriptcode）：默认自然 T，不由批次新写代码。放行两种不是新代码的值：
    // 纯标识符（如 YDWE 局部变量名 loc_u，中文名亦可）；整体替换时原样保留的目标触发器已有代码。
    static bool PlainIdentifier(std::string_view value) {
        if (value.empty() || std::isdigit(static_cast<unsigned char>(value.front()))) return false;
        return std::all_of(value.begin(), value.end(), [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte >= 0x80 || std::isalnum(byte) || c == '_';
        });
    }
    bool CodeValueAllowed(StableId trigger, const std::string& value) const {
        return PlainIdentifier(value) || preservedCode.contains({trigger, value});
    }
    // “注释”动作的参数按 // 单行输出，不执行；单行文本不是新代码，换行可能夹带代码，仍拒绝。
    static bool CommentText(std::string_view function, std::string_view value) {
        return function == "CommentString" && value.find_first_of("\r\n") == std::string_view::npos;
    }
    Status CodeRejected() const {
        return Status::Failure(kDomain, "TRIGGER_GRAPH_CODE_UNSUPPORTED",
            "代码类参数只接受标识符（如 YDWE 局部变量名）或整体替换时原样保留的已有代码；新写代码须作者授权，默认用自然 T。");
    }
    void CollectCode(std::uint32_t index, const std::vector<LocalVariable>& locals, std::size_t depth) {
        if (depth > 256 || index >= doc.ecaNodes.size()) return;
        const auto& eca = doc.ecaNodes[index];
        for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
            if (eca.arguments[i] >= doc.argumentNodes.size()) continue;
            const auto& argument = doc.argumentNodes[eca.arguments[i]];
            if (argument.type == 3) {
                const auto expected = types.ExpectedType(doc, eca, i, locals);
                if (types.LiteralKind(expected) == TriggerTypeSystem::Literal::Script) preservedCode.emplace(eca.sourceTriggerId, argument.value.text);
                else preservedLiterals.emplace(eca.sourceTriggerId, expected, argument.value.text);
            }
            if (argument.type == 2 && argument.callEca) CollectCode(*argument.callEca, locals, depth + 1);
        }
        for (const auto child : eca.children) CollectCode(child, locals, depth + 1);
    }
    static void Locate(Status& status, const std::string& origin) {
        if (origin.empty() || status.diagnostics.empty()) return;
        auto& message = status.diagnostics.front().message;
        if (message.find("【位置") == std::string::npos) message += "【位置：" + origin + "】";
    }
    Status ConfigureEca(StableId id, const GraphEcaValue& spec, std::size_t depth) {
        if (!spec.origin.empty()) origins[id] = spec.origin;
        auto status = ConfigureEcaBody(id, spec, depth);
        if (!status.Succeeded()) Locate(status, spec.origin);
        return status;
    }
    Status ConfigureEcaBody(StableId id, const GraphEcaValue& spec, std::size_t depth) {
        if (depth > limits.maxDepth) return Fail("TRIGGER_GRAPH_BUDGET", "ECA 递归超过预算。");
        if (auto status = Bind(spec.clientKey, GraphRecordKind::Eca, id); !status.Succeeded()) return status;
        auto* node = Find(doc.ecaNodes, id);
        if (!node || node->arguments.size() != spec.arguments.size()) return Fail("TRIGGER_GRAPH_ARGUMENT_COUNT", "递归参数数量与真实 Catalog 签名不符。");
        node->enabled = spec.enabled ? 1 : 0; typedEcas.insert(id);
        std::vector<StableId> arguments;
        for (const auto index : node->arguments) arguments.push_back(doc.argumentNodes[index].id);
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            node = Find(doc.ecaNodes, id);
            const auto expected = types.ExpectedType(doc, *node, i);
            if (expected.empty() || expected == "Null") return Fail("TRIGGER_GRAPH_TYPE_UNRESOLVED", "递归参数期望类型无法从真实 Catalog 和已配置前参数确定。");
            if (types.LiteralKind(expected) == TriggerTypeSystem::Literal::Script) {
                const auto& value = spec.arguments[i];
                // 函数库定义的预设也不是新代码。
                const bool preset = value.scalar.kind == 0 && FindPreset(catalog, value.scalar.value).has_value();
                const bool comment = value.scalar.kind == 3 && CommentText(node->name.text, value.scalar.value);
                if (value.call || value.index || (!preset && !comment
                        && (value.scalar.kind != 3 || !CodeValueAllowed(node->sourceTriggerId, value.scalar.value))))
                    return CodeRejected();
            }
            if (auto status = ConfigureArgument(arguments[i], spec.arguments[i], expected, depth + 1); !status.Succeeded()) return status;
        }
        for (const auto& child : spec.children) {
            if (!child || !child->childBranch || *child->childBranch < 0 || child->kind == UiDefinitionKind::Call)
                return Fail("TRIGGER_GRAPH_BRANCH_INVALID", "子 ECA 必须带非负分支号，不能将 Call 当作分支语句。");
            const auto triggerId = Find(doc.ecaNodes, id)->sourceTriggerId;
            auto created = TriggerDocumentEditor::AppendRootEca(doc, catalog, triggerId, child->kind, child->function);
            if (!created.Succeeded()) return {created.diagnostics};
            if (auto status = FreshAppend(*created.value); !status.Succeeded()) return status;
            auto moved = TriggerDocumentEditor::MoveEca(doc, created.value->rootEcaId, 0, id, child->childBranch);
            if (!moved.Succeeded()) return {moved.diagnostics};
            if (auto status = ConfigureEca(created.value->rootEcaId, *child, depth + 1); !status.Succeeded()) return status;
        }
        return Status::Ok();
    }
    Status Apply(const GraphInsertSubtree& op) {
        if (!op.root || (op.parent.kind != GraphParentKind::TriggerEcas && op.parent.kind != GraphParentKind::EcaChildren))
            return Fail("TRIGGER_GRAPH_PARENT_INVALID", "递归子树须插入触发器根或 ECA children。");
        if (auto status = Guard(op.parent); !status.Succeeded()) return status;
        const auto before = Before(op.parent, op.before); if (!before.Succeeded()) return {before.diagnostics};
        const auto owner = Resolve(op.parent.owner); if (!owner.Succeeded()) return {owner.diagnostics};
        const auto triggerId = op.parent.kind == GraphParentKind::TriggerEcas ? *owner.value : Find(doc.ecaNodes, *owner.value)->sourceTriggerId;
        return InsertRoot(op.parent, *before.value, triggerId, *op.root);
    }
    Status InsertRoot(const GraphParentExpectation& parent, StableId before, StableId triggerId, const GraphEcaValue& root) {
        if (root.kind == UiDefinitionKind::Call || root.childBranch != parent.childBranch)
            return Fail("TRIGGER_GRAPH_BRANCH_INVALID", "根 ECA 的种类或分支与父挂载不符。");
        auto created = TriggerDocumentEditor::AppendRootEca(doc, catalog, triggerId, root.kind, root.function);
        if (!created.Succeeded()) { Status status{created.diagnostics}; Locate(status, root.origin); return status; }
        if (auto status = FreshAppend(*created.value); !status.Succeeded()) return status;
        if (auto status = Position(GraphRecordKind::Eca, created.value->rootEcaId, parent, before); !status.Succeeded()) return status;
        return ConfigureEca(created.value->rootEcaId, root, 0);
    }
    Status Apply(const GraphInsertText& op) {
        if (op.parent.kind != GraphParentKind::TriggerEcas && op.parent.kind != GraphParentKind::EcaChildren)
            return Fail("TRIGGER_GRAPH_PARENT_INVALID", "紧凑写法须插入触发器根或 ECA children。");
        if (auto status = Guard(op.parent); !status.Succeeded()) return status;
        const auto before = Before(op.parent, op.before); if (!before.Succeeded()) return {before.diagnostics};
        const auto owner = Resolve(op.parent.owner); if (!owner.Succeeded()) return {owner.diagnostics};
        TriggerTextTarget target;
        StableId triggerId = *owner.value;
        if (op.parent.kind == GraphParentKind::EcaChildren) {
            const auto* block = Find(doc.ecaNodes, *owner.value);
            target.blockFunction = block->name.text; target.childBranch = *op.parent.childBranch; triggerId = block->sourceTriggerId;
        }
        const auto roots = ParseTriggerText(op.text, doc, catalog, target, limits);
        if (!roots.Succeeded()) return {roots.diagnostics};
        if (roots.value->empty()) return Fail("TRIGGER_TEXT_EMPTY", "文本里没有任何语句。");
        for (const auto& root : *roots.value)
            if (auto status = InsertRoot(op.parent, *before.value, triggerId, *root); !status.Succeeded()) return status;
        return Status::Ok();
    }
    Status Apply(const GraphReplaceTriggerText& op) {
        if (op.trigger.kind != GraphRecordKind::Trigger) return Fail("TRIGGER_GRAPH_PARENT_INVALID", "整体替换的目标必须是触发器。");
        const auto id = Resolve(op.trigger); if (!id.Succeeded()) return {id.diagnostics};
        const auto* trigger = Find(doc.triggers, *id.value);
        if (trigger->customTextFlag != 0) return Fail("TRIGGER_GRAPH_CUSTOM_READ_ONLY", "自定义代码触发器没有 T 树，不能用紧凑写法替换。");
        const auto roots = ParseTriggerText(op.text, doc, catalog, {}, limits);
        if (!roots.Succeeded()) return {roots.diagnostics};
        const auto locals = types.CollectLocals(doc, *trigger);
        for (const auto index : trigger->ecas) CollectCode(index, locals, 0);
        if (auto cleared = TriggerDocumentEditor::ClearTriggerEcas(doc, *id.value); !cleared.Succeeded()) return cleared;
        const GraphParentExpectation parent{GraphParentKind::TriggerEcas, {GraphRecordKind::Trigger, *id.value, {}}, {}, std::nullopt};
        for (const auto& root : *roots.value)
            if (auto status = InsertRoot(parent, 0, *id.value, *root); !status.Succeeded()) return status;
        return Status::Ok();
    }
    // 功能写法：按名字对齐类别、变量与触发器，各步复用本批次的创建、设置与整体替换（同一校验与撤销）；
    // 变量与触发器的错误一次列出，各带功能文本中的行号。
    Status Apply(const GraphApplyFeatureText& op) {
        auto feature = ParseFeatureText(op.text);
        if (!feature.Succeeded() || !feature.value) return {feature.diagnostics};
        const std::string prefix = op.clientKey.empty() ? "feature" : op.clientKey;
        const auto key = [&](std::string_view kind, const std::string& name, std::size_t index) {
            const auto readable = prefix + "/" + std::string(kind) + "/" + name;
            return readable.size() <= 128 ? readable : prefix + "/" + std::string(kind) + "#" + std::to_string(index);
        };
        const auto ordered = [&](GraphParentKind kind, GraphRecordRef owner) {
            GraphParentExpectation parent{kind, std::move(owner), {}, std::nullopt};
            if (const auto ids = Children(parent); ids.Succeeded())
                for (const auto id : *ids.value) parent.orderedChildren.push_back({ChildKind(kind), id, {}});
            return parent;
        };
        std::vector<Diagnostic> errors;
        const auto collect = [&](Status status, std::size_t line, const std::string& what) {
            for (auto& diagnostic : status.diagnostics) {
                if (diagnostic.severity != DiagnosticSeverity::Error || errors.size() >= 64) continue;
                // 触发器体的紧凑 T 报错已带整段文本的行列；其余补上标题行号。
                if (diagnostic.message.find("第 ") != 0) diagnostic.message = "第 " + std::to_string(line) + " 行（" + what + "）：" + diagnostic.message;
                errors.push_back(std::move(diagnostic));
            }
            return status.Succeeded();
        };
        const auto set = [&](GraphRecordRef target, GraphField field, GraphFieldValue value) -> Status {
            auto current = Field(target.kind, target.id, field);
            if (!current.Succeeded() || !current.value) return {current.diagnostics};
            if (*current.value == value) return Status::Ok();
            return Apply(GraphSet{target, field, *current.value, std::move(value)});
        };

        StableId category = 0;
        for (const auto& row : doc.categories) {
            if (row.name.text != op.category) continue;
            if (category != 0) return Fail("TRIGGER_FEATURE_CATEGORY_AMBIGUOUS", "有多个类别叫“" + op.category + "”，无法确定写入哪一个。");
            category = row.id;
        }
        if (category == 0) {
            const auto categoryKey = prefix + "/category";
            if (auto status = Apply(GraphCreate{categoryKey, GraphCategoryValue{op.category, false},
                    ordered(GraphParentKind::Categories, {}), std::nullopt}); !status.Succeeded()) return status;
            category = receipt.created.at(categoryKey).id;
        }

        for (std::size_t index = 0; index < feature.value->variables.size(); ++index) {
            const auto& variable = feature.value->variables[index];
            const auto what = "变量 " + variable.name;
            const auto existing = std::find_if(doc.variables.begin(), doc.variables.end(),
                [&](const auto& row) { return row.name.text == variable.name; });
            if (existing == doc.variables.end()) {
                collect(Apply(GraphCreate{key("variable", variable.name, index),
                    GraphVariableValue{variable.name, variable.type, variable.isArray, variable.size,
                        variable.initial.has_value(), variable.initial.value_or("")},
                    ordered(GraphParentKind::Variables, {}), std::nullopt}), variable.line, what);
                continue;
            }
            if (existing->typeName.text != variable.type || (existing->isArray != 0) != variable.isArray) {
                collect(Fail("TRIGGER_FEATURE_VARIABLE_CONFLICT", "已有同名变量，类型为 " + existing->typeName.text
                    + (existing->isArray != 0 ? "[]" : "") + "；功能文本不能改变已有变量的类型或是否数组。"), variable.line, what);
                continue;
            }
            const GraphRecordRef ref{GraphRecordKind::Variable, existing->id, {}};
            if (variable.isArray && !collect(set(ref, GraphField::VariableArraySize, variable.size), variable.line, what)) continue;
            if (!collect(set(ref, GraphField::VariableInitialized, variable.initial.has_value()), variable.line, what)) continue;
            if (variable.initial) collect(set(ref, GraphField::VariableInitialValue, *variable.initial), variable.line, what);
        }
        if (!errors.empty()) return {std::move(errors)};

        const auto categoryId = Find(doc.categories, category)->categoryId;
        std::optional<GraphRecordRef> anchor = op.after;
        if (anchor) {
            const auto id = anchor->kind == GraphRecordKind::Trigger ? Resolve(*anchor)
                : Result<StableId>::Failure(kDomain, "TRIGGER_FEATURE_ANCHOR_INVALID", "after 须是触发器。");
            if (!id.Succeeded()) return {id.diagnostics};
            if (Find(doc.triggers, *id.value)->categoryId != categoryId)
                return Fail("TRIGGER_FEATURE_ANCHOR_INVALID", "after 指定的触发器不在类别“" + op.category + "”里。");
        }
        // 先建好（或找到）全部触发器再写触发器体：同一功能里的触发器互相运行时，前面的也能引用后面的。
        std::vector<std::optional<GraphRecordRef>> bodies(feature.value->triggers.size());
        for (std::size_t index = 0; index < feature.value->triggers.size(); ++index) {
            const auto& trigger = feature.value->triggers[index];
            const auto what = "触发器 " + trigger.name;
            const TriggerRecord* existing = nullptr;
            std::size_t matches = 0;
            for (const auto& row : doc.triggers) if (row.name.text == trigger.name) { existing = &row; ++matches; }
            GraphRecordRef ref;
            if (matches > 1 || (existing && existing->categoryId != categoryId)) {
                collect(Fail("TRIGGER_FEATURE_TRIGGER_ELSEWHERE", matches > 1 ? "有多个同名触发器。" : "同名触发器在其他类别里；先移动或改名。"),
                    trigger.line, what);
                continue;
            }
            if (!existing) {
                const auto triggerKey = key("trigger", trigger.name, index);
                if (!collect(Apply(GraphCreate{triggerKey, GraphTriggerValue{trigger.name, {}, trigger.enabled, trigger.initiallyOn, trigger.runOnInit, false},
                        ordered(GraphParentKind::CategoryTriggers, {GraphRecordKind::Category, category, {}}), std::nullopt}), trigger.line, what)) continue;
                ref = receipt.created.at(triggerKey);
                if (anchor) {
                    if (!collect(Apply(GraphPlace{ref, *anchor, true}), trigger.line, what)) continue;
                    anchor = ref;
                }
            } else {
                ref = {GraphRecordKind::Trigger, existing->id, {}};
                if (!collect(set(ref, GraphField::Enabled, trigger.enabled), trigger.line, what)
                    || !collect(set(ref, GraphField::InitiallyOn, trigger.initiallyOn), trigger.line, what)
                    || !collect(set(ref, GraphField::RunOnInit, trigger.runOnInit), trigger.line, what)) continue;
            }
            bodies[index] = ref;
        }
        for (std::size_t index = 0; index < feature.value->triggers.size(); ++index) {
            const auto& trigger = feature.value->triggers[index];
            if (bodies[index]) collect(Apply(GraphReplaceTriggerText{*bodies[index], trigger.body}), trigger.line, "触发器 " + trigger.name);
        }
        if (!errors.empty()) return {std::move(errors)};
        return Status::Ok();
    }
    Status Apply(const GraphPatchTriggerText& op) {
        if (op.trigger.kind != GraphRecordKind::Trigger) return Fail("TRIGGER_GRAPH_PARENT_INVALID", "局部替换的目标必须是触发器。");
        const auto id = Resolve(op.trigger); if (!id.Succeeded()) return {id.diagnostics};
        const auto current = RenderTriggerText(doc, catalog, *id.value, limits.maxRequestTextBytes, nullptr, mapObjects);
        if (!current.Succeeded() || !current.value) return {current.diagnostics};
        std::vector<std::string_view> lines;
        for (std::string_view rest = *current.value; !rest.empty();) {
            const auto end = rest.find('\n');
            lines.push_back(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
        if (op.fromLine < 1 || op.fromLine > lines.size() + 1 || op.toLine + 1 < op.fromLine || op.toLine > lines.size())
            return Fail("TRIGGER_TEXT_PATCH_RANGE", "行号范围无效：当前导出共 " + std::to_string(lines.size()) + " 行。");
        std::string actual, composed;
        for (std::size_t line = op.fromLine; line <= op.toLine; ++line) { actual.append(lines[line - 1]); actual += '\n'; }
        auto expected = op.expected;
        if (!expected.empty() && expected.back() != '\n') expected += '\n';
        if (expected != actual)
            return Fail("TRIGGER_TEXT_PATCH_STALE", "第 " + std::to_string(op.fromLine) + "–" + std::to_string(op.toLine)
                + " 行与 expected 不同，未修改；请重新读取 trigger_text。当前内容：\n" + actual.substr(0, 4096));
        for (std::size_t line = 1; line < op.fromLine; ++line) { composed.append(lines[line - 1]); composed += '\n'; }
        composed += op.text;
        if (!op.text.empty() && op.text.back() != '\n') composed += '\n';
        for (std::size_t line = op.toLine + 1; line <= lines.size(); ++line) { composed.append(lines[line - 1]); composed += '\n'; }
        return Apply(GraphReplaceTriggerText{{GraphRecordKind::Trigger, *id.value, {}}, std::move(composed)});
    }
    Status Apply(const GraphReplaceArgument& op) {
        if (op.target.kind != GraphRecordKind::Argument)
            return Fail("TRIGGER_GRAPH_ARGUMENT_TARGET_INVALID", "原位替换目标必须是已有参数槽。");
        const auto id = Resolve(op.target); if (!id.Succeeded()) return {id.diagnostics};
        const auto inspected = detail::InspectArgumentReplacement(doc, catalog, *id.value, limits, mapObjects);
        if (!inspected.Succeeded()) return {inspected.diagnostics};
        if (inspected.value->guard != op.expected)
            return Fail("TRIGGER_GRAPH_ARGUMENT_STALE", "参数挂载、实际类型、目录签名或完整子树原字节身份已变化。");
        // 替换值来源，不重建父 ECA；未编辑的兄弟脚本槽不进入 typedEcas/typedArguments。
        const auto ownerId = inspected.value->guard.ownerEcaId;
        const auto* owner = Find(doc.ecaNodes, ownerId);
        const auto sourceId = inspected.value->guard.sourceTriggerId;
        const auto localsBefore = types.CollectLocals(doc, *Find(doc.triggers, sourceId));
        std::vector<StableId> siblings;
        std::vector<std::string> siblingTypes;
        for (std::size_t ordinal = 0; ordinal < owner->arguments.size(); ++ordinal) {
            siblings.push_back(doc.argumentNodes[owner->arguments[ordinal]].id);
            siblingTypes.push_back(types.ExpectedType(doc, *owner, ordinal, localsBefore));
        }
        GraphArgumentReplacementReceipt replacement;
        replacement.argumentId = *id.value;
        replacement.beforeSubtreeSha256 = inspected.value->guard.subtreeSha256;
        if (Find(doc.argumentNodes, *id.value)->callEca) {
            const auto detached = TriggerDocumentEditor::DetachArgumentCall(doc, *id.value);
            if (!detached.Succeeded()) return {detached.diagnostics};
            replacement.removedEcas = detached.value->removedEcas.size();
            replacement.removedArguments = detached.value->removedArguments.size();
        }
        if (replacement.removedEcas != inspected.value->removedEcas
            || replacement.removedArguments != inspected.value->removedArguments)
            return Fail("TRIGGER_GRAPH_ARGUMENT_CLOSURE_INVALID", "摘除节点数量与已验证参数闭包不符。");
        if (auto status = ConfigureArgument(*id.value, op.value, inspected.value->guard.expectedType, 0); !status.Succeeded())
            return status;
        owner = Find(doc.ecaNodes, ownerId);
        if (!owner || owner->arguments.size() != siblings.size())
            return Fail("TRIGGER_GRAPH_ARGUMENT_OWNER_INVALID", "原位替换不得改变父 ECA 参数列表。");
        const auto localsAfter = types.CollectLocals(doc, *Find(doc.triggers, sourceId));
        for (std::size_t ordinal = 0; ordinal < siblings.size(); ++ordinal) {
            if (doc.argumentNodes[owner->arguments[ordinal]].id != siblings[ordinal])
                return Fail("TRIGGER_GRAPH_ARGUMENT_OWNER_INVALID", "原位替换不得改动兄弟参数身份或顺序。");
            if (ordinal != inspected.value->guard.argumentOrdinal
                && types.ExpectedType(doc, *owner, ordinal, localsAfter) != siblingTypes[ordinal])
                return Fail("TRIGGER_GRAPH_ARGUMENT_DEPENDENT_TYPE_CHANGED", "本次来源替换改变了未编辑兄弟参数的动态类型，不能静默重解释原树。");
        }
        const auto after = detail::FingerprintArgumentSubtree(doc, catalog, *id.value, limits, mapObjects);
        if (!after.Succeeded()) return {after.diagnostics};
        replacement.afterSubtreeSha256 = *after.value;
        receipt.argumentReplacements.push_back(std::move(replacement));
        return Status::Ok();
    }
    Status TypedArgument(const ArgumentNode& argument, const std::string& expected, StableId trigger,
        std::string_view function = {}) {
        if (expected.empty() || expected == "Null") return Fail("TRIGGER_GRAPH_TYPE_UNRESOLVED", "期望类型不确定，不能将未知当兼容。");
        if (types.LiteralKind(expected) == TriggerTypeSystem::Literal::Script)
            return !argument.indexArgument && ((argument.type == 3 && (CodeValueAllowed(trigger, argument.value.text)
                || CommentText(function, argument.value.text)))
                || (argument.type == 0 && FindPreset(catalog, argument.value.text).has_value())) ? Status::Ok() : CodeRejected();
        if (argument.type == 0) {
            const auto actual = types.PresetType(argument.value.text);
            if (!actual || !types.Accepts(expected, *actual)) return Fail("TRIGGER_GRAPH_TYPE_MISMATCH", "预设缺失或类型不符：" + argument.value.text);
        } else if (argument.type == 1) {
            const auto reference = ResolveVariableReference(doc, argument.value.text, mapObjects);
            if (!reference.Succeeded()) return {reference.diagnostics};
            if (!types.Accepts(expected, reference.value->type) || reference.value->isArray != argument.indexArgument.has_value())
                return Fail("TRIGGER_GRAPH_TYPE_MISMATCH", "变量类型或数组下标资格不符：" + argument.value.text);
        } else if (argument.type == 2 && argument.callEca && *argument.callEca < doc.ecaNodes.size()) {
            const auto& call = doc.ecaNodes[*argument.callEca];
            if (call.type != 3) {
                const auto slot = TriggerTypeSystem::FunctionSlotKind(expected);
                if (!slot || EcaTypeFromUiDefinitionKind(*slot) != call.type)
                    return Fail("TRIGGER_GRAPH_TYPE_MISMATCH", "函数槽种类与参数类型不符：" + call.name.text);
            } else {
                const auto actual = types.ReturnType(doc, call);
                if (actual.empty() || !types.Accepts(expected, actual)) return Fail("TRIGGER_GRAPH_TYPE_MISMATCH", "参数函数返回类型不符。");
            }
        } else if (argument.type == 3) {
            if (expected == "code") return Fail("TRIGGER_GRAPH_CODE_UNSUPPORTED", "代码回调不能由字面源码代替 typed 函数。");
            // 整体替换可原样保留触发器里已有的值（即使不规范），但不能新写无效值。
            if (auto status = types.ValidateLiteral(expected, argument.value.text);
                !status.Succeeded() && !preservedLiterals.contains({trigger, expected, argument.value.text})) return status;
        } else return Fail("TRIGGER_GRAPH_ARGUMENT_KIND", "参数来源结构无效。");
        if (argument.indexArgument) {
            if (*argument.indexArgument >= doc.argumentNodes.size()) return Fail("TRIGGER_GRAPH_INDEX_INVALID", "数组下标索引无效。");
            return TypedArgument(doc.argumentNodes[*argument.indexArgument], "integer", trigger);
        }
        return Status::Ok();
    }
    // 一次列出全部参数类型问题（上限 64 条），紧凑写法的节点带行列。
    // 自身或上级语句被禁用的 ECA（含参数函数）。
    std::vector<char> InactiveEcas() const {
        std::vector<char> inactive(doc.ecaNodes.size(), 0);
        const auto mark = [&](const auto& self, std::uint32_t index, bool off, std::size_t depth) -> void {
            if (index >= doc.ecaNodes.size() || depth > 512) return;
            const auto& node = doc.ecaNodes[index];
            off = off || node.enabled == 0;
            inactive[index] = off ? 1 : 0;
            for (const auto argument : node.arguments)
                if (argument < doc.argumentNodes.size() && doc.argumentNodes[argument].callEca)
                    self(self, *doc.argumentNodes[argument].callEca, off, depth + 1);
            for (const auto child : node.children) self(self, child, off, depth + 1);
        };
        for (const auto& trigger : doc.triggers) for (const auto root : trigger.ecas) mark(mark, root, false, 0);
        return inactive;
    }
    Status ValidateTouched() {
        Status failures;
        std::vector<StableId> safetyScope;
        std::optional<std::vector<char>> inactive;
        std::map<StableId, std::vector<LocalVariable>> replacementLocals;
        for (const auto id : typedVariables) if (const auto* row = Find(doc.variables, id)) {
            auto status = ValidateVariable(*row);
            if (!status.Succeeded()) return status;
            for (auto& diagnostic : status.diagnostics) {
                diagnostic.message += "（variable_id=" + std::to_string(row->id) + "，变量“" + row->name.text + "”初值）";
                failures.diagnostics.push_back(std::move(diagnostic));
            }
        }
        for (const auto& eca : doc.ecaNodes) for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
            const auto& argument = doc.argumentNodes[eca.arguments[i]];
            bool changed = typedEcas.contains(eca.id);
            const auto* definition = catalog.Find(eca.type, eca.name.text);
            if (definition && TriggerTypeSystem::DeclaredType(*definition, i) == "Null") {
                for (std::size_t prior = 0; prior < i; ++prior) {
                    const auto& selector = doc.argumentNodes[eca.arguments[prior]];
                    if (typedArguments.contains(selector.id) && types.IsTypeSelector(TriggerTypeSystem::DeclaredType(*definition, prior))) changed = true;
                    if (selector.type == 1) for (const auto& variable : doc.variables)
                        if (changedVariableTypes.contains(variable.id) && (selector.value.text == variable.name.text || selector.value.text == "udg_" + variable.name.text)) changed = true;
                }
            }
            auto index = eca.arguments[i];
            for (std::size_t depth = 0; depth <= 256 && index < doc.argumentNodes.size(); ++depth) {
                const auto& current = doc.argumentNodes[index];
                changed = changed || typedArguments.contains(current.id) || (current.type == 1 && !typedVariables.empty()
                    && std::any_of(doc.variables.begin(), doc.variables.end(), [&](const auto& row) {
                        return typedVariables.contains(row.id) && (row.name.text == current.value.text || "udg_" + row.name.text == current.value.text);
                    }));
                if (!current.indexArgument) break;
                index = *current.indexArgument;
            }
            if (changed) {
                safetyScope.push_back(argument.id);
                // 禁用语句（含其禁用上级下的语句）不生成脚本，WE 允许其中留着未设置或失效的参数，不做类型门禁。
                if (!inactive) inactive = InactiveEcas();
                if ((*inactive)[static_cast<std::size_t>(&eca - doc.ecaNodes.data())]) continue;
                if (eca.type == 2 && eca.name.text == "SetVariable" && !eca.arguments.empty()) {
                    const auto& target = doc.argumentNodes[eca.arguments[0]];
                    if (auto status = types.ValidateWritableVariable(doc, target, types.ValueType(doc, target)); !status.Succeeded()) return status;
                }
                if (i == 0 && eca.type == 2 && (eca.name.text == "ForLoopVar" || eca.name.text == "ForLoopVarMultiple")) {
                    if (auto status = types.ValidateWritableVariable(doc, argument, "integer"); !status.Succeeded()) return status;
                }
                auto expected = types.ExpectedType(doc, eca, i);
                if (std::any_of(receipt.argumentReplacements.begin(), receipt.argumentReplacements.end(),
                        [&](const auto& replacement) { return replacement.argumentId == argument.id; })) {
                    auto found = replacementLocals.find(eca.sourceTriggerId);
                    if (found == replacementLocals.end()) {
                        const auto* trigger = Find(doc.triggers, eca.sourceTriggerId);
                        if (!trigger) return Fail("TRIGGER_GRAPH_ARGUMENT_OWNER_INVALID", "参数替换的触发器上下文已不存在。");
                        found = replacementLocals.emplace(eca.sourceTriggerId, types.CollectLocals(doc, *trigger)).first;
                    }
                    expected = types.ExpectedType(doc, eca, i, found->second);
                }
                auto status = argument.type == -1
                    ? Fail("TRIGGER_GRAPH_ARGUMENT_UNSET", "参数未设置（?）只能留在禁用的语句里。")
                    : TypedArgument(argument, expected, eca.sourceTriggerId, eca.name.text);
                if (!status.diagnostics.empty()) {
                    for (auto& diagnostic : status.diagnostics) diagnostic.message += "（trigger_id=" + std::to_string(eca.sourceTriggerId)
                        + "，eca_id=" + std::to_string(eca.id) + "，argument_id=" + std::to_string(argument.id) + "）";
                    if (const auto argumentOrigin = origins.find(argument.id); argumentOrigin != origins.end())
                        Locate(status, argumentOrigin->second);
                    else if (const auto found = origins.find(eca.id); found != origins.end())
                        Locate(status, found->second + " 第 " + std::to_string(i + 1) + " 个参数");
                    for (auto& diagnostic : status.diagnostics) failures.diagnostics.push_back(std::move(diagnostic));
                    if (failures.diagnostics.size() >= 64) return failures;
                }
            }
        }
        if (!safetyScope.empty()) {
            auto safety = AnalyzeTriggerValues(doc, catalog, safetyScope, limits.storageCapabilities.get());
            if (!safety.Succeeded()) return Status{safety.diagnostics};
            if (!limits.storageCapabilities && limits.storageCapabilityProvider) {
                std::set<std::string> required;
                for (const auto& issue : safety.value->issues) if (issue.enabled && !issue.storageType.empty()) required.insert(issue.storageType);
                if (!required.empty()) {
                    const std::vector<std::string> names(required.begin(), required.end());
                    const auto captured = limits.storageCapabilityProvider(names);
                    if (!captured.Succeeded()) return Status{captured.diagnostics};
                    safety = AnalyzeTriggerValues(doc, catalog, safetyScope, captured.value->get());
                    if (!safety.Succeeded()) return Status{safety.diagnostics};
                }
            }
            std::map<StableId, std::vector<TriggerTextArgumentAnchor>> renderedPositions;
            for (auto& issue : safety.value->issues) {
                Status located{{std::move(issue.diagnostic)}};
                if (const auto found = origins.find(issue.argumentId); found != origins.end()) Locate(located, found->second);
                else if (const auto ecaOrigin = origins.find(issue.ecaId); ecaOrigin != origins.end())
                    Locate(located, ecaOrigin->second + " 第 " + std::to_string(issue.position + 1) + " 个参数");
                else {
                    auto positions = renderedPositions.find(issue.triggerId);
                    if (positions == renderedPositions.end()) {
                        std::vector<TriggerTextArgumentAnchor> anchors;
                        const auto rendered = RenderTriggerText(doc, catalog, issue.triggerId, limits.maxRequestTextBytes, nullptr, mapObjects, &anchors);
                        if (!rendered.Succeeded()) return Fail("TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE", "不能将参数诊断定位回当前T文本，请先补齐该触发器的可读上下文。");
                        positions = renderedPositions.emplace(issue.triggerId, std::move(anchors)).first;
                    }
                    const auto anchor = std::find_if(positions->second.begin(), positions->second.end(), [&](const auto& row) { return row.argumentId == issue.argumentId; });
                    if (anchor == positions->second.end()) return Fail("TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE", "当前T文本没有此参数的精确位置。");
                    Locate(located, "当前T第 " + std::to_string(anchor->line) + " 行第 " + std::to_string(anchor->column) + " 列");
                }
                failures.diagnostics.push_back(std::move(located.diagnostics.front()));
            }
        }
        return failures;
    }
};
} // namespace

Result<TriggerGraphBinding> CaptureTriggerGraphBinding(const TriggerDocument& document, const UiCatalog& catalog,
    const MapObjectReferenceSnapshot* mapObjects) {
    if (!document.HasTrustedSourceSeal() || document.UiCatalogSha256() != catalog.Sha256())
        return Result<TriggerGraphBinding>::Failure(kDomain, "TRIGGER_GRAPH_SOURCE_UNTRUSTED", "图编辑需要真实 codec 来源 seal 和原 Catalog 身份。");
    const auto encoded = WtgWctCodec{}.Serialize(document, catalog);
    if (!encoded.Succeeded()) return Failed<TriggerGraphBinding>(encoded);
    const auto wtg = Sha256(encoded.value->wtg), wct = Sha256(encoded.value->wct);
    if (!wtg.Succeeded()) return Failed<TriggerGraphBinding>(wtg);
    if (!wct.Succeeded()) return Failed<TriggerGraphBinding>(wct);
    return Result<TriggerGraphBinding>::Success({std::string(document.SourceWtgSha256()), std::string(document.SourceWctSha256()),
        catalog.Sha256(), *wtg.value, *wct.value, mapObjects ? mapObjects->Identity() : std::string{}});
}

namespace {
Status CheckGraphBatchRequest(const TriggerGraphEditBatch& batch, const TriggerGraphEditLimits& limits) {
    if (batch.operations.empty() || batch.operations.size() > limits.maxOperations || limits.maxDepth == 0 || limits.maxDepth > 128)
        return Fail("TRIGGER_GRAPH_BUDGET", "批次为空、操作预算不足或递归预算不安全。");
    RequestBudget budget{limits};
    for (const auto& op : batch.operations) if (!budget.Operation(op))
        return Fail("TRIGGER_GRAPH_BUDGET", "请求文本、递归或新增记录超过预算，或包含 NUL/空子节点。");
    return Status::Ok();
}
struct GraphCodeProtection {
    struct Custom { StableId id; std::int32_t flag; RawText code; };
    RawText code, comment;
    std::vector<Custom> customs;
};
GraphCodeProtection CaptureGraphCodeProtection(const TriggerDocument& source) {
    GraphCodeProtection result{source.wct.customCode, source.wct.customComment, {}};
    for (const auto& old : source.triggers) if (old.customTextFlag)
        result.customs.push_back({old.id, old.customTextFlag, old.customText});
    return result;
}
Status CheckGraphCodeProtection(const GraphCodeProtection& source, TriggerDocument& candidate) {
    if (!SameRaw(source.code, candidate.wct.customCode) || !SameRaw(source.comment, candidate.wct.customComment))
        return Fail("TRIGGER_GRAPH_CODE_CHANGED", "批次不得修改全局代码或 WCT 头部。");
    for (const auto& old : source.customs) {
        const auto* now = Find(candidate.triggers, old.id);
        if (!now || now->customTextFlag != old.flag || !SameRaw(old.code, now->customText))
            return Fail("TRIGGER_GRAPH_CODE_CHANGED", "既有自定义代码必须原样保留。");
    }
    return Status::Ok();
}
// 唯一 private-candidate 执行/类型/codec/代码保护路径；调用者负责来源和原子发布。
Result<TriggerGraphEditReceipt> ApplyPrivateGraphCandidate(const GraphCodeProtection& source,
    TriggerDocument& candidate, const UiCatalog& catalog, const TriggerGraphEditBatch& batch,
    const TriggerGraphBinding& before, const TriggerGraphEditLimits& limits,
    const MapObjectReferenceSnapshot* mapObjects) {
    Editor editor{candidate, catalog, TriggerTypeSystem(catalog, mapObjects), limits, mapObjects, {}, {}, {}, {}, detail::NextGeneratedCounter(candidate)};
    editor.receipt.before = before;
    std::size_t added = 0;
    for (const auto& op : batch.operations) {
        const auto oldCount = RecordCount(candidate);
        const auto status = std::visit([&](const auto& value) { return editor.Apply(value); }, op);
        if (!status.Succeeded()) return Failed<TriggerGraphEditReceipt>(status);
        const auto newCount = RecordCount(candidate);
        if (newCount > oldCount) {
            if (newCount - oldCount > limits.maxAddedRecords - added)
                return Result<TriggerGraphEditReceipt>::Failure(kDomain, "TRIGGER_GRAPH_BUDGET", "实际新增图（含 Catalog 默认函数）超过工作预算。");
            added += newCount - oldCount;
        }
        ++editor.receipt.appliedOperations;
    }
    if (const auto valid = WtgWctCodec{}.Validate(candidate, catalog); !valid.Succeeded()) return Failed<TriggerGraphEditReceipt>(valid);
    auto valueSafety = editor.ValidateTouched();
    if (!valueSafety.Succeeded()) return Failed<TriggerGraphEditReceipt>(valueSafety);
    // ordinary T 不应产生 WCT 变化；对全部既有代码槽再次逐字节复核。
    if (!editor.receipt.codeIdentityChanges) {
        if (const auto valid = CheckGraphCodeProtection(source, candidate); !valid.Succeeded()) return Failed<TriggerGraphEditReceipt>(valid);
    } else if (const auto valid = candidate.VerifyCodeIdentityAuthorization(candidate); !valid.Succeeded())
        return Failed<TriggerGraphEditReceipt>(valid);
    auto result = Result<TriggerGraphEditReceipt>::Success(std::move(editor.receipt));
    result.diagnostics = std::move(valueSafety.diagnostics);
    return result;
}
} // namespace

Result<TriggerGraphEditReceipt> ApplyTriggerGraphEditBatch(TriggerDocument& document, const UiCatalog& catalog,
    const TriggerGraphEditBatch& batch, const TriggerGraphEditLimits& limits, const MapObjectReferenceSnapshot* mapObjects) {
    if (const auto request = CheckGraphBatchRequest(batch, limits); !request.Succeeded()) return Failed<TriggerGraphEditReceipt>(request);
    const auto before = CaptureTriggerGraphBinding(document, catalog, mapObjects); if (!before.Succeeded()) return Failed<TriggerGraphEditReceipt>(before);
    if (!SameBinding(*before.value, batch.expected)) return Result<TriggerGraphEditReceipt>::Failure(kDomain,
        "TRIGGER_GRAPH_SOURCE_STALE", "原来源、Catalog、当前 overlay 的 WTG/WCT 或预放对象身份已变化。");
    TriggerDocument candidate = document;
    const auto protectedCode = CaptureGraphCodeProtection(document);
    auto edited = ApplyPrivateGraphCandidate(protectedCode, candidate, catalog, batch, *before.value, limits, mapObjects);
    if (!edited.Succeeded()) return edited;
    const auto after = CaptureTriggerGraphBinding(candidate, catalog, mapObjects); if (!after.Succeeded()) return Failed<TriggerGraphEditReceipt>(after);
    edited.value->after = *after.value;
    document = std::move(candidate);
    return edited;
}

#include "TriggerGraphScalarReplay.h"

Result<DeclarationRemovalReview> QueryDeclarationRemovalAudit(const TriggerDocument& document, const UiCatalog& catalog,
    const TriggerGraphEditBatch& batch, std::size_t operationIndex, const TriggerGraphEditLimits& limits,
    const MapObjectReferenceSnapshot* objects) {
    using Out = Result<DeclarationRemovalReview>;
    if (const auto status = CheckGraphBatchRequest(batch, limits); !status.Succeeded()) return Failed<DeclarationRemovalReview>(status);
    if (operationIndex >= batch.operations.size() || !std::holds_alternative<GraphRemove>(batch.operations[operationIndex]))
        return Out::Failure(kDomain, "TRIGGER_GRAPH_REMOVE_UNSUPPORTED", "operation_index必须指向remove操作。");
    const auto binding = CaptureTriggerGraphBinding(document, catalog, objects);
    if (!binding.Succeeded()) return Failed<DeclarationRemovalReview>(binding);
    if (!SameBinding(*binding.value, batch.expected)) return Out::Failure(kDomain, "TRIGGER_GRAPH_SOURCE_STALE", "删除取证的来源修订已变化。");
    auto candidate = document;
    auto operation = std::get<GraphRemove>(batch.operations[operationIndex]);
    if (operationIndex) {
        auto prefix = batch; prefix.operations.resize(operationIndex);
        const auto applied = ApplyTriggerGraphEditBatch(candidate, catalog, prefix, limits, objects);
        if (!applied.Succeeded()) return Failed<DeclarationRemovalReview>(applied);
        if (!operation.target.clientKey.empty()) {
            const auto found = applied.value->created.find(operation.target.clientKey);
            if (found == applied.value->created.end()) return Out::Failure(kDomain, "TRIGGER_GRAPH_TARGET_MISSING", "删除目标client_key未由前序操作创建。");
            operation.target = found->second;
        }
    }
    return QueryDeclarationRemovalAudit(candidate, catalog, operation, objects);
}

} // namespace War3Map::Trigger
