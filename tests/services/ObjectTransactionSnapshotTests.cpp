#include "War3MapCore/Services/ObjectTransactionSnapshot.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/ObjectData/ObjectImport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::Services;
using SnapshotResult = Result<std::shared_ptr<const ObjectTransactionSnapshot>>;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& diagnostic) { return diagnostic.code == code; });
}

void RequireRejected(const SnapshotResult& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "rejected freeze must not expose a partial snapshot");
    Require(HasCode(result.diagnostics, code), "expected freeze diagnostic missing");
}

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

std::string SourceSha(char ch = 'a') { return std::string(64, ch); }

struct Binary {
    std::vector<std::byte> bytes;
    void U32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
    void Id(std::string_view id) {
        Require(id.size() == 4, "synthetic native rawcode is exactly four bytes");
        for (const unsigned char ch : id) bytes.push_back(static_cast<std::byte>(ch));
    }
    void String(std::string_view value) {
        for (const unsigned char ch : value) bytes.push_back(static_cast<std::byte>(ch));
        bytes.push_back(std::byte{0});
    }
};

bool HasIndexes(ObjectType type, std::uint32_t version) {
    return type == ObjectType::Ability || type == ObjectType::Upgrade
        || (type == ObjectType::Doodad && version != 1);
}

void FieldHeader(Binary& binary, ObjectType type, std::uint32_t version, std::string_view name, NativeValueType valueType) {
    binary.Id(name);
    binary.U32(static_cast<std::uint32_t>(valueType));
    if (HasIndexes(type, version)) { binary.U32(1); binary.U32(0); }
}

// 原创真实二进制结构：一个标准覆盖和一个自定义记录，含Int/String/Unreal及非零合法尾标记。
std::vector<std::byte> Native(ObjectType type = ObjectType::Unit, std::uint32_t version = 2) {
    Binary binary;
    binary.U32(version);
    binary.U32(1);
    binary.Id("hfoo"); binary.U32(0);
    if (version == 3) binary.U32(0);
    binary.U32(3);
    FieldHeader(binary, type, version, "uhpm", NativeValueType::Int32);
    binary.U32(420); binary.Id("hfoo");
    FieldHeader(binary, type, version, "unam", NativeValueType::String);
    binary.String("Synthetic unit"); binary.U32(0);
    FieldHeader(binary, type, version, "usca", NativeValueType::Unreal);
    binary.U32(std::bit_cast<std::uint32_t>(1.25f)); binary.Id("hfoo");
    binary.U32(1);
    binary.Id("hfoo"); binary.Id("H001");
    if (version == 3) binary.U32(0);
    binary.U32(1);
    FieldHeader(binary, type, version, "uhpm", NativeValueType::Int32);
    binary.U32(17); binary.Id("H001");
    return std::move(binary.bytes);
}

LniObjectDocument LniFromText(ObjectType type, std::string_view text) {
    const auto parsed = LniObjectCodec(type).Parse(Bytes(text));
    const auto* document = parsed.Succeeded() && parsed.document
        ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
    Require(document != nullptr && document->IsStructurallySafe(), "synthetic LNI must parse through the real public codec");
    return *document;
}

LniObjectDocument LniFromNative(ObjectType type, std::span<const std::byte> native) {
    const auto converted = ConvertNativeObjectDataToLni(type, native);
    Require(converted.Succeeded() && converted.value, "native fixture must import into a real LNI document");
    const auto parsed = LniObjectCodec(type).Parse(*converted.value);
    const auto* document = parsed.Succeeded() && parsed.document
        ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
    Require(document != nullptr, "imported LNI fixture parses");
    return *document;
}

const ObjectModification& Field(const NativeObjectDocument& document, std::string_view rawcode, std::string_view fieldId) {
    const auto* object = document.FindObject({document.Type(), std::string(rawcode)});
    Require(object != nullptr, "reopened native object identity exists");
    const auto found = std::find_if(object->modifications.begin(), object->modifications.end(),
        [&](const auto& field) { return field.key.fieldId == fieldId; });
    Require(found != object->modifications.end(), "reopened native field identity exists");
    return *found;
}

void FreezeFieldEditAndOwnInputs() {
    auto original = Native();
    const auto originalCopy = original;
    auto edited = LniFromNative(ObjectType::Unit, original);
    Require(edited.SetModification({ObjectType::Unit, "hfoo"}, {"uhpm"}, std::int32_t{900}).Succeeded(), "field edit uses normal LNI API");
    Require(edited.SetModification({ObjectType::Unit, "hfoo"}, {"usca"}, std::bit_cast<float>(0x80000000U)).Succeeded(),
        "existing Unreal receives a negative-zero value for exact bit readback");
    const auto frozen = ObjectTransactionSnapshot::Freeze(edited, original, SourceSha());
    Require(frozen.Succeeded() && frozen.value, "edited native snapshot freezes");
    const auto& snapshot = **frozen.value;
    Require(snapshot.Type() == ObjectType::Unit && snapshot.OriginalNative(), "snapshot has exact category and original presence");
    Require(std::get<std::int32_t>(Field(snapshot.CandidateNative(), "hfoo", "uhpm").value) == 900, "encoded candidate contains real edited value");
    const auto& unreal = Field(snapshot.CandidateNative(), "hfoo", "usca");
    Require(snapshot.CandidateNative().ModificationValueType({ObjectType::Unit, "hfoo"}, unreal.key) == NativeValueType::Unreal,
        "existing Unreal wire type is preserved rather than inferred as Real");
    Require(std::bit_cast<std::uint32_t>(std::get<float>(unreal.value)) == 0x80000000U,
        "floating values retain their exact bit pattern through candidate readback");
    const auto originalHash = Sha256(originalCopy), candidateHash = Sha256(snapshot.EncodedNative());
    Require(originalHash.Succeeded() && candidateHash.Succeeded()
        && snapshot.OriginalSha256() == *originalHash.value && snapshot.CandidateSha256() == *candidateHash.value,
        "hashes derive from actual owned member bytes");
    const auto encodedCopy = std::vector<std::byte>(snapshot.EncodedNative().begin(), snapshot.EncodedNative().end());
    const auto lniCopy = std::vector<std::byte>(snapshot.SerializedLni().begin(), snapshot.SerializedLni().end());
    const auto lniHash = Sha256(lniCopy);
    Require(lniHash.Succeeded() && snapshot.LniSha256() == *lniHash.value,
        "final LNI identity derives from the serialized bytes owned by this frozen transaction");
    original.back() = std::byte{0x77};
    Require(edited.SetModification({ObjectType::Unit, "hfoo"}, {"uhpm"}, std::int32_t{1000}).Succeeded(), "caller may continue editing independently");
    Require(std::ranges::equal(*snapshot.OriginalNative(), originalCopy)
        && std::ranges::equal(snapshot.EncodedNative(), encodedCopy)
        && std::ranges::equal(snapshot.SerializedLni(), lniCopy), "caller mutations cannot alter frozen LNI or native bytes");
    const auto text = LniObjectCodec(ObjectType::Unit).Serialize(snapshot.EditedDocument());
    Require(text.Succeeded() && text.value, "frozen LNI remains usable by the shared script-save task");
    Require(*text.value == lniCopy, "frozen document and final serialized LNI describe the same state");
    Require(snapshot.VerifyAgainst(SourceSha(), originalCopy).Succeeded(), "actual original member verifies");
    Require(snapshot.VerifyAgainst(SourceSha('A'), originalCopy).Succeeded(), "SHA accepts strict hexadecimal uppercase and normalizes identity");
    Require(HasCode(snapshot.VerifyAgainst(SourceSha('b'), originalCopy).diagnostics, "OBJECT_TRANSACTION_SOURCE_CONFLICT"), "different map source is rejected");
    Require(HasCode(snapshot.VerifyAgainst(SourceSha(), original).diagnostics, "OBJECT_TRANSACTION_MEMBER_CONFLICT"), "different original bytes are rejected even if caller repeats map identity");
    Require(HasCode(snapshot.VerifyAgainst(SourceSha(), std::nullopt).diagnostics, "OBJECT_TRANSACTION_MEMBER_PRESENCE_CONFLICT"), "present original cannot become absent");
}

void RejectInvalidValuesWithoutPartialState() {
    const auto original = Native();
    auto edited = LniFromNative(ObjectType::Unit, original);
    Require(edited.SetModification({ObjectType::Unit, "hfoo"}, {"uhpm"}, std::string{"not an integer"}).Succeeded(), "LNI text can express an invalid native value for the negative fixture");
    const auto before = LniObjectCodec(ObjectType::Unit).Serialize(edited);
    const auto result = ObjectTransactionSnapshot::Freeze(edited, original, SourceSha());
    RequireRejected(result, "OBJECT_TRANSACTION_APPLY_REJECTED");
    const auto after = LniObjectCodec(ObjectType::Unit).Serialize(edited);
    Require(before.value == after.value, "rejected native conversion never mutates the edited document");
    RequireRejected(ObjectTransactionSnapshot::Freeze(edited, original, ""), "OBJECT_TRANSACTION_SOURCE_IDENTITY_INVALID");
    RequireRejected(ObjectTransactionSnapshot::Freeze(edited, original, std::string(64, 'z')), "OBJECT_TRANSACTION_SOURCE_IDENTITY_INVALID");
    RequireRejected(ObjectTransactionSnapshot::Freeze(edited, original, " " + SourceSha()), "OBJECT_TRANSACTION_SOURCE_IDENTITY_INVALID");
}

void DistinguishMissingEmptyAndValidEmptyMember() {
    auto emptyLni = LniFromText(ObjectType::Unit, "");
    const auto missing = ObjectTransactionSnapshot::Freeze(emptyLni, std::nullopt, SourceSha());
    Require(missing.Succeeded() && missing.value, "explicitly missing member uses a classic v2 empty candidate");
    const auto& snapshot = **missing.value;
    Require(!snapshot.OriginalNative() && snapshot.OriginalSha256().empty()
        && snapshot.CandidateNative().Version() == 2 && snapshot.EncodedNative().size() == 12,
        "missing original is represented separately from encoded empty v2 table");
    Require(snapshot.VerifyAgainst(SourceSha(), std::nullopt).Succeeded(), "same missing member verifies");
    Require(HasCode(snapshot.VerifyAgainst(SourceSha(), std::span<const std::byte>{}).diagnostics,
        "OBJECT_TRANSACTION_MEMBER_PRESENCE_CONFLICT"), "present zero-byte member cannot match a frozen missing member");
    RequireRejected(ObjectTransactionSnapshot::Freeze(emptyLni, std::span<const std::byte>{}, SourceSha()), "OBJECT_TRANSACTION_NATIVE_EMPTY");
    const auto validEmpty = std::vector<std::byte>(snapshot.EncodedNative().begin(), snapshot.EncodedNative().end());
    const auto present = ObjectTransactionSnapshot::Freeze(emptyLni, validEmpty, SourceSha());
    Require(present.Succeeded() && present.value && (*present.value)->OriginalNative(), "real encoded empty member remains present with its own hash");
    Require(HasCode(snapshot.VerifyAgainst(SourceSha(), validEmpty).diagnostics,
        "OBJECT_TRANSACTION_MEMBER_PRESENCE_CONFLICT"), "adding even a valid empty member changes source identity");
    auto added = LniFromText(ObjectType::Unit, "[H009]\n_parent = \"hfoo\"\nuhpm = 120\n");
    const auto created = ObjectTransactionSnapshot::Freeze(added, std::nullopt, SourceSha());
    Require(created.Succeeded() && created.value && (*created.value)->CandidateNative().CustomObjectCount() == 1,
        "first custom object is frozen using the same absent-member path");
}

void AllSevenTypesAndOriginalGroupingRoundTrip() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
            ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        const auto original = Native(type);
        const auto edited = LniFromNative(type, original);
        const auto frozen = ObjectTransactionSnapshot::Freeze(edited, original, SourceSha());
        Require(frozen.Succeeded() && frozen.value && (*frozen.value)->Type() == type,
            "all seven categories freeze through their actual native layouts");
        Require(std::ranges::equal((*frozen.value)->EncodedNative(), original), "unchanged members roundtrip byte for byte");
    }
    const auto original = Native();
    auto edited = LniFromNative(ObjectType::Unit, original);
    Require(edited.AddOriginalObject("hbar").Succeeded(), "new original override is added through LNI API");
    Require(edited.SetModification({ObjectType::Unit, "hbar"}, {"uhpm"}, std::int32_t{510}).Succeeded(), "new original field exists in the edit");
    const auto frozen = ObjectTransactionSnapshot::Freeze(edited, original, SourceSha());
    Require(frozen.Succeeded() && frozen.value && (*frozen.value)->CandidateNative().OriginalObjectCount() == 2,
        "writing Original before Custom does not fail due to memory insertion order");
    Require(std::get<std::int32_t>(Field((*frozen.value)->CandidateNative(), "hbar", "uhpm").value) == 510,
        "new original override survives the complete candidate readback");
}

void VersionOneEditsAreNarrowAndUnknownVersionsReject() {
    const auto original = Native(ObjectType::Unit, 1);
    auto edited = LniFromNative(ObjectType::Unit, original);
    Require(edited.SetModification({ObjectType::Unit, "hfoo"}, {"uhpm"}, std::int32_t{900}).Succeeded(), "v1 existing value is edited");
    const auto frozen = ObjectTransactionSnapshot::Freeze(edited, original, SourceSha());
    Require(frozen.Succeeded() && frozen.value && (*frozen.value)->CandidateNative().Version() == 1,
        "v1 existing-value edit retains the source version");
    Require(edited.AddObject("H009", "hfoo").Succeeded(), "LNI can represent a structure edit for the v1 rejection test");
    const auto rejected = ObjectTransactionSnapshot::Freeze(edited, original, SourceSha());
    RequireRejected(rejected, "OBJECT_TRANSACTION_APPLY_REJECTED");
    Require(HasCode(rejected.diagnostics, "native-v1-structure-unsupported"), "v1 refusal explains the exact structure capability boundary");
    const auto unsupported = Native(ObjectType::Unit, 3);
    const auto current = LniFromNative(ObjectType::Unit, Native());
    const auto unknown = ObjectTransactionSnapshot::Freeze(current, unsupported, SourceSha());
    Require(!unknown.Succeeded() && !unknown.value, "readable but unsupported v3 cannot become a write snapshot");
}

void OpaqueFieldsKeepActualNativeBytesOrReject() {
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    Require(native.Succeeded() && native.value && native.value->AddOriginalObject("hfoo").Succeeded(), "synthetic opaque source object is created");
    Require(native.value->AddModification({ObjectType::Unit, "hfoo"}, {"S001"}, NativeValueType::String,
        std::string{"Original\r\ntext\n"}).Succeeded(), "original multiline source is known native string data");
    const auto original = NativeObjectCodec(ObjectType::Unit).Serialize(*native.value);
    Require(original.Succeeded() && original.value, "native opaque fixture has real source bytes");
    const auto edited = LniFromText(ObjectType::Unit, "[hfoo]\nS001 = {\n\"legacy\",\n}\n");
    Require(edited.Objects().front().modifications.front().valueWasOpaque, "fixture is an actual opaque legacy block");
    const auto frozen = ObjectTransactionSnapshot::Freeze(edited, *original.value, SourceSha());
    Require(frozen.Succeeded() && frozen.value && std::ranges::equal((*frozen.value)->EncodedNative(), *original.value),
        "opaque LNI entry keeps the actual original native field bytes");
    RequireRejected(ObjectTransactionSnapshot::Freeze(edited, std::nullopt, SourceSha()), "OBJECT_TRANSACTION_APPLY_REJECTED");
    const auto newOpaque = LniFromText(ObjectType::Unit, "[hfoo]\nS999 = {\n\"new\",\n}\n");
    RequireRejected(ObjectTransactionSnapshot::Freeze(newOpaque, *original.value, SourceSha()), "OBJECT_TRANSACTION_APPLY_REJECTED");
}

void MemberByteBudgetRejectsBeforeParsing() {
    const auto edited = LniFromText(ObjectType::Unit, "");
    const std::vector<std::byte> tooLarge(ObjectTransactionSnapshot::kMaximumMemberBytes + 1, std::byte{0});
    const auto result = ObjectTransactionSnapshot::Freeze(edited, tooLarge, SourceSha());
    RequireRejected(result, "OBJECT_TRANSACTION_BYTE_BUDGET");
    Require(!HasCode(result.diagnostics, "native-version-truncated"), "oversized member is rejected before codec work");
}

} // namespace

int main() {
    try {
        FreezeFieldEditAndOwnInputs();
        RejectInvalidValuesWithoutPartialState();
        DistinguishMissingEmptyAndValidEmptyMember();
        AllSevenTypesAndOriginalGroupingRoundTrip();
        VersionOneEditsAreNarrowAndUnknownVersionsReject();
        OpaqueFieldsKeepActualNativeBytesOrReject();
        MemberByteBudgetRejectsBeforeParsing();
        std::cout << "ObjectTransactionSnapshotTests: 7 groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ObjectTransactionSnapshotTests: " << error.what() << '\n';
        return 1;
    }
}
