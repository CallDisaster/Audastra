// 原创隔离控制台夹具；无作者地图、游戏、GPU 或作者设置输入。
// 仅测试自有 PMR upstream 拒绝一次 reserve 请求；公共 Validate 没有 OOM 注入。
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "../../src/Trigger/TriggerStableIdReservation.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <new>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
// 不修改 default_resource、CRT 或全局 new；计数只由真实 upstream do_allocate 产生。
class ReserveFailureResource final : public std::pmr::memory_resource {
public:
    void BeginReserveFailure() noexcept { armed_ = true; remaining_ = 1; }
    void EndReserveFailure() noexcept { armed_ = false; }
    std::size_t reserveRequests = 0, deniedRequests = 0, deniedBytes = 0, deniedAlignment = 0;
    std::size_t forwardedRequests = 0;
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (armed_) {
            ++reserveRequests;
            if (remaining_ != 0) {
                --remaining_; ++deniedRequests; deniedBytes = bytes; deniedAlignment = alignment;
                throw std::bad_alloc{};
            }
        }
        ++forwardedRequests;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    bool armed_ = false;
    std::size_t remaining_ = 0;
};

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw std::runtime_error("owned Codec reservation fixture failed");
    return std::move(*result.value);
}
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    Require(!std::filesystem::exists(path), "owned catalog must not overwrite existing file");
    std::ofstream output(path, std::ios::binary); output.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(output.good(), "owned catalog write");
}
namespace LegacyCodecDiagnosticPaths {
Diagnostic MakeDiagnostic(
    DiagnosticSeverity severity,
    std::string code,
    std::string message,
    std::optional<OriginSpan> origin = std::nullopt) {
    return Diagnostic{
        severity,
        "Trigger.WtgWct",
        std::move(code),
        std::move(message),
        std::move(origin),
    };
}

struct GraphState {
    std::vector<std::uint8_t> ecaState;
    std::vector<std::uint8_t> argumentState;
    std::vector<std::size_t> ecaReferences;
    std::vector<std::size_t> argumentReferences;
};

void AddValidationError(Status& status, std::string code, std::string message) {
    status.diagnostics.push_back(MakeDiagnostic(
        DiagnosticSeverity::Error, std::move(code), std::move(message)));
}

void ValidateArgument(
    std::uint32_t index,
    StableId owner,
    const TriggerDocument& document,
    const UiCatalog& catalog,
    GraphState& graph,
    Status& status,
    std::string path,
    std::size_t depth);

void ValidateEca(
    std::uint32_t index,
    bool child,
    StableId owner,
    const TriggerDocument& document,
    const UiCatalog& catalog,
    GraphState& graph,
    Status& status,
    std::string path,
    std::size_t depth) {
    if (depth > 256) {
        AddValidationError(status, "wtg.graph.depth", path + ": nesting exceeds 256 levels");
        return;
    }
    if (index >= document.ecaNodes.size()) {
        AddValidationError(status, "wtg.eca.bad-index", path + ": ECA index is out of range");
        return;
    }
    ++graph.ecaReferences[index];
    if (graph.ecaReferences[index] > 1) {
        AddValidationError(status, "wtg.eca.shared-node", path + ": ECA node has multiple owners");
        return;
    }
    if (graph.ecaState[index] == 1) {
        AddValidationError(status, "wtg.eca.cycle", path + ": ECA graph contains a cycle");
        return;
    }
    graph.ecaState[index] = 1;
    const EcaNode& eca = document.ecaNodes[index];
    const UiDefinition* definition = catalog.Find(eca.type, eca.name.text);
    if (definition == nullptr) {
        AddValidationError(status, "wtg.eca.unknown-ui", path + ": unknown UI definition [" + eca.name.text + "]");
    } else {
        if (definition->ArgumentCount() != eca.arguments.size()) {
            AddValidationError(status, "wtg.eca.argument-count", path +
                ": argument count does not match " + definition->Signature());
        }
    }
    if (eca.id == 0) {
        AddValidationError(status, "wtg.eca.missing-id", path + ": stable id is zero");
    }
    if (eca.enabled != 0 && eca.enabled != 1) {
        AddValidationError(status, "wtg.eca.enabled", path + ": enabled must be 0 or 1");
    }
    if (child != eca.childId.has_value()) {
        AddValidationError(status, "wtg.eca.child-id", path +
            (child ? ": child ECA is missing childId" : ": root/call ECA contains childId"));
    }
    for (std::size_t argument = 0; argument < eca.arguments.size(); ++argument) {
        ValidateArgument(
            eca.arguments[argument], owner, document, catalog, graph, status,
            path + "/arguments/" + std::to_string(argument), depth + 1);
    }
    for (std::size_t childIndex = 0; childIndex < eca.children.size(); ++childIndex) {
        ValidateEca(
            eca.children[childIndex], true, owner, document, catalog, graph, status,
            path + "/children/" + std::to_string(childIndex), depth + 1);
    }
    graph.ecaState[index] = 2;
}

void ValidateArgument(
    std::uint32_t index,
    StableId owner,
    const TriggerDocument& document,
    const UiCatalog& catalog,
    GraphState& graph,
    Status& status,
    std::string path,
    std::size_t depth) {
    if (depth > 256) {
        AddValidationError(status, "wtg.graph.depth", path + ": nesting exceeds 256 levels");
        return;
    }
    if (index >= document.argumentNodes.size()) {
        AddValidationError(status, "wtg.argument.bad-index", path + ": argument index is out of range");
        return;
    }
    ++graph.argumentReferences[index];
    if (graph.argumentReferences[index] > 1) {
        AddValidationError(status, "wtg.argument.shared-node", path + ": argument node has multiple owners");
        return;
    }
    if (graph.argumentState[index] == 1) {
        AddValidationError(status, "wtg.argument.cycle", path + ": argument graph contains a cycle");
        return;
    }
    graph.argumentState[index] = 1;
    const ArgumentNode& argument = document.argumentNodes[index];
    if (argument.id == 0) {
        AddValidationError(status, "wtg.argument.missing-id", path + ": stable id is zero");
    }
    if (argument.type < -1 || argument.type > 3) {
        AddValidationError(status, "wtg.argument.type", path + ": type must be in -1..3");
    }
    if (argument.insertCall != 0 && argument.insertCall != 1) {
        AddValidationError(status, "wtg.argument.insert-call", path + ": insertCall must be 0 or 1");
    }
    if ((argument.insertCall == 1) != argument.callEca.has_value()) {
        AddValidationError(status, "wtg.argument.call-presence", path +
            ": insertCall and call ECA presence disagree");
    }
    if (argument.callEca.has_value()) {
        ValidateEca(
            *argument.callEca, false, owner, document, catalog, graph, status,
            path + "/call", depth + 1);
    }
    if (argument.insertIndex != 0 && argument.insertIndex != 1) {
        AddValidationError(status, "wtg.argument.insert-index", path + ": insertIndex must be 0 or 1");
    }
    if ((argument.insertIndex == 1) != argument.indexArgument.has_value()) {
        AddValidationError(status, "wtg.argument.index-presence", path +
            ": insertIndex and index argument presence disagree");
    }
    if (argument.indexArgument.has_value()) {
        ValidateArgument(
            *argument.indexArgument, owner, document, catalog, graph, status,
            path + "/index", depth + 1);
    }
    graph.argumentState[index] = 2;
}

void ValidateGraph(
    const TriggerDocument& document,
    const UiCatalog& catalog,
    Status& status) {
    GraphState graph{
        std::vector<std::uint8_t>(document.ecaNodes.size()),
        std::vector<std::uint8_t>(document.argumentNodes.size()),
        std::vector<std::size_t>(document.ecaNodes.size()),
        std::vector<std::size_t>(document.argumentNodes.size()),
    };
    for (std::size_t triggerIndex = 0; triggerIndex < document.triggers.size(); ++triggerIndex) {
        const TriggerRecord& trigger = document.triggers[triggerIndex];
        for (std::size_t ecaIndex = 0; ecaIndex < trigger.ecas.size(); ++ecaIndex) {
            ValidateEca(
                trigger.ecas[ecaIndex], false, trigger.id, document, catalog, graph, status,
                "/triggers/" + std::to_string(triggerIndex) + "/ecas/" +
                    std::to_string(ecaIndex), 0);
        }
    }
    for (std::size_t index = 0; index < graph.ecaReferences.size(); ++index) {
        if (graph.ecaReferences[index] == 0) {
            AddValidationError(status, "wtg.eca.orphan", "/ecaNodes/" +
                std::to_string(index) + ": node is not owned by a trigger");
        }
    }
    for (std::size_t index = 0; index < graph.argumentReferences.size(); ++index) {
        if (graph.argumentReferences[index] == 0) {
            AddValidationError(status, "wtg.argument.orphan", "/argumentNodes/" +
                std::to_string(index) + ": node is not owned by an ECA");
        }
    }
}

} // namespace LegacyCodecDiagnosticPaths

namespace LegacyCodecStableIds {
using LegacyCodecDiagnosticPaths::AddValidationError;
using LegacyCodecDiagnosticPaths::ValidateGraph;
Status Validate(
    const TriggerDocument& document,
    const UiCatalog& catalog) {
    Status status;
    if (!document.HasTrustedSourceSeal()) {
        AddValidationError(
            status,
            "wtg.source-seal.missing",
            "WTG 文档缺少解析器建立的可信来源 seal，禁止验证或序列化。");
    } else if (document.SourceWtgSha256().size() != 64 ||
        document.SourceWctSha256().size() != 64 ||
        document.UiCatalogSha256().size() != 64 ||
        document.UiCatalogSha256() != catalog.Sha256()) {
        AddValidationError(
            status,
            "wtg.source-seal.identity-mismatch",
            "WTG 来源 seal 或 UI Catalog 身份不匹配，禁止验证或序列化。");
    }
    const std::int32_t expectedGameVersion = document.wtgVersion == kWtgLegacyVersion ? 1 : 2;
    if (document.variablesHeader != expectedGameVersion) {
        AddValidationError(status, "wtg.variables-header", "WTG v" + std::to_string(document.wtgVersion)
            + " variablesHeader (game version) must be " + std::to_string(expectedGameVersion));
    }
    if (document.wct.version != kWctVersion && document.wct.version != kWctLegacyVersion) {
        AddValidationError(status, "wct.version", "WCT version must be 0 or 1");
    }

    std::set<std::int32_t> categoryIds;
    // 局部 arena 只改变临时集合分配，保留原增长与完整身份校验。
    alignas(std::max_align_t) std::array<std::byte, 16 * 1024> stableIdStorage;
    std::pmr::monotonic_buffer_resource stableIdResource(
        stableIdStorage.data(), stableIdStorage.size(), std::pmr::new_delete_resource());
    std::pmr::unordered_set<StableId> stableIds{std::pmr::polymorphic_allocator<StableId>{&stableIdResource}};
    for (std::size_t index = 0; index < document.categories.size(); ++index) {
        const CategoryRecord& category = document.categories[index];
        if (!categoryIds.insert(category.categoryId).second) {
            AddValidationError(status, "wtg.category.duplicate-id", "/categories/" +
                std::to_string(index) + ": duplicate category id");
        }
        if (category.comment != 0 && category.comment != 1) {
            AddValidationError(status, "wtg.category.comment", "/categories/" +
                std::to_string(index) + ": comment must be 0 or 1");
        }
        if (category.id == 0 || !stableIds.insert(category.id).second) {
            AddValidationError(status, "wtg.stable-id", "/categories/" +
                std::to_string(index) + ": stable id is zero or duplicated");
        }
    }

    std::set<std::string> variableNames;
    for (std::size_t index = 0; index < document.variables.size(); ++index) {
        const VariableRecord& variable = document.variables[index];
        if (!variableNames.insert(variable.name.text).second) {
            AddValidationError(status, "wtg.variable.duplicate-name", "/variables/" +
                std::to_string(index) + ": duplicate variable name");
        }
        if ((variable.isArray != 0 && variable.isArray != 1) ||
            (variable.hasDefault != 0 && variable.hasDefault != 1)) {
            AddValidationError(status, "wtg.variable.flags", "/variables/" +
                std::to_string(index) + ": array/default flags must be 0 or 1");
        }
        if (variable.id == 0 || !stableIds.insert(variable.id).second) {
            AddValidationError(status, "wtg.stable-id", "/variables/" +
                std::to_string(index) + ": stable id is zero or duplicated");
        }
    }

    for (std::size_t index = 0; index < document.triggers.size(); ++index) {
        const TriggerRecord& trigger = document.triggers[index];
        const std::string path = "/triggers/" + std::to_string(index);
        if (!categoryIds.contains(trigger.categoryId)) {
            AddValidationError(status, "wtg.trigger.category", path + ": category does not exist");
        }
        for (const auto [flag, name] : std::array<std::pair<std::int32_t, const char*>, 4>{
                 std::pair{trigger.enabled, "enabled"},
                 std::pair{trigger.customTextFlag, "customTextFlag"},
                 std::pair{trigger.initiallyClosed, "initiallyClosed"},
                 std::pair{trigger.runOnInit, "runOnInit"}}) {
            if (flag != 0 && flag != 1) {
                AddValidationError(status, "wtg.trigger.flag", path + ": " + name + " must be 0 or 1");
            }
        }
        if (trigger.id == 0 || !stableIds.insert(trigger.id).second) {
            AddValidationError(status, "wtg.stable-id", path + ": stable id is zero or duplicated");
        }
    }
    for (std::size_t index = 0; index < document.ecaNodes.size(); ++index) {
        const StableId id = document.ecaNodes[index].id;
        if (id == 0 || !stableIds.insert(id).second) {
            AddValidationError(status, "wtg.stable-id", "/ecaNodes/" +
                std::to_string(index) + ": stable id is zero or duplicated");
        }
    }
    for (std::size_t index = 0; index < document.argumentNodes.size(); ++index) {
        const StableId id = document.argumentNodes[index].id;
        if (id == 0 || !stableIds.insert(id).second) {
            AddValidationError(status, "wtg.stable-id", "/argumentNodes/" +
                std::to_string(index) + ": stable id is zero or duplicated");
        }
    }
    ValidateGraph(document, catalog, status);
    return status;
}

} // namespace LegacyCodecStableIds

void SameDiagnostics(const Status& a, const Status& b) {
    Require(a.Succeeded() == b.Succeeded() && a.diagnostics.size() == b.diagnostics.size(), "complete old diagnostic result/count");
    for (std::size_t i = 0; i < a.diagnostics.size(); ++i) {
        const auto& x = a.diagnostics[i]; const auto& y = b.diagnostics[i];
        Require(x.severity == y.severity && x.subsystem == y.subsystem && x.code == y.code && x.message == y.message
            && !x.origin && !y.origin, "old severity/subsystem/code/message/order and no origin are exact");
    }
}
bool SameRaw(const RawText& a, const RawText& b) {
    return a.text == b.text && a.encoding == b.encoding && a.originalBytes == b.originalBytes;
}
bool SameOrigin(const OriginSpan& a, const OriginSpan& b) {
    return a.source == b.source && a.offset == b.offset && a.length == b.length && a.fingerprint == b.fingerprint;
}
void SameMetadata(const TriggerDocument& a, const TriggerDocument& b) {
    Require(a.HasTrustedSourceSeal() == b.HasTrustedSourceSeal() && a.SourceWtgSha256() == b.SourceWtgSha256()
        && a.SourceWctSha256() == b.SourceWctSha256() && a.UiCatalogSha256() == b.UiCatalogSha256()
        && a.variablesHeader == b.variablesHeader && a.wct.version == b.wct.version
        && SameRaw(a.wct.customComment, b.wct.customComment) && SameRaw(a.wct.customCode, b.wct.customCode), "source/header unchanged");
    Require(a.categories.size() == b.categories.size() && a.variables.size() == b.variables.size() && a.triggers.size() == b.triggers.size()
        && a.ecaNodes.size() == b.ecaNodes.size() && a.argumentNodes.size() == b.argumentNodes.size(), "all five vector sizes unchanged");
    for (std::size_t i = 0; i < a.categories.size(); ++i) {
        const auto& x = a.categories[i]; const auto& y = b.categories[i];
        Require(x.id == y.id && x.categoryId == y.categoryId && x.comment == y.comment && SameRaw(x.name, y.name) && SameOrigin(x.origin, y.origin), "category full metadata");
    }
    for (std::size_t i = 0; i < a.variables.size(); ++i) {
        const auto& x = a.variables[i]; const auto& y = b.variables[i];
        Require(x.id == y.id && SameRaw(x.name, y.name) && SameRaw(x.typeName, y.typeName) && SameRaw(x.value, y.value)
            && x.unknown == y.unknown && x.isArray == y.isArray && x.size == y.size && x.hasDefault == y.hasDefault && SameOrigin(x.origin, y.origin), "variable full metadata");
    }
    for (std::size_t i = 0; i < a.triggers.size(); ++i) {
        const auto& x = a.triggers[i]; const auto& y = b.triggers[i];
        Require(x.id == y.id && SameRaw(x.name, y.name) && SameRaw(x.description, y.description) && SameRaw(x.customText, y.customText)
            && x.type == y.type && x.enabled == y.enabled && x.customTextFlag == y.customTextFlag && x.initiallyClosed == y.initiallyClosed
            && x.runOnInit == y.runOnInit && x.categoryId == y.categoryId && x.ecas == y.ecas && SameOrigin(x.origin, y.origin), "trigger full metadata");
    }
    for (std::size_t i = 0; i < a.ecaNodes.size(); ++i) {
        const auto& x = a.ecaNodes[i]; const auto& y = b.ecaNodes[i];
        Require(x.id == y.id && x.sourceTriggerId == y.sourceTriggerId && x.type == y.type && x.childId == y.childId
            && SameRaw(x.name, y.name) && x.enabled == y.enabled && x.arguments == y.arguments && x.children == y.children
            && x.uiSource == y.uiSource && x.uiLayer == y.uiLayer && x.uiSignature == y.uiSignature && SameOrigin(x.origin, y.origin), "ECA full metadata");
    }
    for (std::size_t i = 0; i < a.argumentNodes.size(); ++i) {
        const auto& x = a.argumentNodes[i]; const auto& y = b.argumentNodes[i];
        Require(x.id == y.id && x.type == y.type && SameRaw(x.value, y.value) && x.insertCall == y.insertCall
            && x.callEca == y.callEca && x.insertIndex == y.insertIndex && x.indexArgument == y.indexArgument && SameOrigin(x.origin, y.origin), "argument full metadata");
    }
}
bool HasCode(const Status& status, std::string_view code) {
    return std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [&](const auto& value) { return value.code == code; });
}
TriggerDocument Fixture(const UiCatalog& catalog, std::size_t records) {
    Require(records >= 6 && records <= 16384, "test only materializes bounded small synthetic IR");
    auto document = Take(WtgWctCodec{}.CreateEmpty(catalog));
    Take(TriggerDirectoryEditor::AddCategory(document, "Reservation"));
    const auto triggerId = Take(TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "Reservation"));
    StableId next = (std::max)(document.categories.front().id, triggerId) + 1;
    VariableRecord variable; variable.id = next++; variable.name.text = "OwnedInteger"; variable.typeName.text = "integer";
    document.variables.push_back(std::move(variable));
    document.ecaNodes.reserve(records - 5); document.argumentNodes.reserve(2);
    auto& trigger = document.triggers.front(); trigger.ecas.reserve(records - 5);
    const auto append = [&](std::string_view name, std::size_t arguments) {
        EcaNode eca; eca.id = next++; eca.sourceTriggerId = triggerId;
        // WTG 的 Action 编号为 2；UiDefinitionKind 的分类枚举不能当 wire 编号。
        eca.type = 2; eca.name.text = name;
        for (std::size_t index = 0; index < arguments; ++index) {
            ArgumentNode argument; argument.id = next++; argument.type = 3; argument.value.text = "1";
            eca.arguments.push_back(static_cast<std::uint32_t>(document.argumentNodes.size()));
            document.argumentNodes.push_back(std::move(argument));
        }
        trigger.ecas.push_back(static_cast<std::uint32_t>(document.ecaNodes.size()));
        document.ecaNodes.push_back(std::move(eca));
    };
    append("SetVariable", 2);
    for (std::size_t count = 6; count < records; ++count) append("DoNothing", 0);
    Require(document.categories.size() + document.variables.size() + document.triggers.size()
        + document.ecaNodes.size() + document.argumentNodes.size() == records, "actual five-domain fixture count");
    Require(document.HasTrustedSourceSeal(), "actual unique Codec/CreateEmpty parser seal");
    return document;
}
void PolicyBoundariesAndSoftFailure() {
    using namespace War3Map::Trigger::Detail;
    Require(StableIdReservationCount({4095, 0, 0, 0, 0}) == 0, "small domain preserves growth");
    Require(StableIdReservationCount({1, 1, 1, 1, 4092}) == 4096, "actual five-domain inclusive threshold");
    Require(StableIdReservationCount({4097, 0, 0, 0, 0}) == 4097, "minimum plus one");
    Require(StableIdReservationCount({kStableIdReserveMaximum - 1, 0, 0, 0, 0}) == kStableIdReserveMaximum - 1, "cap minus one");
    Require(StableIdReservationCount({1, 1, 1, 1, kStableIdReserveMaximum - 4}) == kStableIdReserveMaximum, "inclusive cap");
    Require(StableIdReservationCount({kStableIdReserveMaximum, 1, 0, 0, 0}) == 0, "above cap skips only optional reservation");
    Require(StableIdReservationCount({(std::numeric_limits<std::size_t>::max)(), 1, 0, 0, 0}) == 0, "checked overflow");
    Require(StableIdReservationCount({0, 0, 0, 0, 0}) == 0, "empty domain");
    struct RefusingSet {
        int mode = 0; std::size_t calls = 0;
        void reserve(std::size_t) {
            ++calls;
            if (mode == 0) throw std::bad_alloc{};
            if (mode == 1) throw std::length_error("owned reserve limit");
            throw std::runtime_error("unexpected reserve failure");
        }
    };
    for (const int mode : {0, 1}) {
        RefusingSet set; set.mode = mode;
        ReserveStableIdBuckets(set, {4096, 0, 0, 0, 0}); Require(set.calls == 1, "only expected optimization failure swallowed");
        ReserveStableIdBuckets(set, {4095, 0, 0, 0, 0}); Require(set.calls == 1, "small domain never invokes reserve");
        ReserveStableIdBuckets(set, {kStableIdReserveMaximum, 1, 0, 0, 0}); Require(set.calls == 1, "over-cap does not allocate");
    }
    RefusingSet unexpected; unexpected.mode = 2; bool thrown = false;
    try { ReserveStableIdBuckets(unexpected, {4096, 0, 0, 0, 0}); } catch (const std::runtime_error&) { thrown = true; }
    Require(thrown, "unrelated exception is never silently swallowed");
}
void ActualFullDiagnosticAndWireBoundaries(const UiCatalog& catalog, const UiCatalog& other) {
    for (const auto records : {std::size_t{4095}, std::size_t{4096}, std::size_t{4097}, std::size_t{16384}}) {
        auto document = Fixture(catalog, records); const auto before = document;
        const auto expected = LegacyCodecStableIds::Validate(document, catalog); const auto actual = WtgWctCodec{}.Validate(document, catalog);
        Require(expected.Succeeded(), "independent original full Validate accepts real all-domain fixture"); SameDiagnostics(actual, expected);
        const auto wire = Take(WtgWctCodec{}.Serialize(document, catalog));
        const auto parsed = Take(WtgWctCodec{}.Parse(wire.wtg, wire.wct, catalog));
        const auto roundtrip = Take(WtgWctCodec{}.Serialize(parsed, catalog));
        Require(roundtrip.wtg == wire.wtg && roundtrip.wct == wire.wct, "valid complete wire/Parse roundtrip"); SameMetadata(document, before);
    }
    const auto base = Fixture(catalog, 4096);
    for (int mutation = 0; mutation < 10; ++mutation) {
        auto document = base;
        switch (mutation) {
        case 0: document.categories.front().id = 0; break;
        case 1: document.variables.front().id = document.categories.front().id; break;
        case 2: document.triggers.front().id = document.variables.front().id; break;
        case 3: document.ecaNodes.back().id = document.triggers.front().id; break;
        case 4: document.argumentNodes.front().id = document.ecaNodes.front().id; break;
        case 5: document.ecaNodes.back().id = document.ecaNodes.front().id; break;
        case 6: document.variablesHeader = 3; document.wct.version = 2; break;
        case 7: document.argumentNodes.back().insertCall = 1; document.triggers.front().ecas.pop_back(); break;
        case 8: document.categories.front().comment = 2; document.triggers.front().enabled = 2;
            document.variables.front().isArray = 2; document.ecaNodes.front().enabled = 2; break;
        case 9: {
            auto duplicate = document.categories.front(); duplicate.id = 1000000; document.categories.push_back(std::move(duplicate)); break;
        }
        default: throw std::runtime_error("unknown fixture mutation");
        }
        const auto before = document; const auto expected = LegacyCodecStableIds::Validate(document, catalog);
        Require(!expected.Succeeded(), "real malformed negative"); SameDiagnostics(WtgWctCodec{}.Validate(document, catalog), expected);
        const auto wire = WtgWctCodec{}.Serialize(document, catalog);
        Require(!wire.Succeeded() && !wire.value, "malformed document never produces partial wire"); SameDiagnostics(Status{wire.diagnostics}, expected);
        SameMetadata(document, before);
    }
    TriggerDocument noSeal;
    noSeal.categories = base.categories; noSeal.variables = base.variables; noSeal.triggers = base.triggers;
    noSeal.ecaNodes = base.ecaNodes; noSeal.argumentNodes = base.argumentNodes; noSeal.wct = base.wct;
    const auto untrusted = LegacyCodecStableIds::Validate(noSeal, catalog);
    Require(HasCode(untrusted, "wtg.source-seal.missing"), "actual source seal absence is still a diagnostic");
    SameDiagnostics(WtgWctCodec{}.Validate(noSeal, catalog), untrusted);
    const auto foreign = LegacyCodecStableIds::Validate(base, other);
    Require(HasCode(foreign, "wtg.source-seal.identity-mismatch"), "actual separately loaded catalog source is rejected");
    SameDiagnostics(WtgWctCodec{}.Validate(base, other), foreign);
}
void ActualPrivatePmrReserveFailureAndSeparatePublicValidation(const UiCatalog& catalog) {
    constexpr std::size_t records = 16384;
    auto document = Fixture(catalog, records); const auto before = document;
    const auto verifyPrivateSetContinuation = [&](const TriggerDocument& input, bool lateDuplicate) {
        const std::array<std::size_t, 5> counts{input.categories.size(), input.variables.size(),
            input.triggers.size(), input.ecaNodes.size(), input.argumentNodes.size()};
        Require(Detail::StableIdReservationCount(counts) == records, "real five-domain counts admit optional reserve");
        ReserveFailureResource upstream;
        alignas(std::max_align_t) std::array<std::byte, 16 * 1024> storage;
        std::pmr::monotonic_buffer_resource resource(storage.data(), storage.size(), &upstream);
        std::pmr::unordered_set<StableId> ids{std::pmr::polymorphic_allocator<StableId>{&resource}};
        Require(ids.empty(), "only the actual empty-set bucket reservation is faulted");
        upstream.BeginReserveFailure();
        Detail::ReserveStableIdBuckets(ids, counts);
        upstream.EndReserveFailure();
        Require(upstream.reserveRequests == 1 && upstream.deniedRequests == 1
            && upstream.deniedBytes > storage.size() && upstream.deniedAlignment != 0,
            "private PMR upstream denied exactly one real bucket reservation request");
        Require(ids.empty(), "failed optional reserve does not insert or authorize any ID");
        std::array<std::size_t, 5> visits{};
        std::size_t rejected = 0;
        const auto visit = [&](StableId id, std::size_t domain) {
            ++visits[domain];
            if (id == 0 || !ids.insert(id).second) ++rejected;
        };
        // 与生产身份循环相同的五域顺序和 zero/duplicate 判定；不声称替代整图 Validate。
        for (const auto& row : input.categories) visit(row.id, 0);
        for (const auto& row : input.variables) visit(row.id, 1);
        for (const auto& row : input.triggers) visit(row.id, 2);
        for (const auto& row : input.ecaNodes) visit(row.id, 3);
        for (const auto& row : input.argumentNodes) visit(row.id, 4);
        Require(visits == counts, "all five actual domains were fully visited after reserve denial");
        Require(rejected == (lateDuplicate ? std::size_t{1} : std::size_t{0})
            && ids.size() == records - rejected, "late cross-domain duplicate never gains uniqueness after reserve failure");
        Require(upstream.forwardedRequests != 0 && upstream.deniedRequests == 1,
            "same failed-reserve PMR set continues real allocation and full insertion without another denial");
    };
    verifyPrivateSetContinuation(document, false);
    const auto expected = LegacyCodecStableIds::Validate(document, catalog);
    // 此 public 调用使用真实生产资源且没有故障注入；它独立核原完整校验/诊断/来源。
    const auto actual = WtgWctCodec{}.Validate(document, catalog);
    SameDiagnostics(actual, expected); Require(actual.Succeeded(), "separate public whole valid graph passes without OOM injection");
    SameMetadata(document, before);
    document.ecaNodes.back().id = document.variables.front().id;
    document.argumentNodes.back().insertCall = 1;
    document.triggers.front().ecas.pop_back();
    const auto brokenBefore = document; const auto brokenExpected = LegacyCodecStableIds::Validate(document, catalog);
    Require(HasCode(brokenExpected, "wtg.stable-id") && HasCode(brokenExpected, "wtg.argument.call-presence")
        && HasCode(brokenExpected, "wtg.eca.orphan"), "independent old oracle includes late identity and Graph errors");
    verifyPrivateSetContinuation(document, true);
    SameDiagnostics(WtgWctCodec{}.Validate(document, catalog), brokenExpected);
    const auto brokenWire = WtgWctCodec{}.Serialize(document, catalog);
    Require(!brokenWire.Succeeded() && !brokenWire.value, "separate public malformed graph produces no partial wire");
    SameDiagnostics(Status{brokenWire.diagnostics}, brokenExpected); SameMetadata(document, brokenBefore);
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-CodecIdReservation-" + std::to_string(GetCurrentProcessId()));
    try {
        Require(!std::filesystem::exists(root), "owned fixture directory already exists; never overwrite");
        for (const auto name : {"base", "other"}) {
            const auto folder = root / name;
            Write(folder / "ui/config", "layer\n");
            Write(folder / "ui/layer/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
            Write(folder / "ui/layer/action.txt", std::string("[DoNothing]\ntitle=Nothing ") + name
                + "\n[SetVariable]\ntitle=Set\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n");
        }
        const auto catalog = Take(UiCatalog::Load(root / "base")); const auto other = Take(UiCatalog::Load(root / "other"));
        Require(catalog.Sha256() != other.Sha256(), "actual catalog identity fixture");
        PolicyBoundariesAndSoftFailure(); ActualFullDiagnosticAndWireBoundaries(catalog, other);
        ActualPrivatePmrReserveFailureAndSeparatePublicValidation(catalog);
        std::filesystem::remove_all(root);
        std::cout << "Codec bounded reservation: private PMR reserve denial/ID continuation and separate public legacy diagnostics/wire passed\n";
        return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
