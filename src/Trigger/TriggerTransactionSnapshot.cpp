#include "War3MapCore/Trigger/TriggerTransactionSnapshot.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <algorithm>
#include <exception>
#include <unordered_map>
#include <utility>

namespace War3Map::Trigger {
namespace {

using SnapshotResult = Result<std::shared_ptr<const TriggerTransactionSnapshot>>;

Status Fail(std::string code, std::string message) {
    return Status::Failure("Trigger.Transaction", std::move(code), std::move(message));
}

SnapshotResult Forward(Status status) {
    SnapshotResult result;
    result.diagnostics = std::move(status.diagnostics);
    return result;
}

bool ValidSha(std::string_view sha) {
    return sha.size() == 64 && std::all_of(sha.begin(), sha.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    });
}

Status CheckBudget(const TriggerDocument& document, std::span<const std::byte> wtg,
    std::span<const std::byte> wct, const TriggerTransactionLimits& limits) {
    if (!limits.maxMemberBytes || !limits.maxRecords || !limits.maxTextBytes
        || wtg.size() > limits.maxMemberBytes || wct.size() > limits.maxMemberBytes)
        return Fail("TRIGGER_TRANSACTION_BYTE_BUDGET", "触发器事务配对成员超出明确的字节预算。");
    std::size_t records = 0;
    for (const auto count : {document.categories.size(), document.variables.size(), document.triggers.size(),
            document.ecaNodes.size(), document.argumentNodes.size()}) {
        if (count > limits.maxRecords - records)
            return Fail("TRIGGER_TRANSACTION_GRAPH_BUDGET", "触发器事务记录超出明确的图预算。");
        records += count;
    }
    std::size_t textBytes = 0;
    const auto add = [&](const RawText& text) {
        for (const auto count : {text.text.size(), text.originalBytes.size()}) {
            if (count > limits.maxTextBytes - textBytes) return false;
            textBytes += count;
        }
        return true;
    };
    bool bounded = add(document.wct.customComment) && add(document.wct.customCode);
    for (const auto& category : document.categories) bounded = bounded && add(category.name);
    for (const auto& variable : document.variables)
        bounded = bounded && add(variable.name) && add(variable.typeName) && add(variable.value);
    for (const auto& trigger : document.triggers)
        bounded = bounded && add(trigger.name) && add(trigger.description) && add(trigger.customText);
    for (const auto& eca : document.ecaNodes) bounded = bounded && add(eca.name);
    for (const auto& argument : document.argumentNodes) bounded = bounded && add(argument.value);
    return bounded ? Status::Ok()
        : Fail("TRIGGER_TRANSACTION_TEXT_BUDGET", "触发器事务的解码文字和原始字节超出明确预算。");
}

Status SameText(const RawText& original, const RawText& edited) {
    if (original.encoding != edited.encoding)
        return Fail("TRIGGER_TRANSACTION_CUSTOM_CODE_CHANGED", "完整 GUI 事务不得改变已有自定义代码的原编码。");
    const auto before = original.ToBytes(), after = edited.ToBytes();
    if (!before.Succeeded() || !before.value) return {before.diagnostics};
    if (!after.Succeeded() || !after.value) return {after.diagnostics};
    return *before.value == *after.value ? Status::Ok()
        : Fail("TRIGGER_TRANSACTION_CUSTOM_CODE_CHANGED", "完整 GUI 事务保留已有全局和触发器自定义代码原字节。");
}

Status PreserveGuiAndCustomCode(const TriggerDocument& original, const TriggerDocument& edited) {
    if (edited.VerifyCodeIdentityAuthorization(original).Succeeded()) return Status::Ok();
    for (const auto& check : {SameText(original.wct.customComment, edited.wct.customComment),
            SameText(original.wct.customCode, edited.wct.customCode)})
        if (!check.Succeeded()) return check;
    std::unordered_map<StableId, const TriggerRecord*> before;
    before.reserve(original.triggers.size());
    for (const auto& trigger : original.triggers) before.emplace(trigger.id, &trigger);
    std::unordered_map<StableId, const TriggerRecord*> after;
    after.reserve(edited.triggers.size());
    for (const auto& trigger : edited.triggers) {
        after.emplace(trigger.id, &trigger);
        const auto source = before.find(trigger.id);
        if (source == before.end()) {
            if (trigger.customTextFlag || !trigger.customText.text.empty() || !trigger.customText.originalBytes.empty())
                return Fail("TRIGGER_TRANSACTION_GUI_REQUIRED", "新增触发器必须保留可编辑 GUI 图，不能提供自定义代码代替。");
            continue;
        }
        if (trigger.customTextFlag != source->second->customTextFlag)
            return Fail("TRIGGER_TRANSACTION_GUI_REQUIRED", "完整 GUI 事务不得在 GUI 与自定义代码之间隐式转换。");
        const auto same = SameText(source->second->customText, trigger.customText);
        if (!same.Succeeded()) return same;
    }
    for (const auto& trigger : original.triggers) {
        if (trigger.customTextFlag && !after.contains(trigger.id))
            return Fail("TRIGGER_TRANSACTION_CUSTOM_CODE_CHANGED", "完整 GUI 事务不得删除已有自定义代码槽。");
    }
    return Status::Ok();
}

Status CheckSource(const TriggerDocument& document, const UiCatalog& catalog,
    std::span<const std::byte> wtg, std::span<const std::byte> wct) {
    if (!document.HasTrustedSourceSeal())
        return Fail("TRIGGER_TRANSACTION_SOURCE_SEAL_MISSING", "触发器文档没有配对解析器建立的可信来源 seal。");
    const auto wtgSha = Sha256(wtg), wctSha = Sha256(wct);
    if (!wtgSha.Succeeded() || !wtgSha.value) return {wtgSha.diagnostics};
    if (!wctSha.Succeeded() || !wctSha.value) return {wctSha.diagnostics};
    if (*wtgSha.value != document.SourceWtgSha256() || *wctSha.value != document.SourceWctSha256()
        || document.UiCatalogSha256() != catalog.Sha256())
        return Fail("TRIGGER_TRANSACTION_SOURCE_STALE", "原 WTG/WCT 或实际 Catalog 与触发器事务来源 seal 不一致。");
    return Status::Ok();
}

} // namespace

TriggerTransactionSnapshot::TriggerTransactionSnapshot(TriggerDocument document, UiCatalog catalog,
    std::vector<std::byte> originalWtg, std::vector<std::byte> originalWct,
    EncodedTriggerDocument encoded, TriggerTransactionReceipt receipt, TriggerTransactionLimits limits,
    bool sourceCompanion)
    : binding_(receipt.binding), document_(std::move(document)), catalog_(std::move(catalog)),
      originalWtg_(std::move(originalWtg)), originalWct_(std::move(originalWct)),
      encoded_(std::move(encoded)), receipt_(std::move(receipt)), limits_(limits), sourceCompanion_(sourceCompanion) {}

SnapshotResult TriggerTransactionSnapshot::Freeze(const TriggerDocument& document, const UiCatalog& catalog,
    std::span<const std::byte> originalWtg, std::span<const std::byte> originalWct,
    TriggerTransactionBinding binding, const TriggerTransactionLimits& limits) {
    return FreezeImpl(document, catalog, originalWtg, originalWct, std::move(binding), limits, false);
}

SnapshotResult TriggerTransactionSnapshot::FreezeSourceCompanion(const TriggerDocument& document, const UiCatalog& catalog,
    std::span<const std::byte> originalWtg, std::span<const std::byte> originalWct,
    TriggerTransactionBinding binding, const TriggerTransactionLimits& limits) {
    return FreezeImpl(document, catalog, originalWtg, originalWct, std::move(binding), limits, true);
}

SnapshotResult TriggerTransactionSnapshot::FreezeImpl(const TriggerDocument& document, const UiCatalog& catalog,
    std::span<const std::byte> originalWtg, std::span<const std::byte> originalWct,
    TriggerTransactionBinding binding, const TriggerTransactionLimits& limits, bool sourceCompanion) {
    try {
        if (!binding.transactionId || !binding.editGeneration || !ValidSha(binding.sourceSha256)
            || !ValidSha(binding.baseRevision.sourceSha256)
            || binding.baseRevision.uiCatalogSha256 != catalog.Sha256() || catalog.Layers().empty())
            return Forward(Fail("TRIGGER_TRANSACTION_BINDING_INVALID", "冻结事务必须具有明确整图 SHA、工作区修订、当前 generation 和有效 Catalog 层序。"));
        const auto bounded = CheckBudget(document, originalWtg, originalWct, limits);
        if (!bounded.Succeeded()) return Forward(bounded);
        const auto source = CheckSource(document, catalog, originalWtg, originalWct);
        if (!source.Succeeded()) return Forward(source);
        const WtgWctCodec codec;
        const auto original = codec.Parse(originalWtg, originalWct, catalog);
        if (!original.Succeeded() || !original.value) return Forward({original.diagnostics});
        const auto originalRoundtrip = codec.Serialize(*original.value, catalog);
        if (!originalRoundtrip.Succeeded() || !originalRoundtrip.value) return Forward({originalRoundtrip.diagnostics});
        if (!std::ranges::equal(originalRoundtrip.value->wtg, originalWtg)
            || !std::ranges::equal(originalRoundtrip.value->wct, originalWct))
            return Forward(Fail("TRIGGER_TRANSACTION_SOURCE_ROUNDTRIP", "原配对成员未通过完整逐字节往返，不能冻结完整编辑事务。"));
        const auto preserved = PreserveGuiAndCustomCode(*original.value, document);
        if (!preserved.Succeeded()) return Forward(preserved);
        auto encoded = codec.Serialize(document, catalog);
        if (!encoded.Succeeded() || !encoded.value) return Forward({encoded.diagnostics});
        if (encoded.value->wtg.size() > limits.maxMemberBytes || encoded.value->wct.size() > limits.maxMemberBytes)
            return Forward(Fail("TRIGGER_TRANSACTION_BYTE_BUDGET", "冻结后的触发器成员超出明确字节预算。"));
        const auto reparsed = codec.Parse(encoded.value->wtg, encoded.value->wct, catalog);
        if (!reparsed.Succeeded() || !reparsed.value) return Forward({reparsed.diagnostics});
        const auto candidateRoundtrip = codec.Serialize(*reparsed.value, catalog);
        if (!candidateRoundtrip.Succeeded() || !candidateRoundtrip.value) return Forward({candidateRoundtrip.diagnostics});
        if (candidateRoundtrip.value->wtg != encoded.value->wtg || candidateRoundtrip.value->wct != encoded.value->wct)
            return Forward(Fail("TRIGGER_TRANSACTION_CANDIDATE_ROUNDTRIP", "候选配对成员未通过完整回读往返，没有封印为保存输入。"));
        const bool unchanged = std::ranges::equal(encoded.value->wtg, originalWtg)
            && std::ranges::equal(encoded.value->wct, originalWct);
        if (sourceCompanion && !unchanged)
            return Forward(Fail("TRIGGER_TRANSACTION_SOURCE_COMPANION_CHANGED", "物编保存的原样触发器配对必须与原 WTG/WCT 完全相同，不能携带触发器改动。"));
        if (!sourceCompanion && unchanged)
            return Forward(Fail("TRIGGER_TRANSACTION_NO_CHANGE", "完整触发器事务与原配对成员相同，不生成空改动候选。"));
        const auto wtgSha = Sha256(encoded.value->wtg), wctSha = Sha256(encoded.value->wct);
        if (!wtgSha.Succeeded() || !wtgSha.value) return Forward({wtgSha.diagnostics});
        if (!wctSha.Succeeded() || !wctSha.value) return Forward({wctSha.diagnostics});
        TriggerTransactionReceipt receipt{binding, std::string(document.SourceWtgSha256()),
            std::string(document.SourceWctSha256()), catalog.Sha256(), *wtgSha.value, *wctSha.value,
            catalog.Layers(), document.variables.size(), document.triggers.size(),
            document.ecaNodes.size(), document.argumentNodes.size()};
        auto snapshot = std::shared_ptr<const TriggerTransactionSnapshot>(new TriggerTransactionSnapshot(
            document, catalog, {originalWtg.begin(), originalWtg.end()}, {originalWct.begin(), originalWct.end()},
            std::move(*encoded.value), std::move(receipt), limits, sourceCompanion));
        return SnapshotResult::Success(std::move(snapshot));
    } catch (const std::exception&) {
        return Forward(Fail("TRIGGER_TRANSACTION_FREEZE_FAILED", "冻结完整触发器事务失败；原文档和工作区事务未改变。"));
    }
}

Status TriggerTransactionSnapshot::VerifyAgainst(std::string_view sourceSha256,
    std::span<const std::byte> originalWtg, std::span<const std::byte> originalWct,
    const UiCatalog& catalog) const {
    try {
        if (sourceSha256 != binding_.sourceSha256)
            return Fail("TRIGGER_TRANSACTION_ARCHIVE_STALE", "实际源归档 SHA 与冻结触发器事务不一致。");
        if (catalog.Sha256() != receipt_.catalogSha256 || catalog.Layers() != receipt_.effectiveLayers)
            return Fail("TRIGGER_TRANSACTION_CATALOG_STALE", "实际 Catalog 或有效层序与冻结事务不一致。");
        const auto bounded = CheckBudget(document_, originalWtg, originalWct, limits_);
        if (!bounded.Succeeded()) return bounded;
        const auto source = CheckSource(document_, catalog, originalWtg, originalWct);
        if (!source.Succeeded()) return source;
        if (!std::ranges::equal(originalWtg, originalWtg_) || !std::ranges::equal(originalWct, originalWct_))
            return Fail("TRIGGER_TRANSACTION_SOURCE_STALE", "实际原配对成员字节与冻结快照不一致。");
        const auto encoded = WtgWctCodec{}.Serialize(document_, catalog);
        if (!encoded.Succeeded() || !encoded.value) return {encoded.diagnostics};
        if (encoded.value->wtg != encoded_.wtg || encoded.value->wct != encoded_.wct)
            return Fail("TRIGGER_TRANSACTION_SNAPSHOT_INVALID", "冻结的类型文档与封印配对产物不一致。");
        if (sourceCompanion_ && (encoded_.wtg != originalWtg_ || encoded_.wct != originalWct_))
            return Fail("TRIGGER_TRANSACTION_SOURCE_COMPANION_CHANGED", "原样触发器配对封印与其来源字节不同，未提交候选。");
        return Status::Ok();
    } catch (const std::exception&) {
        return Fail("TRIGGER_TRANSACTION_VERIFY_FAILED", "无法复核冻结触发器事务；没有提交候选。");
    }
}

} // namespace War3Map::Trigger
