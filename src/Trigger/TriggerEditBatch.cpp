#include "War3MapCore/Trigger/TriggerEditBatch.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerDataText.h"

#include <algorithm>
#include <string_view>
#include <type_traits>

namespace War3Map::Trigger {
namespace {

Status Fail(std::string code, std::string message) {
    return Status::Failure("Trigger.Batch", std::move(code), std::move(message));
}

bool ValidSha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

template<class T> Result<T> Forward(const Status& status) {
    Result<T> result;
    result.diagnostics = status.diagnostics;
    return result;
}

const EcaNode* FindEca(const TriggerDocument& document, StableId id) {
    const auto found = std::find_if(document.ecaNodes.begin(), document.ecaNodes.end(),
        [id](const EcaNode& node) { return node.id == id; });
    return found == document.ecaNodes.end() ? nullptr : &*found;
}

const TriggerRecord* FindTrigger(const TriggerDocument& document, StableId id) {
    const auto found = std::find_if(document.triggers.begin(), document.triggers.end(),
        [id](const TriggerRecord& trigger) { return trigger.id == id; });
    return found == document.triggers.end() ? nullptr : &*found;
}

Status CheckText(const RawText& text, const TriggerTextExpectation& expected) {
    if (text.encoding != expected.encoding) return Fail("TRIGGER_EDIT_TEXT_STALE", "文本槽原编码已变化。");
    const auto utf8 = Sha256Text(text.text);
    const auto bytes = text.ToBytes();
    if (!utf8.Succeeded()) return {utf8.diagnostics};
    if (!bytes.Succeeded()) return {bytes.diagnostics};
    const auto encoded = Sha256(*bytes.value);
    if (!encoded.Succeeded()) return {encoded.diagnostics};
    return *utf8.value == expected.expectedUtf8Sha256 && *encoded.value == expected.expectedEncodedSha256
        ? Status::Ok() : Fail("TRIGGER_EDIT_TEXT_STALE", "文本槽的 UTF-8/原编码字节前值与请求不符。");
}

// 只构造审核指纹，图删除/压实仍使用既有编辑器；引用编码成 StableId，
// 不能把删除导致的 flat vector 重排误报为另一个子树。整批原图只复制一次。
Result<std::string> Fingerprint(const TriggerDocument& document, StableId id) {
    struct Visit { bool eca; std::uint32_t index; std::size_t depth; };
    std::uint32_t root = kInvalidNodeIndex;
    for (std::size_t index = 0; index < document.ecaNodes.size(); ++index) {
        if (document.ecaNodes[index].id == id) { root = static_cast<std::uint32_t>(index); break; }
    }
    if (root == kInvalidNodeIndex)
        return Forward<std::string>(Fail("TRIGGER_EDIT_TARGET_STALE", "目标 ECA 已不存在。"));
    constexpr std::size_t maxBytes = 64 * 1024 * 1024;
    std::string payload;
    bool bounded = true;
    const auto add = [&](std::string_view value) {
        const auto length = std::to_string(value.size());
        if (length.size() + 1 > maxBytes - payload.size()
            || value.size() > maxBytes - payload.size() - length.size() - 1) {
            bounded = false;
            return;
        }
        payload += length; payload += ':'; payload += value;
    };
    const auto number = [&](auto value) { add(std::to_string(value)); };
    std::vector<Visit> pending{{true, root, 0}};
    std::size_t count = 0;
    while (!pending.empty() && bounded) {
        const auto visit = pending.back(); pending.pop_back();
        if (++count > 250000 || visit.depth > 256)
            return Forward<std::string>(Fail("TRIGGER_EDIT_GRAPH_BUDGET", "子树审核超过节点/深度预算。"));
        if (visit.eca) {
            if (visit.index >= document.ecaNodes.size())
                return Forward<std::string>(Fail("TRIGGER_EDIT_GRAPH_INVALID", "ECA 引用越界。"));
            const auto& node = document.ecaNodes[visit.index];
            add("eca"); number(node.id); number(node.sourceTriggerId); number(node.type);
            number(node.enabled); add(node.childId ? std::to_string(*node.childId) : "none");
            add(node.name.text); number(static_cast<unsigned>(node.name.encoding)); add(node.uiSignature);
            number(node.arguments.size()); number(node.children.size());
            for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
                pending.push_back({true, *child, visit.depth + 1});
            for (auto argument = node.arguments.rbegin(); argument != node.arguments.rend(); ++argument)
                pending.push_back({false, *argument, visit.depth + 1});
        } else {
            if (visit.index >= document.argumentNodes.size())
                return Forward<std::string>(Fail("TRIGGER_EDIT_GRAPH_INVALID", "参数引用越界。"));
            const auto& argument = document.argumentNodes[visit.index];
            add("argument"); number(argument.id); number(argument.type);
            add(argument.value.text); number(static_cast<unsigned>(argument.value.encoding));
            number(argument.insertCall); number(argument.insertIndex);
            number(argument.callEca.has_value()); number(argument.indexArgument.has_value());
            if (argument.indexArgument) pending.push_back({false, *argument.indexArgument, visit.depth + 1});
            if (argument.callEca) pending.push_back({true, *argument.callEca, visit.depth + 1});
        }
    }
    if (!bounded) return Forward<std::string>(Fail("TRIGGER_EDIT_GRAPH_BUDGET", "子树审核超过字节预算。"));
    return Sha256Text(payload);
}

Status CheckTarget(const TriggerDocument& original, const TriggerDocument& candidate,
    std::span<const std::byte> originalWtg, const TriggerEditEcaTarget& expected) {
    const auto* source = FindEca(original, expected.ecaId);
    const auto* current = FindEca(candidate, expected.ecaId);
    if (!source || !current) return Fail("TRIGGER_EDIT_TARGET_STALE", "目标 ECA 不属于当前来源或已被前次操作删除。");
    if (source->sourceTriggerId != expected.triggerId || current->sourceTriggerId != expected.triggerId
        || current->type != expected.expectedKind || current->name.text != expected.expectedFunction
        || current->enabled != expected.expectedEnabled || current->childId != expected.expectedChildId
        || current->arguments.size() != expected.expectedArgumentCount
        || current->children.size() != expected.expectedChildCount)
        return Fail("TRIGGER_EDIT_EXPECTATION_STALE", "目标 ECA 的来源、类型、函数、启用或结构与前值不符。");
    const auto mount = TriggerDocumentEditor::LocateEcaPlacement(candidate, expected.ecaId);
    if (!mount.found || mount.parentEcaId != expected.parentEcaId
        || (expected.parentEcaId == 0 ? mount.parentTriggerId != expected.triggerId : mount.parentTriggerId != 0))
        return Fail("TRIGGER_EDIT_MOUNT_STALE", "目标只能是指定触发器根或父 ECA children，不能删除/启停参数调用槽。");
    if (source->origin.offset > originalWtg.size()
        || source->origin.length > originalWtg.size() - source->origin.offset)
        return Fail("TRIGGER_EDIT_SOURCE_SPAN_INVALID", "目标来源区间越界。");
    const auto sourceHash = Sha256(originalWtg.subspan(source->origin.offset, source->origin.length));
    if (!sourceHash.Succeeded()) return {sourceHash.diagnostics};
    if (*sourceHash.value != expected.expectedSourceSpanSha256)
        return Fail("TRIGGER_EDIT_SOURCE_SPAN_STALE", "目标来源区间 SHA 与请求不符。");
    const auto before = Fingerprint(original, expected.ecaId);
    const auto now = Fingerprint(candidate, expected.ecaId);
    if (!before.Succeeded()) return {before.diagnostics};
    if (!now.Succeeded()) return {now.diagnostics};
    if (*before.value != *now.value)
        return Fail("TRIGGER_EDIT_SUBTREE_STALE", "前次操作已改变目标子树；不能继续应用旧结构请求。");
    return Status::Ok();
}

Status CheckTriggerTarget(const TriggerDocument& original, const TriggerDocument& candidate,
    std::span<const std::byte> originalWtg, const TriggerEditTriggerTarget& target) {
    const auto* source = FindTrigger(original, target.triggerId);
    const auto* current = FindTrigger(candidate, target.triggerId);
    if (!source || !current || current->type != 0 || current->customTextFlag != 0)
        return Fail("TRIGGER_EDIT_GUI_TARGET_STALE", "目标须为来源中仍存在的普通 GUI 触发器。");
    if (current->ecas.size() != target.expectedRootIds.size())
        return Fail("TRIGGER_EDIT_ROOTS_STALE", "完整根节点身份与请求不符。");
    for (std::size_t index = 0; index < current->ecas.size(); ++index) {
        if (current->ecas[index] >= candidate.ecaNodes.size()
            || candidate.ecaNodes[current->ecas[index]].id != target.expectedRootIds[index])
            return Fail("TRIGGER_EDIT_ROOTS_STALE", "完整根节点顺序与请求不符。");
        const auto before = Fingerprint(original, target.expectedRootIds[index]);
        const auto now = Fingerprint(candidate, target.expectedRootIds[index]);
        if (!before.Succeeded()) return {before.diagnostics};
        if (!now.Succeeded()) return {now.diagnostics};
        if (*before.value != *now.value) return Fail("TRIGGER_EDIT_SUBTREE_STALE", "GUI 原树已被前次操作改变。");
    }
    if (source->origin.offset > originalWtg.size() || source->origin.length > originalWtg.size() - source->origin.offset)
        return Fail("TRIGGER_EDIT_SOURCE_SPAN_INVALID", "触发器来源区间越界。");
    const auto hash = Sha256(originalWtg.subspan(source->origin.offset, source->origin.length));
    if (!hash.Succeeded()) return {hash.diagnostics};
    return *hash.value == target.expectedSourceSpanSha256 ? Status::Ok()
        : Fail("TRIGGER_EDIT_SOURCE_SPAN_STALE", "完整触发器来源区间与请求不符。");
}

Status SetArgument(TriggerDocument& candidate, const UiCatalog& catalog, const TriggerSetArgumentEdit& edit) {
    const auto* owner = FindEca(candidate, edit.ownerEcaId);
    auto* argument = TriggerDocumentEditor::FindArgument(candidate, edit.argumentId);
    if (!owner || !argument || owner->sourceTriggerId != edit.triggerId
        || argument->type != edit.expectedValueKind || argument->value.text != edit.expectedValue)
        return Fail("TRIGGER_EDIT_ARGUMENT_STALE", "参数身份、所有者或原值与请求不符。");
    if (argument->insertCall != 0 || argument->callEca || argument->insertIndex != 0 || argument->indexArgument)
        return Fail("TRIGGER_EDIT_ARGUMENT_SCALAR_REQUIRED", "本次参数修改只接受没有调用/数组下标子树的标量槽。");
    std::optional<std::size_t> ordinal;
    for (std::size_t index = 0; index < owner->arguments.size(); ++index) {
        if (owner->arguments[index] < candidate.argumentNodes.size()
            && candidate.argumentNodes[owner->arguments[index]].id == edit.argumentId) ordinal = index;
    }
    if (!ordinal) return Fail("TRIGGER_EDIT_ARGUMENT_OWNER_INVALID", "参数不是指定 ECA 的直接参数槽。");
    const auto* definition = catalog.Find(UiDefinitionKindFromEcaType(owner->type), owner->name.text);
    if (!definition || owner->uiSignature != definition->Signature())
        return Fail("TRIGGER_EDIT_ARGUMENT_SIGNATURE_STALE", "参数所有者的真实 Catalog 签名不符。");
    const TriggerTypeSystem types(catalog);
    const auto expectedType = types.ExpectedType(candidate, *owner, *ordinal);
    if (expectedType.empty() || expectedType == "Null" || expectedType == "AnyType" || expectedType == "anytype"
        || expectedType != edit.expectedType
        || (types.LiteralKind(expectedType) == TriggerTypeSystem::Literal::Other && !types.Find(expectedType)))
        return Fail("TRIGGER_EDIT_ARGUMENT_TYPE_UNRESOLVED", "参数类型不能明确解析或与请求不符；未放宽未知类型。");
    if (edit.valueKind == 3) {
        if (types.LiteralKind(expectedType) == TriggerTypeSystem::Literal::Other)
            return Fail("TRIGGER_EDIT_ARGUMENT_DIRECT_UNSUPPORTED", "该类型不允许未经证明的直接标量文本。");
        const auto valid = types.ValidateLiteral(expectedType, edit.value);
        if (!valid.Succeeded()) return valid;
    } else if (edit.valueKind == 0) {
        const auto type = types.PresetType(edit.value);
        if (!types.AllowedValueKinds(expectedType).preset || !type || !types.Accepts(expectedType, *type))
            return Fail("TRIGGER_EDIT_ARGUMENT_PRESET_INVALID", "预设不存在或不匹配参数类型。");
    } else {
        const auto variable = std::find_if(candidate.variables.begin(), candidate.variables.end(),
            [&](const VariableRecord& value) { return value.name.text == edit.value; });
        if (!types.AllowedValueKinds(expectedType).variable || variable == candidate.variables.end()
            || variable->isArray != 0 || !types.Accepts(expectedType, variable->typeName.text))
            return Fail("TRIGGER_EDIT_ARGUMENT_VARIABLE_INVALID", "全局变量不存在、需要下标或不匹配类型。");
    }
    RawText value{edit.value, argument->value.encoding, {}};
    const auto bytes = value.ToBytes();
    if (!bytes.Succeeded()) return {bytes.diagnostics};
    if (argument->type == edit.valueKind && argument->value.text == edit.value) return Status::Ok();
    return TriggerDocumentEditor::SetScalarArgument(candidate, edit.argumentId, edit.valueKind, edit.value);
}

} // namespace

Status ValidateTriggerEditBatch(const TriggerEditBatch& batch) {
    if (!ValidSha(batch.expectedWtgSha256) || !ValidSha(batch.expectedWctSha256)
        || !ValidSha(batch.expectedCatalogSha256) || batch.operations.empty()
        || batch.operations.size() > kMaxTriggerEditOperations)
        return Fail("TRIGGER_EDIT_BATCH_INVALID", "批次须绑定 WTG/WCT/Catalog SHA 且包含 1..256 个操作。");
    std::size_t textBytes = 0;
    const auto textBounded = [&](std::string_view text) {
        if (text.size() > 1024 * 1024 || text.size() > 4 * 1024 * 1024 - textBytes || text.find('\0') != std::string::npos)
            return false;
        textBytes += text.size(); return true;
    };
    const auto textExpectationValid = [](const TriggerTextExpectation& expected) {
        return !TriggerEditEncodingName(expected.encoding).empty() && ValidSha(expected.expectedUtf8Sha256)
            && ValidSha(expected.expectedEncodedSha256);
    };
    for (const auto& operation : batch.operations) {
      const auto valid = std::visit([&](const auto& edit) -> Status {
        using Edit = std::decay_t<decltype(edit)>;
        if constexpr (std::is_same_v<Edit, TriggerRemoveEcaEdit> || std::is_same_v<Edit, TriggerSetEcaEnabledEdit>) {
        const auto& target = edit.target;
        if (target.triggerId == 0 || target.ecaId == 0 || target.expectedKind < 0 || target.expectedKind > 2
            || (target.expectedEnabled != 0 && target.expectedEnabled != 1)
            || target.expectedFunction.empty() || target.expectedFunction.size() > 1024
            || target.expectedFunction.find('\0') != std::string::npos
            || target.expectedArgumentCount > 250000 || target.expectedChildCount > 250000
            || !ValidSha(target.expectedSourceSpanSha256)
            || (target.parentEcaId == 0) != !target.expectedChildId.has_value())
            return Fail("TRIGGER_EDIT_TARGET_INVALID", "ECA 前值、来源区间 SHA 或根/children 挂载信息无效。");
        } else if constexpr (std::is_same_v<Edit, TriggerSetArgumentEdit>) {
            if (!edit.triggerId || !edit.ownerEcaId || !edit.argumentId || edit.expectedType.empty() || edit.expectedType.size() > 1024
                || (edit.valueKind != 0 && edit.valueKind != 1 && edit.valueKind != 3)
                || (edit.expectedValueKind != 0 && edit.expectedValueKind != 1 && edit.expectedValueKind != 3)
                || !textBounded(edit.expectedValue) || !textBounded(edit.value))
                return Fail("TRIGGER_EDIT_ARGUMENT_INVALID", "参数请求身份、种类或文本预算无效。");
        } else if constexpr (std::is_same_v<Edit, TriggerAppendRootEdit> || std::is_same_v<Edit, TriggerConvertCustomTextEdit>) {
            if (!edit.target.triggerId || !ValidSha(edit.target.expectedSourceSpanSha256) || edit.target.expectedRootIds.size() > 4096
                || std::find(edit.target.expectedRootIds.begin(), edit.target.expectedRootIds.end(), StableId{0}) != edit.target.expectedRootIds.end())
                return Fail("TRIGGER_EDIT_ROOTS_INVALID", "完整 GUI 触发器绑定或根列表无效。");
            if constexpr (std::is_same_v<Edit, TriggerAppendRootEdit>) {
                if ((edit.kind != UiDefinitionKind::Event && edit.kind != UiDefinitionKind::Condition && edit.kind != UiDefinitionKind::Action)
                    || edit.function.empty() || edit.function.size() > 1024 || edit.function.find('\0') != std::string::npos)
                    return Fail("TRIGGER_EDIT_APPEND_INVALID", "根类型或真实函数名无效。");
            } else if (!textExpectationValid(edit.expected) || !textBounded(edit.text))
                return Fail("TRIGGER_EDIT_TEXT_INVALID", "自定义文本前值、编码或预算无效。");
        } else {
            if (!textExpectationValid(edit.expected) || !textBounded(edit.text))
                return Fail("TRIGGER_EDIT_TEXT_INVALID", "自定义文本前值、编码或预算无效。");
            if constexpr (std::is_same_v<Edit, TriggerSetCustomTextEdit>) {
                if (!edit.triggerId) return Fail("TRIGGER_EDIT_TARGET_INVALID", "自定义触发器 ID 不能为零。");
            }
        }
        return Status::Ok();
      }, operation);
      if (!valid.Succeeded()) return valid;
    }
    return Status::Ok();
}

Result<TriggerEditBatchReceipt> ApplyTriggerEditBatch(TriggerDocument& document,
    const UiCatalog& catalog, std::span<const std::byte> originalWtg, const TriggerEditBatch& batch) {
    const auto request = ValidateTriggerEditBatch(batch);
    if (!request.Succeeded()) return Forward<TriggerEditBatchReceipt>(request);
    if (originalWtg.size() > 64 * 1024 * 1024 || document.ecaNodes.size() > 250000
        || document.argumentNodes.size() > 250000 - document.ecaNodes.size())
        return Forward<TriggerEditBatchReceipt>(Fail("TRIGGER_EDIT_GRAPH_BUDGET", "触发器批次来源超出字节或节点预算。"));
    const auto identity = [&](std::string_view role, std::string_view expected, std::string_view actual) {
        return expected == actual ? Status::Ok() : Fail("TRIGGER_EDIT_SOURCE_STALE",
            std::string(role) + " 来源身份与请求不符；expected=" + std::string(expected)
                + "; actual=" + std::string(actual));
    };
    for (const auto& check : {
        identity("WTG", batch.expectedWtgSha256, document.SourceWtgSha256()),
        identity("WCT", batch.expectedWctSha256, document.SourceWctSha256()),
        identity("document Catalog", batch.expectedCatalogSha256, document.UiCatalogSha256()),
        identity("loaded Catalog", batch.expectedCatalogSha256, catalog.Sha256())}) {
        if (!check.Succeeded()) return Forward<TriggerEditBatchReceipt>(check);
    }
    const auto sourceHash = Sha256(originalWtg);
    if (!sourceHash.Succeeded()) return Forward<TriggerEditBatchReceipt>({sourceHash.diagnostics});
    if (*sourceHash.value != batch.expectedWtgSha256)
        return Forward<TriggerEditBatchReceipt>(Fail("TRIGGER_EDIT_SOURCE_STALE",
            "原 WTG 字节与来源 seal 不符；expected=" + batch.expectedWtgSha256
                + "; actual=" + *sourceHash.value));
    const WtgWctCodec codec;
    const auto initial = codec.Validate(document, catalog);
    if (!initial.Succeeded()) return Forward<TriggerEditBatchReceipt>(initial);
    const auto originalEncoded = codec.Serialize(document, catalog);
    if (!originalEncoded.Succeeded()) return Forward<TriggerEditBatchReceipt>({originalEncoded.diagnostics});
    const auto encodedWtgSha = Sha256(originalEncoded.value->wtg), encodedWctSha = Sha256(originalEncoded.value->wct);
    if (!encodedWtgSha.Succeeded()) return Forward<TriggerEditBatchReceipt>({encodedWtgSha.diagnostics});
    if (!encodedWctSha.Succeeded()) return Forward<TriggerEditBatchReceipt>({encodedWctSha.diagnostics});
    if (*encodedWtgSha.value != batch.expectedWtgSha256 || *encodedWctSha.value != batch.expectedWctSha256)
        return Forward<TriggerEditBatchReceipt>(Fail("TRIGGER_EDIT_DOCUMENT_STALE", "批次必须从请求绑定的原始配对文档开始；已有 overlay 不会隐式混入。"));
    TriggerDocument candidate = document;
    TriggerEditBatchReceipt receipt;
    for (const auto& operation : batch.operations) {
        const auto applied = std::visit([&](const auto& edit) -> Status {
            using Edit = std::decay_t<decltype(edit)>;
            if constexpr (std::is_same_v<Edit, TriggerRemoveEcaEdit> || std::is_same_v<Edit, TriggerSetEcaEnabledEdit>) {
            const auto check = CheckTarget(document, candidate, originalWtg, edit.target);
            if (!check.Succeeded()) return check;
            if constexpr (std::is_same_v<std::decay_t<decltype(edit)>, TriggerRemoveEcaEdit>) {
                const auto removed = TriggerDocumentEditor::RemoveEcaSubtree(candidate, edit.target.ecaId);
                if (!removed.Succeeded()) return {removed.diagnostics};
                receipt.removedEcaCount += removed.value->removedEcas.size();
                receipt.removedArgumentCount += removed.value->removedArguments.size();
                return Status::Ok();
            } else {
                if (edit.enabled != (edit.target.expectedEnabled != 0)) ++receipt.enabledChangeCount;
                return TriggerDocumentEditor::SetEcaEnabled(candidate, edit.target.ecaId, edit.enabled);
            }
            } else if constexpr (std::is_same_v<Edit, TriggerSetArgumentEdit>) {
                const auto set = SetArgument(candidate, catalog, edit);
                if (set.Succeeded() && (edit.expectedValueKind != edit.valueKind || edit.expectedValue != edit.value)) ++receipt.argumentChangeCount;
                return set;
            } else if constexpr (std::is_same_v<Edit, TriggerAppendRootEdit>) {
                const auto check = CheckTriggerTarget(document, candidate, originalWtg, edit.target);
                if (!check.Succeeded()) return check;
                const auto appended = TriggerDocumentEditor::AppendRootEca(candidate, catalog, edit.target.triggerId, edit.kind, edit.function);
                if (!appended.Succeeded()) return {appended.diagnostics};
                ++receipt.appendedRootCount; return Status::Ok();
            } else if constexpr (std::is_same_v<Edit, TriggerSetGlobalCodeEdit>) {
                const auto check = CheckText(candidate.wct.customCode, edit.expected);
                if (!check.Succeeded()) return check;
                const bool changed = candidate.wct.customCode.text != edit.text;
                const auto set = TriggerDocumentEditor::SetGlobalCustomCode(candidate, edit.text);
                if (set.Succeeded() && changed) ++receipt.textChangeCount;
                return set;
            } else if constexpr (std::is_same_v<Edit, TriggerSetCustomTextEdit>) {
                const auto* trigger = FindTrigger(candidate, edit.triggerId);
                if (!trigger) return Fail("TRIGGER_EDIT_TARGET_STALE", "自定义文本触发器已不存在。");
                const auto check = CheckText(trigger->customText, edit.expected);
                if (!check.Succeeded()) return check;
                const bool changed = trigger->customText.text != edit.text;
                const auto set = TriggerDocumentEditor::SetCustomText(candidate, edit.triggerId, edit.text);
                if (set.Succeeded() && changed) ++receipt.textChangeCount;
                return set;
            } else {
                const auto check = CheckTriggerTarget(document, candidate, originalWtg, edit.target);
                if (!check.Succeeded()) return check;
                const auto* trigger = FindTrigger(candidate, edit.target.triggerId);
                const auto text = CheckText(trigger->customText, edit.expected);
                if (!text.Succeeded()) return text;
                const bool textChanged = trigger->customText.text != edit.text;
                const auto ecasBefore = candidate.ecaNodes.size(), argumentsBefore = candidate.argumentNodes.size();
                const auto converted = TriggerDocumentEditor::ConvertToCustomText(candidate, edit.target.triggerId, edit.text);
                if (converted.Succeeded()) {
                    receipt.removedEcaCount += ecasBefore - candidate.ecaNodes.size();
                    receipt.removedArgumentCount += argumentsBefore - candidate.argumentNodes.size();
                    ++receipt.convertedGuiCount; if (textChanged) ++receipt.textChangeCount;
                }
                return converted;
            }
        }, operation);
        if (!applied.Succeeded()) return Forward<TriggerEditBatchReceipt>(applied);
        ++receipt.operationCount;
    }
    const auto encoded = codec.Serialize(candidate, catalog);
    if (!encoded.Succeeded()) return Forward<TriggerEditBatchReceipt>({encoded.diagnostics});
    if (encoded.value->wtg == originalEncoded.value->wtg && encoded.value->wct == originalEncoded.value->wct)
        return Forward<TriggerEditBatchReceipt>(Fail("TRIGGER_EDIT_NO_CHANGE", "批次未产生触发器内容变化，未发布空候选。"));
    document = std::move(candidate);
    return Result<TriggerEditBatchReceipt>::Success(std::move(receipt));
}

std::string_view TriggerEditEncodingName(TextEncoding encoding) noexcept {
    switch (encoding) {
    case TextEncoding::Utf8: return "utf8";
    case TextEncoding::Utf8Bom: return "utf8-bom";
    case TextEncoding::Gb18030: return "gb18030";
    case TextEncoding::Latin1: return "latin1";
    }
    return {};
}

std::optional<TextEncoding> ParseTriggerEditEncoding(std::string_view name) noexcept {
    for (const auto encoding : {TextEncoding::Utf8, TextEncoding::Utf8Bom, TextEncoding::Gb18030, TextEncoding::Latin1})
        if (name == TriggerEditEncodingName(encoding)) return encoding;
    return std::nullopt;
}

Result<TriggerTextPage> QueryTriggerText(const TriggerDocument& document, const UiCatalog& catalog,
    const TriggerTextQuery& query) {
    if (!document.HasTrustedSourceSeal() || document.UiCatalogSha256() != catalog.Sha256())
        return Forward<TriggerTextPage>(Fail("TRIGGER_EDIT_SOURCE_STALE", "文本查询须使用可信来源配对文档和同一 Catalog。"));
    if ((query.slot != TriggerTextSlot::GlobalCode && query.slot != TriggerTextSlot::TriggerCode)
        || query.limit < 4 || query.limit > 65536
        || (query.slot == TriggerTextSlot::GlobalCode ? query.triggerId != 0 : query.triggerId == 0))
        return Forward<TriggerTextPage>(Fail("TRIGGER_TEXT_QUERY_INVALID", "文本页须明确槽身份且 limit 在 4..65536 字节内。"));
    const auto valid = WtgWctCodec{}.Validate(document, catalog);
    if (!valid.Succeeded()) return Forward<TriggerTextPage>(valid);
    const RawText* text = &document.wct.customCode;
    bool custom = true;
    if (query.slot == TriggerTextSlot::TriggerCode) {
        const auto* trigger = FindTrigger(document, query.triggerId);
        if (!trigger) return Forward<TriggerTextPage>(Fail("TRIGGER_EDIT_TARGET_STALE", "找不到该触发器文本槽。"));
        text = &trigger->customText; custom = trigger->customTextFlag != 0;
    }
    const auto continuation = [](unsigned char c) { return (c & 0xc0) == 0x80; };
    if (query.offset > text->text.size() || (query.offset < text->text.size() && continuation(text->text[query.offset])))
        return Forward<TriggerTextPage>(Fail("TRIGGER_TEXT_OFFSET_INVALID", "文本起点越界或不在 UTF-8 码点边界。"));
    std::size_t end = query.offset + std::min(query.limit, text->text.size() - query.offset);
    while (end < text->text.size() && end > query.offset && continuation(text->text[end])) --end;
    const auto bytes = text->ToBytes();
    const auto utf8 = Sha256Text(text->text);
    if (!bytes.Succeeded()) return Forward<TriggerTextPage>({bytes.diagnostics});
    if (!utf8.Succeeded()) return Forward<TriggerTextPage>({utf8.diagnostics});
    const auto encoded = Sha256(*bytes.value);
    if (!encoded.Succeeded()) return Forward<TriggerTextPage>({encoded.diagnostics});
    return Result<TriggerTextPage>::Success({query.slot, query.triggerId, std::string(document.SourceWtgSha256()),
        std::string(document.SourceWctSha256()), catalog.Sha256(), text->encoding, *utf8.value, *encoded.value,
        text->text.substr(query.offset, end - query.offset), text->text.size(), query.offset, end, custom});
}

} // namespace War3Map::Trigger
