#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::Services;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    Require(stream.good(), "write synthetic catalog");
}

struct Fixture {
    inline static std::atomic<std::uint64_t> sequence{0};
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("Audastra-ObjectSave-" + std::to_string(GetCurrentProcessId()) + "-"
            + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
    std::vector<std::byte> originalNative;
    Trigger::UiCatalog catalog;
    Trigger::TriggerDocument sourceTriggers;
    Trigger::EncodedTriggerDocument originalPair;
    LniObjectDocument edited;
    MapEditSaveRequest request;

    Fixture() {
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
        const auto loadedCatalog = Trigger::UiCatalog::Load(root / "ui");
        Require(loadedCatalog.Succeeded() && loadedCatalog.value, "load synthetic catalog");
        catalog = *loadedCatalog.value;
        auto triggers = Trigger::WtgWctCodec{}.CreateEmpty(catalog);
        Require(triggers.Succeeded() && triggers.value, "create sealed synthetic triggers");
        const auto pair = Trigger::WtgWctCodec{}.Serialize(*triggers.value, catalog);
        Require(pair.Succeeded() && pair.value, "encode original trigger pair");
        originalPair = *pair.value;
        auto parsedTriggers = Trigger::WtgWctCodec{}.Parse(originalPair.wtg, originalPair.wct, catalog);
        Require(parsedTriggers.Succeeded() && parsedTriggers.value, "seal actual source pair");
        sourceTriggers = std::move(*parsedTriggers.value);
        auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
        Require(native.Succeeded() && native.value, "create synthetic native baseline");
        Require(native.value->AddCustomObject("H001", "hfoo").Succeeded(), "create original native object");
        Require(native.value->AddModification({ObjectType::Unit, "H001"}, {"uhpm"}, NativeValueType::Int32, std::int32_t{420}).Succeeded(),
            "set original integer field");
        const auto encoded = NativeObjectCodec(ObjectType::Unit).Serialize(*native.value);
        Require(encoded.Succeeded() && encoded.value, "encode source object member");
        originalNative = *encoded.value;
        const std::vector<MapArchiveMember> members{{"war3map.wtg", pair.value->wtg},
            {"war3map.wct", pair.value->wct}, {"war3map.w3u", originalNative},
            {"war3map.j", {std::byte{'/'}, std::byte{'/'}}}, {"opaque.bin", {std::byte{19}, std::byte{0}}}};
        const auto written = WriteVerifiedArchive(root / "source.w3x", members);
        Require(written.Succeeded() && written.value, "write synthetic source archive");
        const auto text = ConvertNativeObjectDataToLni(ObjectType::Unit, originalNative);
        Require(text.Succeeded() && text.value, "project native into real editable document");
        auto parsed = LniObjectCodec(ObjectType::Unit).Parse(*text.value);
        const auto* lni = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
        Require(lni != nullptr, "parse source LNI");
        edited = *lni;
        Require(edited.SetModification({ObjectType::Unit, "H001"}, {"uhpm"}, std::int32_t{900}).Succeeded(), "edit typed field");
        request.sourceMap = written.value->path;
        request.targetCandidate = root / "candidate.w3x";
        request.expectedSourceSha256 = written.value->sha256;
        request.toolRoot = root / "missing-toolchain";
        request.uiRoot = root / "ui";
        request.gameRoot = root;
    }
    auto Freeze(std::string sha) {
        return ObjectTransactionSnapshot::Freeze(edited, originalNative, std::move(sha));
    }
};

void TestCommonSaveRetainsSnapshotWhenCompilerIsMissing() {
    Fixture fixture;
    const auto frozen = fixture.Freeze(fixture.request.expectedSourceSha256);
    Require(frozen.Succeeded() && frozen.value, "freeze trusted source plus candidate");
    fixture.request.objectTransactions = {*frozen.value};
    const auto result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "SCRIPT_TOOLCHAIN_MISSING"),
        "object-only transaction reaches the same supervised script preparation");
    Require(!std::filesystem::exists(fixture.request.targetCandidate), "missing compiler never writes partial archive");
    Require(Sha256File(fixture.request.sourceMap).value == fixture.request.expectedSourceSha256, "source archive stays untouched");
    Require((*frozen.value)->VerifyAgainst(fixture.request.expectedSourceSha256, fixture.originalNative).Succeeded(),
        "failure does not consume or mutate immutable transaction");
}

void TestSaveRejectsSourceConflictDuplicateAndMixedDomains() {
    Fixture fixture;
    auto frozen = fixture.Freeze(std::string(64, 'a'));
    Require(frozen.Succeeded() && frozen.value, "freeze differently bound source fixture");
    fixture.request.objectTransactions = {*frozen.value};
    auto result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "OBJECT_TRANSACTION_SOURCE_CONFLICT"), "source conflict checked before compile");
    frozen = fixture.Freeze(fixture.request.expectedSourceSha256);
    Require(frozen.Succeeded() && frozen.value, "freeze actual source");
    fixture.request.objectTransactions = {*frozen.value, *frozen.value};
    result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "SAVE_OBJECT_TRANSACTION_DUPLICATE"), "duplicate category never reaches script pipeline");
    fixture.request.objectTransactions = {nullptr};
    result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "SAVE_OBJECT_TRANSACTION_INVALID"), "null candidate rejected");
    fixture.request.objectTransactions = {*frozen.value};
    fixture.request.triggerEdits.emplace();
    result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "SAVE_OPERATION_SCOPE_INVALID"), "object and trigger edits cannot evade domain scope");
    Require(!std::filesystem::exists(fixture.request.targetCandidate), "all failed requests preserve absent target");
}

void TestSaveVerifiesActualNativeBytesAndPresence() {
    Fixture fixture;
    auto parsed = NativeObjectCodec(ObjectType::Unit).Parse(fixture.originalNative);
    auto* native = dynamic_cast<NativeObjectDocument*>(parsed.document.get());
    Require(parsed.Succeeded() && native, "parse actual native baseline");
    Require(native->SetModification({ObjectType::Unit, "H001"}, {"uhpm"}, std::int32_t{421}).Succeeded(),
        "make separately bound original bytes");
    const auto differentOriginal = NativeObjectCodec(ObjectType::Unit).Serialize(*native);
    Require(differentOriginal.Succeeded() && differentOriginal.value, "encode other original");
    auto frozen = ObjectTransactionSnapshot::Freeze(fixture.edited, *differentOriginal.value,
        fixture.request.expectedSourceSha256);
    Require(frozen.Succeeded() && frozen.value, "freeze candidate against other native bytes");
    fixture.request.objectTransactions = {*frozen.value};
    auto result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "OBJECT_TRANSACTION_MEMBER_CONFLICT"),
        "same archive SHA label cannot replace actual original member verification");
    frozen = ObjectTransactionSnapshot::Freeze(fixture.edited, std::nullopt,
        fixture.request.expectedSourceSha256);
    Require(frozen.Succeeded() && frozen.value, "freeze explicitly missing baseline");
    fixture.request.objectTransactions = {*frozen.value};
    result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "OBJECT_TRANSACTION_MEMBER_PRESENCE_CONFLICT"),
        "existing native member cannot be treated as absent");
    Require(!std::filesystem::exists(fixture.request.targetCandidate), "source mismatch never publishes");
    Require(Sha256File(fixture.request.sourceMap).value == fixture.request.expectedSourceSha256,
        "native source guards preserve archive");
}

void TestChangedAndUnchangedTriggerCompanionsReachCommonObjectSave() {
    Fixture fixture;
    const auto object = fixture.Freeze(fixture.request.expectedSourceSha256);
    Require(object.Succeeded() && object.value, "freeze joint object input");
    fixture.request.objectTransactions = {*object.value};
    const Trigger::TriggerTransactionBinding binding{fixture.request.expectedSourceSha256, 7,
        {0, std::string(64, 'a'), fixture.catalog.Sha256()}, 1};
    auto changed = fixture.sourceTriggers;
    Require(Trigger::TriggerDocumentEditor::AddVariable(changed, "JointInteger", "integer").Succeeded(),
        "make genuine GUI variable change");
    auto trigger = Trigger::TriggerTransactionSnapshot::Freeze(changed, fixture.catalog,
        fixture.originalPair.wtg, fixture.originalPair.wct, binding);
    Require(trigger.Succeeded() && trigger.value, "freeze complete changed GUI companion");
    fixture.request.triggerTransaction = *trigger.value;
    auto result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "SCRIPT_TOOLCHAIN_MISSING"),
        "changed GUI and object seals share the supervised script preparation");
    trigger = Trigger::TriggerTransactionSnapshot::FreezeSourceCompanion(fixture.sourceTriggers, fixture.catalog,
        fixture.originalPair.wtg, fixture.originalPair.wct, binding);
    Require(trigger.Succeeded() && trigger.value && (*trigger.value)->IsSourceCompanion(),
        "freeze byte-exact unchanged source pair");
    fixture.request.triggerTransaction = *trigger.value;
    result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "SCRIPT_TOOLCHAIN_MISSING"),
        "pure object edit with unchanged pair reaches the same script preparation");
    fixture.request.objectTransactions.clear();
    result = SaveMapEditCandidate(fixture.request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "SAVE_SOURCE_COMPANION_WITHOUT_OBJECTS"),
        "unchanged trigger companion cannot authorize an empty save");
    Require(!std::filesystem::exists(fixture.request.targetCandidate), "missing compiler and empty save never publish");
    Require(Sha256File(fixture.request.sourceMap).value == fixture.request.expectedSourceSha256,
        "joint transaction failures preserve archive");
}
} // namespace

int main() {
    try {
        TestCommonSaveRetainsSnapshotWhenCompilerIsMissing();
        TestSaveRejectsSourceConflictDuplicateAndMixedDomains();
        TestSaveVerifiesActualNativeBytesAndPresence();
        TestChangedAndUnchangedTriggerCompanionsReachCommonObjectSave();
        std::cout << "typed object transactions use the common fail-closed save pipeline\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
