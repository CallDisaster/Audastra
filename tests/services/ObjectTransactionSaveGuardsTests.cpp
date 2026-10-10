#include "War3MapCore/Services/ObjectTransactionSaveGuards.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::Services;
using Snapshot = std::shared_ptr<const ObjectTransactionSnapshot>;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool HasCode(const Status& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const Diagnostic& diagnostic) { return diagnostic.code == code; });
}
void Reject(const Status& result, std::string_view code) {
    Require(!result.Succeeded() && HasCode(result, code), "expected template submission boundary rejection missing");
}
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}
std::vector<std::byte> Encoded(const NativeObjectDocument& native) {
    const auto bytes = NativeObjectCodec(native.Type()).Serialize(native);
    Require(bytes.Succeeded() && bytes.value, "native fixture serializes through its real codec");
    return *bytes.value;
}
LniObjectDocument Editable(ObjectType type, std::span<const std::byte> native) {
    const auto text = ConvertNativeObjectDataToLni(type, native);
    Require(text.Succeeded() && text.value, "real native fixture projects into editable LNI");
    const auto parsed = LniObjectCodec(type).Parse(*text.value);
    const auto* document = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
    Require(document != nullptr, "projected LNI parses");
    return *document;
}
NativeObjectDocument Native(ObjectType type) {
    const auto empty = NativeObjectDocument::CreateEmpty(type);
    Require(empty.Succeeded() && empty.value, "native fixture uses classic v2 empty baseline");
    auto native = *empty.value;
    if (type == ObjectType::Unit) {
        Require(native.AddCustomObject("H001", "hfoo").Succeeded(), "unit fixture object exists");
        Require(native.AddModification({type, "H001"}, {"uhpm"}, NativeValueType::Int32, std::int32_t{420}).Succeeded(),
            "unit fixture contains an actual integer field");
    } else {
        Require(type == ObjectType::Ability && native.AddCustomObject("A001", "AHbz").Succeeded(), "ability fixture object exists");
        Require(native.AddModification({type, "A001"}, {"alev", 1u, 0u}, NativeValueType::Int32, std::int32_t{1}).Succeeded(),
            "ability fixture uses its actual indexed binary layout");
    }
    return native;
}

std::filesystem::path ScratchRoot() {
    static std::atomic<std::uint64_t> sequence{0};
    return std::filesystem::temp_directory_path()
        / ("Audastra-ObjectSaveGuards-" + std::to_string(GetCurrentProcessId()) + "-"
            + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
}

struct Fixture {
    std::filesystem::path root = ScratchRoot();
    NativeObjectDocument unit = Native(ObjectType::Unit);
    NativeObjectDocument ability = Native(ObjectType::Ability);
    std::vector<std::byte> originalUnit = Encoded(unit);
    std::vector<std::byte> originalAbility = Encoded(ability);
    MapArchiveSession source;
    LniObjectDocument edited;
    Snapshot snapshot;
    std::string sourceSha;

    explicit Fixture(bool unitPresent = true, bool abilityPresent = true, bool zeroByteUnit = false) {
        std::filesystem::create_directory(root);
        std::vector<MapArchiveMember> members{{"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")},
            {"preserve.bin", {std::byte{0}, std::byte{0xff}}}};
        if (unitPresent) members.push_back({"war3map.w3u", zeroByteUnit ? std::vector<std::byte>{} : originalUnit});
        if (abilityPresent) members.push_back({"war3map.w3a", originalAbility});
        const auto saved = WriteVerifiedArchive(root / "source.w3x", members);
        Require(saved.Succeeded() && saved.value, "original guard fixture is a verified MPQ archive");
        auto opened = MapArchiveSession::Open(saved.value->path);
        Require(opened.Succeeded() && opened.value, "actual source session opens");
        source = std::move(*opened.value);
        sourceSha = source.SourceSha256();
        edited = Editable(ObjectType::Unit, originalUnit);
        Require(edited.SetModification({ObjectType::Unit, "H001"}, {"uhpm"}, std::int32_t{900}).Succeeded(), "unit field is edited through the real LNI API");
        const auto frozen = ObjectTransactionSnapshot::Freeze(edited, originalUnit, sourceSha);
        Require(frozen.Succeeded() && frozen.value, "real typed snapshot freezes");
        snapshot = *frozen.value;
    }
    ~Fixture() {
        std::error_code error;
        if (root.filename().wstring().starts_with(L"Audastra-ObjectSaveGuards-")) std::filesystem::remove_all(root, error);
    }
    Script::ScriptSaveBundle Bundle(std::vector<MemberReplacement> objects) const {
        // 单元证据只验证已检查产物的提交边界；这个 synthetic checkedBy 不冒称真实编译。
        Script::PipelineResult pipeline;
        pipeline.script = "function main takes nothing returns nothing\nendfunction\n";
        pipeline.syntaxChecked = true;
        pipeline.checkedBy = "synthetic object transaction save-guard contract";
        pipeline.objectMembers = std::move(objects);
        auto bundle = Script::PrepareScriptSaveBundle(pipeline, "war3map.j");
        Require(bundle.Succeeded() && bundle.value, "generated bytes pass the real script save-bundle gate");
        return std::move(*bundle.value);
    }
    MemberReplacement SealedMember() const {
        return {"war3map.w3u", {snapshot->EncodedNative().begin(), snapshot->EncodedNative().end()}};
    }
    Status Guard(const Script::ScriptSaveBundle& bundle, const std::vector<Snapshot>& seals) const {
        return ValidateGeneratedObjectMembersForSave(source, seals, bundle.members, "war3map.j");
    }
    void Unchanged() const {
        const auto hash = Sha256File(source.SourcePath());
        Require(hash.Succeeded() && hash.value && *hash.value == sourceSha, "guard checks never modify the original MPQ");
        Require(snapshot->VerifyAgainst(sourceSha, originalUnit).Succeeded(), "guard does not consume or alter frozen source bytes");
        const auto candidateHash = Sha256(snapshot->EncodedNative()), lniHash = Sha256(snapshot->SerializedLni());
        Require(candidateHash.Succeeded() && candidateHash.value && lniHash.Succeeded() && lniHash.value
            && snapshot->CandidateSha256() == *candidateHash.value
            && snapshot->LniSha256() == *lniHash.value, "owned final LNI/native identities remain intact");
        Require(!std::filesystem::exists(root / "candidate.w3x"), "pure guard never publishes a candidate");
    }
};

void TestAuthorizedCandidateAndUnsealedOriginalSucceed() {
    Fixture fixture;
    const auto bundle = fixture.Bundle({fixture.SealedMember(), {"war3map.w3a", fixture.originalAbility}});
    Require(fixture.Guard(bundle, {fixture.snapshot}).Succeeded(), "authorized candidate and another category's exact original bytes both pass");
    const auto scriptOnly = fixture.Bundle({});
    Require(fixture.Guard(scriptOnly, {fixture.snapshot}).Succeeded(), "a template need not regenerate a snapshot member already in final-save replacements");
    const auto unchanged = fixture.Bundle({{"war3map.w3u", fixture.originalUnit}, {"war3map.w3a", fixture.originalAbility}});
    Require(fixture.Guard(unchanged, {}).Succeeded(), "zero seals retains the existing narrow map-data equality contract");
    fixture.Unchanged();
}

void TestSameCategoryTemplateCannotChangeSealedCandidate() {
    Fixture fixture;
    auto changed = fixture.snapshot->CandidateNative();
    Require(changed.SetModification({ObjectType::Unit, "H001"}, {"uhpm"}, std::int32_t{901}).Succeeded(), "template field mutation fixture is real native data");
    auto bundle = fixture.Bundle({{"war3map.w3u", Encoded(changed)}});
    Reject(fixture.Guard(bundle, {fixture.snapshot}), "SAVE_GENERATED_OBJECT_TRANSACTION_CHANGED");
    changed = fixture.snapshot->CandidateNative();
    Require(changed.AddCustomObject("H002", "hfoo").Succeeded(), "template extra object fixture uses normal native API");
    bundle = fixture.Bundle({{"war3map.w3u", Encoded(changed)}});
    Reject(fixture.Guard(bundle, {fixture.snapshot}), "SAVE_GENERATED_OBJECT_TRANSACTION_CHANGED");
    const auto empty = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    Require(empty.Succeeded() && empty.value, "template parent mutation has an isolated native table");
    changed = *empty.value;
    Require(changed.AddCustomObject("H001", "hrif").Succeeded()
        && changed.AddModification({ObjectType::Unit, "H001"}, {"uhpm"}, NativeValueType::Int32, std::int32_t{900}).Succeeded(),
        "template parent changes without relying on an unavailable setter");
    bundle = fixture.Bundle({{"war3map.w3u", Encoded(changed)}});
    Reject(fixture.Guard(bundle, {fixture.snapshot}), "SAVE_GENERATED_OBJECT_TRANSACTION_CHANGED");
    fixture.Unchanged();
}

void TestOtherCategoryNeverInheritsUnitAuthorization() {
    Fixture fixture;
    auto altered = fixture.ability;
    Require(altered.SetModification({ObjectType::Ability, "A001"}, {"alev", 1u, 0u}, std::int32_t{2}).Succeeded(), "unsealed ability field can be mutated for the negative fixture");
    const auto changed = fixture.Bundle({fixture.SealedMember(), {"war3map.w3a", Encoded(altered)}});
    Reject(fixture.Guard(changed, {fixture.snapshot}), "SAVE_GENERATED_NATIVE_CHANGED");
    fixture.Unchanged();
    Fixture absent(true, false);
    const auto added = absent.Bundle({absent.SealedMember(), {"war3map.w3a", absent.originalAbility}});
    Reject(absent.Guard(added, {absent.snapshot}), "SAVE_GENERATED_NATIVE_CHANGED");
    absent.Unchanged();
}

void TestEverySealRetainsSourceAndPresenceIdentity() {
    Fixture fixture;
    const auto scriptOnly = fixture.Bundle({});
    auto wrongMap = ObjectTransactionSnapshot::Freeze(fixture.edited, fixture.originalUnit, std::string(64, 'a'));
    Require(wrongMap.Succeeded() && wrongMap.value, "negative source identity fixture freezes independently");
    Reject(fixture.Guard(scriptOnly, {*wrongMap.value}), "OBJECT_TRANSACTION_SOURCE_CONFLICT");
    auto wrongSource = fixture.unit;
    Require(wrongSource.SetModification({ObjectType::Unit, "H001"}, {"uhpm"}, std::int32_t{421}).Succeeded(), "different original native fixture is valid");
    const auto wrongBytes = Encoded(wrongSource);
    auto wrongMember = ObjectTransactionSnapshot::Freeze(fixture.edited, wrongBytes, fixture.sourceSha);
    Require(wrongMember.Succeeded() && wrongMember.value, "snapshot can demonstrate actual source member conflict");
    Reject(fixture.Guard(scriptOnly, {*wrongMember.value}), "OBJECT_TRANSACTION_MEMBER_CONFLICT");
    const auto missing = ObjectTransactionSnapshot::Freeze(fixture.edited, std::nullopt, fixture.sourceSha);
    Require(missing.Succeeded() && missing.value, "explicitly missing-source fixture creates a classic v2 candidate");
    Reject(fixture.Guard(scriptOnly, {*missing.value}), "OBJECT_TRANSACTION_MEMBER_PRESENCE_CONFLICT");
    Reject(fixture.Guard(scriptOnly, {fixture.snapshot, fixture.snapshot}), "SAVE_OBJECT_TRANSACTION_DUPLICATE");
    Reject(fixture.Guard(scriptOnly, {nullptr}), "SAVE_OBJECT_TRANSACTION_INVALID");
    fixture.Unchanged();
    Fixture absent(false, false);
    const auto absentScript = absent.Bundle({});
    Reject(absent.Guard(absentScript, {absent.snapshot}), "OBJECT_TRANSACTION_MEMBER_PRESENCE_CONFLICT");
    const auto newCategory = ObjectTransactionSnapshot::Freeze(absent.edited, std::nullopt, absent.sourceSha);
    Require(newCategory.Succeeded() && newCategory.value, "actual absent category receives its own missing-source seal");
    const auto created = absent.Bundle({{"war3map.w3u", {(*newCategory.value)->EncodedNative().begin(), (*newCategory.value)->EncodedNative().end()}}});
    Require(absent.Guard(created, {*newCategory.value}).Succeeded(), "a correctly sealed absent category may generate its exact new native bytes");
    Fixture empty(true, false, true);
    const auto emptyScript = empty.Bundle({});
    const auto emptyMissing = ObjectTransactionSnapshot::Freeze(empty.edited, std::nullopt, empty.sourceSha);
    Require(emptyMissing.Succeeded() && emptyMissing.value, "zero-byte original is not used as a missing-source freeze input");
    Reject(empty.Guard(emptyScript, {*emptyMissing.value}), "OBJECT_TRANSACTION_MEMBER_PRESENCE_CONFLICT");
}

void TestGeneratedMemberDomainCannotBypassGuard() {
    Fixture fixture;
    auto bundle = fixture.Bundle({fixture.SealedMember()});
    bundle.members.push_back({"unrelated.bin", {std::byte{0}}});
    Reject(fixture.Guard(bundle, {fixture.snapshot}), "SAVE_GENERATED_OBJECT_MEMBER_UNSUPPORTED");
    bundle = fixture.Bundle({fixture.SealedMember()});
    bundle.members.push_back({"WAR3MAP.W3U", fixture.originalUnit});
    Reject(fixture.Guard(bundle, {fixture.snapshot}), "SAVE_GENERATED_OBJECT_MEMBER_DUPLICATE");
    bundle = fixture.Bundle({fixture.SealedMember()});
    Reject(ValidateGeneratedObjectMembersForSave(fixture.source, std::vector<Snapshot>{fixture.snapshot}, bundle.members, "../war3map.j"),
        "SAVE_GENERATED_SCRIPT_MEMBER_INVALID");
    fixture.Unchanged();
}

} // namespace

int main() {
    try {
        TestAuthorizedCandidateAndUnsealedOriginalSucceed();
        TestSameCategoryTemplateCannotChangeSealedCandidate();
        TestOtherCategoryNeverInheritsUnitAuthorization();
        TestEverySealRetainsSourceAndPresenceIdentity();
        TestGeneratedMemberDomainCannotBypassGuard();
        std::cout << "ObjectTransactionSaveGuardsTests: 5 groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ObjectTransactionSaveGuardsTests: " << error.what() << '\n';
        return 1;
    }
}
