#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
using namespace War3Map::Services;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool Code(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& item) { return item.code == code; });
}
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output << text; Require(output.good(), "write private catalog");
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("Forge-TriggerTransaction-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    UiCatalog catalog;
    TriggerDocument source, edited;
    EncodedTriggerDocument original;
    TriggerTransactionBinding binding;
    Fixture() {
        Write(root / "ui" / "config", "base\n");
        Write(root / "ui" / "base" / "define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
        Write(root / "ui" / "base" / "action.txt", "[StoreInteger]\n[[.args]]\ntype=integer\n");
        Write(root / "ui" / "base" / "call.txt", "[ReadInteger]\nreturns=integer\n");
        auto loaded = UiCatalog::Load(root / "ui"); Require(loaded.Succeeded() && loaded.value, "load private catalog");
        catalog = std::move(*loaded.value);
        WtgWctCodec codec;
        auto empty = codec.CreateEmpty(catalog); Require(empty.Succeeded() && empty.value, "sealed empty document");
        source = std::move(*empty.value);
        source.categories.push_back({1, 1, RawText{"T"}, 0, {}});
        auto legacy = TriggerDocumentEditor::AddTrigger(source, 1, "ExistingCode"); Require(legacy.Succeeded(), "source code slot");
        source.triggers.back().customTextFlag = 1;
        source.triggers.back().customText.text = "// retained custom code\r\n";
        source.wct.customCode.text = "// retained global code 中文\r\n";
        auto encoded = codec.Serialize(source, catalog); Require(encoded.Succeeded() && encoded.value, "encode original pair");
        original = std::move(*encoded.value);
        auto parsed = codec.Parse(original.wtg, original.wct, catalog); Require(parsed.Succeeded() && parsed.value, "seal actual source pair");
        source = std::move(*parsed.value); edited = source;
        auto added = TriggerDocumentEditor::AddTrigger(edited, 1, "GUI Dispatch"); Require(added.Succeeded() && added.value, "create GUI trigger");
        Require(TriggerDocumentEditor::AddVariable(edited, "CachedInteger", "integer").Succeeded(), "create variable");
        auto rootEca = TriggerDocumentEditor::AppendRootEca(edited, catalog, *added.value, UiDefinitionKind::Action, "StoreInteger");
        Require(rootEca.Succeeded() && rootEca.value, "append typed action");
        const auto* eca = TriggerDocumentEditor::FindEca(edited, rootEca.value->rootEcaId);
        Require(eca && eca->arguments.size() == 1, "action parameter identity");
        auto call = TriggerDocumentEditor::AppendArgumentCall(edited, catalog,
            edited.argumentNodes[eca->arguments[0]].id, "integer", "ReadInteger"); Require(call.Succeeded(), "nested argument call");
        const std::vector<MapArchiveMember> members{{"war3map.wtg", original.wtg}, {"war3map.wct", original.wct},
            {"war3map.j", {std::byte{'/'}, std::byte{'/'}}}, {"opaque.bin", {std::byte{19}, std::byte{0}}}};
        Require(WriteVerifiedArchive(root / "source.w3x", members).Succeeded(), "private source archive");
        const auto sha = Sha256File(root / "source.w3x"); Require(sha.Succeeded() && sha.value, "source SHA");
        binding = {*sha.value, 7, {0, std::string(64, 'a'), catalog.Sha256()}, 3};
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    auto Freeze(const TriggerDocument& document) const {
        return TriggerTransactionSnapshot::Freeze(document, catalog, original.wtg, original.wct, binding);
    }
};

void TestOwnedGuiGraphAndSourceGuards() {
    Fixture f; auto frozen = f.Freeze(f.edited); Require(frozen.Succeeded() && frozen.value, "freeze complete GUI graph");
    const auto& snapshot = **frozen.value;
    Require(!snapshot.IsSourceCompanion(), "changed GUI graph retains ordinary transaction identity");
    Require(snapshot.Receipt().variableCount == 1 && snapshot.Receipt().triggerCount == 2
        && snapshot.Receipt().ecaCount == 2 && snapshot.Receipt().argumentCount == 1, "full graph counts");
    f.edited.variables.clear(); Require(snapshot.Document().variables.size() == 1, "snapshot owns typed graph");
    Require(snapshot.VerifyAgainst(f.binding.sourceSha256, f.original.wtg, f.original.wct, f.catalog).Succeeded(), "verify actual source");
    Require(!snapshot.VerifyAgainst(std::string(64, 'b'), f.original.wtg, f.original.wct, f.catalog).Succeeded(), "wrong archive rejected");
    auto stale = f.original.wtg; stale.back() ^= std::byte{1};
    Require(!snapshot.VerifyAgainst(f.binding.sourceSha256, stale, f.original.wct, f.catalog).Succeeded(), "wrong pair rejected");
    auto changed = f.source; changed.wct.customCode.text += "// changed\n";
    const auto global = f.Freeze(changed); Require(!global.Succeeded() && Code(global.diagnostics, "TRIGGER_TRANSACTION_CUSTOM_CODE_CHANGED"), "preserve global bytes");
    changed = snapshot.Document(); changed.triggers.back().customTextFlag = 1;
    const auto converted = f.Freeze(changed); Require(!converted.Succeeded() && Code(converted.diagnostics, "TRIGGER_TRANSACTION_GUI_REQUIRED"), "GUI conversion rejected");
    TriggerDocument unsealed;
    Require(!f.Freeze(unsealed).Succeeded(), "arbitrary document lacks trusted seal");
    TriggerTransactionLimits tiny; tiny.maxRecords = 1;
    Require(!TriggerTransactionSnapshot::Freeze(snapshot.Document(), f.catalog, f.original.wtg, f.original.wct, f.binding, tiny).Succeeded(), "graph budget enforced");
    Write(f.root / "ui" / "base" / "action.txt", "[Changed]\n");
    const auto catalog = UiCatalog::Load(f.root / "ui"); Require(catalog.Succeeded() && catalog.value, "reload changed catalog");
    Require(!snapshot.VerifyAgainst(f.binding.sourceSha256, f.original.wtg, f.original.wct, *catalog.value).Succeeded(), "catalog bytes bound");
}

void TestSourceCompanionRequiresExactUnchangedPair() {
    Fixture f;
    const auto unchanged = f.Freeze(f.source);
    Require(!unchanged.Succeeded() && !unchanged.value && unchanged.diagnostics.size() == 1
        && unchanged.diagnostics.front().severity == DiagnosticSeverity::Error
        && unchanged.diagnostics.front().code == "TRIGGER_TRANSACTION_NO_CHANGE",
        "ordinary GUI freeze still rejects an unchanged pair with the sole NO_CHANGE error");
    const auto frozen = TriggerTransactionSnapshot::FreezeSourceCompanion(
        f.source, f.catalog, f.original.wtg, f.original.wct, f.binding);
    Require(frozen.Succeeded() && frozen.value, "unchanged trusted pair freezes as an explicit source companion");
    const auto& snapshot = **frozen.value;
    Require(snapshot.IsSourceCompanion() && snapshot.Encoded().wtg == f.original.wtg
        && snapshot.Encoded().wct == f.original.wct
        && snapshot.Receipt().originalWtgSha256 == snapshot.Receipt().savedWtgSha256
        && snapshot.Receipt().originalWctSha256 == snapshot.Receipt().savedWctSha256,
        "source companion owns exact original WTG/WCT and unchanged receipt identities");
    Require(snapshot.Binding().transactionId == f.binding.transactionId
        && snapshot.Binding().editGeneration == f.binding.editGeneration
        && snapshot.Binding().baseRevision == f.binding.baseRevision
        && snapshot.VerifyAgainst(f.binding.sourceSha256, f.original.wtg, f.original.wct, f.catalog).Succeeded(),
        "companion remains bound to the same source, revision, generation and catalog");
    Require(snapshot.Document().wct.customCode.text == f.source.wct.customCode.text
        && snapshot.Document().triggers.front().customText.text == f.source.triggers.front().customText.text,
        "companion preserves global and per-trigger custom code");
    const auto changedGui = TriggerTransactionSnapshot::FreezeSourceCompanion(
        f.edited, f.catalog, f.original.wtg, f.original.wct, f.binding);
    Require(!changedGui.Succeeded() && Code(changedGui.diagnostics, "TRIGGER_TRANSACTION_SOURCE_COMPANION_CHANGED"),
        "changed GUI graph cannot masquerade as a source companion");
    auto changedCode = f.source; changedCode.wct.customCode.text += "// forbidden companion change\n";
    const auto changedGlobal = TriggerTransactionSnapshot::FreezeSourceCompanion(
        changedCode, f.catalog, f.original.wtg, f.original.wct, f.binding);
    Require(!changedGlobal.Succeeded() && Code(changedGlobal.diagnostics, "TRIGGER_TRANSACTION_CUSTOM_CODE_CHANGED"),
        "companion retains the existing custom-code preservation gate");
    auto converted = f.source; converted.triggers.front().customTextFlag = 0;
    const auto conversion = TriggerTransactionSnapshot::FreezeSourceCompanion(
        converted, f.catalog, f.original.wtg, f.original.wct, f.binding);
    Require(!conversion.Succeeded() && Code(conversion.diagnostics, "TRIGGER_TRANSACTION_GUI_REQUIRED"),
        "companion cannot convert an existing custom-code trigger to GUI");

    const auto originalWtg = f.original.wtg, originalWct = f.original.wct;
    f.original.wtg.clear(); f.original.wct.clear(); f.source.wct.customCode.text.clear();
    Require(snapshot.Encoded().wtg == originalWtg && snapshot.Encoded().wct == originalWct
        && !snapshot.Document().wct.customCode.text.empty()
        && snapshot.VerifyAgainst(f.binding.sourceSha256, originalWtg, originalWct, f.catalog).Succeeded(),
        "companion owns both original bytes and the trusted source document independently of caller mutation");
}

void TestSourceCompanionRetainsIdentityAndBudgetGuards() {
    Fixture f;
    const auto freeze = [&](const TriggerDocument& document, const TriggerTransactionBinding& binding,
        const TriggerTransactionLimits& limits = {}) {
        return TriggerTransactionSnapshot::FreezeSourceCompanion(
            document, f.catalog, f.original.wtg, f.original.wct, binding, limits);
    };
    auto binding = f.binding; binding.editGeneration = 0;
    const auto generation = freeze(f.source, binding);
    Require(!generation.Succeeded() && Code(generation.diagnostics, "TRIGGER_TRANSACTION_BINDING_INVALID"),
        "companion requires an actual edit generation");
    binding = f.binding; binding.sourceSha256.clear();
    const auto archive = freeze(f.source, binding);
    Require(!archive.Succeeded() && Code(archive.diagnostics, "TRIGGER_TRANSACTION_BINDING_INVALID"),
        "companion requires a complete source archive identity");
    TriggerDocument unsealed;
    const auto seal = freeze(unsealed, f.binding);
    Require(!seal.Succeeded() && Code(seal.diagnostics, "TRIGGER_TRANSACTION_SOURCE_SEAL_MISSING"),
        "companion does not seal an arbitrary client document");
    auto stale = f.original.wtg; stale.back() ^= std::byte{1};
    const auto pair = TriggerTransactionSnapshot::FreezeSourceCompanion(f.source, f.catalog,
        stale, f.original.wct, f.binding);
    Require(!pair.Succeeded() && Code(pair.diagnostics, "TRIGGER_TRANSACTION_SOURCE_STALE"),
        "companion cannot bind a different actual source pair");
    TriggerTransactionLimits tiny; tiny.maxMemberBytes = 1;
    const auto bytes = freeze(f.source, f.binding, tiny);
    Require(!bytes.Succeeded() && Code(bytes.diagnostics, "TRIGGER_TRANSACTION_BYTE_BUDGET"),
        "companion retains member byte budget");
    tiny = {}; tiny.maxRecords = 1;
    const auto graph = freeze(f.source, f.binding, tiny);
    Require(!graph.Succeeded() && Code(graph.diagnostics, "TRIGGER_TRANSACTION_GRAPH_BUDGET"),
        "companion retains trusted graph budget");
    tiny = {}; tiny.maxTextBytes = 1;
    const auto text = freeze(f.source, f.binding, tiny);
    Require(!text.Succeeded() && Code(text.diagnostics, "TRIGGER_TRANSACTION_TEXT_BUDGET"),
        "companion retains decoded and original text budget");
    const auto frozen = freeze(f.source, f.binding);
    Require(frozen.Succeeded() && frozen.value, "all rejected companion attempts preserve a valid original seal");
    const auto& snapshot = **frozen.value;
    Require(!snapshot.VerifyAgainst(std::string(64, 'b'), f.original.wtg, f.original.wct, f.catalog).Succeeded(),
        "source companion cannot be reused for a different archive");
    Require(!snapshot.VerifyAgainst(f.binding.sourceSha256, stale, f.original.wct, f.catalog).Succeeded(),
        "source companion cannot be reused for a changed WTG member");
    Write(f.root / "ui" / "base" / "action.txt", "[Changed]\n");
    const auto catalog = UiCatalog::Load(f.root / "ui"); Require(catalog.Succeeded() && catalog.value, "reload companion catalog");
    Require(!snapshot.VerifyAgainst(f.binding.sourceSha256, f.original.wtg, f.original.wct, *catalog.value).Succeeded(),
        "source companion verifies current catalog bytes");
    binding = f.binding; binding.baseRevision.uiCatalogSha256 = catalog.value->Sha256();
    const auto recataloged = TriggerTransactionSnapshot::FreezeSourceCompanion(
        f.source, *catalog.value, f.original.wtg, f.original.wct, binding);
    Require(!recataloged.Succeeded() && Code(recataloged.diagnostics, "TRIGGER_TRANSACTION_SOURCE_STALE"),
        "updating binding catalog identity cannot override the document's trusted catalog seal");
}

void TestCommonSaveRefusesPartialWrite() {
    Fixture f; auto frozen = f.Freeze(f.edited); Require(frozen.Succeeded() && frozen.value, "freeze save input");
    MapEditSaveRequest request;
    request.sourceMap = f.root / "source.w3x"; request.targetCandidate = f.root / "candidate.w3x";
    request.toolRoot = f.root / "missing-tool"; request.uiRoot = f.root / "ui"; request.gameRoot = f.root;
    request.expectedSourceSha256 = f.binding.sourceSha256; request.triggerTransaction = *frozen.value;
    const auto rejected = SaveMapEditCandidate(request);
    Require(!rejected.Succeeded() && !std::filesystem::exists(request.targetCandidate), "missing compiler cannot persist unbuilt GUI");
    Require(Code(rejected.diagnostics, "SCRIPT_TOOLCHAIN_MISSING"), "complete snapshot reaches existing script preparation");
    request.triggerEdits.emplace();
    const auto conflict = SaveMapEditCandidate(request);
    Require(!conflict.Succeeded() && Code(conflict.diagnostics, "SAVE_TRIGGER_INPUT_CONFLICT"), "old ops and snapshot mutually exclusive");
    const auto sha = Sha256File(request.sourceMap); Require(sha.Succeeded() && sha.value == f.binding.sourceSha256, "source archive retained");
    Require(request.triggerTransaction->Document().variables.size() == 1, "failure does not consume immutable transaction");
}
} // namespace

int main() {
    try { TestOwnedGuiGraphAndSourceGuards(); TestSourceCompanionRequiresExactUnchangedPair();
        TestSourceCompanionRetainsIdentityAndBudgetGuards(); TestCommonSaveRefusesPartialWrite();
        std::cout << "sealed GUI/source companion transaction and common save rejection passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
