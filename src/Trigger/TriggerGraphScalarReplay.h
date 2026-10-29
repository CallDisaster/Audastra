// 仅由 TriggerGraphEditing.cpp 在同一命名空间包含；复用其唯一 Editor 路径。
namespace detail {
struct GraphScalarSlot {
    std::size_t argumentIndex = 0, ownerIndex = 0;
    GraphArgumentReplacementGuard guard;
    ArgumentNode before, after;
};
struct GraphScalarEditState {
    TriggerGraphEditReceipt receipt;
    TriggerGraphScalarEditCost cost;
    TriggerGraphReplayIdentity beforeIdentity, afterIdentity;
    std::vector<GraphScalarSlot> slots;
    std::size_t maxOwnedBytes = 0, maxWorkUnits = 0;
    TriggerScalarWorkPolicy policy = TriggerScalarWorkPolicy::BoundedSourcesV2;
};
}
namespace {
using ScalarState = detail::GraphScalarEditState;
constexpr std::size_t kScalarOwnedHardLimit = 512 * 1024 * 1024;
constexpr std::size_t kScalarWorkHardLimit = 256 * 1024 * 1024;
bool ScalarAdd(std::size_t& total, std::size_t extra) {
    if (extra > std::numeric_limits<std::size_t>::max() - total) return false;
    total += extra; return true;
}
bool ScalarProduct(std::size_t& total, std::size_t count, std::size_t size) {
    return count <= std::numeric_limits<std::size_t>::max() / size && ScalarAdd(total, count * size);
}
struct ScalarDocumentSize {
    std::size_t bytes = sizeof(TriggerDocument), records = 0, edges = 0, text = 0, maxEdges = 2097152;
    bool Text(std::size_t size) { return ScalarAdd(bytes, size) && ScalarAdd(text, size); }
    bool Origin(const OriginSpan& origin) {
        return origin.source.native().size() <= std::numeric_limits<std::size_t>::max() / sizeof(std::filesystem::path::value_type)
            && Text(origin.source.native().size() * sizeof(std::filesystem::path::value_type)) && Text(origin.fingerprint.size());
    }
    bool Raw(const RawText& value) { return Text(value.text.size()) && Text(value.originalBytes.size()); }
    bool Array(std::size_t count, std::size_t size) { return ScalarAdd(records, count) && ScalarProduct(bytes, count, size); }
    bool Edge(std::size_t count) { return edges <= maxEdges && count <= maxEdges - edges && ScalarAdd(edges, count) && ScalarProduct(bytes, count, sizeof(std::uint32_t)); }
};
bool MeasureScalarDocument(const TriggerDocument& document, ScalarDocumentSize& size, std::size_t maximum, std::size_t records = 1048576, std::size_t edges = 2097152) {
    size.maxEdges = edges;
    if (!size.Array(document.categories.size(), sizeof(CategoryRecord)) || !size.Array(document.variables.size(), sizeof(VariableRecord))
        || !size.Array(document.triggers.size(), sizeof(TriggerRecord)) || !size.Array(document.ecaNodes.size(), sizeof(EcaNode))
        || !size.Array(document.argumentNodes.size(), sizeof(ArgumentNode)) || size.bytes > maximum || size.records > records) return false;
    for (const auto& value : document.categories) if (!size.Raw(value.name) || !size.Origin(value.origin) || size.bytes > maximum) return false;
    for (const auto& value : document.variables) if (!size.Raw(value.name) || !size.Raw(value.typeName) || !size.Raw(value.value)
        || !size.Origin(value.origin) || size.bytes > maximum) return false;
    for (const auto& value : document.triggers) if (!size.Raw(value.name) || !size.Raw(value.description) || !size.Raw(value.customText)
        || !size.Origin(value.origin) || !size.Edge(value.ecas.size()) || size.bytes > maximum) return false;
    for (const auto& value : document.ecaNodes) if (!size.Raw(value.name) || !size.Origin(value.origin)
        || !size.Text(value.uiLayer.size()) || !size.Text(value.uiSignature.size())
        || value.uiSource.native().size() > std::numeric_limits<std::size_t>::max() / sizeof(std::filesystem::path::value_type)
        || !size.Text(value.uiSource.native().size() * sizeof(std::filesystem::path::value_type))
        || !size.Edge(value.arguments.size()) || !size.Edge(value.children.size()) || size.bytes > maximum) return false;
    for (const auto& value : document.argumentNodes) if (!size.Raw(value.value) || !size.Origin(value.origin)
        || !size.Edge(static_cast<std::size_t>(value.callEca.has_value()) + static_cast<std::size_t>(value.indexArgument.has_value()))
        || size.bytes > maximum) return false;
    return size.Raw(document.wct.customComment) && size.Raw(document.wct.customCode) && size.bytes <= maximum;
}
bool MeasureScalarNode(const ArgumentNode& node, std::size_t& bytes, std::size_t& text) {
    ScalarDocumentSize size; size.bytes = sizeof(ArgumentNode);
    if (!size.Raw(node.value) || !size.Origin(node.origin)) return false;
    return ScalarAdd(bytes, size.bytes) && ScalarAdd(text, size.text);
}
// 只计构造既有 TypeSystem 的输入/临时逻辑持有，不复制或实现其类型规则。
bool MeasureScalarTypePreparation(const UiCatalog& catalog, TriggerGraphScalarEditCost& cost) {
    if (catalog.Manifest().size() > 1024 || catalog.Documents().size() > 1024) return false;
    std::size_t definitions = 0, rawBytes = 0;
    for (const auto& source : catalog.Manifest()) {
        if (!ScalarAdd(definitions, source.definitionCount) || definitions > 32768) return false;
    }
    for (const auto& source : catalog.Documents()) {
        if (!ScalarAdd(rawBytes, source.OriginalBytes().size()) || rawBytes > 16 * 1024 * 1024) return false;
    }
    cost.catalogBytesCharged = rawBytes;
    const auto rows = catalog.Definitions(UiDefinitionKind::Define);
    if (rows.size() > definitions) return false;
    std::size_t owned = sizeof(TriggerTypeSystem), splitPeak = 0;
    if (!ScalarProduct(owned, rows.size(), sizeof(const UiDefinition*))) return false;
    for (const auto* row : rows) {
        if (!row) return false;
        std::size_t bytes = row->name.size(), fieldBytes = 0;
        for (const auto& [key, value] : row->Active().fields) {
            if (!ScalarAdd(fieldBytes, key.size()) || !ScalarAdd(fieldBytes, value.size()) || fieldBytes > 16 * 1024 * 1024) return false;
            if (key == "value") {
                std::size_t parts = 1;
                for (const auto character : value) if (character == ',') ++parts;
                std::size_t scratch = 0;
                if (!ScalarProduct(scratch, parts, sizeof(std::string)) || !ScalarProduct(scratch, value.size(), 2)) return false;
                splitPeak = std::max(splitPeak, scratch);
                if (!ScalarProduct(bytes, value.size(), 4)) return false;
            }
        }
        // types/preset/selector 的 key、值及临时 selector 表均纳入上界；不是 allocator 节点尺寸。
        if (!ScalarProduct(bytes, row->name.size(), 3) || !ScalarAdd(owned, bytes)
            || !ScalarAdd(owned, sizeof(std::pair<const std::string, TriggerTypeInfo>)
                + 2 * sizeof(std::pair<const std::string, std::string>) + sizeof(std::pair<const std::string, bool>))) return false;
    }
    if (!ScalarAdd(owned, splitPeak)) return false;
    cost.typePreparationScratchBytes = owned;
    return true;
}
bool SameScalarNode(const ArgumentNode& a, const ArgumentNode& b) {
    return a.id == b.id && a.type == b.type && a.insertCall == b.insertCall && a.callEca == b.callEca
        && a.insertIndex == b.insertIndex && a.indexArgument == b.indexArgument
        && a.value.text == b.value.text && a.value.encoding == b.value.encoding && a.value.originalBytes == b.value.originalBytes
        && a.origin.source == b.origin.source && a.origin.offset == b.origin.offset
        && a.origin.length == b.origin.length && a.origin.fingerprint == b.origin.fingerprint;
}
TriggerGraphBinding ScalarBinding(const TriggerDocument& document, const UiCatalog& catalog,
    const TriggerGraphReplayIdentity& identity, const MapObjectReferenceSnapshot* mapObjects) {
    return {std::string(document.SourceWtgSha256()), std::string(document.SourceWctSha256()), catalog.Sha256(),
        std::string(identity.CurrentWtgSha256()), std::string(identity.CurrentWctSha256()), mapObjects ? mapObjects->Identity() : std::string{}};
}
std::string ScalarCostText(const TriggerGraphScalarEditCost& cost) {
    return "logical_bill_not_RSS source=" + std::to_string(cost.sourceDocumentBytes)
        + " candidate=" + std::to_string(cost.candidateOwnedBytes) + " growth=" + std::to_string(cost.candidateGrowthBytes)
        + " compact=" + std::to_string(cost.compactOwnedBytes) + " publicationScratch=" + std::to_string(cost.publicationScratchBytes)
        + " guardScratch=" + std::to_string(cost.guardScratchBytes) + " additionalOwned=" + std::to_string(cost.additionalOwnedBytes)
        + " lookupScratch=" + std::to_string(cost.lookupScratchBytes) + " operationScratch=" + std::to_string(cost.operationScratchBytes)
        + " validationScratch=" + std::to_string(cost.validationScratchBytes)
        + " typeScratch=" + std::to_string(cost.typePreparationScratchBytes) + " mountScratch=" + std::to_string(cost.mountIndexScratchBytes)
        + " catalogInput=" + std::to_string(cost.catalogBytesCharged)
        + " encodedPair=" + std::to_string(cost.encodedPairBytes) + " encodedCapacity=" + std::to_string(cost.encodedPairCapacityBytes)
        + " identityScratch=" + std::to_string(cost.identityScratchBytes) + " peak=" + std::to_string(cost.peakOwnedBytes)
        + " copiedText=" + std::to_string(cost.copiedTextBytes) + " codecEncoded=" + std::to_string(cost.codecEncodedBytesCharged)
        + " identityInput=" + std::to_string(cost.identityBytesCharged) + " recordVisits=" + std::to_string(cost.recordVisitsCharged)
        + " edgeVisits=" + std::to_string(cost.edgeVisitsCharged) + " additionalWork=" + std::to_string(cost.additionalWorkUnits)
        + " work=" + std::to_string(cost.workUnitsCharged)
        + " workCoverageIncomplete=true policy=" + std::to_string(static_cast<unsigned>(cost.workPolicy))
        + " codeProtection=" + std::to_string(cost.codeProtectionScratchBytes)
        + " ownedInputConsumed=" + std::to_string(cost.ownedInputConsumed)
        + " captures=" + std::to_string(cost.identityCaptures) + " candidateCopies=" + std::to_string(cost.candidateCopies)
        + " coreBulkGuards=" + std::to_string(cost.bulkGuardCaptures) + " candidateValidations=" + std::to_string(cost.candidateValidations);
}
Status ScalarCostFailure(std::string code, const TriggerGraphScalarEditCost& cost) {
    return Fail(std::move(code), "紧凑标量事务预算拒绝；未发布源。" + ScalarCostText(cost));
}
bool ChargeScalarIdentity(TriggerGraphScalarEditCost& cost, const TriggerGraphReplayIdentity& identity) {
    ++cost.identityCaptures;
    cost.encodedPairBytes = std::max(cost.encodedPairBytes, identity.EncodedBytes());
    cost.encodedPairCapacityBytes = std::max(cost.encodedPairCapacityBytes, identity.EncodedCapacityBytes());
    cost.identityScratchBytes = std::max(cost.identityScratchBytes, identity.ScratchBytes());
    // 每次 capture 的 codec 写出一次、两成员摘要读一次；完整 identity 流另计其实际输入。
    return ScalarProduct(cost.codecEncodedBytesCharged, identity.EncodedBytes(), 2)
        && ScalarAdd(cost.identityBytesCharged, identity.BytesCharged());
}
bool FinishScalarCost(TriggerGraphScalarEditCost& cost) {
    std::size_t preparation = cost.candidateOwnedBytes, publication = cost.compactOwnedBytes;
    std::size_t guardPreparation = cost.guardScratchBytes;
    if (!ScalarAdd(preparation, cost.compactOwnedBytes) || !ScalarAdd(preparation, cost.guardScratchBytes)
        || !ScalarAdd(preparation, cost.additionalOwnedBytes) || !ScalarAdd(preparation, cost.lookupScratchBytes)
        || !ScalarAdd(preparation, cost.operationScratchBytes) || !ScalarAdd(preparation, cost.validationScratchBytes)
        || !ScalarAdd(preparation, cost.typePreparationScratchBytes) || !ScalarAdd(preparation, cost.codeProtectionScratchBytes)
        || !ScalarAdd(preparation, cost.encodedPairCapacityBytes)
        || !ScalarAdd(preparation, cost.identityScratchBytes) || !ScalarAdd(publication, cost.publicationScratchBytes)
        || !ScalarAdd(publication, cost.additionalOwnedBytes) || !ScalarAdd(publication, cost.validationScratchBytes)
        || !ScalarAdd(publication, cost.encodedPairCapacityBytes)
        || !ScalarAdd(publication, cost.identityScratchBytes)
        || !ScalarAdd(guardPreparation, cost.additionalOwnedBytes) || !ScalarAdd(guardPreparation, cost.mountIndexScratchBytes)
        || !ScalarAdd(guardPreparation, cost.typePreparationScratchBytes) || !ScalarAdd(guardPreparation, cost.validationScratchBytes)) return false;
    cost.peakOwnedBytes = std::max({preparation, publication, guardPreparation});
    cost.workUnitsCharged = 0;
    return ScalarAdd(cost.workUnitsCharged, cost.copiedTextBytes) && ScalarAdd(cost.workUnitsCharged, cost.codecEncodedBytesCharged)
        && ScalarAdd(cost.workUnitsCharged, cost.identityBytesCharged) && ScalarAdd(cost.workUnitsCharged, cost.recordVisitsCharged)
        && ScalarAdd(cost.workUnitsCharged, cost.edgeVisitsCharged) && ScalarAdd(cost.workUnitsCharged, cost.additionalWorkUnits)
        && ScalarProduct(cost.workUnitsCharged, cost.catalogBytesCharged, 2); // 仅已知费用估计；不包含底层全部重复扫描，不能称操作硬上界。
}
Status CheckScalarSlots(const TriggerDocument& document, const ScalarState& state, bool afterExpected) {
    for (const auto& slot : state.slots) {
        if (slot.argumentIndex >= document.argumentNodes.size() || slot.ownerIndex >= document.ecaNodes.size())
            return Fail("TRIGGER_GRAPH_SCALAR_SLOT_STALE", "紧凑收据的参数或 ECA 位置已不存在。");
        const auto& owner = document.ecaNodes[slot.ownerIndex]; const auto& expected = afterExpected ? slot.after : slot.before;
        if (owner.id != slot.guard.ownerEcaId || owner.sourceTriggerId != slot.guard.sourceTriggerId
            || slot.guard.argumentOrdinal >= owner.arguments.size() || owner.arguments[slot.guard.argumentOrdinal] != slot.argumentIndex
            || !SameScalarNode(document.argumentNodes[slot.argumentIndex], expected))
            return Fail("TRIGGER_GRAPH_SCALAR_SLOT_STALE", "稳定身份、真实挂载或完整参数原值已变化。");
    }
    return Status::Ok();
}
class ScalarSwapRollback {
public:
    ScalarSwapRollback(TriggerDocument& document, const ScalarState& state, std::vector<ArgumentNode>& replacements)
        : document_(document), state_(state), replacements_(replacements) {}
    ~ScalarSwapRollback() { if (!committed_) while (swapped_) { --swapped_; std::swap(document_.argumentNodes[state_.slots[swapped_].argumentIndex], replacements_[swapped_]); } }
    void SwapAll() noexcept { for (; swapped_ < state_.slots.size(); ++swapped_) std::swap(document_.argumentNodes[state_.slots[swapped_].argumentIndex], replacements_[swapped_]); }
    void Commit() noexcept { committed_ = true; }
private:
    TriggerDocument& document_; const ScalarState& state_; std::vector<ArgumentNode>& replacements_;
    std::size_t swapped_ = 0; bool committed_ = false;
};
static_assert(std::is_nothrow_swappable_v<ArgumentNode>);
static_assert(std::is_nothrow_move_constructible_v<TriggerDocument>); // owned 参数间移动不会退化为第二份整图复制。
}

bool TriggerGraphScalarEditReceipt::Empty() const noexcept { return !state_ || state_->slots.empty(); }
const TriggerGraphEditReceipt& TriggerGraphScalarEditReceipt::Receipt() const noexcept {
    static const TriggerGraphEditReceipt empty; return state_ ? state_->receipt : empty;
}
const TriggerGraphScalarEditCost& TriggerGraphScalarEditReceipt::Cost() const noexcept {
    static const TriggerGraphScalarEditCost empty; return state_ ? state_->cost : empty;
}

std::string_view TriggerGraphScalarEditReceipt::CurrentWtgSha256(bool afterExpected) const noexcept {
    return state_ ? (afterExpected ? state_->afterIdentity : state_->beforeIdentity).CurrentWtgSha256() : std::string_view{};
}
std::string_view TriggerGraphScalarEditReceipt::CurrentWctSha256(bool afterExpected) const noexcept {
    return state_ ? (afterExpected ? state_->afterIdentity : state_->beforeIdentity).CurrentWctSha256() : std::string_view{};
}

Result<TriggerGraphScalarEditCost> InspectTriggerGraphScalarSourceCost(const TriggerDocument& document,
    const TriggerGraphEditLimits& limits) {
    TriggerGraphScalarEditCost cost; cost.workPolicy = limits.scalarWorkPolicy;
    if (!limits.maxScalarOwnedBytes || limits.maxScalarOwnedBytes > kScalarOwnedHardLimit
        || !limits.maxScalarRecords || limits.maxScalarRecords > 1048576 || !limits.maxScalarEdges || limits.maxScalarEdges > 2097152
        || !limits.maxScalarWorkUnits || limits.maxScalarWorkUnits > kScalarWorkHardLimit
        || static_cast<unsigned>(limits.scalarWorkPolicy) > static_cast<unsigned>(TriggerScalarWorkPolicy::LegacyKnownWork))
        return Failed<TriggerGraphScalarEditCost>(Fail("TRIGGER_GRAPH_SCALAR_LIMITS", "标量来源准入必须具有有限内存预算。"));
    ScalarDocumentSize size;
    const bool measured = MeasureScalarDocument(document, size, limits.maxScalarOwnedBytes, limits.maxScalarRecords, limits.maxScalarEdges);
    cost.sourceDocumentBytes = cost.candidateOwnedBytes = size.bytes;
    cost.recordVisitsCharged = size.records; cost.edgeVisitsCharged = size.edges;
    cost.copiedTextBytes = size.text; // 后续 candidate 复制将读取的字节；本函数没有复制。
    if (!measured) return Failed<TriggerGraphScalarEditCost>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_MEMORY_BUDGET", cost));
    return Result<TriggerGraphScalarEditCost>::Success(cost);
}

namespace {
Result<std::shared_ptr<ScalarState>> PrepareScalarOwnedImpl(TriggerDocument& document,
    const UiCatalog& catalog, const TriggerGraphEditBatch& batch, const TriggerGraphEditLimits& limits,
    const MapObjectReferenceSnapshot* mapObjects, std::stop_token stop, bool copiedInCore) {
    try {
        if (stop.stop_requested()) return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_CANCELLED", "准备已取消；私有输入未发布。"));
        if (const auto request = CheckGraphBatchRequest(batch, limits); !request.Succeeded()) return Failed<std::shared_ptr<ScalarState>>(request);
        if (!limits.maxScalarOwnedBytes || limits.maxScalarOwnedBytes > kScalarOwnedHardLimit || !limits.maxScalarWorkUnits
            || limits.maxScalarWorkUnits > kScalarWorkHardLimit || batch.operations.size() > 256
            || !limits.maxScalarRecords || limits.maxScalarRecords > 1048576 || !limits.maxScalarEdges || limits.maxScalarEdges > 2097152
            || static_cast<unsigned>(limits.scalarWorkPolicy) > static_cast<unsigned>(TriggerScalarWorkPolicy::LegacyKnownWork))
            return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_LIMITS", "紧凑标量准备必须具有有限内存/工作预算及最多 256 项。"));
        TriggerGraphScalarEditCost cost; cost.workPolicy = limits.scalarWorkPolicy; cost.ownedInputConsumed = !copiedInCore;
        cost.candidateCopies = copiedInCore ? 1 : 0; cost.additionalOwnedBytes = limits.scalarAdditionalOwnedBytes; cost.additionalWorkUnits = limits.scalarAdditionalWorkUnits;
        ScalarDocumentSize sourceSize;
        const bool sourceMeasured = MeasureScalarDocument(document, sourceSize, limits.maxScalarOwnedBytes, limits.maxScalarRecords, limits.maxScalarEdges);
        cost.sourceDocumentBytes = cost.candidateOwnedBytes = sourceSize.bytes;
        if (!sourceMeasured)
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_MEMORY_BUDGET", cost));
        if (!MeasureScalarTypePreparation(catalog, cost))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_CATALOG_BUDGET", cost));
        if (!FinishScalarCost(cost) || cost.peakOwnedBytes > limits.maxScalarOwnedBytes)
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_MEMORY_BUDGET", cost));
        std::vector<StableId> ids; ids.reserve(batch.operations.size()); std::set<StableId> unique;
        for (const auto& operation : batch.operations) {
            const auto* set = std::get_if<GraphSet>(&operation);
            if (!set || set->field != GraphField::ArgumentScalar || set->target.kind != GraphRecordKind::Argument
                || !set->target.id || !set->target.clientKey.empty() || !std::holds_alternative<GraphScalarValue>(set->expected)
                || !std::holds_alternative<GraphScalarValue>(set->value) || !unique.insert(set->target.id).second)
                return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_SHAPE", "紧凑路径仅接受唯一已有参数的 GraphSet 标量操作；不丢弃混合/重复项。"));
            ids.push_back(set->target.id);
        }
        // Guard 会复制真实 owner.uiSignature；先按已持有挂载元数据收费，防同一巨大签名乘选择数后才拒绝。
        // 这里只做边界/费用预检，实际唯一归属/类型仍全部由原 bulk Inspect 授权。
        for (const auto& owner : document.ecaNodes) for (const auto index : owner.arguments) {
            if (index >= document.argumentNodes.size()) return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_OWNER", "参数挂载索引越界。"));
            if (unique.contains(document.argumentNodes[index].id)
                && (!ScalarAdd(cost.guardScratchBytes, sizeof(ScalarArgumentReplacementGuard) + 128)
                    || !ScalarProduct(cost.guardScratchBytes, owner.uiSignature.size(), 2)))
                return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        }
        if (!FinishScalarCost(cost) || cost.peakOwnedBytes > limits.maxScalarOwnedBytes)
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_MEMORY_BUDGET", cost));
        if (stop.stop_requested()) return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_CANCELLED", "批量 guard 前取消；私有输入未发布。"));
        const auto guards = CaptureScalarArgumentReplacementGuards(document, catalog, ids, limits, mapObjects);
        cost.bulkGuardCaptures = 1;
        if (!guards.Succeeded() || !guards.value || guards.value->size() != ids.size()) return Failed<std::shared_ptr<ScalarState>>(guards);
        for (const auto& guarded : *guards.value) if (!guarded.guard) {
            if (guarded.diagnostics.empty()) return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_GUARD_MISSING", "标量请求没有取得同源实际 guard；整批拒绝。"));
            return Failed<std::shared_ptr<ScalarState>>(Status{guarded.diagnostics});
        }
        cost.guardScratchBytes = 0; // 成功捕获后换为实际输出的逻辑 owned 量。
        if (!ScalarProduct(cost.guardScratchBytes, guards.value->size(), sizeof(ScalarArgumentReplacementGuard)))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        std::map<StableId, std::size_t> argumentIndices, ownerIndices;
        for (std::size_t i = 0; i < document.argumentNodes.size(); ++i) if (unique.contains(document.argumentNodes[i].id)) argumentIndices.emplace(document.argumentNodes[i].id, i);
        std::set<StableId> owners;
        for (const auto& guarded : *guards.value) owners.insert(guarded.guard->ownerEcaId);
        for (std::size_t i = 0; i < document.ecaNodes.size(); ++i) if (owners.contains(document.ecaNodes[i].id)) ownerIndices.emplace(document.ecaNodes[i].id, i);
        cost.compactOwnedBytes = sizeof(ScalarState);
        cost.identityScratchBytes = 64 * 1024; // 已知流式 pending 下界；实际 capture getter 后取实际较大值。
        // 仅容器成员的逻辑值/数组账，allocator 节点/bucket/capacity 不假称 RSS。
        if (!ScalarProduct(cost.lookupScratchBytes, ids.size(), sizeof(StableId) * 3 + sizeof(std::pair<const StableId, std::size_t>) * 2)
            || !ScalarProduct(cost.validationScratchBytes, sourceSize.records, sizeof(StableId))
            || !ScalarProduct(cost.validationScratchBytes, document.ecaNodes.size() + document.argumentNodes.size(), sizeof(std::uint8_t) + sizeof(std::size_t))
            || !ScalarAdd(cost.validationScratchBytes, Detail::kCodecFixedValidationScratchBytes))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        struct MountSize { int kind; std::size_t parent; std::uint32_t ordinal; };
        if (!ScalarProduct(cost.mountIndexScratchBytes, document.ecaNodes.size() + document.argumentNodes.size(), sizeof(MountSize) + sizeof(std::uint32_t))
            || !ScalarProduct(cost.mountIndexScratchBytes, document.argumentNodes.size(), sizeof(std::pair<const StableId, std::uint32_t>)))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        // private State 固定字段 + source/current/digest 字符串；实际节点及 guard 文本下方逐项计费。
        if (!ScalarAdd(cost.compactOwnedBytes, 2 * (document.SourceWtgSha256().size() + document.SourceWctSha256().size() + catalog.Sha256().size()
            + 128 + (mapObjects ? mapObjects->Identity().size() : 0))) || !ScalarAdd(cost.compactOwnedBytes, 384))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        std::size_t addedText = 0;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto& guard = *guards.value->at(i).guard; const auto argument = argumentIndices.find(ids[i]); const auto owner = ownerIndices.find(guard.ownerEcaId);
            if (argument == argumentIndices.end() || owner == ownerIndices.end()) return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_OWNER", "已验证的参数/父 ECA 身份没有唯一实际位置。"));
            const auto& node = document.argumentNodes[argument->second]; const auto& wanted = std::get<GraphScalarValue>(std::get<GraphSet>(batch.operations[i]).value);
            std::size_t nodeBytes = 0, nodeText = 0;
            if (!MeasureScalarNode(node, nodeBytes, nodeText) || !ScalarAdd(addedText, wanted.value.size()) || !ScalarAdd(cost.guardScratchBytes, guard.uiSignature.size())
                || !ScalarAdd(cost.guardScratchBytes, guard.expectedType.size()) || !ScalarAdd(cost.guardScratchBytes, guard.subtreeSha256.size())
                || !ScalarAdd(cost.compactOwnedBytes, sizeof(detail::GraphScalarSlot) - 2 * sizeof(ArgumentNode))
                || !ScalarAdd(cost.compactOwnedBytes, nodeBytes) || !ScalarAdd(cost.compactOwnedBytes, nodeBytes - node.value.text.size() - node.value.originalBytes.size())
                || !ScalarAdd(cost.compactOwnedBytes, wanted.value.size()) || !ScalarAdd(cost.compactOwnedBytes, guard.uiSignature.size())
                || !ScalarAdd(cost.compactOwnedBytes, guard.expectedType.size()) || !ScalarAdd(cost.compactOwnedBytes, guard.subtreeSha256.size())
                || !ScalarAdd(cost.publicationScratchBytes, nodeBytes + wanted.value.size())
                || !ScalarAdd(cost.copiedTextBytes, 2 * nodeText + node.value.text.size() + 3 * wanted.value.size()
                    + guard.uiSignature.size() + guard.expectedType.size() + guard.subtreeSha256.size()))
                return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
            cost.operationScratchBytes = std::max(cost.operationScratchBytes, node.value.text.size() + 2 * wanted.value.size()
                + sizeof(GraphFieldValue) + sizeof(GraphScalarValue));
        }
        // 原 originalBytes.clear() 保留 vector capacity；不拿清空后的 size 给候选复制抵扣内存。
        // 也不假设 string move 一定缩小旧 capacity。原完整 D 加新值是有界保守持有账。
        cost.candidateOwnedBytes = sourceSize.bytes;
        if (!ScalarAdd(cost.candidateOwnedBytes, addedText) || !ScalarAdd(cost.copiedTextBytes, sourceSize.text))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        cost.candidateGrowthBytes = cost.candidateOwnedBytes > sourceSize.bytes ? cost.candidateOwnedBytes - sourceSize.bytes : 0;
        if (!FinishScalarCost(cost) || cost.peakOwnedBytes > limits.maxScalarOwnedBytes)
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_MEMORY_BUDGET", cost));
        if (stop.stop_requested()) return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_CANCELLED", "前身份捕获前取消；私有输入未发布。"));
        const auto before = CaptureTriggerGraphReplayIdentity(document, catalog); if (!before.Succeeded()) return Failed<std::shared_ptr<ScalarState>>(before);
        const auto binding = ScalarBinding(document, catalog, *before.value, mapObjects);
        // 新 const Prepare 的 source-only 模式：本次实际 capture 自行建立完整当前身份。
        // general Apply 不接收该空 current 模式；半空或客户端提供的 stale current 均拒绝。
        const bool sourceOnly = batch.expected.currentWtgSha256.empty() && batch.expected.currentWctSha256.empty();
        const bool sourceMatches = batch.expected.sourceWtgSha256 == binding.sourceWtgSha256
            && batch.expected.sourceWctSha256 == binding.sourceWctSha256 && batch.expected.uiCatalogSha256 == binding.uiCatalogSha256
            && batch.expected.mapObjectReferencesSha256 == binding.mapObjectReferencesSha256;
        if (!sourceMatches || (!sourceOnly && !SameBinding(binding, batch.expected)))
            return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SOURCE_STALE", "来源、Catalog、当前配对或预放对象身份已变化；半空 current 身份亦拒绝。"));
        if (!ChargeScalarIdentity(cost, *before.value) || !FinishScalarCost(cost) || cost.peakOwnedBytes > limits.maxScalarOwnedBytes)
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_MEMORY_BUDGET", cost));
        // 改前只冻结所选完整节点与既有代码槽；不再拥有第二份整图。
        ScalarDocumentSize protectedSize; protectedSize.bytes = sizeof(GraphCodeProtection);
        if (!protectedSize.Raw(document.wct.customCode) || !protectedSize.Raw(document.wct.customComment))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        for (const auto& old : document.triggers) if (old.customTextFlag)
            if (!ScalarAdd(protectedSize.bytes, sizeof(GraphCodeProtection::Custom)) || !protectedSize.Raw(old.customText))
                return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        cost.codeProtectionScratchBytes = protectedSize.bytes;
        if (!ScalarAdd(cost.copiedTextBytes, protectedSize.text) || !FinishScalarCost(cost) || cost.peakOwnedBytes > limits.maxScalarOwnedBytes)
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_MEMORY_BUDGET", cost));
        auto state = std::make_shared<ScalarState>(); state->slots.reserve(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto& guard = *guards.value->at(i).guard; const auto index = argumentIndices.at(ids[i]);
            state->slots.push_back({index, ownerIndices.at(guard.ownerEcaId), guard, document.argumentNodes[index], {}});
        }
        const auto protectedCode = CaptureGraphCodeProtection(document);
        if (stop.stop_requested()) return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_CANCELLED", "修改私有输入前取消；没有发布。"));
        auto& candidate = document;
        auto edited = ApplyPrivateGraphCandidate(protectedCode, candidate, catalog, batch, binding, limits, mapObjects);
        cost.candidateValidations = 1;
        if (!edited.Succeeded()) return Failed<std::shared_ptr<ScalarState>>(edited);
        if (stop.stop_requested()) return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_CANCELLED", "私有输入已修改但准备取消；直接丢弃，不发布。"));
        const auto after = CaptureTriggerGraphReplayIdentity(candidate, catalog); if (!after.Succeeded()) return Failed<std::shared_ptr<ScalarState>>(after);
        edited.value->after = ScalarBinding(candidate, catalog, *after.value, mapObjects);
        if (!ChargeScalarIdentity(cost, *after.value)) return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        // 已知费用估计，不是工作上界或插桩次数：尚未覆盖 locals/ExpectedType 等全部重复扫描。
        // 两次编码/identity 元信息、尺寸/挂载/index/拷贝/NextId/ValidateTouched 的线性部分。
        if (!ScalarProduct(cost.recordVisitsCharged, sourceSize.records, 32) || !ScalarProduct(cost.edgeVisitsCharged, sourceSize.edges, 16))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        // 原 Editor GraphSet 按 StableId 做 Exists、Field、SetScalarArgument 三次 Find；不得冒为 O(1)。
        for (const auto id : ids) if (!ScalarProduct(cost.recordVisitsCharged, argumentIndices.at(id) + 1, 3))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        // 代码保持门仅按真实 custom-only 快照执行 Find；仍记其实际位置的已知费用估计。
        for (const auto& old : protectedCode.customs) {
            const auto now = std::find_if(document.triggers.begin(), document.triggers.end(), [&](const auto& row) { return row.id == old.id; });
            if (!ScalarAdd(cost.recordVisitsCharged, static_cast<std::size_t>(std::distance(document.triggers.begin(), now)) + 1))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        }
        if (!FinishScalarCost(cost))
            return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_COST_OVERFLOW", cost));
        if (cost.peakOwnedBytes > limits.maxScalarOwnedBytes) return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_MEMORY_BUDGET", cost));
        if (limits.scalarWorkPolicy == TriggerScalarWorkPolicy::LegacyKnownWork && cost.workUnitsCharged > limits.maxScalarWorkUnits) return Failed<std::shared_ptr<ScalarState>>(ScalarCostFailure("TRIGGER_GRAPH_SCALAR_WORK_BUDGET", cost));
        for (auto& slot : state->slots) slot.after = candidate.argumentNodes[slot.argumentIndex];
        if (stop.stop_requested()) return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_CANCELLED", "终态收据返回前取消；未发布。"));
        state->receipt = std::move(*edited.value); state->beforeIdentity = *before.value; state->afterIdentity = *after.value;
        state->policy = limits.scalarWorkPolicy; state->cost = cost; state->maxOwnedBytes = limits.maxScalarOwnedBytes; state->maxWorkUnits = limits.maxScalarWorkUnits;
        auto result = Result<std::shared_ptr<ScalarState>>::Success(std::move(state));
        result.diagnostics = std::move(edited.diagnostics);
        return result;
    } catch (const std::bad_alloc&) { return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_ALLOCATION_FAILED", "紧凑候选分配失败；未发布源。")); }
    catch (...) { return Failed<std::shared_ptr<ScalarState>>(Fail("TRIGGER_GRAPH_SCALAR_PREPARE_FAILED", "紧凑候选准备失败；未发布源。")); }
}
}
Result<TriggerGraphScalarEditReceipt> PrepareOwnedTriggerGraphScalarEditBatch(TriggerDocument ownedDocument,
    const UiCatalog& catalog, const TriggerGraphEditBatch& batch, const TriggerGraphEditLimits& limits,
    const MapObjectReferenceSnapshot* mapObjects, std::stop_token stop) {
    const auto state = PrepareScalarOwnedImpl(ownedDocument, catalog, batch, limits, mapObjects, stop, false);
    if (!state.Succeeded()) return Failed<TriggerGraphScalarEditReceipt>(state);
    TriggerGraphScalarEditReceipt result; result.state_ = *state.value;
    auto output = Result<TriggerGraphScalarEditReceipt>::Success(std::move(result));
    output.diagnostics = state.diagnostics;
    return output;
}
Result<TriggerGraphScalarEditReceipt> PrepareTriggerGraphScalarEditBatch(const TriggerDocument& document,
    const UiCatalog& catalog, const TriggerGraphEditBatch& batch, const TriggerGraphEditLimits& limits,
    const MapObjectReferenceSnapshot* mapObjects) {
    // const 入口先做同元数据准入，之后唯一一次 clone；和 owned 入口没有第二套编辑算法。
    const auto source = InspectTriggerGraphScalarSourceCost(document, limits);
    if (!source.Succeeded()) return Failed<TriggerGraphScalarEditReceipt>(source);
    try {
        TriggerDocument candidate = document;
        const auto state = PrepareScalarOwnedImpl(candidate, catalog, batch, limits, mapObjects, {}, true);
        if (!state.Succeeded()) return Failed<TriggerGraphScalarEditReceipt>(state);
        TriggerGraphScalarEditReceipt result; result.state_ = *state.value;
        auto output = Result<TriggerGraphScalarEditReceipt>::Success(std::move(result));
        output.diagnostics = state.diagnostics;
        return output;
    } catch (...) { return Failed<TriggerGraphScalarEditReceipt>(Fail("TRIGGER_GRAPH_SCALAR_ALLOCATION_FAILED", "唯一私有候选复制失败；未发布源。")); }
}

Status VerifyTriggerGraphScalarReplay(const TriggerDocument& document, const UiCatalog& catalog,
    const TriggerGraphScalarEditReceipt& receipt, bool afterExpected) {
    if (receipt.Empty()) return Fail("TRIGGER_GRAPH_SCALAR_UNBOUND", "没有实际 Core 创建的紧凑收据。");
    try {
        const auto& state = *receipt.state_;
        if (!state.maxOwnedBytes || !state.maxWorkUnits || state.cost.peakOwnedBytes > state.maxOwnedBytes || (state.policy == TriggerScalarWorkPolicy::LegacyKnownWork && state.cost.workUnitsCharged > state.maxWorkUnits)
            || static_cast<unsigned>(state.policy) > static_cast<unsigned>(TriggerScalarWorkPolicy::LegacyKnownWork))
            return Fail("TRIGGER_GRAPH_SCALAR_COST_INVALID", "紧凑收据没有同 source/policy/逻辑预算准入凭据；工作估计不是完整上界。");
        if (const auto identity = VerifyTriggerGraphReplayIdentity(document, catalog, afterExpected ? state.afterIdentity : state.beforeIdentity); !identity.Succeeded()) return identity;
        return CheckScalarSlots(document, state, afterExpected);
    } catch (...) { return Fail("TRIGGER_GRAPH_SCALAR_VERIFY_FAILED", "紧凑收据校验失败；未发布源。"); }
}
Status ReplayTriggerGraphScalarEdit(TriggerDocument& document, const UiCatalog& catalog,
    const TriggerGraphScalarEditReceipt& receipt, bool forward) {
    if (const auto verified = VerifyTriggerGraphScalarReplay(document, catalog, receipt, !forward); !verified.Succeeded()) return verified;
    try {
        const auto& state = *receipt.state_; std::vector<ArgumentNode> replacements; replacements.reserve(state.slots.size());
        for (const auto& slot : state.slots) replacements.push_back(forward ? slot.after : slot.before);
        ScalarSwapRollback rollback(document, state, replacements); rollback.SwapAll();
        if (const auto identity = VerifyTriggerGraphReplayIdentity(document, catalog, forward ? state.afterIdentity : state.beforeIdentity); !identity.Succeeded()) return identity;
        if (const auto slots = CheckScalarSlots(document, state, forward); !slots.Succeeded()) return slots;
        rollback.Commit(); return Status::Ok();
    } catch (...) { return Fail("TRIGGER_GRAPH_SCALAR_REPLAY_FAILED", "紧凑收据重放失败；源已保持或完整回滚。"); }
}
