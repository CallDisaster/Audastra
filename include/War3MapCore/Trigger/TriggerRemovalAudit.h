#pragma once
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

namespace War3Map::Trigger {
struct OpaqueArgumentSample { StableId id = 0; std::size_t bytes = 0; std::string text; };
struct OpaqueArgumentClass {
    std::string expectedType;
    int literalKind = 0;
    bool validateLiteral = false, opaque = false, dispatch = false;
    std::size_t count = 0, bytes = 0, shortCount = 0;
    std::vector<OpaqueArgumentSample> samples;
};
struct OpaqueArgumentInventory {
    std::size_t arguments = 0, opaqueArguments = 0, totalBytes = 0;
    std::vector<OpaqueArgumentClass> classes;
};
// 完整有界统计，不是删除证据或授权；不会改变正式审计的块数上限。
[[nodiscard]] Result<OpaqueArgumentInventory> InspectOpaqueArgumentInventory(const TriggerDocument&, const UiCatalog&);
struct RemovalReferenceEvidence {
    std::string context, text;
    std::size_t decodedByteOffset = 0, decodedByteLength = 0;
    std::size_t rawByteOffset = 0, rawByteLength = 0;
    bool rawRangeExact = false;
    std::size_t line = 1, column = 1;
    bool definite = false;
};
struct RemovalCodeBlock {
    RemovalBlockIdentity identity;
    std::vector<RemovalReferenceEvidence> evidence;
    bool groupable = false, complete = true, dynamic = false, referenced = false;
    std::string incompleteReason;
    std::string sampleText;
    bool sampleTruncated = false;
};
struct DeclarationRemovalReview {
    TriggerGraphBinding binding;
    GraphRecordRef target;
    std::string expectedName, reviewSha256;
    std::vector<std::string> symbols;
    std::vector<StableId> typedArgumentIds;
    std::size_t groupableCount = 0;
    std::string groupableSha256;
    std::vector<RemovalCodeBlock> groupableSamples, individualBlocks;
    bool complete = true;
    bool externalDependenciesVerified = false;
private:
    // 只供旧逐块确认核对，查询调用方只获得整组身份与有界样本。
    std::vector<RemovalBlockIdentity> groupableIdentities_;
    friend Result<DeclarationRemovalReview> QueryDeclarationRemovalAudit(const TriggerDocument&, const UiCatalog&, const GraphRemove&, const MapObjectReferenceSnapshot*);
    friend Status CheckDeclarationRemovalAudit(const TriggerDocument&, const UiCatalog&, const GraphRemove&, const MapObjectReferenceSnapshot*);
};
// 只读查询。按与实际编辑相同的前序操作准备候选；operationIndex指向remove。
[[nodiscard]] Result<DeclarationRemovalReview> QueryDeclarationRemovalAudit(
    const TriggerDocument&, const UiCatalog&, const TriggerGraphEditBatch&, std::size_t operationIndex,
    const TriggerGraphEditLimits& limits = {}, const MapObjectReferenceSnapshot* mapObjects = nullptr);
// 当前删前候选使用的同一取证入口，不提供按名称搜索或静默选择。
[[nodiscard]] Result<DeclarationRemovalReview> QueryDeclarationRemovalAudit(
    const TriggerDocument&, const UiCatalog&, const GraphRemove&, const MapObjectReferenceSnapshot* mapObjects = nullptr);
[[nodiscard]] Status CheckDeclarationRemovalAudit(const TriggerDocument&, const UiCatalog&, const GraphRemove&,
    const MapObjectReferenceSnapshot* mapObjects = nullptr);
}
