#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDataText.h"

#include "War3MapCore/Trigger/TriggerTypes.h"

#include "TriggerIdAllocation.h"
#include "TriggerCodeAuditBlocks.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Trigger {
namespace {

using detail::AllocateId;
using detail::NewTextEncoding;
using detail::NextGeneratedCounter;

constexpr std::size_t kMaximumDefaultDepth = 32;

std::optional<std::string_view> Field(
    const UiDefinitionRevision& revision,
    std::string_view name) {
    const auto found = std::find_if(
        revision.fields.begin(),
        revision.fields.end(),
        [name](const auto& field) { return field.first == name; });
    return found == revision.fields.end()
        ? std::nullopt
        : std::optional<std::string_view>(found->second);
}

struct Builder {
    TriggerDocument& document;
    const UiCatalog& catalog;
    StableId triggerId;
    TextEncoding encoding;
    std::uint64_t counter;

    Result<std::uint32_t> Build(
        const UiDefinition& definition,
        std::size_t depth) {
        if (depth > kMaximumDefaultDepth) {
            return Result<std::uint32_t>::Failure(
                "Trigger.Edit",
                "TRIGGER_DEFAULT_RECURSION",
                "UI 默认函数递归超过安全上限，已拒绝创建节点。");
        }
        if (document.ecaNodes.size() >= kInvalidNodeIndex) {
            return Result<std::uint32_t>::Failure(
                "Trigger.Edit", "TRIGGER_ECA_LIMIT", "ECA 数量超过格式上限。");
        }

        const std::uint32_t nodeIndex =
            static_cast<std::uint32_t>(document.ecaNodes.size());
        document.ecaNodes.emplace_back();
        EcaNode node;
        node.id = AllocateId(counter);
        if (node.id == 0) {
            return Result<std::uint32_t>::Failure(
                "Trigger.Edit", "TRIGGER_ID_EXHAUSTED", "生成节点 ID 空间已耗尽。");
        }
        node.sourceTriggerId = triggerId;
        switch (definition.kind) {
        case UiDefinitionKind::Event: node.type = 0; break;
        case UiDefinitionKind::Condition: node.type = 1; break;
        case UiDefinitionKind::Action: node.type = 2; break;
        case UiDefinitionKind::Call: node.type = 3; break;
        default:
            return Result<std::uint32_t>::Failure(
                "Trigger.Edit", "TRIGGER_DEFINITION_KIND",
                "该 UI 定义不能创建 ECA 节点。");
        }
        node.name = RawText{definition.name, encoding, {}};
        node.enabled = 1;
        node.uiSource = definition.Active().source;
        node.uiLayer = definition.Active().layer;
        node.uiSignature = definition.Signature();

        for (const UiArgumentDefinition& argumentDefinition : definition.Active().arguments) {
            if (argumentDefinition.type == "nothing") {
                continue;
            }
            if (document.argumentNodes.size() >= kInvalidNodeIndex) {
                return Result<std::uint32_t>::Failure(
                    "Trigger.Edit", "TRIGGER_ARGUMENT_LIMIT", "参数数量超过格式上限。");
            }
            const std::uint32_t argumentIndex =
                static_cast<std::uint32_t>(document.argumentNodes.size());
            document.argumentNodes.emplace_back();
            ArgumentNode argument;
            argument.id = AllocateId(counter);
            argument.value = RawText{argumentDefinition.defaultValue, encoding, {}};

            const UiDefinition* defaultCall = argumentDefinition.defaultValue.empty()
                ? nullptr
                : catalog.Find(UiDefinitionKind::Call, argumentDefinition.defaultValue);
            if (defaultCall != nullptr) {
                auto call = Build(*defaultCall, depth + 1);
                if (!call.Succeeded()) {
                    return call;
                }
                argument.type = 2;
                argument.insertCall = 1;
                argument.callEca = *call.value;
            } else if (!argumentDefinition.defaultValue.empty() &&
                catalog.Find(
                    UiDefinitionKind::Define,
                    argumentDefinition.defaultValue,
                    "TriggerParams") != nullptr) {
                argument.type = 0;
            } else {
                argument.type = 3;
            }
            document.argumentNodes[argumentIndex] = std::move(argument);
            node.arguments.push_back(argumentIndex);
        }
        document.ecaNodes[nodeIndex] = std::move(node);
        return Result<std::uint32_t>::Success(nodeIndex);
    }
};

Status ValidateTail(
    const TriggerDocument& document,
    const EcaAppendReceipt& receipt,
    bool expectPresent) {
    const std::size_t expectedEcas = receipt.previousEcaCount +
        (expectPresent ? receipt.appendedEcas.size() : 0);
    const std::size_t expectedArguments = receipt.previousArgumentCount +
        (expectPresent ? receipt.appendedArguments.size() : 0);
    if (document.ecaNodes.size() != expectedEcas ||
        document.argumentNodes.size() != expectedArguments) {
        return Status::Failure(
            "Trigger.Edit",
            "TRIGGER_UNDO_ORDER_CONFLICT",
            "撤销/重做顺序与触发器图尾部不一致，已拒绝修改。");
    }
    return Status{};
}

} // namespace

Result<EcaAppendReceipt> TriggerDocumentEditor::AppendRootEca(
    TriggerDocument& document,
    const UiCatalog& catalog,
    StableId triggerId,
    UiDefinitionKind kind,
    std::string_view functionName,
    std::optional<std::size_t> insertRow) {
    const UiDefinition* definition = catalog.Find(kind, functionName);
    if (definition == nullptr) {
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_UI_DEFINITION_MISSING",
            "当前 UI Catalog 不存在所选函数：" + std::string(functionName));
    }
    auto trigger = std::find_if(
        document.triggers.begin(), document.triggers.end(),
        [triggerId](const TriggerRecord& value) { return value.id == triggerId; });
    if (trigger == document.triggers.end()) {
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_OWNER_MISSING", "找不到目标触发器。");
    }
    if (trigger->customTextFlag != 0) {
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_CUSTOM_TEXT_READ_ONLY",
            "自定义脚本触发器不能插入 GUI ECA。");
    }

    EcaAppendReceipt receipt;
    receipt.placement = EcaAppendPlacement::TriggerRoot;
    receipt.triggerId = triggerId;
    const std::size_t row = insertRow.has_value()
        ? std::min(*insertRow, trigger->ecas.size())
        : trigger->ecas.size();
    receipt.row = row;
    receipt.previousEcaCount = document.ecaNodes.size();
    receipt.previousArgumentCount = document.argumentNodes.size();
    Builder builder{
        document,
        catalog,
        triggerId,
        NewTextEncoding(document),
        NextGeneratedCounter(document),
    };
    auto built = builder.Build(*definition, 0);
    if (!built.Succeeded()) {
        document.ecaNodes.resize(receipt.previousEcaCount);
        document.argumentNodes.resize(receipt.previousArgumentCount);
        Result<EcaAppendReceipt> result;
        result.diagnostics = std::move(built.diagnostics);
        return result;
    }
    trigger = std::find_if(
        document.triggers.begin(), document.triggers.end(),
        [triggerId](const TriggerRecord& value) { return value.id == triggerId; });
    trigger->ecas.insert(
        trigger->ecas.begin() + static_cast<std::ptrdiff_t>(row), *built.value);
    receipt.rootEcaId = document.ecaNodes[*built.value].id;
    receipt.appendedEcas.assign(
        document.ecaNodes.begin() + static_cast<std::ptrdiff_t>(receipt.previousEcaCount),
        document.ecaNodes.end());
    receipt.appendedArguments.assign(
        document.argumentNodes.begin() + static_cast<std::ptrdiff_t>(receipt.previousArgumentCount),
        document.argumentNodes.end());
    return Result<EcaAppendReceipt>::Success(std::move(receipt));
}

Result<EcaAppendReceipt> TriggerDocumentEditor::AppendArgumentCall(
    TriggerDocument& document,
    const UiCatalog& catalog,
    StableId argumentId,
    std::string_view expectedReturnType,
    std::string_view functionName,
    std::optional<UiDefinitionKind> kind) {
    ArgumentNode* target = FindArgument(document, argumentId);
    if (target == nullptr) {
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_ARGUMENT_MISSING", "找不到目标参数。");
    }
    if (target->insertCall != 0 || target->callEca.has_value()) {
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_CALL_REPLACE_UNSUPPORTED",
            "该参数已经包含函数；请进入现有函数编辑其参数。");
    }
    // 函数槽（如 AddTriggerEvent 的 eventcall）存的是事件/条件/动作 ECA，与 WE 相同；
    // 其余参数只接受按返回类型匹配的 Call。
    const auto slot = TriggerTypeSystem::FunctionSlotKind(expectedReturnType);
    const UiDefinitionKind chosen = kind.value_or(
        slot && catalog.Find(*slot, functionName) != nullptr ? *slot : UiDefinitionKind::Call);
    if (chosen != UiDefinitionKind::Call && chosen != slot) {
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_CALL_SLOT_MISMATCH",
            slot ? "参数类型 " + std::string(expectedReturnType) + " 只接受" + std::string(ToString(*slot))
                    + " 函数或返回该类型的 call。"
                 : "参数类型 " + std::string(expectedReturnType) + " 不是函数槽，只接受返回该类型的 call。");
    }
    const UiDefinition* definition = catalog.Find(chosen, functionName);
    if (definition == nullptr) {
        if (chosen == UiDefinitionKind::Call && slot && catalog.Find(*slot, functionName) != nullptr) {
            return Result<EcaAppendReceipt>::Failure(
                "Trigger.Edit", "TRIGGER_CALL_SLOT_MISMATCH",
                std::string(functionName) + " 是" + std::string(ToString(*slot)) + " 函数；此参数槽请按 "
                    + std::string(ToString(*slot)) + " 种类填写。");
        }
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_CALL_NOT_CREATABLE",
            "所选函数在当前 UI Catalog 中不存在：" + std::string(ToString(chosen)) + " " + std::string(functionName));
    }
    const auto returnType = Field(definition->Active(), "returns");
    if (chosen == UiDefinitionKind::Call && !expectedReturnType.empty() &&
        (!returnType.has_value() ||
            !TriggerTypeSystem(catalog).Accepts(expectedReturnType, *returnType))) {
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_CALL_TYPE_MISMATCH",
            "所选函数返回类型与参数要求不一致。");
    }

    bool indexOwner = false;
    const auto owner = std::find_if(
        document.ecaNodes.begin(), document.ecaNodes.end(),
        [argumentId, &document, &indexOwner](const EcaNode& eca) {
            return std::any_of(eca.arguments.begin(), eca.arguments.end(),
                [argumentId, &document, &indexOwner](std::uint32_t index) {
                    // 数组下标仍属于同一 ECA；有界沿 indexArgument 找真实 owner，
                    // 不通过改挂载关系临时冒充直接参数。
                    for (std::size_t depth = 0; depth <= 256 && index < document.argumentNodes.size(); ++depth) {
                        const auto& argument = document.argumentNodes[index];
                        if (argument.id == argumentId) { indexOwner = depth != 0; return true; }
                        if (!argument.indexArgument) break;
                        index = *argument.indexArgument;
                    }
                    return false;
                });
        });
    if (owner == document.ecaNodes.end()) {
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_ARGUMENT_OWNER_MISSING",
            "目标参数没有可验证的 ECA 所有者。");
    }
    if (indexOwner && expectedReturnType != "integer") {
        return Result<EcaAppendReceipt>::Failure(
            "Trigger.Edit", "TRIGGER_CALL_TYPE_MISMATCH", "数组下标函数必须显式绑定 integer 返回类型。");
    }

    EcaAppendReceipt receipt;
    receipt.placement = EcaAppendPlacement::ArgumentCall;
    receipt.triggerId = owner->sourceTriggerId;
    receipt.argumentId = argumentId;
    receipt.previousEcaCount = document.ecaNodes.size();
    receipt.previousArgumentCount = document.argumentNodes.size();
    receipt.argumentBefore = *target;
    Builder builder{
        document,
        catalog,
        owner->sourceTriggerId,
        target->value.encoding,
        NextGeneratedCounter(document),
    };
    auto built = builder.Build(*definition, 0);
    if (!built.Succeeded()) {
        document.ecaNodes.resize(receipt.previousEcaCount);
        document.argumentNodes.resize(receipt.previousArgumentCount);
        Result<EcaAppendReceipt> result;
        result.diagnostics = std::move(built.diagnostics);
        return result;
    }
    target = FindArgument(document, argumentId);
    target->type = 2;
    target->value.text = definition->name;
    target->value.originalBytes.clear();
    target->insertCall = 1;
    target->callEca = *built.value;
    receipt.argumentAfter = *target;
    receipt.rootEcaId = document.ecaNodes[*built.value].id;
    receipt.appendedEcas.assign(
        document.ecaNodes.begin() + static_cast<std::ptrdiff_t>(receipt.previousEcaCount),
        document.ecaNodes.end());
    receipt.appendedArguments.assign(
        document.argumentNodes.begin() + static_cast<std::ptrdiff_t>(receipt.previousArgumentCount),
        document.argumentNodes.end());
    return Result<EcaAppendReceipt>::Success(std::move(receipt));
}

Status TriggerDocumentEditor::UndoAppend(
    TriggerDocument& document,
    const EcaAppendReceipt& receipt) {
    Status tail = ValidateTail(document, receipt, true);
    if (!tail.Succeeded()) {
        return tail;
    }
    if (receipt.placement == EcaAppendPlacement::TriggerRoot) {
        auto trigger = std::find_if(
            document.triggers.begin(), document.triggers.end(),
            [&](const TriggerRecord& value) { return value.id == receipt.triggerId; });
        if (trigger == document.triggers.end() || receipt.row >= trigger->ecas.size() ||
            document.ecaNodes[trigger->ecas[receipt.row]].id != receipt.rootEcaId) {
            return Status::Failure(
                "Trigger.Edit", "TRIGGER_UNDO_ATTACHMENT_MISSING",
                "找不到待撤销的新建 ECA 附着点。");
        }
        trigger->ecas.erase(trigger->ecas.begin() + static_cast<std::ptrdiff_t>(receipt.row));
    } else {
        ArgumentNode* target = FindArgument(document, receipt.argumentId);
        if (target == nullptr || !receipt.argumentBefore.has_value()) {
            return Status::Failure(
                "Trigger.Edit", "TRIGGER_UNDO_ARGUMENT_MISSING",
                "找不到待撤销的函数参数。");
        }
        *target = *receipt.argumentBefore;
    }
    document.ecaNodes.resize(receipt.previousEcaCount);
    document.argumentNodes.resize(receipt.previousArgumentCount);
    return Status{};
}

Status TriggerDocumentEditor::RedoAppend(
    TriggerDocument& document,
    const EcaAppendReceipt& receipt) {
    Status tail = ValidateTail(document, receipt, false);
    if (!tail.Succeeded()) {
        return tail;
    }
    if (receipt.placement == EcaAppendPlacement::TriggerRoot) {
        auto trigger = std::find_if(
            document.triggers.begin(), document.triggers.end(),
            [&](const TriggerRecord& value) { return value.id == receipt.triggerId; });
        if (trigger == document.triggers.end() || receipt.row > trigger->ecas.size()) {
            return Status::Failure(
                "Trigger.Edit", "TRIGGER_REDO_ATTACHMENT_MISSING",
                "找不到待重做的新建 ECA 附着点。");
        }
    } else {
        ArgumentNode* target = FindArgument(document, receipt.argumentId);
        if (target == nullptr || !receipt.argumentAfter.has_value()) {
            return Status::Failure(
                "Trigger.Edit", "TRIGGER_REDO_ARGUMENT_MISSING",
                "找不到待重做的函数参数。");
        }
    }

    document.ecaNodes.insert(
        document.ecaNodes.end(), receipt.appendedEcas.begin(), receipt.appendedEcas.end());
    document.argumentNodes.insert(
        document.argumentNodes.end(),
        receipt.appendedArguments.begin(),
        receipt.appendedArguments.end());
    if (receipt.placement == EcaAppendPlacement::TriggerRoot) {
        auto trigger = std::find_if(
            document.triggers.begin(), document.triggers.end(),
            [&](const TriggerRecord& value) { return value.id == receipt.triggerId; });
        trigger->ecas.insert(
            trigger->ecas.begin() + static_cast<std::ptrdiff_t>(receipt.row),
            static_cast<std::uint32_t>(receipt.previousEcaCount));
    } else {
        ArgumentNode* target = FindArgument(document, receipt.argumentId);
        *target = *receipt.argumentAfter;
    }
    return Status{};
}

Status TriggerDocumentEditor::SetScalarArgument(
    TriggerDocument& document,
    StableId argumentId,
    std::int32_t valueKind,
    std::string value) {
    ArgumentNode* argument = FindArgument(document, argumentId);
    if (argument == nullptr) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_ARGUMENT_MISSING", "找不到目标参数。");
    }
    if (argument->insertCall != 0 || argument->callEca.has_value() ||
        argument->insertIndex != 0 || argument->indexArgument.has_value()) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_COMPLEX_ARGUMENT_READ_ONLY",
            "嵌套函数或数组索引参数需要进入对应节点编辑，不能改写为标量。");
    }
    // -1 是 WE 的“参数未设置”，只能是空值。
    if ((valueKind != 0 && valueKind != 1 && valueKind != 3 && valueKind != -1) || (valueKind == -1 && !value.empty())) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_ARGUMENT_KIND_INVALID",
            "标量参数类型只能是预设、变量、直接值或未设置。");
    }
    if (value.find('\0') != std::string::npos) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_ARGUMENT_NUL",
            "参数文本不能包含 NUL 字符。");
    }
    argument->type = valueKind;
    argument->value.text = std::move(value);
    argument->value.originalBytes.clear();
    return Status{};
}

EcaNode* TriggerDocumentEditor::FindEca(
    TriggerDocument& document,
    StableId ecaId) noexcept {
    const auto found = std::find_if(
        document.ecaNodes.begin(), document.ecaNodes.end(),
        [ecaId](const EcaNode& value) { return value.id == ecaId; });
    return found == document.ecaNodes.end() ? nullptr : &*found;
}

ArgumentNode* TriggerDocumentEditor::FindArgument(
    TriggerDocument& document,
    StableId argumentId) noexcept {
    const auto found = std::find_if(
        document.argumentNodes.begin(), document.argumentNodes.end(),
        [argumentId](const ArgumentNode& value) { return value.id == argumentId; });
    return found == document.argumentNodes.end() ? nullptr : &*found;
}

TriggerRecord* TriggerDocumentEditor::FindTrigger(
    TriggerDocument& document,
    StableId triggerId) noexcept {
    const auto found = std::find_if(
        document.triggers.begin(), document.triggers.end(),
        [triggerId](const TriggerRecord& value) { return value.id == triggerId; });
    return found == document.triggers.end() ? nullptr : &*found;
}

Status TriggerDocumentEditor::SetTriggerEnabled(
    TriggerDocument& document,
    StableId triggerId,
    bool enabled) {
    TriggerRecord* trigger = FindTrigger(document, triggerId);
    if (trigger == nullptr) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_MISSING", "找不到目标触发器。");
    }
    trigger->enabled = enabled ? 1 : 0;
    return Status{};
}

Status TriggerDocumentEditor::SetTriggerInitiallyOn(
    TriggerDocument& document,
    StableId triggerId,
    bool initiallyOn) {
    TriggerRecord* trigger = FindTrigger(document, triggerId);
    if (trigger == nullptr) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_MISSING", "找不到目标触发器。");
    }
    // WE “初始开启”与存储字段 initiallyClosed 互为反值。
    trigger->initiallyClosed = initiallyOn ? 0 : 1;
    return Status{};
}

Status TriggerDocumentEditor::SetTriggerRunOnInit(
    TriggerDocument& document,
    StableId triggerId,
    bool runOnInit) {
    TriggerRecord* trigger = FindTrigger(document, triggerId);
    if (trigger == nullptr) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_MISSING", "找不到目标触发器。");
    }
    trigger->runOnInit = runOnInit ? 1 : 0;
    return Status{};
}

Status TriggerDocumentEditor::SetTriggerComment(
    TriggerDocument& document,
    StableId triggerId,
    std::string comment) {
    if (comment.find('\0') != std::string::npos) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_COMMENT_NUL", "触发器注释不能包含 NUL 字符。");
    }
    TriggerRecord* trigger = FindTrigger(document, triggerId);
    if (trigger == nullptr) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_MISSING", "找不到目标触发器。");
    }
    trigger->description.text = std::move(comment);
    trigger->description.originalBytes.clear();
    return Status{};
}

Status TriggerDocumentEditor::SetEcaEnabled(
    TriggerDocument& document,
    StableId ecaId,
    bool enabled) {
    EcaNode* eca = FindEca(document, ecaId);
    if (eca == nullptr) {
        return Status::Failure(
            "Trigger.Edit", "ECA_MISSING", "找不到目标 ECA 节点。");
    }
    eca->enabled = enabled ? 1 : 0;
    return Status{};
}

namespace {
Result<RawText> EditedText(const RawText& source, std::string text) {
    if (text.find('\0') != std::string::npos)
        return Result<RawText>::Failure("Trigger.Edit", "TRIGGER_TEXT_NUL", "自定义文本不能包含 NUL。");
    if (text == source.text) return Result<RawText>::Success(source);
    RawText edited{std::move(text), source.encoding, {}};
    const auto encoded = edited.ToBytes();
    if (!encoded.Succeeded()) { Result<RawText> result; result.diagnostics = encoded.diagnostics; return result; }
    if (source.encoding == TextEncoding::Utf8Bom) {
        edited.originalBytes = {std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}};
        edited.originalBytes.insert(edited.originalBytes.end(), encoded.value->begin(), encoded.value->end());
    }
    return Result<RawText>::Success(std::move(edited));
}

using RenameResult = Result<AtomicRenameReceipt>;

RenameResult RenameFailure(std::vector<Diagnostic> diagnostics) {
    RenameResult result;
    result.diagnostics = std::move(diagnostics);
    return result;
}

bool GeneratedObjectReference(std::string_view value) {
    // 与生产生成器的预生成对象命名域一致；这些参数不是普通 udg 变量。
    constexpr std::string_view prefixes[] = {
        "gg_trg_", "gg_rct_", "gg_snd_", "gg_cam_", "gg_unit_", "gg_item_", "gg_dest_"};
    return std::any_of(std::begin(prefixes), std::end(prefixes),
        [value](auto prefix) { return value.starts_with(prefix); });
}

std::string AuditKey(AtomicRenameCodeKind kind, StableId id) {
    return std::to_string(static_cast<unsigned>(kind)) + ':' + std::to_string(id);
}

bool MetadataOnlyComment(const TriggerRecord& trigger) {
    return trigger.type == 1 && trigger.ecas.empty() && trigger.customTextFlag == 0
        && trigger.customText.text.empty() && trigger.customText.originalBytes.empty();
}

const TriggerRecord* RuntimeScriptNameConflict(
    const TriggerDocument& document, std::string_view name, StableId self) {
    const auto wanted = TriggerDirectoryEditor::ScriptName(name);
    for (const auto& trigger : document.triggers) {
        // MapScript 的 globals 和主体均跳过合法 type=1 说明记录；停用的 type=0
        // 仍有全局声明，必须保留其冲突检查。未知/混合记录也不能当作说明略过。
        if (trigger.id != self && !MetadataOnlyComment(trigger)
            && TriggerDirectoryEditor::ScriptName(trigger.name.text) == wanted) return &trigger;
    }
    return nullptr;
}

RenameResult RenameCommentMetadata(TriggerDocument& document, TriggerRecord& trigger,
    AtomicRenameReceipt receipt, std::span<const AtomicRenameCodeAudit> audits) {
    if (!audits.empty()) {
        return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_METADATA_AUDIT_INVALID",
            "说明记录没有运行时声明，不接受冗余的符号代码审计。");
    }
    for (const auto& argument : document.argumentNodes) {
        if (argument.type != 1) continue;
        if (TriggerDirectoryEditor::IsTriggerSymbolOf(argument.value.text, trigger.name)) {
            return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_METADATA_REFERENCE",
                "说明记录被触发器变量参数 " + std::to_string(argument.id)
                    + " 引用，不能假设它具有运行时声明或静默改写该引用。");
        }
    }
    // 合法说明记录只更新 WTG 名称，不生成 gg_trg_/InitTrig_，也不改任何 WCT、
    // typed 参数或不透明代码。编码准备与收据分配完成之后才原子交换名称。
    auto edited = EditedText(trigger.name, receipt.newName);
    if (!edited.Succeeded()) return RenameFailure(std::move(edited.diagnostics));
    auto result = RenameResult::Success(std::move(receipt));
    std::swap(trigger.name, *edited.value);
    return result;
}

// 不解析或改写代码：每个当前代码块必须有绑定本次符号对的显式外部审计。
// 普通字面量、rawcode 与 type=1 引用不进入此机制。
Status CheckRenameCode(const TriggerDocument& document, const UiCatalog& catalog,
    AtomicRenameReceipt& receipt, std::span<const AtomicRenameCodeAudit> audits) {
    constexpr std::size_t maxCodeBytes = 64 * 1024 * 1024;
    std::size_t auditBytes = 0;
    std::unordered_map<std::string, const AtomicRenameCodeAudit*> byBlock;
    for (const auto& audit : audits) {
        for (const auto* value : {&audit.sourceSha256, &audit.uiCatalogSha256, &audit.oldName,
                 &audit.newName, &audit.oldSymbol, &audit.newSymbol, &audit.reason}) {
            if (value->size() > maxCodeBytes - auditBytes)
                return Status::Failure("Trigger.Edit", "TRIGGER_RENAME_CODE_BUDGET", "代码审计超过字节预算。");
            auditBytes += value->size();
        }
        if (sizeof(AtomicRenameCodeAudit) > maxCodeBytes - auditBytes)
            return Status::Failure("Trigger.Edit", "TRIGGER_RENAME_CODE_BUDGET", "代码审计超过结构预算。");
        auditBytes += sizeof(AtomicRenameCodeAudit);
        if (audit.reason.empty() || audit.reason.size() > 4096 || audit.reason.find('\0') != std::string::npos
            || audit.oldName != receipt.oldName || audit.newName != receipt.newName
            || audit.oldSymbol != receipt.oldSymbol || audit.newSymbol != receipt.newSymbol
            || audit.uiCatalogSha256 != catalog.Sha256()
            || !byBlock.emplace(AuditKey(audit.kind, audit.id), &audit).second) {
            return Status::Failure("Trigger.Edit", "TRIGGER_RENAME_CODE_AUDIT_INVALID",
                "代码审计须逐块唯一，并绑定当前 UI 目录、本次旧/新符号和非空审计说明。");
        }
    }
    std::size_t codeBytes = 0;
    const auto check = [&](AtomicRenameCodeKind kind, StableId id, const RawText& text) -> Status {
        if (text.text.empty()) return Status::Ok();
        // 原始编码可能比 UTF-8 正文大；在编码/哈希分配之前核对输入和累计预算。
        const std::size_t bytes = std::max(text.text.size(), text.originalBytes.size());
        if (bytes > maxCodeBytes - codeBytes) {
            return Status::Failure("Trigger.Edit", "TRIGGER_RENAME_CODE_BUDGET",
                "原子改名的代码审计原文超过 64 MiB 预算，未修改文档。");
        }
        codeBytes += bytes;
        const auto sha = detail::CodeBlockSha256(text);
        if (!sha.Succeeded()) return {sha.diagnostics};
        const auto key = AuditKey(kind, id);
        const auto found = byBlock.find(key);
        if (found == byBlock.end()) {
            return Status::Failure("Trigger.Edit", "TRIGGER_RENAME_CODE_AUDIT_REQUIRED",
                "代码块 " + key + " 含未解析的潜在引用；需先证明本次改名不影响它。"
                " 原字节SHA=" + *sha.value + "；旧符号=" + receipt.oldSymbol
                + "；新符号=" + receipt.newSymbol + "。代码保持原样，改名未提交。");
        }
        const auto& audit = *found->second;
        if (audit.encoding != text.encoding || audit.sourceSha256 != *sha.value) {
            return Status::Failure("Trigger.Edit", "TRIGGER_RENAME_CODE_AUDIT_STALE",
                "代码块 " + key + " 的原字节或编码已变化，拒绝复用旧改名审计。");
        }
        receipt.externallyAuditedCode.push_back(audit);
        receipt.codeCoverage = AtomicRenameCodeCoverage::CallerAssertedNotRuntimeProven;
        byBlock.erase(found);
        return Status::Ok();
    };
    const std::array symbols{receipt.oldSymbol};
    if (auto status = detail::VisitOpaqueCodeBlocks(document, catalog, symbols, check); !status.Succeeded()) return status;
    if (!byBlock.empty()) {
        return Status::Failure("Trigger.Edit", "TRIGGER_RENAME_CODE_AUDIT_STALE",
            "审计包含当前文档未消费的代码块；拒绝旧槽位、错误种类或冗余放行。");
    }
    return Status::Ok();
}

struct RenameTextChange { RawText* target; RawText replacement; };

RenameResult CommitAtomicRename(TriggerDocument& document, const UiCatalog& catalog,
    RawText& declaration, AtomicRenameReceipt receipt,
    std::vector<ArgumentNode*> references, std::string referenceValue,
    std::span<const AtomicRenameCodeAudit> audits) {
    if (auto status = CheckRenameCode(document, catalog, receipt, audits); !status.Succeeded())
        return RenameFailure(std::move(status.diagnostics));
    std::vector<RenameTextChange> changes;
    changes.reserve(references.size() + 1);
    constexpr std::size_t maxChangeBytes = 64 * 1024 * 1024;
    std::size_t changeBytes = 0;
    const auto prepare = [&](RawText& target, std::string text) -> Status {
        if (text.size() > maxChangeBytes - changeBytes)
            return Status::Failure("Trigger.Edit", "TRIGGER_RENAME_REFERENCE_BUDGET", "改名引用文本超过字节预算。");
        changeBytes += text.size();
        auto edited = EditedText(target, std::move(text));
        if (!edited.Succeeded()) return {edited.diagnostics};
        changes.push_back({&target, std::move(*edited.value)});
        return Status::Ok();
    };
    if (auto status = prepare(declaration, receipt.newName); !status.Succeeded())
        return RenameFailure(std::move(status.diagnostics));
    receipt.changedArgumentIds.reserve(references.size());
    for (auto* argument : references) {
        if (auto status = prepare(argument->value, referenceValue); !status.Succeeded())
            return RenameFailure(std::move(status.diagnostics));
        receipt.changedArgumentIds.push_back(argument->id);
    }
    // 所有身份、结构、编码和外部审计核对结束后才提交；以下 swap 不分配且不失败。
    auto result = RenameResult::Success(std::move(receipt));
    for (auto& change : changes) std::swap(*change.target, change.replacement);
    return result;
}
}

Status TriggerDocumentEditor::SetCustomText(TriggerDocument& document, StableId triggerId, std::string text) {
    auto* trigger = FindTrigger(document, triggerId);
    if (!trigger || trigger->customTextFlag != 1)
        return Status::Failure("Trigger.Edit", "TRIGGER_CUSTOM_SLOT_REQUIRED", "目标须为既有自定义文本触发器；GUI 转换必须显式请求。");
    auto edited = EditedText(trigger->customText, std::move(text));
    if (!edited.Succeeded()) return {edited.diagnostics};
    trigger->customText = std::move(*edited.value);
    return Status::Ok();
}

Status TriggerDocumentEditor::SetGlobalCustomCode(TriggerDocument& document, std::string text) {
    auto edited = EditedText(document.wct.customCode, std::move(text));
    if (!edited.Succeeded()) return {edited.diagnostics};
    document.wct.customCode = std::move(*edited.value);
    return Status::Ok();
}

Status TriggerDocumentEditor::ConvertToCustomText(TriggerDocument& document, StableId triggerId, std::string text) {
    const auto* trigger = FindTrigger(document, triggerId);
    if (!trigger || trigger->customTextFlag != 0 || trigger->type != 0)
        return Status::Failure("Trigger.Edit", "TRIGGER_GUI_SLOT_REQUIRED", "显式转换只接受普通 GUI 触发器。");
    auto edited = EditedText(trigger->customText, std::move(text));
    if (!edited.Succeeded()) return {edited.diagnostics};
    // 复用一次完整闭包压实，保留同一 TriggerRecord、ID、名称、属性和 WCT 槽位置。
    auto removal = RemoveTrigger(document, triggerId);
    if (!removal.Succeeded()) return {removal.diagnostics};
    auto record = std::move(removal.value->trigger);
    record.ecas.clear(); record.customTextFlag = 1; record.customText = std::move(*edited.value);
    document.triggers.insert(document.triggers.begin() + static_cast<std::ptrdiff_t>(removal.value->row), std::move(record));
    return Status::Ok();
}

namespace {

// 闭包标记子树涉及的全部 ECA/参数节点
//（ECA→children/arguments；参数→callEca 嵌套调用 ECA / indexArgument 参数）。
void MarkSubtreeNodes(
    const TriggerDocument& document,
    std::span<const std::uint32_t> roots,
    std::vector<char>& removedEca,
    std::vector<char>& removedArg) {
    std::vector<std::uint32_t> pendingEcas(roots.begin(), roots.end());
    std::vector<std::uint32_t> pendingArgs;
    while (!pendingEcas.empty() || !pendingArgs.empty()) {
        while (!pendingEcas.empty()) {
            const std::uint32_t index = pendingEcas.back();
            pendingEcas.pop_back();
            if (index >= document.ecaNodes.size() || removedEca[index]) {
                continue;
            }
            removedEca[index] = 1;
            const EcaNode& node = document.ecaNodes[index];
            pendingEcas.insert(
                pendingEcas.end(), node.children.begin(), node.children.end());
            pendingArgs.insert(
                pendingArgs.end(), node.arguments.begin(), node.arguments.end());
        }
        while (!pendingArgs.empty()) {
            const std::uint32_t index = pendingArgs.back();
            pendingArgs.pop_back();
            if (index >= document.argumentNodes.size() || removedArg[index]) {
                continue;
            }
            removedArg[index] = 1;
            const ArgumentNode& argument = document.argumentNodes[index];
            if (argument.callEca.has_value()) {
                pendingEcas.push_back(*argument.callEca);
            }
            if (argument.indexArgument.has_value()) {
                pendingArgs.push_back(*argument.indexArgument);
            }
        }
    }
}

// 快照被标记的节点（按旧索引升序，撤销时按同一顺序补回）。
void SnapshotRemovedNodes(
    const TriggerDocument& document,
    const std::vector<char>& removedEca,
    const std::vector<char>& removedArg,
    std::vector<EcaNode>& ecas,
    std::vector<std::uint32_t>& ecaOldIndices,
    std::vector<ArgumentNode>& arguments,
    std::vector<std::uint32_t>& argumentOldIndices) {
    for (std::size_t i = 0; i < document.ecaNodes.size(); ++i) {
        if (removedEca[i]) {
            ecas.push_back(document.ecaNodes[i]);
            ecaOldIndices.push_back(static_cast<std::uint32_t>(i));
        }
    }
    for (std::size_t i = 0; i < document.argumentNodes.size(); ++i) {
        if (removedArg[i]) {
            arguments.push_back(document.argumentNodes[i]);
            argumentOldIndices.push_back(static_cast<std::uint32_t>(i));
        }
    }
}

// 压实：原地前移保留节点 + 重写全部保留引用（触发器根/children/arguments/
// callEca/indexArgument），文档保持无孤儿。大图上逐节点重建子表的分配是主要开销，故全部原地完成。
void CompactNodes(
    TriggerDocument& document,
    const std::vector<char>& removedEca,
    const std::vector<char>& removedArg) {
    std::vector<std::uint32_t> ecaRemap(document.ecaNodes.size(), kInvalidNodeIndex);
    std::size_t keptEcas = 0;
    for (std::size_t i = 0; i < document.ecaNodes.size(); ++i) {
        if (removedEca[i]) {
            continue;
        }
        ecaRemap[i] = static_cast<std::uint32_t>(keptEcas);
        if (keptEcas != i) document.ecaNodes[keptEcas] = std::move(document.ecaNodes[i]);
        ++keptEcas;
    }
    document.ecaNodes.erase(document.ecaNodes.begin() + static_cast<std::ptrdiff_t>(keptEcas), document.ecaNodes.end());
    std::vector<std::uint32_t> argRemap(document.argumentNodes.size(), kInvalidNodeIndex);
    std::size_t keptArgs = 0;
    for (std::size_t i = 0; i < document.argumentNodes.size(); ++i) {
        if (removedArg[i]) {
            continue;
        }
        argRemap[i] = static_cast<std::uint32_t>(keptArgs);
        if (keptArgs != i) document.argumentNodes[keptArgs] = std::move(document.argumentNodes[i]);
        ++keptArgs;
    }
    document.argumentNodes.erase(
        document.argumentNodes.begin() + static_cast<std::ptrdiff_t>(keptArgs), document.argumentNodes.end());
    const auto remapList = [](
        std::vector<std::uint32_t>& list,
        const std::vector<std::uint32_t>& remap) {
        auto out = list.begin();
        for (const std::uint32_t slot : list) {
            if (slot < remap.size() && remap[slot] != kInvalidNodeIndex) {
                *out++ = remap[slot];
            }
            // 指向已删除子树的槽位（仅摘除点）由调用方先行摘除；其余视为损坏丢弃。
        }
        list.erase(out, list.end());
    };
    for (auto& node : document.ecaNodes) {
        remapList(node.children, ecaRemap);
        remapList(node.arguments, argRemap);
    }
    for (auto& argument : document.argumentNodes) {
        if (argument.callEca.has_value()) {
            const std::uint32_t slot = *argument.callEca;
            argument.callEca =
                (slot < ecaRemap.size() && ecaRemap[slot] != kInvalidNodeIndex)
                ? std::optional<std::uint32_t>(ecaRemap[slot])
                : std::nullopt;
        }
        if (argument.indexArgument.has_value()) {
            const std::uint32_t slot = *argument.indexArgument;
            argument.indexArgument =
                (slot < argRemap.size() && argRemap[slot] != kInvalidNodeIndex)
                ? std::optional<std::uint32_t>(argRemap[slot])
                : std::nullopt;
        }
    }
    for (auto& trigger : document.triggers) {
        remapList(trigger.ecas, ecaRemap);
    }
}

// 严格递增且都小于 total 的旧索引表。
bool AscendingBelow(const std::vector<std::uint32_t>& indices, std::size_t total) {
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= total || (i > 0 && indices[i] <= indices[i - 1])) return false;
    }
    return true;
}

// 保留节点的 新索引 → 旧索引（旧索引序列中去掉被删的位置）。
std::vector<std::uint32_t> KeptNewToOld(std::size_t keptCount, const std::vector<std::uint32_t>& removedOld) {
    std::vector<std::uint32_t> map(keptCount);
    std::size_t removed = 0;
    std::size_t kept = 0;
    for (std::uint32_t old = 0; kept < keptCount; ++old) {
        if (removed < removedOld.size() && removedOld[removed] == old) {
            ++removed;
            continue;
        }
        map[kept++] = old;
    }
    return map;
}

// CompactNodes 的逆操作：把快照节点插回原索引，保留节点回到原位置并重写全部引用。
// 撤销严格按栈序进行时，结果与删除前逐项相同，后续依赖尾部位置的撤销（UndoAppend）仍然成立。
bool RestoreAtOldIndices(
    TriggerDocument& document,
    const std::vector<EcaNode>& ecas,
    const std::vector<std::uint32_t>& ecaOldIndices,
    const std::vector<ArgumentNode>& arguments,
    const std::vector<std::uint32_t>& argumentOldIndices) {
    const std::size_t totalEcas = document.ecaNodes.size() + ecas.size();
    const std::size_t totalArguments = document.argumentNodes.size() + arguments.size();
    if (ecas.size() != ecaOldIndices.size() || arguments.size() != argumentOldIndices.size() ||
        !AscendingBelow(ecaOldIndices, totalEcas) || !AscendingBelow(argumentOldIndices, totalArguments)) {
        return false;
    }
    const auto ecaMap = KeptNewToOld(document.ecaNodes.size(), ecaOldIndices);
    const auto argumentMap = KeptNewToOld(document.argumentNodes.size(), argumentOldIndices);
    const auto remap = [](std::uint32_t slot, const std::vector<std::uint32_t>& map) {
        return slot < map.size() ? map[slot] : kInvalidNodeIndex;
    };
    // 原地还原：先完成全部可能失败的分配（快照副本与容量），之后只做不抛异常的移动与改写。
    // 保留节点从尾部向后移回原索引（目标索引不小于当前索引且严格递增，自后向前移动不会覆盖尚未移动的节点），
    // 最后把快照移入空位。
    std::vector<EcaNode> ecaCopies(ecas.begin(), ecas.end());
    std::vector<ArgumentNode> argumentCopies(arguments.begin(), arguments.end());
    document.ecaNodes.reserve(totalEcas);
    document.argumentNodes.reserve(totalArguments);
    for (auto& node : document.ecaNodes) {
        for (auto& child : node.children) child = remap(child, ecaMap);
        for (auto& argument : node.arguments) argument = remap(argument, argumentMap);
    }
    for (auto& argument : document.argumentNodes) {
        if (argument.callEca.has_value()) argument.callEca = remap(*argument.callEca, ecaMap);
        if (argument.indexArgument.has_value()) argument.indexArgument = remap(*argument.indexArgument, argumentMap);
    }
    for (auto& trigger : document.triggers) {
        for (auto& root : trigger.ecas) root = remap(root, ecaMap);
    }
    const std::size_t keptEcas = document.ecaNodes.size();
    const std::size_t keptArguments = document.argumentNodes.size();
    document.ecaNodes.resize(totalEcas);
    for (std::size_t k = keptEcas; k-- > 0;) {
        if (ecaMap[k] != k) document.ecaNodes[ecaMap[k]] = std::move(document.ecaNodes[k]);
    }
    for (std::size_t i = 0; i < ecaCopies.size(); ++i) document.ecaNodes[ecaOldIndices[i]] = std::move(ecaCopies[i]);
    document.argumentNodes.resize(totalArguments);
    for (std::size_t k = keptArguments; k-- > 0;) {
        if (argumentMap[k] != k) document.argumentNodes[argumentMap[k]] = std::move(document.argumentNodes[k]);
    }
    for (std::size_t i = 0; i < argumentCopies.size(); ++i) document.argumentNodes[argumentOldIndices[i]] = std::move(argumentCopies[i]);
    return true;
}

// 把快照的节点补回文档，重写其内部引用；返回 旧索引→新索引 的 ECA 映射。
// 优先精确补回原索引；栈序被打乱（前提不成立）时退回追加到末尾。
std::unordered_map<std::uint32_t, std::uint32_t> RestoreRemovedNodes(
    TriggerDocument& document,
    const std::vector<EcaNode>& ecas,
    const std::vector<std::uint32_t>& ecaOldIndices,
    const std::vector<ArgumentNode>& arguments,
    const std::vector<std::uint32_t>& argumentOldIndices) {
    if (RestoreAtOldIndices(document, ecas, ecaOldIndices, arguments, argumentOldIndices)) {
        std::unordered_map<std::uint32_t, std::uint32_t> identity;
        for (const std::uint32_t old : ecaOldIndices) identity.emplace(old, old);
        return identity;
    }
    const std::uint32_t ecaBase = static_cast<std::uint32_t>(document.ecaNodes.size());
    const std::uint32_t argBase = static_cast<std::uint32_t>(document.argumentNodes.size());
    std::unordered_map<std::uint32_t, std::uint32_t> ecaOldToNew;
    for (std::size_t k = 0; k < ecaOldIndices.size(); ++k) {
        ecaOldToNew.emplace(ecaOldIndices[k], ecaBase + static_cast<std::uint32_t>(k));
    }
    std::unordered_map<std::uint32_t, std::uint32_t> argOldToNew;
    for (std::size_t k = 0; k < argumentOldIndices.size(); ++k) {
        argOldToNew.emplace(argumentOldIndices[k], argBase + static_cast<std::uint32_t>(k));
    }
    const auto remapSlot = [](
        std::uint32_t& slot,
        const std::unordered_map<std::uint32_t, std::uint32_t>& map) {
        const auto found = map.find(slot);
        slot = found == map.end() ? kInvalidNodeIndex : found->second;
    };
    for (const EcaNode& snapshot : ecas) {
        EcaNode node = snapshot;
        for (auto& child : node.children) {
            remapSlot(child, ecaOldToNew);
        }
        for (auto& argument : node.arguments) {
            remapSlot(argument, argOldToNew);
        }
        document.ecaNodes.push_back(std::move(node));
    }
    for (const ArgumentNode& snapshot : arguments) {
        ArgumentNode argument = snapshot;
        if (argument.callEca.has_value()) {
            std::uint32_t slot = *argument.callEca;
            remapSlot(slot, ecaOldToNew);
            argument.callEca = slot == kInvalidNodeIndex
                ? std::nullopt
                : std::optional<std::uint32_t>(slot);
        }
        if (argument.indexArgument.has_value()) {
            std::uint32_t slot = *argument.indexArgument;
            remapSlot(slot, argOldToNew);
            argument.indexArgument = slot == kInvalidNodeIndex
                ? std::nullopt
                : std::optional<std::uint32_t>(slot);
        }
        document.argumentNodes.push_back(std::move(argument));
    }
    return ecaOldToNew;
}

} // namespace

Result<TriggerDocumentEditor::EcaRemoveReceipt>
TriggerDocumentEditor::RemoveEcaSubtree(
    TriggerDocument& document,
    StableId ecaId) {
    using ResultT = Result<EcaRemoveReceipt>;
    // 1. 定位根节点索引。
    std::uint32_t rootIndex = kInvalidNodeIndex;
    for (std::size_t i = 0; i < document.ecaNodes.size(); ++i) {
        if (document.ecaNodes[i].id == ecaId) {
            rootIndex = static_cast<std::uint32_t>(i);
            break;
        }
    }
    if (rootIndex == kInvalidNodeIndex) {
        return ResultT::Failure("Trigger", "eca-not-found",
            "ECA node not found for removal");
    }

    std::vector<char> removedEca(document.ecaNodes.size(), 0);
    std::vector<char> removedArg(document.argumentNodes.size(), 0);
    MarkSubtreeNodes(document, {&rootIndex, 1}, removedEca, removedArg);

    // 2. 找挂载点并摘除根节点。
    EcaRemoveReceipt receipt;
    receipt.ecaId = ecaId;
    receipt.rootOldIndex = rootIndex;
    bool unlinked = false;
    for (auto& trigger : document.triggers) {
        const auto it = std::find(
            trigger.ecas.begin(), trigger.ecas.end(), rootIndex);
        if (it != trigger.ecas.end()) {
            receipt.parentTriggerId = trigger.id;
            receipt.row = static_cast<std::size_t>(it - trigger.ecas.begin());
            trigger.ecas.erase(it);
            unlinked = true;
            break;
        }
    }
    if (!unlinked) {
        for (auto& node : document.ecaNodes) {
            const auto it = std::find(
                node.children.begin(), node.children.end(), rootIndex);
            if (it != node.children.end()) {
                receipt.parentEcaId = node.id;
                receipt.row = static_cast<std::size_t>(it - node.children.begin());
                node.children.erase(it);
                unlinked = true;
                break;
            }
        }
    }
    if (!unlinked) {
        return ResultT::Failure("Trigger", "eca-orphan",
            "ECA node is not referenced by any trigger or parent");
    }

    // 3. 回执快照（按旧索引升序）+ 压实重建。
    SnapshotRemovedNodes(
        document,
        removedEca,
        removedArg,
        receipt.removedEcas,
        receipt.removedEcaOldIndices,
        receipt.removedArguments,
        receipt.removedArgumentOldIndices);
    CompactNodes(document, removedEca, removedArg);
    return ResultT::Success(std::move(receipt));
}

Status TriggerDocumentEditor::ClearTriggerEcas(
    TriggerDocument& document,
    StableId triggerId) {
    const auto trigger = std::find_if(document.triggers.begin(), document.triggers.end(),
        [triggerId](const TriggerRecord& row) { return row.id == triggerId; });
    if (trigger == document.triggers.end()) {
        return Status::Failure("Trigger.Edit", "TRIGGER_OWNER_MISSING", "找不到目标触发器。");
    }
    for (const auto index : trigger->ecas) {
        if (index >= document.ecaNodes.size()) {
            return Status::Failure("Trigger.Edit", "TRIGGER_ECA_INDEX_INVALID", "触发器根节点索引无效。");
        }
    }
    std::vector<char> removedEca(document.ecaNodes.size(), 0);
    std::vector<char> removedArg(document.argumentNodes.size(), 0);
    MarkSubtreeNodes(document, trigger->ecas, removedEca, removedArg);
    trigger->ecas.clear();
    CompactNodes(document, removedEca, removedArg);
    return Status{};
}

Result<TriggerDocumentEditor::ArgumentCallDetachReceipt>
TriggerDocumentEditor::DetachArgumentCall(
    TriggerDocument& document,
    StableId argumentId) {
    using ResultT = Result<ArgumentCallDetachReceipt>;
    ArgumentNode* argument = FindArgument(document, argumentId);
    if (argument == nullptr) {
        return ResultT::Failure("Trigger.Edit", "TRIGGER_ARGUMENT_MISSING", "找不到目标参数。");
    }
    if (!argument->callEca.has_value() || *argument->callEca >= document.ecaNodes.size()) {
        return ResultT::Failure("Trigger.Edit", "TRIGGER_ARGUMENT_NOT_CALL", "该参数当前不是函数。");
    }
    const std::uint32_t root = *argument->callEca;
    std::vector<char> removedEca(document.ecaNodes.size(), 0);
    std::vector<char> removedArg(document.argumentNodes.size(), 0);
    MarkSubtreeNodes(document, {&root, 1}, removedEca, removedArg);
    if (removedArg[static_cast<std::size_t>(argument - document.argumentNodes.data())]) {
        return ResultT::Failure("Trigger.Edit", "TRIGGER_ARGUMENT_CYCLE", "函数子树引用了自身所在的参数。");
    }

    ArgumentCallDetachReceipt receipt;
    receipt.argumentId = argumentId;
    receipt.argumentBefore = *argument;
    receipt.rootOldIndex = root;
    receipt.rootEcaId = document.ecaNodes[root].id;
    // 摘除后参数成为空的直接值，由调用方随后写入新的来源。
    argument->type = 3;
    argument->value.text.clear();
    argument->value.originalBytes.clear();
    argument->insertCall = 0;
    argument->callEca.reset();
    SnapshotRemovedNodes(document, removedEca, removedArg, receipt.removedEcas, receipt.removedEcaOldIndices,
        receipt.removedArguments, receipt.removedArgumentOldIndices);
    CompactNodes(document, removedEca, removedArg);
    return ResultT::Success(std::move(receipt));
}

Status TriggerDocumentEditor::UndoDetachArgumentCall(
    TriggerDocument& document,
    const ArgumentCallDetachReceipt& receipt) {
    if (FindArgument(document, receipt.argumentId) == nullptr) {
        return Status::Failure("Trigger.Edit", "TRIGGER_UNDO_ARGUMENT_MISSING", "找不到待撤销的函数参数。");
    }
    const auto ecaOldToNew = RestoreRemovedNodes(document, receipt.removedEcas, receipt.removedEcaOldIndices,
        receipt.removedArguments, receipt.removedArgumentOldIndices);
    const auto rootIt = ecaOldToNew.find(receipt.rootOldIndex);
    ArgumentNode* argument = FindArgument(document, receipt.argumentId);
    if (rootIt == ecaOldToNew.end() || argument == nullptr) {
        return Status::Failure("Trigger.Edit", "TRIGGER_UNDO_DETACH_INCONSISTENT", "函数参数撤销回执与文档不一致。");
    }
    // 只恢复值与函数挂接；下标参数的引用已随压实 / 补回重写，保持当前值。
    argument->type = receipt.argumentBefore.type;
    argument->value = receipt.argumentBefore.value;
    argument->insertCall = receipt.argumentBefore.insertCall;
    argument->callEca = rootIt->second;
    return Status::Ok();
}

Status TriggerDocumentEditor::RedoDetachArgumentCall(
    TriggerDocument& document,
    const ArgumentCallDetachReceipt& receipt) {
    const ArgumentNode* argument = FindArgument(document, receipt.argumentId);
    if (argument == nullptr || !argument->callEca.has_value() || *argument->callEca >= document.ecaNodes.size() ||
        document.ecaNodes[*argument->callEca].id != receipt.rootEcaId) {
        return Status::Failure("Trigger.Edit", "TRIGGER_REDO_DETACH_INCONSISTENT", "待重做的函数参数已经改变。");
    }
    auto detached = DetachArgumentCall(document, receipt.argumentId);
    Status status;
    status.diagnostics = std::move(detached.diagnostics);
    return status;
}

Status TriggerDocumentEditor::UndoRemoveEcaSubtree(
    TriggerDocument& document,
    const EcaRemoveReceipt& receipt) {
    // 幂等守卫：根节点已在图中则视为已撤销，直接成功。
    if (FindEca(document, receipt.ecaId) != nullptr) {
        return Status::Ok();
    }
    const auto ecaOldToNew = RestoreRemovedNodes(
        document,
        receipt.removedEcas,
        receipt.removedEcaOldIndices,
        receipt.removedArguments,
        receipt.removedArgumentOldIndices);
    const auto rootIt = ecaOldToNew.find(receipt.rootOldIndex);
    if (rootIt == ecaOldToNew.end()) {
        return Status::Failure(
            "Trigger", "eca-undo-root-missing", "receipt root index is inconsistent");
    }
    const std::uint32_t newRoot = rootIt->second;
    const auto insertAt = [newRoot](std::vector<std::uint32_t>& list, std::size_t row) {
        row = std::min(row, list.size());
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(row), newRoot);
    };
    if (receipt.parentTriggerId != 0) {
        TriggerRecord* trigger = FindTrigger(document, receipt.parentTriggerId);
        if (trigger == nullptr) {
            return Status::Failure(
                "Trigger", "eca-undo-trigger-missing", "parent trigger no longer exists");
        }
        insertAt(trigger->ecas, receipt.row);
        return Status::Ok();
    }
    EcaNode* parent = FindEca(document, receipt.parentEcaId);
    if (parent == nullptr) {
        return Status::Failure(
            "Trigger", "eca-undo-parent-missing", "parent ECA no longer exists");
    }
    insertAt(parent->children, receipt.row);
    return Status::Ok();
}

Result<StableId> TriggerDocumentEditor::AddTrigger(
    TriggerDocument& document,
    std::int32_t categoryId,
    std::string name) {
    if (name.empty() || name.find('\0') != std::string::npos) {
        return Result<StableId>::Failure(
            "Trigger.Edit", "TRIGGER_NAME_INVALID", "触发器名不能为空，也不能包含 NUL。");
    }
    const bool categoryExists = std::any_of(
        document.categories.begin(), document.categories.end(),
        [categoryId](const CategoryRecord& category) {
            return category.categoryId == categoryId;
        });
    if (!categoryExists) {
        return Result<StableId>::Failure(
            "Trigger.Edit", "TRIGGER_CATEGORY_MISSING", "目标分类不存在。");
    }
    if (const auto* conflict = TriggerDirectoryEditor::FindScriptNameConflict(document, name)) {
        return Result<StableId>::Failure("Trigger.Edit", "TRIGGER_SCRIPT_NAME_DUPLICATE",
            "新触发器名与“" + conflict->name.text + "”在脚本中的名称相同（gg_trg_" + TriggerDirectoryEditor::ScriptName(name) + "）。");
    }
    std::uint64_t counter = NextGeneratedCounter(document);
    const StableId id = AllocateId(counter);
    if (id == 0) {
        return Result<StableId>::Failure(
            "Trigger.Edit", "TRIGGER_ID_EXHAUSTED", "生成触发器 ID 空间已耗尽。");
    }
    TriggerRecord record;
    record.id = id;
    record.categoryId = categoryId;
    record.name.text = std::move(name);
    record.name.encoding = NewTextEncoding(document);
    record.customText.encoding = record.name.encoding; // 空 WCT 槽
    document.triggers.push_back(std::move(record));
    return Result<StableId>::Success(id);
}

Status TriggerDocumentEditor::RenameTrigger(
    TriggerDocument& document,
    StableId triggerId,
    std::string name) {
    if (name.empty() || name.find('\0') != std::string::npos) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_NAME_INVALID", "触发器名不能为空，也不能包含 NUL。");
    }
    TriggerRecord* trigger = FindTrigger(document, triggerId);
    if (trigger == nullptr) {
        return Status::Failure(
            "Trigger.Edit", "TRIGGER_MISSING", "找不到目标触发器。");
    }
    // 注释条目不生成脚本变量，改名不受脚本名冲突限制。
    const auto* conflict = trigger->type == 0 ? TriggerDirectoryEditor::FindScriptNameConflict(document, name, triggerId) : nullptr;
    if (conflict) {
        return Status::Failure("Trigger.Edit", "TRIGGER_SCRIPT_NAME_DUPLICATE",
            "新名称与“" + conflict->name.text + "”在脚本中的名称相同（gg_trg_" + TriggerDirectoryEditor::ScriptName(name) + "）。");
    }
    trigger->name.text = std::move(name);
    trigger->name.originalBytes.clear();
    return Status::Ok();
}

Result<AtomicRenameReceipt> TriggerDocumentEditor::RenameTriggerAtomic(
    TriggerDocument& document, const UiCatalog& catalog,
    StableId triggerId, std::string name,
    std::span<const AtomicRenameCodeAudit> codeAudits) {
    auto* trigger = FindTrigger(document, triggerId);
    if (!trigger) return RenameResult::Failure("Trigger.Edit", "TRIGGER_MISSING", "找不到目标触发器。");
    if (name.size() > 4096 || name.empty() || name.find('\0') != std::string::npos
        || TriggerDirectoryEditor::ScriptName(name).empty())
        return RenameResult::Failure("Trigger.Edit", "TRIGGER_NAME_INVALID", "触发器名无效或超过 4096 字节预算。");
    if (trigger->type == 1 && !MetadataOnlyComment(*trigger)) {
        return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_METADATA_SHAPE",
            "说明记录含 ECA 根、自定义代码标记或非空 WCT，不能按纯名称元数据改名。");
    }
    if (const auto valid = WtgWctCodec{}.Validate(document, catalog); !valid.Succeeded())
        return RenameFailure(valid.diagnostics);
    AtomicRenameReceipt receipt;
    receipt.declarationId = triggerId;
    receipt.oldName = trigger->name.text; receipt.newName = std::move(name);
    if (trigger->type == 1) {
        return RenameCommentMetadata(document, *trigger, std::move(receipt), codeAudits);
    }
    // 新名按原名的编码写出，脚本名也按写出的字节换算，与 WE 重新生成的声明一致。
    auto renamed = EditedText(trigger->name, receipt.newName);
    if (!renamed.Succeeded()) return RenameFailure(std::move(renamed.diagnostics));
    receipt.oldSymbol = TriggerDirectoryEditor::TriggerSymbol(trigger->name);
    receipt.newSymbol = TriggerDirectoryEditor::TriggerSymbol(*renamed.value);
    if (receipt.oldName == receipt.newName) return RenameResult::Success(std::move(receipt));
    if (RuntimeScriptNameConflict(document, receipt.oldName, triggerId))
        return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_SOURCE_AMBIGUOUS",
            "旧触发器名的生成符号与另一触发器冲突，不能判定引用属于哪一项。");
    if (RuntimeScriptNameConflict(document, receipt.newName, triggerId))
        return RenameResult::Failure("Trigger.Edit", "TRIGGER_SCRIPT_NAME_DUPLICATE", "新触发器名的生成符号已存在。");
    std::vector<ArgumentNode*> references;
    // 引用改写成 WE 写法；旧版本编辑器写过的形式也一并改正。
    const std::string referenceValue = receipt.newSymbol;
    for (auto& argument : document.argumentNodes) {
        if (argument.type != 1 || !TriggerDirectoryEditor::IsTriggerSymbolOf(argument.value.text, trigger->name)) continue;
        if (argument.callEca)
            return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_REFERENCE_AMBIGUOUS",
                "触发器变量参数同时挂接函数，不能当作确定的直接引用改名。");
        if (references.size() >= (64 * 1024 * 1024) /
                (sizeof(RenameTextChange) + sizeof(StableId) + sizeof(ArgumentNode*) + referenceValue.size()))
            return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_REFERENCE_BUDGET", "改名引用超过 owned 字节预算。");
        references.push_back(&argument);
    }
    return CommitAtomicRename(document, catalog, trigger->name, std::move(receipt),
        std::move(references), referenceValue, codeAudits);
}

Result<TriggerDocumentEditor::TriggerRemoveReceipt>
TriggerDocumentEditor::RemoveTrigger(
    TriggerDocument& document,
    StableId triggerId) {
    using ResultT = Result<TriggerRemoveReceipt>;
    std::size_t row = 0;
    bool found = false;
    for (; row < document.triggers.size(); ++row) {
        if (document.triggers[row].id == triggerId) {
            found = true;
            break;
        }
    }
    if (!found) {
        return ResultT::Failure(
            "Trigger.Edit", "TRIGGER_MISSING", "找不到目标触发器。");
    }
    TriggerRemoveReceipt receipt;
    receipt.trigger = document.triggers[row];
    receipt.row = row;
    std::vector<char> removedEca(document.ecaNodes.size(), 0);
    std::vector<char> removedArg(document.argumentNodes.size(), 0);
    MarkSubtreeNodes(document, receipt.trigger.ecas, removedEca, removedArg);
    document.triggers.erase(document.triggers.begin() + static_cast<std::ptrdiff_t>(row));
    SnapshotRemovedNodes(
        document,
        removedEca,
        removedArg,
        receipt.removedEcas,
        receipt.removedEcaOldIndices,
        receipt.removedArguments,
        receipt.removedArgumentOldIndices);
    CompactNodes(document, removedEca, removedArg);
    return ResultT::Success(std::move(receipt));
}

Status TriggerDocumentEditor::UndoRemoveTrigger(
    TriggerDocument& document,
    const TriggerRemoveReceipt& receipt) {
    // 幂等守卫：触发器已存在则视为已撤销。
    if (FindTrigger(document, receipt.trigger.id) != nullptr) {
        return Status::Ok();
    }
    const auto ecaOldToNew = RestoreRemovedNodes(
        document,
        receipt.removedEcas,
        receipt.removedEcaOldIndices,
        receipt.removedArguments,
        receipt.removedArgumentOldIndices);
    TriggerRecord trigger = receipt.trigger;
    for (auto& root : trigger.ecas) {
        const auto found = ecaOldToNew.find(root);
        root = found == ecaOldToNew.end() ? kInvalidNodeIndex : found->second;
    }
    trigger.ecas.erase(
        std::remove(trigger.ecas.begin(), trigger.ecas.end(), kInvalidNodeIndex),
        trigger.ecas.end());
    const std::size_t row = std::min(receipt.row, document.triggers.size());
    document.triggers.insert(
        document.triggers.begin() + static_cast<std::ptrdiff_t>(row),
        std::move(trigger));
    return Status::Ok();
}


Status TriggerDocumentEditor::AddVariable(
    TriggerDocument& document,
    std::string_view name,
    std::string_view typeName,
    bool isArray) {
    // 检查重名。
    for (const auto& var : document.variables) {
        if (var.name.text == name) {
            return Status::Failure("Trigger", "var-duplicate",
                "variable already exists: " + std::string(name));
        }
    }
    VariableRecord var;
    // 简单稳定 ID：基于名称的 FNV-1a 哈希。
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : name) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ull;
    }
    var.id = hash;
    var.name.text = std::string(name);
    var.typeName.text = std::string(typeName);
    var.isArray = isArray ? 1 : 0;
    var.size = isArray ? 1 : 1;
    var.hasDefault = 0;
    document.variables.push_back(std::move(var));
    return Status::Ok();
}

Status TriggerDocumentEditor::RenameVariable(
    TriggerDocument& document,
    std::string_view oldName,
    std::string_view newName) {
    for (auto& var : document.variables) {
        if (var.name.text == oldName) {
            // 检查新名不重复。
            for (const auto& other : document.variables) {
                if (&other != &var && other.name.text == newName) {
                    return Status::Failure("Trigger", "var-duplicate",
                        "cannot rename: target name exists: " + std::string(newName));
                }
            }
            var.name.text = std::string(newName);
            return Status::Ok();
        }
    }
    return Status::Failure("Trigger", "var-not-found",
        "variable not found: " + std::string(oldName));
}

Result<AtomicRenameReceipt> TriggerDocumentEditor::RenameVariableAtomic(
    TriggerDocument& document, const UiCatalog& catalog,
    StableId variableId, std::string name,
    std::span<const AtomicRenameCodeAudit> codeAudits) {
    const auto target = std::find_if(document.variables.begin(), document.variables.end(),
        [variableId](const auto& variable) { return variable.id == variableId; });
    if (target == document.variables.end())
        return RenameResult::Failure("Trigger.Edit", "VARIABLE_MISSING", "找不到目标变量。");
    if (name.empty() || name.size() > 4096 || name.find('\0') != std::string::npos
        || std::any_of(name.begin(), name.end(), [](unsigned char c) {
            return c < 128 && c != '_' && !(c >= 'a' && c <= 'z')
                && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9');
        }) || (name.front() >= '0' && name.front() <= '9'))
        return RenameResult::Failure("Trigger.Edit", "VARIABLE_NAME_INVALID",
            "变量名须为标识符，不能含空白、标点、NUL、开头数字或超过 4096 字节。");
    if (const auto valid = WtgWctCodec{}.Validate(document, catalog); !valid.Succeeded())
        return RenameFailure(valid.diagnostics);
    AtomicRenameReceipt receipt;
    receipt.declarationId = variableId;
    receipt.oldName = target->name.text; receipt.newName = std::move(name);
    receipt.oldSymbol = "udg_" + receipt.oldName;
    receipt.newSymbol = "udg_" + receipt.newName;
    if (receipt.oldName == receipt.newName) return RenameResult::Success(std::move(receipt));
    for (const auto& other : document.variables) {
        if (&other == &*target) continue;
        if (other.name.text == receipt.newName)
            return RenameResult::Failure("Trigger.Edit", "VARIABLE_NAME_DUPLICATE", "新变量名已存在。");
        if ((receipt.oldName.starts_with("udg_") && other.name.text == receipt.oldName.substr(4))
            || (receipt.newName.starts_with("udg_") && other.name.text == receipt.newName.substr(4)))
            return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_VARIABLE_SYMBOL_AMBIGUOUS",
                "原名或新名同时可解释为另一变量的 udg_ 别名，不能绑定确定引用。");
    }
    if (GeneratedObjectReference(receipt.oldName) || GeneratedObjectReference(receipt.newName))
        return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_VARIABLE_SYMBOL_AMBIGUOUS",
            "变量名与生产生成器的预生成对象命名域冲突，不能改写成普通变量引用。");
    const bool aliasDeclared = std::any_of(document.variables.begin(), document.variables.end(),
        [&](const auto& variable) { return variable.name.text == receipt.oldSymbol; });
    std::vector<ArgumentNode*> references;
    for (auto& argument : document.argumentNodes) {
        if (argument.type != 1) continue;
        const bool raw = argument.value.text == receipt.oldName;
        const bool alias = argument.value.text == receipt.oldSymbol;
        if (!raw && !alias) continue;
        if ((!raw && aliasDeclared) || argument.callEca)
            return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_REFERENCE_AMBIGUOUS",
                "变量参数有声明/udg_ 别名歧义或同时挂接函数，未修改文档。");
        // 生产 JASS/Lua 的 WTG 变量参数保存原名；明确的 udg_ 别名同步规范化，避免双前缀。
        if (references.size() >= (64 * 1024 * 1024) /
                (sizeof(RenameTextChange) + sizeof(StableId) + sizeof(ArgumentNode*) + receipt.newName.size()))
            return RenameResult::Failure("Trigger.Edit", "TRIGGER_RENAME_REFERENCE_BUDGET", "改名引用超过 owned 字节预算。");
        references.push_back(&argument);
    }
    const std::string referenceValue = receipt.newName;
    return CommitAtomicRename(document, catalog, target->name, std::move(receipt),
        std::move(references), referenceValue, codeAudits);
}

Status TriggerDocumentEditor::RemoveVariable(
    TriggerDocument& document,
    std::string_view name) {
    auto it = std::find_if(document.variables.begin(), document.variables.end(),
        [name](const VariableRecord& v) { return v.name.text == name; });
    if (it == document.variables.end()) {
        return Status::Failure("Trigger", "var-not-found",
            "variable not found: " + std::string(name));
    }
    document.variables.erase(it);
    return Status::Ok();
}

TriggerDocumentEditor::EcaPlacement TriggerDocumentEditor::LocateEcaPlacement(
    const TriggerDocument& document,
    StableId ecaId) noexcept {
    EcaPlacement placement;
    std::uint32_t target = kInvalidNodeIndex;
    for (std::size_t index = 0; index < document.ecaNodes.size(); ++index) {
        if (document.ecaNodes[index].id == ecaId) {
            target = static_cast<std::uint32_t>(index);
            break;
        }
    }
    if (target == kInvalidNodeIndex) {
        return placement;
    }
    for (const auto& trigger : document.triggers) {
        for (std::size_t row = 0; row < trigger.ecas.size(); ++row) {
            if (trigger.ecas[row] == target) {
                placement.parentTriggerId = trigger.id;
                placement.row = row;
                placement.found = true;
                return placement;
            }
        }
    }
    for (const auto& node : document.ecaNodes) {
        for (std::size_t row = 0; row < node.children.size(); ++row) {
            if (node.children[row] == target) {
                placement.parentEcaId = node.id;
                placement.row = row;
                placement.found = true;
                return placement;
            }
        }
    }
    return placement;
}

Result<TriggerDocumentEditor::EcaSubtreeSnapshot>
TriggerDocumentEditor::CopyEcaSubtree(
    const TriggerDocument& document,
    StableId ecaId) {
    using ResultT = Result<EcaSubtreeSnapshot>;
    std::uint32_t rootIndex = kInvalidNodeIndex;
    for (std::size_t index = 0; index < document.ecaNodes.size(); ++index) {
        if (document.ecaNodes[index].id == ecaId) {
            rootIndex = static_cast<std::uint32_t>(index);
            break;
        }
    }
    if (rootIndex == kInvalidNodeIndex) {
        return ResultT::Failure("Trigger", "eca-not-found",
            "ECA node not found for copy");
    }
    std::vector<char> removedEca(document.ecaNodes.size(), 0);
    std::vector<char> removedArg(document.argumentNodes.size(), 0);
    MarkSubtreeNodes(document, {&rootIndex, 1}, removedEca, removedArg);
    EcaSubtreeSnapshot snapshot;
    snapshot.rootOldIndex = rootIndex;
    SnapshotRemovedNodes(
        document,
        removedEca,
        removedArg,
        snapshot.ecas,
        snapshot.ecaOldIndices,
        snapshot.arguments,
        snapshot.argumentOldIndices);
    return ResultT::Success(std::move(snapshot));
}

Result<TriggerDocumentEditor::EcaPasteReceipt>
TriggerDocumentEditor::PasteEcaSubtree(
    TriggerDocument& document,
    const UiCatalog& catalog,
    const EcaSubtreeSnapshot& snapshot,
    StableId parentTriggerId,
    StableId parentEcaId,
    std::size_t row,
    std::optional<std::int32_t> rootChildId) {
    using ResultT = Result<EcaPasteReceipt>;
    if (snapshot.ecas.empty() ||
        snapshot.ecas.size() != snapshot.ecaOldIndices.size() ||
        snapshot.arguments.size() != snapshot.argumentOldIndices.size()) {
        return ResultT::Failure(
            "Trigger", "eca-paste-empty", "粘贴快照为空或内部不一致。");
    }
    for (const EcaNode& node : snapshot.ecas) {
        const UiDefinition* definition =
            catalog.Find(node.type, node.name.text);
        if (definition == nullptr) {
            return ResultT::Failure(
                "Trigger", "eca-paste-unknown-definition",
                "粘贴快照引用当前 UI Catalog 中不存在的原始函数 [" +
                    node.name.text + "]。");
        }
        if (definition->ArgumentCount() != node.arguments.size()) {
            return ResultT::Failure(
                "Trigger", "eca-paste-argument-count",
                "粘贴快照的参数数量与当前 UI Catalog 不一致。");
        }
    }
    if ((parentTriggerId == 0) == (parentEcaId == 0)) {
        return ResultT::Failure(
            "Trigger", "eca-paste-parent-ambiguous",
            "粘贴目标必须且只能指定一个触发器或父 ECA。");
    }
    if ((parentEcaId != 0) != rootChildId.has_value()) {
        return ResultT::Failure(
            "Trigger", "eca-paste-child-id",
            "嵌套粘贴必须提供分支号，触发器根粘贴不得携带分支号。");
    }
    // 目标触发器（sourceTriggerId 继承）与挂载点校验。
    // 注意：挂载点指针必须在克隆追加（vector 可能重分配）之后重新获取。
    StableId sourceTriggerId = 0;
    if (parentTriggerId != 0) {
        if (FindTrigger(document, parentTriggerId) == nullptr) {
            return ResultT::Failure(
                "Trigger", "eca-paste-trigger-missing", "粘贴目标触发器不存在。");
        }
        sourceTriggerId = parentTriggerId;
    } else {
        if (FindEca(document, parentEcaId) == nullptr) {
            return ResultT::Failure(
                "Trigger", "eca-paste-parent-missing", "粘贴目标父节点不存在。");
        }
        sourceTriggerId = FindEca(document, parentEcaId)->sourceTriggerId;
    }

    // 在触碰文档前完成快照拓扑、索引、容量和 ID 的全部校验。任何 Result
    // 失败都必须保持节点数组、挂载点与 dirty/undo 状态完全不变。
    constexpr std::size_t kMaximumNodeCount =
        static_cast<std::size_t>(kInvalidNodeIndex);
    if (document.ecaNodes.size() > kMaximumNodeCount ||
        document.argumentNodes.size() > kMaximumNodeCount ||
        snapshot.ecas.size() > kMaximumNodeCount - document.ecaNodes.size() ||
        snapshot.arguments.size() >
            kMaximumNodeCount - document.argumentNodes.size()) {
        return ResultT::Failure(
            "Trigger", "eca-paste-node-limit", "粘贴后的节点数量超过 WTG 索引上限。");
    }

    const std::uint32_t ecaBase =
        static_cast<std::uint32_t>(document.ecaNodes.size());
    const std::uint32_t argBase =
        static_cast<std::uint32_t>(document.argumentNodes.size());
    std::unordered_map<std::uint32_t, std::size_t> ecaOldToPosition;
    std::unordered_map<std::uint32_t, std::size_t> argOldToPosition;
    std::unordered_map<std::uint32_t, std::uint32_t> ecaOldToNew;
    std::unordered_map<std::uint32_t, std::uint32_t> argOldToNew;
    ecaOldToPosition.reserve(snapshot.ecas.size());
    argOldToPosition.reserve(snapshot.arguments.size());
    ecaOldToNew.reserve(snapshot.ecas.size());
    argOldToNew.reserve(snapshot.arguments.size());
    for (std::size_t index = 0; index < snapshot.ecaOldIndices.size(); ++index) {
        const std::uint32_t oldIndex = snapshot.ecaOldIndices[index];
        if (!ecaOldToPosition.emplace(oldIndex, index).second) {
            return ResultT::Failure(
                "Trigger", "eca-paste-duplicate-index",
                "粘贴快照包含重复的 ECA 旧索引。");
        }
        ecaOldToNew.emplace(
            oldIndex, ecaBase + static_cast<std::uint32_t>(index));
    }
    for (std::size_t index = 0; index < snapshot.argumentOldIndices.size(); ++index) {
        const std::uint32_t oldIndex = snapshot.argumentOldIndices[index];
        if (!argOldToPosition.emplace(oldIndex, index).second) {
            return ResultT::Failure(
                "Trigger", "eca-paste-duplicate-index",
                "粘贴快照包含重复的参数旧索引。");
        }
        argOldToNew.emplace(
            oldIndex, argBase + static_cast<std::uint32_t>(index));
    }
    const auto rootPosition = ecaOldToPosition.find(snapshot.rootOldIndex);
    if (rootPosition == ecaOldToPosition.end()) {
        return ResultT::Failure(
            "Trigger", "eca-paste-root-missing", "粘贴快照根索引不一致。");
    }

    struct PendingEca {
        std::uint32_t oldIndex = 0;
        bool child = false;
    };
    std::vector<PendingEca> pendingEcas{
        PendingEca{snapshot.rootOldIndex, rootChildId.has_value()}};
    std::vector<std::uint32_t> pendingArguments;
    std::vector<std::uint8_t> visitedEcas(snapshot.ecas.size(), 0);
    std::vector<std::uint8_t> visitedArguments(snapshot.arguments.size(), 0);
    while (!pendingEcas.empty() || !pendingArguments.empty()) {
        while (!pendingEcas.empty()) {
            const PendingEca pending = pendingEcas.back();
            pendingEcas.pop_back();
            const auto position = ecaOldToPosition.find(pending.oldIndex);
            if (position == ecaOldToPosition.end() ||
                visitedEcas[position->second] != 0) {
                return ResultT::Failure(
                    "Trigger", "eca-paste-invalid-graph",
                    "粘贴快照包含缺失、重复引用或循环的 ECA。");
            }
            visitedEcas[position->second] = 1;
            const EcaNode& node = snapshot.ecas[position->second];
            if (pending.oldIndex != snapshot.rootOldIndex &&
                node.childId.has_value() != pending.child) {
                return ResultT::Failure(
                    "Trigger", "eca-paste-child-id",
                    "粘贴快照的子节点分支号与挂载关系不一致。");
            }
            if (node.enabled != 0 && node.enabled != 1) {
                return ResultT::Failure(
                    "Trigger", "eca-paste-enabled",
                    "粘贴快照包含无效的 ECA 启用标志。");
            }
            for (const std::uint32_t child : node.children) {
                if (!ecaOldToPosition.contains(child)) {
                    return ResultT::Failure(
                        "Trigger", "eca-paste-missing-reference",
                        "粘贴快照的子 ECA 引用不在快照内。");
                }
                pendingEcas.push_back(PendingEca{child, true});
            }
            for (const std::uint32_t argument : node.arguments) {
                if (!argOldToPosition.contains(argument)) {
                    return ResultT::Failure(
                        "Trigger", "eca-paste-missing-reference",
                        "粘贴快照的参数引用不在快照内。");
                }
                pendingArguments.push_back(argument);
            }
        }
        while (!pendingArguments.empty()) {
            const std::uint32_t oldIndex = pendingArguments.back();
            pendingArguments.pop_back();
            const auto position = argOldToPosition.find(oldIndex);
            if (position == argOldToPosition.end() ||
                visitedArguments[position->second] != 0) {
                return ResultT::Failure(
                    "Trigger", "eca-paste-invalid-graph",
                    "粘贴快照包含缺失、重复引用或循环的参数。");
            }
            visitedArguments[position->second] = 1;
            const ArgumentNode& argument = snapshot.arguments[position->second];
            if (argument.type < -1 || argument.type > 3 ||
                (argument.insertCall != 0 && argument.insertCall != 1) ||
                (argument.insertIndex != 0 && argument.insertIndex != 1) ||
                ((argument.insertCall == 1) != argument.callEca.has_value()) ||
                ((argument.insertIndex == 1) != argument.indexArgument.has_value())) {
                return ResultT::Failure(
                    "Trigger", "eca-paste-invalid-argument",
                    "粘贴快照包含无效的参数标志。");
            }
            if (argument.callEca.has_value()) {
                if (!ecaOldToPosition.contains(*argument.callEca)) {
                    return ResultT::Failure(
                        "Trigger", "eca-paste-missing-reference",
                        "粘贴快照的 call ECA 引用不在快照内。");
                }
                pendingEcas.push_back(PendingEca{*argument.callEca, false});
            }
            if (argument.indexArgument.has_value()) {
                if (!argOldToPosition.contains(*argument.indexArgument)) {
                    return ResultT::Failure(
                        "Trigger", "eca-paste-missing-reference",
                        "粘贴快照的数组索引参数不在快照内。");
                }
                pendingArguments.push_back(*argument.indexArgument);
            }
        }
    }
    if (std::find(visitedEcas.begin(), visitedEcas.end(), 0) !=
            visitedEcas.end() ||
        std::find(visitedArguments.begin(), visitedArguments.end(), 0) !=
            visitedArguments.end()) {
        return ResultT::Failure(
            "Trigger", "eca-paste-orphan",
            "粘贴快照包含不属于根子树的孤立节点。");
    }

    std::uint64_t counter = NextGeneratedCounter(document);
    std::vector<EcaNode> clonedEcas;
    std::vector<ArgumentNode> clonedArguments;
    clonedEcas.reserve(snapshot.ecas.size());
    clonedArguments.reserve(snapshot.arguments.size());
    for (std::size_t index = 0; index < snapshot.ecas.size(); ++index) {
        EcaNode node = snapshot.ecas[index];
        const StableId newId = AllocateId(counter);
        if (newId == 0) {
            return ResultT::Failure(
                "Trigger", "TRIGGER_ID_EXHAUSTED", "生成节点 ID 空间已耗尽。");
        }
        node.id = newId;
        node.sourceTriggerId = sourceTriggerId;
        const UiDefinition* definition =
            catalog.Find(node.type, node.name.text);
        node.uiSource = definition->Active().source;
        node.uiLayer = definition->Active().layer;
        node.uiSignature = definition->Signature();
        for (std::uint32_t& child : node.children) {
            child = ecaOldToNew.at(child);
        }
        for (std::uint32_t& argument : node.arguments) {
            argument = argOldToNew.at(argument);
        }
        clonedEcas.push_back(std::move(node));
    }
    for (std::size_t index = 0; index < snapshot.arguments.size(); ++index) {
        ArgumentNode argument = snapshot.arguments[index];
        const StableId newId = AllocateId(counter);
        if (newId == 0) {
            return ResultT::Failure(
                "Trigger", "TRIGGER_ID_EXHAUSTED", "生成节点 ID 空间已耗尽。");
        }
        argument.id = newId;
        if (argument.callEca.has_value()) {
            argument.callEca = ecaOldToNew.at(*argument.callEca);
        }
        if (argument.indexArgument.has_value()) {
            argument.indexArgument =
                argOldToNew.at(*argument.indexArgument);
        }
        clonedArguments.push_back(std::move(argument));
    }

    const std::uint32_t newRoot = ecaOldToNew.at(snapshot.rootOldIndex);
    clonedEcas[rootPosition->second].childId = rootChildId;
    const StableId newRootId = clonedEcas[rootPosition->second].id;

    // 所有可能分配内存的 reserve 均发生在改变 size 之前。此后仅执行已验证、
    // 已预留容量的 noexcept move 与挂载，不再存在返回失败的半写分支。
    if (parentTriggerId != 0) {
        TriggerRecord* parentTrigger = FindTrigger(document, parentTriggerId);
        parentTrigger->ecas.reserve(parentTrigger->ecas.size() + 1);
        row = std::min(row, parentTrigger->ecas.size());
    } else {
        EcaNode* parentEca = FindEca(document, parentEcaId);
        parentEca->children.reserve(parentEca->children.size() + 1);
        row = std::min(row, parentEca->children.size());
    }
    document.ecaNodes.reserve(
        document.ecaNodes.size() + clonedEcas.size());
    document.argumentNodes.reserve(
        document.argumentNodes.size() + clonedArguments.size());
    for (EcaNode& node : clonedEcas) {
        document.ecaNodes.push_back(std::move(node));
    }
    for (ArgumentNode& argument : clonedArguments) {
        document.argumentNodes.push_back(std::move(argument));
    }

    if (parentTriggerId != 0) {
        TriggerRecord* parentTrigger = FindTrigger(document, parentTriggerId);
        parentTrigger->ecas.insert(
            parentTrigger->ecas.begin() + static_cast<std::ptrdiff_t>(row),
            newRoot);
    } else {
        EcaNode* parentEca = FindEca(document, parentEcaId);
        parentEca->children.insert(
            parentEca->children.begin() + static_cast<std::ptrdiff_t>(row),
            newRoot);
    }
    return ResultT::Success(EcaPasteReceipt{
        newRootId,
        parentTriggerId,
        parentEcaId,
        row,
    });
}

} // namespace War3Map::Trigger
