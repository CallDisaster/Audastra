#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include "../../src/Trigger/TriggerCodecDefinitionLookupMemo.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file << text;
    Require(file.good(), "write owned original catalog fixture");
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Audastra-CompactScalar-"
        + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    UiCatalog catalog;
    TriggerDocument document;
    Fixture() {
        Require(std::filesystem::create_directory(root), "fresh owned fixture directory");
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nstring=0,1,1,String,string\n"
            "scriptcode=0,0,0,Script,string\n");
        Write(root / "ui/base/action.txt", "[StoreInt]\n[[.args]]\ntype=integer\n[StoreString]\n[[.args]]\ntype=string\n"
            "[ScriptSlot]\n[[.args]]\ntype=scriptcode\n");
        Write(root / "ui/base/call.txt", "[ReadInt]\nreturns=integer\n");
        const auto loaded = UiCatalog::Load(root / "ui"); Require(loaded.Succeeded(), "actual catalog load"); catalog = *loaded.value;
        const auto empty = WtgWctCodec{}.CreateEmpty(catalog); Require(empty.Succeeded(), "actual trusted source seal"); document = *empty.value;
        const auto category = TriggerDirectoryEditor::AddCategory(document, "Original"); Require(category.Succeeded(), "actual category");
        Require(TriggerDocumentEditor::AddTrigger(document, *category.value, "OriginalTrigger").Succeeded(), "actual trigger");
        Require(TriggerDocumentEditor::AddVariable(document, "Target", "integer").Succeeded(), "actual typed target");
        Require(TriggerDocumentEditor::AddVariable(document, "Wrong", "string").Succeeded(), "actual wrong typed target");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    StableId Add(std::string_view function, std::int32_t kind, std::string value) {
        const auto appended = TriggerDocumentEditor::AppendRootEca(document, catalog, document.triggers.front().id,
            UiDefinitionKind::Action, function);
        Require(appended.Succeeded(), "actual root ECA");
        const auto* owner = TriggerDocumentEditor::FindEca(document, appended.value->rootEcaId);
        Require(owner && owner->arguments.size() == 1, "actual direct slot");
        const auto id = document.argumentNodes[owner->arguments.front()].id;
        Require(TriggerDocumentEditor::SetScalarArgument(document, id, kind, std::move(value)).Succeeded(), "actual scalar");
        return id;
    }
    TriggerGraphBinding Binding() const {
        const auto value = CaptureTriggerGraphBinding(document, catalog); Require(value.Succeeded(), "actual whole binding"); return *value.value;
    }
    TriggerGraphEditBatch Batch(StableId id, std::int32_t kind, std::string value, bool full = false) const {
        const auto argument = std::find_if(document.argumentNodes.begin(), document.argumentNodes.end(),
            [id](const auto& node) { return node.id == id; });
        Require(argument != document.argumentNodes.end(), "actual selected argument");
        TriggerGraphEditBatch batch; batch.expected = Binding();
        if (!full) { batch.expected.currentWtgSha256.clear(); batch.expected.currentWctSha256.clear(); }
        batch.operations.emplace_back(GraphSet{{GraphRecordKind::Argument, id, {}}, GraphField::ArgumentScalar,
            GraphScalarValue{argument->type, argument->value.text}, GraphScalarValue{kind, std::move(value)}});
        return batch;
    }
    TriggerGraphReplayIdentity Identity() const {
        const auto identity = CaptureTriggerGraphReplayIdentity(document, catalog); Require(identity.Succeeded(), "actual opaque whole replay identity"); return *identity.value;
    }
};
void RequireSource(const Fixture& fixture, const TriggerGraphReplayIdentity& identity) {
    Require(VerifyTriggerGraphReplayIdentity(fixture.document, fixture.catalog, identity).Succeeded(), "entire source metadata seal and paired bytes unchanged");
}
void TestSourceOnlyAndLegacyBindingPolicy() {
    Fixture f; const auto id = f.Add("StoreInt", 3, "1"); const auto identity = f.Identity();
    const auto sourceOnly = f.Batch(id, 1, "Target");
    auto legacyCopy = f.document;
    Require(!ApplyTriggerGraphEditBatch(legacyCopy, f.catalog, sourceOnly).Succeeded(), "general Apply still rejects empty current digests");
    Require(VerifyTriggerGraphReplayIdentity(legacyCopy, f.catalog, identity).Succeeded(), "general failed batch is atomic");
    const auto prepared = PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, sourceOnly);
    Require(prepared.Succeeded() && !prepared.value->Empty() && prepared.value->Receipt().appliedOperations == 1, "scalar const prepare freshly captures complete before");
    Require(!prepared.value->Receipt().before.currentWtgSha256.empty() && !prepared.value->Receipt().before.currentWctSha256.empty(), "opaque receipt owns full before pair");
    RequireSource(f, identity);
    auto half = sourceOnly; half.expected.currentWtgSha256 = f.Binding().currentWtgSha256;
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, half).Succeeded(), "half empty current pair refuses");
    auto stale = f.Batch(id, 1, "Target", true); stale.expected.currentWctSha256[0] = stale.expected.currentWctSha256[0] == '0' ? '1' : '0';
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, stale).Succeeded(), "provided whole current pair remains strict");
    auto wrongSource = sourceOnly; wrongSource.expected.sourceWtgSha256[0] = wrongSource.expected.sourceWtgSha256[0] == '0' ? '1' : '0';
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, wrongSource).Succeeded(), "source-only never means source wildcard");
    RequireSource(f, identity);
}
void TestFullGbMetadataRoundtripAndUnselectedNodes() {
    Fixture f;
    for (std::size_t i = 0; i < 300; ++i) f.Add("StoreString", 3, "中文原字节");
    const std::vector<std::byte> gb{std::byte{0xd6}, std::byte{0xd0}, std::byte{0xce}, std::byte{0xc4},
        std::byte{0xd4}, std::byte{0xad}, std::byte{0xd7}, std::byte{0xd6}, std::byte{0xbd}, std::byte{0xda}};
    for (auto& argument : f.document.argumentNodes) argument.value.encoding = TextEncoding::Gb18030;
    const auto original = WtgWctCodec{}.Serialize(f.document, f.catalog); Require(original.Succeeded(), "actual original GB codec encoding");
    Require(std::search(original.value->wtg.begin(), original.value->wtg.end(), gb.begin(), gb.end()) != original.value->wtg.end(), "known original GB bytes in WTG");
    const auto parsed = WtgWctCodec{}.Parse(original.value->wtg, original.value->wct, f.catalog); Require(parsed.Succeeded(), "actual GB parse establishes source seal");
    f.document = *parsed.value;
    Require(f.document.argumentNodes.front().value.encoding == TextEncoding::Gb18030
        && f.document.argumentNodes.front().value.originalBytes == gb, "actual GB metadata is present before replay");
    const auto beforeIdentity = f.Identity(); const auto beforeNodes = f.document.argumentNodes;
    TriggerGraphEditBatch batch; batch.expected = f.Binding(); batch.expected.currentWtgSha256.clear(); batch.expected.currentWctSha256.clear();
    for (std::size_t i = 0; i < 256; ++i) batch.operations.emplace_back(GraphSet{{GraphRecordKind::Argument, beforeNodes[i].id, {}},
        GraphField::ArgumentScalar, GraphScalarValue{3, beforeNodes[i].value.text}, GraphScalarValue{3, "替换中文"}});
    const auto prepared = PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, batch);
    Require(prepared.Succeeded() && prepared.value->Receipt().appliedOperations == 256, "actual256 compact full node preparation");
    const auto& cost = prepared.value->Cost();
    Require(cost.sourceDocumentBytes && cost.candidateOwnedBytes >= cost.sourceDocumentBytes && cost.compactOwnedBytes
        && cost.encodedPairBytes && cost.encodedPairCapacityBytes >= cost.encodedPairBytes && cost.identityBytesCharged
        && cost.peakOwnedBytes <= 128 * 1024 * 1024 && cost.workUnitsCharged <= 64 * 1024 * 1024,
        "actual bounded logical cost includes candidate compact encoded capacity and identity input");
    RequireSource(f, beforeIdentity);
    Require(ReplayTriggerGraphScalarEdit(f.document, f.catalog, *prepared.value, true).Succeeded(), "actual forward scalar publication");
    Require(VerifyTriggerGraphScalarReplay(f.document, f.catalog, *prepared.value, true).Succeeded(), "actual after entire graph identity");
    const auto afterIdentity = f.Identity();
    for (std::size_t i = 0; i < 300; ++i) {
        const auto& value = f.document.argumentNodes[i]; const auto& before = beforeNodes[i];
        Require(value.id == before.id && value.value.encoding == before.value.encoding && value.origin.source == before.origin.source
            && value.origin.offset == before.origin.offset && value.origin.length == before.origin.length && value.origin.fingerprint == before.origin.fingerprint,
            "all stable IDs encodings and source metadata preserved");
        if (i < 256) Require(value.value.text == "替换中文" && value.value.originalBytes.empty(), "selected ordinary editor scalar result");
        else Require(value.value.text == before.value.text && value.value.originalBytes == before.value.originalBytes, "unselected44 exact raw metadata");
    }
    Require(ReplayTriggerGraphScalarEdit(f.document, f.catalog, *prepared.value, false).Succeeded(), "actual backward full node restore");
    RequireSource(f, beforeIdentity);
    const auto restored = WtgWctCodec{}.Serialize(f.document, f.catalog);
    Require(restored.Succeeded() && restored.value->wtg == original.value->wtg && restored.value->wct == original.value->wct,
        "whole WTG WCT original bytes restored without Parse reconstruction");
    for (std::size_t i = 0; i < 300; ++i) Require(f.document.argumentNodes[i].value.originalBytes == beforeNodes[i].value.originalBytes,
        "selected and unselected originalBytes restored");
    Require(ReplayTriggerGraphScalarEdit(f.document, f.catalog, *prepared.value, true).Succeeded(), "actual redo");
    RequireSource(f, afterIdentity);
}
void TestStaleForeignEmptyAndInvalidShapesAreAtomic() {
    Fixture f; const auto id = f.Add("StoreInt", 3, "1"); const auto identity = f.Identity(); const auto batch = f.Batch(id, 1, "Target");
    const auto prepared = PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, batch); Require(prepared.Succeeded(), "actual baseline receipt");
    const TriggerGraphScalarEditReceipt empty;
    Require(empty.Empty() && !ReplayTriggerGraphScalarEdit(f.document, f.catalog, empty, true).Succeeded(), "default receipt has no authority");
    auto foreign = f.document; Require(TriggerDocumentEditor::SetScalarArgument(foreign, id, 3, "8").Succeeded(), "foreign same seal changed source");
    const auto foreignIdentity = CaptureTriggerGraphReplayIdentity(foreign, f.catalog); Require(foreignIdentity.Succeeded(), "foreign actual identity");
    Require(!ReplayTriggerGraphScalarEdit(foreign, f.catalog, *prepared.value, true).Succeeded()
        && VerifyTriggerGraphReplayIdentity(foreign, f.catalog, *foreignIdentity.value).Succeeded(), "foreign receipt refused without writes");
    auto metadata = f.document; ++metadata.argumentNodes.front().origin.offset;
    const auto plain = WtgWctCodec{}.Serialize(f.document, f.catalog), changed = WtgWctCodec{}.Serialize(metadata, f.catalog);
    Require(plain.Succeeded() && changed.Succeeded() && plain.value->wtg == changed.value->wtg && plain.value->wct == changed.value->wct,
        "metadata change really has identical file bytes");
    const auto metadataIdentity = CaptureTriggerGraphReplayIdentity(metadata, f.catalog); Require(metadataIdentity.Succeeded(), "actual metadata-only identity");
    Require(!ReplayTriggerGraphScalarEdit(metadata, f.catalog, *prepared.value, true).Succeeded()
        && VerifyTriggerGraphReplayIdentity(metadata, f.catalog, *metadataIdentity.value).Succeeded(), "entire replay metadata gate refuses same-byte stale graph");
    auto duplicate = batch; duplicate.operations.push_back(duplicate.operations.front());
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, duplicate).Succeeded(), "duplicate selection rejected atomically");
    auto wrongType = f.Batch(id, 1, "Wrong"); Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, wrongType).Succeeded(), "existing TypeSystem rejects wrong type");
    auto wrongOld = batch; std::get<GraphSet>(wrongOld.operations.front()).expected = GraphScalarValue{3, "7"};
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, wrongOld).Succeeded(), "exact scalar expected original remains mandatory");
    auto nonScalar = batch; std::get<GraphSet>(nonScalar.operations.front()).field = GraphField::Description;
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, nonScalar).Succeeded(), "mixed semantic field never enters compact path");
    RequireSource(f, identity);
    const auto complex = f.Add("StoreInt", 3, "2");
    Require(TriggerDocumentEditor::AppendArgumentCall(f.document, f.catalog, complex, "integer", "ReadInt").Succeeded(), "actual complex call");
    const auto complexIdentity = f.Identity();
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, f.Batch(complex, 3, "5")).Succeeded(), "complex root refused without flattening");
    RequireSource(f, complexIdentity);
}
void TestMemoryWorkAndAdditionalOwnershipGates() {
    Fixture f; const auto id = f.Add("StoreInt", 3, "1"); const auto identity = f.Identity(); const auto batch = f.Batch(id, 1, "Target");
    const auto baseline = PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, batch);
    Require(baseline.Succeeded(), "actual scratch billing receipt");
    const auto records = f.document.categories.size() + f.document.variables.size() + f.document.triggers.size()
        + f.document.ecaNodes.size() + f.document.argumentNodes.size();
    const auto expectedScratch = records * sizeof(StableId)
        + (f.document.ecaNodes.size() + f.document.argumentNodes.size()) * (sizeof(std::uint8_t) + sizeof(std::size_t))
        + 16 * 1024 + sizeof(War3Map::Trigger::Detail::CodecDefinitionLookupMemo);
    Require(baseline.value->Cost().validationScratchBytes == expectedScratch,
        "actual memo and still-live ID arena fixed stack metadata charged alongside old ID/graph arrays");
    TriggerGraphEditLimits scratchBoundary;
    scratchBoundary.maxScalarOwnedBytes = baseline.value->Cost().peakOwnedBytes - 1;
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, batch, scratchBoundary).Succeeded(),
        "new known fixed scratch remains subject to unchanged logical ownership peak cap");
    TriggerGraphEditLimits memory; memory.maxScalarOwnedBytes = 1;
    const auto deniedMemory = PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, batch, memory);
    Require(!deniedMemory.Succeeded(), "source admission before candidate allocations");
    TriggerGraphEditLimits work; work.maxScalarWorkUnits = 1; work.scalarWorkPolicy = TriggerScalarWorkPolicy::LegacyKnownWork;
    const auto deniedWork = PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, batch, work);
    Require(!deniedWork.Succeeded(), "actual codec and identity work cannot be omitted");
    TriggerGraphEditLimits owned; owned.scalarAdditionalOwnedBytes = owned.maxScalarOwnedBytes;
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, batch, owned).Succeeded(), "consumer-owned preview values share same new ownership cap");
    TriggerGraphEditLimits additional; additional.scalarAdditionalWorkUnits = additional.maxScalarWorkUnits; additional.scalarWorkPolicy = TriggerScalarWorkPolicy::LegacyKnownWork;
    Require(!PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, batch, additional).Succeeded(), "consumer guard work shares exact work cap");
    RequireSource(f, identity);
}
void TestOwnedPreparePoliciesCancellationAndCodeProtection() {
    Fixture f; const auto id = f.Add("StoreInt", 3, "1");
    f.document.wct.customComment.text = "保留原全局说明";
    f.document.wct.customCode.text = "function OriginalGlobal takes nothing returns nothing\nendfunction\n";
    const auto custom = TriggerDocumentEditor::AddTrigger(f.document, f.document.triggers.front().categoryId, "OriginalCustom");
    Require(custom.Succeeded(), "actual existing custom slot trigger");
    auto* customRow = TriggerDocumentEditor::FindTrigger(f.document, *custom.value);
    Require(customRow != nullptr, "actual custom row"); customRow->customTextFlag = 1;
    customRow->customText.text = "function OriginalCustom takes nothing returns nothing\nendfunction\n";
    const auto identity = f.Identity(); auto batch = f.Batch(id, 1, "Target");
    batch.expected.currentWtgSha256.clear(); batch.expected.currentWctSha256.clear();
    TriggerGraphEditLimits bounded; bounded.maxScalarWorkUnits = 1;
    const auto owned = PrepareOwnedTriggerGraphScalarEditBatch(f.document, f.catalog, batch, bounded);
    Require(owned.Succeeded() && owned.value->Cost().ownedInputConsumed && owned.value->Cost().candidateCopies == 0
        && owned.value->Cost().identityCaptures == 2 && !owned.value->Cost().workCoverageComplete
        && owned.value->Cost().workUnitsCharged > bounded.maxScalarWorkUnits
        && owned.value->Cost().workPolicy == TriggerScalarWorkPolicy::BoundedSourcesV2
        && owned.value->Cost().codeProtectionScratchBytes != 0, "new bounded source policy explicitly does not claim old known-work pass or complete upper bound");
    auto publication = f.document;
    Require(ReplayTriggerGraphScalarEdit(publication, f.catalog, *owned.value, true).Succeeded(), "actual owned scalar publication same engine");
    Require(publication.wct.customCode.text == f.document.wct.customCode.text
        && publication.wct.customComment.text == f.document.wct.customComment.text
        && publication.triggers.back().customText.text == f.document.triggers.back().customText.text, "global and custom-only protection snapshot preserves existing code");
    Require(ReplayTriggerGraphScalarEdit(publication, f.catalog, *owned.value, false).Succeeded()
        && VerifyTriggerGraphReplayIdentity(publication, f.catalog, identity).Succeeded(), "full whole metadata original identity restored");
    Require(owned.value->CurrentWtgSha256(false) == owned.value->Receipt().before.currentWtgSha256
        && owned.value->CurrentWctSha256(true) == owned.value->Receipt().after.currentWctSha256, "readonly paired hash getters expose real directional captures");
    const auto constPath = PrepareTriggerGraphScalarEditBatch(f.document, f.catalog, batch, bounded);
    Require(constPath.Succeeded() && constPath.value->Cost().candidateCopies == 1 && !constPath.value->Cost().ownedInputConsumed,
        "const path delegates through one private clone");
    TriggerGraphEditLimits legacy = bounded; legacy.scalarWorkPolicy = TriggerScalarWorkPolicy::LegacyKnownWork;
    Require(!PrepareOwnedTriggerGraphScalarEditBatch(f.document, f.catalog, batch, legacy).Succeeded(), "old known-work estimate policy still fails exact low gate");
    std::stop_source stop; stop.request_stop();
    Require(!PrepareOwnedTriggerGraphScalarEditBatch(f.document, f.catalog, batch, bounded, nullptr, stop.get_token()).Succeeded(), "cooperative pre-cancel creates no receipt");
    auto half = batch; half.expected.currentWtgSha256 = "supplied-half";
    Require(!PrepareOwnedTriggerGraphScalarEditBatch(f.document, f.catalog, half).Succeeded(), "owned source-only rejects half-empty binding");
    auto wrongOld = batch; std::get<GraphSet>(wrongOld.operations.front()).expected = GraphScalarValue{3, "wrong"};
    Require(!PrepareOwnedTriggerGraphScalarEditBatch(f.document, f.catalog, wrongOld).Succeeded(), "owned input must match exact selected original value");
    TriggerGraphEditLimits records; records.maxScalarRecords = 1;
    Require(!PrepareOwnedTriggerGraphScalarEditBatch(f.document, f.catalog, batch, records).Succeeded(), "finite source record cap remains enforced");
    TriggerGraphEditLimits invalid; invalid.scalarWorkPolicy = static_cast<TriggerScalarWorkPolicy>(255);
    Require(!PrepareOwnedTriggerGraphScalarEditBatch(f.document, f.catalog, batch, invalid).Succeeded(), "unknown policy cannot acquire receipt");
    RequireSource(f, identity);
}
}
int main() {
    try {
        TestSourceOnlyAndLegacyBindingPolicy();
        TestFullGbMetadataRoundtripAndUnselectedNodes();
        TestStaleForeignEmptyAndInvalidShapesAreAtomic();
        TestMemoryWorkAndAdditionalOwnershipGates();
        TestOwnedPreparePoliciesCancellationAndCodeProtection();
        std::cout << "5 compact scalar Core groups passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
