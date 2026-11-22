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
    std::string sourceFile;
    std::size_t sourceLine = 0;
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
    std::string preprocessIdentitySha256, preprocessProfileSha256;
    std::size_t includedFileCount = 0;
private:
    // 只供旧逐块确认核对，查询调用方只获得整组身份与有界样本。
    std::vector<RemovalBlockIdentity> groupableIdentities_;
    friend Result<DeclarationRemovalReview> QueryDeclarationRemovalAudit(const TriggerDocument&, const UiCatalog&, const GraphRemove&, const MapObjectReferenceSnapshot*, const RemovalPreprocessProvider*);
    friend Status CheckDeclarationRemovalAudit(const TriggerDocument&, const UiCatalog&, const GraphRemove&, const MapObjectReferenceSnapshot*, const RemovalPreprocessProvider*);
};
// 只读查询。按与实际编辑相同的前序操作准备候选；operationIndex指向remove。
[[nodiscard]] Result<DeclarationRemovalReview> QueryDeclarationRemovalAudit(
    const TriggerDocument&, const UiCatalog&, const TriggerGraphEditBatch&, std::size_t operationIndex,
    const TriggerGraphEditLimits& limits = {}, const MapObjectReferenceSnapshot* mapObjects = nullptr);
// 当前删前候选使用的同一取证入口，不提供按名称搜索或静默选择。
[[nodiscard]] Result<DeclarationRemovalReview> QueryDeclarationRemovalAudit(
    const TriggerDocument&, const UiCatalog&, const GraphRemove&, const MapObjectReferenceSnapshot* mapObjects = nullptr,
    const RemovalPreprocessProvider* preprocessing = nullptr);
[[nodiscard]] Status CheckDeclarationRemovalAudit(const TriggerDocument&, const UiCatalog&, const GraphRemove&,
    const MapObjectReferenceSnapshot* mapObjects = nullptr, const RemovalPreprocessProvider* preprocessing = nullptr);

struct DeclarationBatchRemovalTarget {
    GraphRecordRef target;
    std::string expectedName;
    std::vector<std::string> symbols;
    std::vector<StableId> typedArgumentIds;
};
struct DeclarationBatchRemovalReview {
    TriggerGraphBinding binding;
    std::vector<DeclarationBatchRemovalTarget> targets;
    std::string reviewSha256;
    std::vector<StableId> typedArgumentIds;
    std::size_t groupableCount = 0;
    std::string groupableSha256;
    std::vector<RemovalCodeBlock> groupableSamples, individualBlocks;
    bool complete = true;
    bool externalDependenciesVerified = false;
    std::string preprocessIdentitySha256, preprocessProfileSha256;
    std::size_t includedFileCount = 0;
private:
    std::vector<RemovalBlockIdentity> groupableIdentities_;
    friend Result<DeclarationBatchRemovalReview> QueryBatchDeclarationRemovalAudit(
        const TriggerDocument&, const UiCatalog&, std::span<const GraphRemove>,
        const MapObjectReferenceSnapshot*, const BatchRemovalPreprocessProvider*);
    friend Status CheckBatchDeclarationRemovalAudit(
        const TriggerDocument&, const UiCatalog&, std::span<const GraphRemove>,
        std::span<const RemovalCodeConfirmation>, const MapObjectReferenceSnapshot*,
        const BatchRemovalPreprocessProvider*);
};
// 批量删除取证：整批所有目标触发器与变量一次构建私密投影、一次整图预处理、一次代码扫描；
// 同批触发器彼此间的引用及自身代码块自动闭合，不产生虚假互引阻断。最多 512 个声明目标。
[[nodiscard]] Result<DeclarationBatchRemovalReview> QueryBatchDeclarationRemovalAudit(
    const TriggerDocument&, const UiCatalog&, std::span<const GraphRemove> targets,
    const MapObjectReferenceSnapshot* mapObjects = nullptr,
    const BatchRemovalPreprocessProvider* preprocessing = nullptr);
[[nodiscard]] Status CheckBatchDeclarationRemovalAudit(
    const TriggerDocument&, const UiCatalog&, std::span<const GraphRemove> targets,
    std::span<const RemovalCodeConfirmation> codeAudits,
    const MapObjectReferenceSnapshot* mapObjects = nullptr,
    const BatchRemovalPreprocessProvider* preprocessing = nullptr);
}
