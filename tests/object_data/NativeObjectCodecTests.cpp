#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/SkinObjectCatalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::ObjectData;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void AppendInt32(std::vector<std::byte>& output, std::int32_t value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    output.push_back(static_cast<std::byte>(bits & 0xffu));
    output.push_back(static_cast<std::byte>((bits >> 8u) & 0xffu));
    output.push_back(static_cast<std::byte>((bits >> 16u) & 0xffu));
    output.push_back(static_cast<std::byte>((bits >> 24u) & 0xffu));
}

void AppendFloat(std::vector<std::byte>& output, float value) {
    AppendInt32(output, std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(value)));
}

void AppendId(std::vector<std::byte>& output, std::string_view value) {
    Check(value.size() == 4, "fixture id length");
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

void AppendZeroId(std::vector<std::byte>& output) {
    output.insert(output.end(), 4, std::byte{0});
}

[[nodiscard]] bool HasLevelData(ObjectType type) {
    return type == ObjectType::Ability
        || type == ObjectType::Upgrade
        || type == ObjectType::Doodad;
}

void AppendModificationHeader(
    std::vector<std::byte>& output,
    ObjectType objectType,
    std::string_view fieldId,
    NativeValueType valueType,
    std::int32_t levelOrVariation = 2,
    std::int32_t dataIndex = 3) {
    AppendId(output, fieldId);
    AppendInt32(output, static_cast<std::int32_t>(valueType));
    if (HasLevelData(objectType)) {
        AppendInt32(output, levelOrVariation);
        AppendInt32(output, dataIndex);
    }
}

void AppendEndMarker(std::vector<std::byte>& output, std::string_view marker) {
    if (marker.empty()) {
        AppendZeroId(output);
    } else {
        AppendId(output, marker);
    }
}

[[nodiscard]] std::vector<std::byte> BuildFixture(
    ObjectType type, float originalRealValue = 1.25f, float originalUnrealValue = 0.75f,
    std::int32_t version = 2, std::int32_t originalInteger = 42,
    std::string_view originalString = std::string_view{"A\xd6\xd0", 3}, std::int32_t customInteger = 7) {
    std::vector<std::byte> output;
    AppendInt32(output, version);
    const auto wireType = version == 1 && type == ObjectType::Doodad ? ObjectType::Unit : type;

    // 原始对象表：覆盖四种值类型，并混用两种合法结束标记。
    AppendInt32(output, 1);
    AppendId(output, "hfoo");
    AppendZeroId(output);
    AppendInt32(output, 4);

    AppendModificationHeader(output, wireType, "I001", NativeValueType::Int32);
    AppendInt32(output, originalInteger);
    AppendEndMarker(output, {});

    AppendModificationHeader(output, wireType, "R001", NativeValueType::Real);
    AppendFloat(output, originalRealValue);
    AppendEndMarker(output, "hfoo");

    AppendModificationHeader(output, wireType, "U001", NativeValueType::Unreal);
    AppendFloat(output, originalUnrealValue);
    AppendEndMarker(output, {});

    AppendModificationHeader(output, wireType, "S001", NativeValueType::String);
    for (const char byte : originalString) output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    output.push_back(std::byte{0});
    AppendEndMarker(output, "hfoo");

    // 自定义对象表：未知字段 ID 仍能依靠值类型安全分帧并原样保存。
    AppendInt32(output, 1);
    AppendId(output, "hfoo");
    AppendId(output, "H001");
    AppendInt32(output, 1);
    AppendModificationHeader(output, wireType, "ZZZZ", NativeValueType::Int32, 4, 7);
    AppendInt32(output, customInteger);
    AppendEndMarker(output, "H001");
    return output;
}

void AppendCloneSourceObject(std::vector<std::byte>& output, ObjectType type, std::string_view id) {
    AppendId(output, "hfoo");
    AppendId(output, id);
    AppendInt32(output, 4);
    AppendModificationHeader(output, type, "I001", NativeValueType::Int32, 4, 7);
    AppendInt32(output, 42);
    AppendEndMarker(output, id);
    AppendModificationHeader(output, type, "R001", NativeValueType::Real, 5, 8);
    AppendFloat(output, 1.25f);
    AppendEndMarker(output, {});
    AppendModificationHeader(output, type, "U001", NativeValueType::Unreal, 6, 9);
    AppendFloat(output, 0.75f);
    AppendEndMarker(output, "hfoo");
    AppendModificationHeader(output, type, "S001", NativeValueType::String, 7, 10);
    output.insert(output.end(), {std::byte{'A'}, std::byte{0xd6}, std::byte{0xd0}, std::byte{0}});
    AppendEndMarker(output, id);
}

[[nodiscard]] std::vector<std::byte> BuildCloneFixture(ObjectType type, bool withClone) {
    std::vector<std::byte> output;
    AppendInt32(output, 2);
    AppendInt32(output, 0);
    AppendInt32(output, withClone ? 2 : 1);
    AppendCloneSourceObject(output, type, "H001");
    if (withClone) AppendCloneSourceObject(output, type, "H002");
    return output;
}

[[nodiscard]] ModificationKey FixtureKey(
    ObjectType type,
    std::string field,
    std::uint32_t levelOrVariation = 2,
    std::uint32_t dataIndex = 3,
    std::int32_t version = 2) {
    ModificationKey result;
    result.fieldId = std::move(field);
    if (HasLevelData(type) && !(version == 1 && type == ObjectType::Doodad)) {
        result.dataIndex = dataIndex;
        if (type == ObjectType::Doodad) {
            result.variation = levelOrVariation;
        } else {
            result.level = levelOrVariation;
        }
    }
    return result;
}

[[nodiscard]] std::unique_ptr<NativeObjectDocument> ParseOwned(
    const NativeObjectCodec& codec,
    std::span<const std::byte> source,
    bool expectSuccess = true) {
    CodecParseResult parsed = codec.Parse(source);
    Check(parsed.Succeeded() == expectSuccess, "native parse success state");
    auto* raw = dynamic_cast<NativeObjectDocument*>(parsed.document.release());
    Check(raw != nullptr, "native parsed document type");
    return std::unique_ptr<NativeObjectDocument>(raw);
}

void TestSevenTypesRoundTrip() {
    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };
    constexpr std::array<std::string_view, 7> names{
        "war3map.w3u",
        "war3map.w3a",
        "war3map.w3t",
        "war3map.w3h",
        "war3map.w3q",
        "war3map.w3d",
        "war3map.w3b",
    };

    for (std::size_t index = 0; index < types.size(); ++index) {
        const ObjectType type = types[index];
        Check(NativeFileName(type) == names[index], "native filename mapping");
        Check(ObjectTypeFromNativeFileName(names[index]) == type, "native reverse filename mapping");
        Check(ObjectTypeFromNativeFileName(std::string(names[index])) == type, "native owned filename mapping");

        const std::vector<std::byte> fixture = BuildFixture(type);
        NativeObjectCodec codec(type);
        auto document = ParseOwned(codec, fixture);
        if (document == nullptr) {
            continue;
        }
        Check(document->Version() == 2, "native version");
        Check(document->Type() == type, "native object type");
        Check(document->IsStructurallySafe(), "native structure safe");
        Check(document->IsEditable(), "native fixture editable");
        Check(document->Supports(NativeObjectOperation::PreserveSource)
            && document->Supports(NativeObjectOperation::EditExistingValue)
            && document->Supports(NativeObjectOperation::EditStructure)
            && document->Supports(NativeObjectOperation::ReplaceFromTemplate), "v2 retains all native operations");
        Check(document->OriginalObjectCount() == 1, "native original count");
        Check(document->CustomObjectCount() == 1, "native custom count");
        Check(document->Objects().size() == 2, "native object total");
        Check(document->Objects()[0].modifications.size() == 4, "native original modifications");
        Check(document->Objects()[1].modifications.size() == 1, "native custom modifications");
        Check(document->Objects()[1].parentRawcode == "hfoo", "native custom parent");

        const auto serialized = codec.Serialize(*document);
        Check(serialized.Succeeded(), "native unchanged serialization");
        if (serialized.value.has_value()) {
            Check(*serialized.value == fixture, "native unchanged byte roundtrip");
        }

        const ObjectModification& stringField = document->Objects()[0].modifications[3];
        Check(stringField.key == FixtureKey(type, "S001"), "native level/data/variation mapping");
        const auto* stringValue = std::get_if<std::string>(&stringField.value);
        Check(stringValue != nullptr && stringValue->size() == 3, "native non-UTF8 string preserved");
    }
}

void TestSingleFieldEdit() {
    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };
    for (const ObjectType type : types) {
        const std::vector<std::byte> fixture = BuildFixture(type);
        NativeObjectCodec codec(type);
        auto before = ParseOwned(codec, fixture);
        auto after = ParseOwned(codec, fixture);
        if (before == nullptr || after == nullptr) {
            continue;
        }

        const Status changed = after->SetModification(
            ObjectKey{type, "H001"},
            FixtureKey(type, "ZZZZ", 4, 7),
            std::int32_t{9});
        Check(changed.Succeeded(), "native existing unknown field edit");
        const auto serialized = codec.Serialize(*after);
        Check(serialized.Succeeded(), "native edited serialization");
        if (!serialized.value.has_value()) {
            continue;
        }
        const std::size_t differentBytes = static_cast<std::size_t>(std::count_if(
            fixture.begin(),
            fixture.end(),
            [&, position = std::size_t{0}](std::byte value) mutable {
                const bool different = value != (*serialized.value)[position];
                ++position;
                return different;
            }));
        Check(serialized.value->size() == fixture.size(), "native fixed-size edit length");
        Check(differentBytes == 1, "native single Int32 edit changes one byte");

        const auto patch = codec.BuildPatch(
            *after,
            std::filesystem::path(NativeFileName(type)),
            "native 合成字段修改");
        Check(patch.Succeeded(), "native codec seals transaction patch");

        auto reparsed = ParseOwned(codec, *serialized.value);
        if (reparsed != nullptr) {
            const ObjectRecord* object = reparsed->FindObject(ObjectKey{type, "H001"});
            Check(object != nullptr, "native edited object reparsed");
            if (object != nullptr) {
                const auto field = std::find_if(
                    object->modifications.begin(),
                    object->modifications.end(),
                    [&](const ObjectModification& modification) {
                        return modification.key == FixtureKey(type, "ZZZZ", 4, 7);
                    });
                Check(
                    field != object->modifications.end()
                        && std::get_if<std::int32_t>(&field->value) != nullptr
                        && std::get<std::int32_t>(field->value) == 9,
                    "native edited value reparsed");
            }
        }
        Check(codec.Diff(*before, *after).changes.size() == 1, "native semantic diff");

        Check(
            !after->SetModification(
                ObjectKey{type, "H001"},
                FixtureKey(type, "MISSING", 4, 7),
                std::int32_t{1}).Succeeded(),
            "native baseline refuses adding field");
        Check(
            !after->SetModification(
                ObjectKey{type, "H001"},
                FixtureKey(type, "ZZZZ", 4, 7),
                std::string{"wrong type"}).Succeeded(),
            "native baseline enforces stored value type");
    }
}

void TestStringEditPreservesNeighbors() {
    const ObjectType type = ObjectType::Ability;
    const std::vector<std::byte> fixture = BuildFixture(type);
    NativeObjectCodec codec(type);
    auto document = ParseOwned(codec, fixture);
    Check(document != nullptr, "native string edit document");
    if (document == nullptr) {
        return;
    }
    Check(
        document->SetModification(
            ObjectKey{type, "hfoo"},
            FixtureKey(type, "S001"),
            LocalizedStringValue{"已修改"}).Succeeded(),
        "native string wrapper edit");
    const auto serialized = codec.Serialize(*document);
    Check(serialized.Succeeded(), "native variable-size serialization");
    if (!serialized.value.has_value()) {
        return;
    }
    auto reparsed = ParseOwned(codec, *serialized.value);
    if (reparsed == nullptr) {
        return;
    }
    const ObjectRecord* original = reparsed->FindObject(ObjectKey{type, "hfoo"});
    const ObjectRecord* custom = reparsed->FindObject(ObjectKey{type, "H001"});
    Check(original != nullptr && custom != nullptr, "native string edit objects remain");
    if (original != nullptr) {
        const auto* value = std::get_if<std::string>(&original->modifications[3].value);
        Check(value != nullptr && *value == "已修改", "native UTF-8 replacement reparsed");
    }
    if (custom != nullptr) {
        Check(
            std::get<std::int32_t>(custom->modifications.front().value) == 7,
            "native unknown neighboring field preserved");
    }
}

void TestFailClosedInputs() {
    NativeObjectCodec codec(ObjectType::Unit);

    std::vector<std::byte> unknownVersion = BuildFixture(ObjectType::Unit);
    unknownVersion[0] = std::byte{3};
    auto versionDocument = ParseOwned(codec, unknownVersion, false);
    Check(versionDocument != nullptr && !versionDocument->IsEditable(), "native unknown version read-only");
    if (versionDocument != nullptr) {
        Check(!codec.Serialize(*versionDocument).Succeeded(), "native unknown version cannot serialize");
    }

    std::vector<std::byte> truncated = BuildFixture(ObjectType::Unit);
    truncated.pop_back();
    auto truncatedDocument = ParseOwned(codec, truncated, false);
    Check(truncatedDocument != nullptr && !truncatedDocument->IsStructurallySafe(), "native truncation unsafe");

    std::vector<std::byte> trailing = BuildFixture(ObjectType::Unit);
    trailing.push_back(std::byte{0x55});
    auto trailingDocument = ParseOwned(codec, trailing, false);
    Check(trailingDocument != nullptr && !trailingDocument->IsStructurallySafe(), "native trailing bytes unsafe");

    std::vector<std::byte> unknownType = BuildFixture(ObjectType::Unit);
    // 版本、原始对象数、两个 ID、字段数和字段 ID 之后是第一个值类型。
    constexpr std::size_t valueTypeOffset = 4 + 4 + 8 + 4 + 4;
    unknownType[valueTypeOffset] = std::byte{4};
    auto typeDocument = ParseOwned(codec, unknownType, false);
    Check(typeDocument != nullptr && !typeDocument->IsStructurallySafe(), "native unknown value type unsafe");

    std::vector<std::byte> impossibleCount;
    AppendInt32(impossibleCount, 2);
    AppendInt32(impossibleCount, 1'000'000);
    auto countDocument = ParseOwned(codec, impossibleCount, false);
    Check(
        countDocument != nullptr && !countDocument->IsStructurallySafe(),
        "native impossible count rejected before allocation");
}

void TestAmbiguousIdentityIsReadOnly() {
    std::vector<std::byte> fixture;
    AppendInt32(fixture, 2);
    AppendInt32(fixture, 1);
    AppendId(fixture, "hfoo");
    AppendZeroId(fixture);
    AppendInt32(fixture, 2);
    for (int index = 0; index < 2; ++index) {
        AppendModificationHeader(fixture, ObjectType::Unit, "DUPL", NativeValueType::Int32);
        AppendInt32(fixture, index);
        AppendEndMarker(fixture, {});
    }
    AppendInt32(fixture, 0);

    NativeObjectCodec codec(ObjectType::Unit);
    auto document = ParseOwned(codec, fixture);
    Check(document != nullptr && !document->IsEditable(), "native duplicate key read-only");
    if (document == nullptr) {
        return;
    }
    const auto serialized = codec.Serialize(*document);
    Check(serialized.Succeeded() && serialized.value == fixture, "native read-only ambiguity byte passthrough");
    Check(!codec.Validate(*document).Succeeded(), "native ambiguity fails edit validation");
    Check(document->Supports(NativeObjectOperation::PreserveSource)
        && !document->Supports(NativeObjectOperation::EditExistingValue)
        && !document->Supports(NativeObjectOperation::EditStructure)
        && !document->Supports(NativeObjectOperation::ReplaceFromTemplate), "v2 ambiguity only allows original byte preservation");
}

void TestClassicV1ExistingValueEdits() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        const auto source = BuildFixture(type, 1.25f, 0.75f, 1);
        NativeObjectCodec codec(type);
        auto document = ParseOwned(codec, source);
        if (!document) continue;
        const ObjectKey original{type, "hfoo"}, custom{type, "H001"};
        const auto intKey = FixtureKey(type, "I001", 2, 3, 1);
        const auto realKey = FixtureKey(type, "R001", 2, 3, 1);
        const auto unrealKey = FixtureKey(type, "U001", 2, 3, 1);
        const auto textKey = FixtureKey(type, "S001", 2, 3, 1);
        const auto customKey = FixtureKey(type, "ZZZZ", 4, 7, 1);
        Check(document->Version() == 1 && document->IsStructurallySafe() && !document->IsEditable()
            && document->Supports(NativeObjectOperation::PreserveSource)
            && document->Supports(NativeObjectOperation::EditExistingValue)
            && !document->Supports(NativeObjectOperation::EditStructure)
            && !document->Supports(NativeObjectOperation::ReplaceFromTemplate),
            "six observed v1 categories support existing values without full structure permission");
        const auto validated = codec.Validate(*document);
        const auto warningCode = "native-v1-" + std::string(ToString(type)) + "-read-only";
        Check(validated.Succeeded() && std::any_of(validated.diagnostics.begin(), validated.diagnostics.end(),
            [&](const Diagnostic& diagnostic) { return diagnostic.code == warningCode
                && diagnostic.severity == DiagnosticSeverity::Warning; }),
            "v1 source limitation diagnostic is preserved without forbidding existing values");
        Check(codec.Serialize(*document).value == std::optional{source}, "v1 no-op retains every original byte");
        Check(document->ModificationValueType(original, unrealKey) == NativeValueType::Unreal,
            "v1 known field lookup preserves actual Unreal storage type");
        Check(!NativeObjectDocument::CreateEmpty(type, 1).Succeeded(), "v1 creation remains unsupported");
        Check(!document->AddOriginalObject("hrif").Succeeded(), "v1 original object addition denied");
        Check(!document->AddCustomObject("H002", "hfoo").Succeeded(), "v1 custom object addition denied");
        Check(!document->CloneCustomObject(custom, "H002").Succeeded(), "v1 clone denied");
        Check(!document->AddModification(original, FixtureKey(type, "I002", 2, 3, 1),
            NativeValueType::Int32, std::int32_t{1}).Succeeded(), "v1 new field denied");
        Check(!document->RemoveModification(original, intKey).Succeeded(), "v1 field deletion denied");
        Check(!document->RemoveOriginalObjectOverrides(original).Succeeded(), "v1 original override deletion denied");
        Check(!document->DeleteCustomObject(custom).Succeeded(), "v1 custom deletion denied");
        Check(!document->SetModification(original, intKey, std::string{"wrong type"}).Succeeded(),
            "v1 cannot change stored value type");
        Check(!document->SetModification(original, textKey, std::string{"a\0b", 3}).Succeeded(),
            "v1 string edit cannot introduce embedded NUL");
        Check(!document->SetModification(original, FixtureKey(type, "MISS", 2, 3, 1), std::int32_t{1}).Succeeded(),
            "v1 value edit cannot create a missing field");
        Check(codec.Serialize(*document).value == std::optional{source}, "all denied v1 direct operations are non-mutating");

        Check(document->SetModification(custom, customKey, std::int32_t{9}).Succeeded(), "v1 unknown existing field can be edited");
        Check(codec.Serialize(*document).value == std::optional{BuildFixture(type, 1.25f, 0.75f, 1, 42,
            std::string_view{"A\xd6\xd0", 3}, 9)}, "v1 numeric edit preserves non-UTF8 string, wire headers and end markers");
        const std::string changedText = "更长的原生字段\r\nsecond line";
        Check(document->SetModification(original, textKey, LocalizedStringValue{changedText}).Succeeded(),
            "v1 existing string accepts variable-length typed value");
        Check(document->SetModification(original, intKey, std::int32_t{-700}).Succeeded()
            && document->SetModification(original, realKey, std::int32_t{13}).Succeeded()
            && document->SetModification(original, unrealKey, -0.0f).Succeeded(), "v1 four wire value types edit together");
        const auto first = codec.Serialize(*document);
        Check(first.value == std::optional{BuildFixture(type, 13.0f, -0.0f, 1, -700, changedText, 9)},
            "v1 multiple edits preserve exact complete wire bytes including negative zero and later object");
        if (first.value) {
            auto reopened = ParseOwned(codec, *first.value);
            const auto* reopenedCustom = reopened ? reopened->FindObject(custom) : nullptr;
            Check(reopened && codec.Validate(*reopened).Succeeded() && reopened->OriginalObjectCount() == 1
                && reopened->CustomObjectCount() == 1 && reopenedCustom && reopenedCustom->parentRawcode == "hfoo",
                "v1 edited file reopens with original table identities and parent");
        }
        Check(document->SetModification(original, textKey, std::string{}).Succeeded()
            && document->SetModification(custom, customKey, std::int32_t{12345}).Succeeded(),
            "v1 repeated edit can shrink earlier string and change later object");
        Check(codec.Serialize(*document).value == std::optional{BuildFixture(type, 13.0f, -0.0f, 1, -700, {}, 12345)},
            "v1 later edits still use initial source offsets after variable-length serialization");
        Check(document->SetModification(original, intKey, std::int32_t{42}).Succeeded()
            && document->SetModification(original, realKey, 1.25f).Succeeded()
            && document->SetModification(original, unrealKey, 0.75f).Succeeded()
            && document->SetModification(original, textKey, std::string{"A\xd6\xd0", 3}).Succeeded()
            && document->SetModification(custom, customKey, std::int32_t{7}).Succeeded(), "v1 existing value edits can be undone");
        Check(codec.Serialize(*document).value == std::optional{source}, "v1 undo restores original bytes exactly");
        auto truncated = source;
        truncated.pop_back();
        auto invalid = ParseOwned(codec, truncated, false);
        Check(invalid && !invalid->Supports(NativeObjectOperation::PreserveSource)
            && !invalid->Supports(NativeObjectOperation::EditExistingValue) && !codec.Serialize(*invalid).Succeeded(),
            "v1 truncated boundary never grants edit or preservation permission");
    }
    auto buff = ParseOwned(NativeObjectCodec(ObjectType::Buff), BuildFixture(ObjectType::Buff, 1.25f, 0.75f, 1), false);
    Check(buff && !buff->Supports(NativeObjectOperation::PreserveSource)
        && !buff->Supports(NativeObjectOperation::EditExistingValue), "unobserved Buff v1 remains unsupported");
}

void TestClassicV1DuplicateIdentityDenied() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        const auto wireType = type == ObjectType::Doodad ? ObjectType::Unit : type;
        for (const bool duplicateObject : {false, true}) {
            std::vector<std::byte> source;
            AppendInt32(source, 1);
            AppendInt32(source, duplicateObject ? 2 : 1);
            for (int object = 0; object < (duplicateObject ? 2 : 1); ++object) {
                AppendId(source, "hfoo"); AppendZeroId(source);
                AppendInt32(source, duplicateObject ? 1 : 2);
                for (int field = 0; field < (duplicateObject ? 1 : 2); ++field) {
                    AppendModificationHeader(source, wireType, "DUPL", NativeValueType::Int32);
                    AppendInt32(source, field); AppendEndMarker(source, {});
                }
            }
            AppendInt32(source, 0);
            NativeObjectCodec codec(type);
            auto document = ParseOwned(codec, source, false);
            Check(document && !document->IsStructurallySafe()
                && !document->Supports(NativeObjectOperation::PreserveSource)
                && !document->Supports(NativeObjectOperation::EditExistingValue)
                && !codec.Serialize(*document).Succeeded(), "v1 duplicate object or field identity still rejects all writes");
        }
    }
}

void TestVersion3ReadOnlyAcrossSevenTypes() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        std::vector<std::byte> bytes;
        AppendInt32(bytes, 3);
        AppendInt32(bytes, 1);
        AppendId(bytes, "hfoo");
        AppendZeroId(bytes);
        AppendInt32(bytes, 2);
        AppendInt32(bytes, 17);
        AppendInt32(bytes, -29);
        AppendInt32(bytes, 1);
        AppendModificationHeader(bytes, type, "I001", NativeValueType::Int32);
        AppendInt32(bytes, 42);
        AppendEndMarker(bytes, {});
        AppendInt32(bytes, 1);
        AppendId(bytes, "hfoo");
        AppendId(bytes, "H001");
        AppendInt32(bytes, 1);
        AppendInt32(bytes, -123456789);
        AppendInt32(bytes, 0);

        NativeObjectCodec codec(type);
        auto document = ParseOwned(codec, bytes);
        const auto original = document ? document->Version3UnknownValues({type, "hfoo"}) : std::nullopt;
        const auto custom = document ? document->Version3UnknownValues({type, "H001"}) : std::nullopt;
        Check(document && document->Version() == 3 && document->IsStructurallySafe()
            && !document->IsEditable() && document->Objects().size() == 2
            && document->FindObject({type, "hfoo"}) != nullptr
            && document->FindObject({type, "H001"}) != nullptr,
            "v3 object tables and modifications are available for bounded read-only queries");
        Check(original && original->size() == 2 && (*original)[0] == 17 && (*original)[1] == -29
            && custom && custom->size() == 1 && (*custom)[0] == -123456789,
            "v3 per-object unknown integers keep exact order and signed values");
        Check(document && !codec.Serialize(*document).Succeeded()
            && !document->SetModification({type, "hfoo"}, FixtureKey(type, "I001"), std::int32_t{43}).Succeeded(),
            "v3 read-only inspection never permits native writeback");
        Check(document && !document->Supports(NativeObjectOperation::PreserveSource)
            && !document->Supports(NativeObjectOperation::EditExistingValue)
            && !document->Supports(NativeObjectOperation::EditStructure)
            && !document->Supports(NativeObjectOperation::ReplaceFromTemplate), "v3 capabilities do not expand with v1 support");

        auto invalid = bytes;
        invalid[16] = std::byte{0xff};
        invalid[17] = std::byte{0xff};
        invalid[18] = std::byte{0xff};
        invalid[19] = std::byte{0xff};
        Check(!codec.Parse(invalid).Succeeded(), "v3 negative unknown count fails closed");
    }
}

void TestIndependentSkinMembersStayReadOnly() {
    constexpr std::array types{ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
        ObjectType::Buff, ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
    const std::string sourceSha(64, 'a');
    std::vector<MapArchiveMember> members;
    for (const auto type : types) members.push_back({SkinObjectFileName(type), BuildFixture(type)});
    members.push_back({std::string(NativeFileName(ObjectType::Unit)), BuildFixture(ObjectType::Unit)});
    auto capture = SkinObjectCatalog::Capture(members, sourceSha);
    Check(capture.Succeeded() && capture.value && capture.value->Members().size() == 7,
        "seven independent Skin members are catalogued without merging ordinary object data");
    if (!capture.value) return;
    for (std::size_t index = 0; index < types.size(); ++index) {
        const auto& summary = capture.value->Members()[index];
        auto first = capture.value->Query(types[index], sourceSha, 0, 1);
        auto second = capture.value->Query(types[index], sourceSha, 1, 1);
        Check(summary.type == types[index] && summary.state == SkinMemberState::Ready
            && summary.version == 2 && summary.originalCount == 1 && summary.customCount == 1
            && summary.memberSha256.size() == 64 && first.Succeeded() && second.Succeeded()
            && first.value->nextOffset == 1 && !second.value->nextOffset
            && first.value->rows[0].table == NativeObjectTable::Original
            && second.value->rows[0].table == NativeObjectTable::Custom
            && second.value->rows[0].record.key.rawcode == "H001"
            && second.value->rows[0].record.modifications.size() == 1
            && !first.value->rows[0].writable && !second.value->rows[0].writable,
            "Skin pages expose bounded original/custom source fields but no write capability");
    }
    Check(!capture.value->Query(ObjectType::Unit, std::string(64, 'b'), 0, 1).Succeeded()
        && !capture.value->Query(ObjectType::Unit, sourceSha, 0, 129).Succeeded(),
        "Skin page rejects stale source binding and unbounded limit");
    members.push_back({"WAR3MAPSKIN.W3U", BuildFixture(ObjectType::Unit)});
    auto duplicate = SkinObjectCatalog::Capture(members, sourceSha);
    Check(duplicate.value && duplicate.value->Members()[0].state == SkinMemberState::Ambiguous
        && !duplicate.value->Query(ObjectType::Unit, sourceSha, 0, 1).Succeeded(),
        "case-insensitive duplicate Skin members are not silently chosen");
    std::vector<MapArchiveMember> empty;
    auto opaque = SkinObjectCatalog::Capture(empty, sourceSha, true);
    Check(opaque.value && opaque.value->Members()[0].state == SkinMemberState::OpaqueUnknown,
        "unnamed archive blocks prevent claiming a missing Skin layer");
}

[[nodiscard]] std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0) {
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), length);
    }
    if (!input && !bytes.empty()) {
        return {};
    }
    return bytes;
}

void TestOptionalGoldenRoot(const std::filesystem::path& root) {
    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };
    for (const ObjectType type : types) {
        const std::filesystem::path path = root / NativeFileName(type);
        const std::vector<std::byte> bytes = ReadFile(path);
        Check(!bytes.empty(), "native private golden file readable");
        if (bytes.empty()) {
            continue;
        }
        NativeObjectCodec codec(type);
        auto document = ParseOwned(codec, bytes);
        if (document == nullptr) {
            continue;
        }
        const auto serialized = codec.Serialize(*document);
        Check(serialized.Succeeded(), "native private golden serialization");
        if (serialized.value.has_value()) {
            Check(*serialized.value == bytes, "native private golden byte roundtrip");
        }
        std::cout << NativeFileName(type) << ": " << document->Objects().size()
                  << " objects\n";
    }
}

void TestNativeToLniConversion() {
    // 用 Unit 类型的合成 native 数据测试 native→LNI 转换。
    const std::vector<std::byte> nativeBytes = BuildFixture(ObjectType::Unit, 325.0f, -0.0f);
    auto converted = ConvertNativeObjectDataToLni(ObjectType::Unit, nativeBytes);
    Check(converted.Succeeded() && converted.value.has_value(),
          "native-to-LNI conversion must succeed");
    if (!converted.Succeeded() || !converted.value.has_value()) {
        return;
    }
    Check(!converted.value->empty(), "converted LNI must not be empty");

    // 用 LniObjectCodec 重新解析产出的 LNI，验证对象数一致。
    LniObjectCodec lniCodec(ObjectType::Unit);
    CodecParseResult reparsed = lniCodec.Parse(*converted.value);
    Check(reparsed.Succeeded(), "converted LNI must re-parse successfully");
    if (!reparsed.Succeeded() || reparsed.document == nullptr) {
        return;
    }
    auto* lniDoc = dynamic_cast<LniObjectDocument*>(reparsed.document.get());
    Check(lniDoc != nullptr, "re-parsed document must be LniObjectDocument");
    if (lniDoc == nullptr) {
        return;
    }
    // BuildFixture 产出 1 个原始对象 + 1 个自定义对象 = 2 个对象。
    Check(lniDoc->Objects().size() == 2,
          "converted LNI must contain 2 objects (1 original + 1 custom)");
    const auto* negativeZero = std::get_if<float>(&lniDoc->Objects().front().modifications[2].value);
    Check(lniDoc->Objects().front().modifications[1].value == Value{325.0f} && negativeZero && std::bit_cast<std::uint32_t>(*negativeZero) == 0x80000000u, "integer float and negative zero retain native-to-LNI identity");
}

void TestDoodadVariationLniRoundTrip() {
    // Doodad 字段使用 variation（无 level）：旧渲染器会丢弃 dataIndex/variation，
    // 把多变体字段塌缩成重复键（dvb1/dvg1 洪水）；新渲染必须带全部分量。
    const std::vector<std::byte> nativeBytes = BuildFixture(ObjectType::Doodad);
    auto converted = ConvertNativeObjectDataToLni(ObjectType::Doodad, nativeBytes);
    Check(converted.Succeeded() && converted.value.has_value(),
          "doodad native-to-LNI conversion must succeed");
    if (!converted.Succeeded() || !converted.value.has_value()) {
        return;
    }
    const std::string text(
        reinterpret_cast<const char*>(converted.value->data()),
        converted.value->size());
    Check(text.find("[0,3,2]") != std::string::npos,
          "doodad variation field key must render as field[0,dataIndex,variation]");

    LniObjectCodec lniCodec(ObjectType::Doodad);
    CodecParseResult reparsed = lniCodec.Parse(*converted.value);
    Check(reparsed.Succeeded(),
          "doodad converted LNI must re-parse without duplicate-field errors");
    const auto* doodad = dynamic_cast<const LniObjectDocument*>(reparsed.document.get());
    Check(doodad && doodad->Objects().front().modifications.front().key == ModificationKey{"I001", std::nullopt, 3, 2}, "doodad zero level placeholder must restore native key identity");
}

void TestLniWriteBackToNative() {
    // N5b：LNI→native 写回。无编辑往返必须字节一致；编辑字段后写回生效。
    const std::vector<std::byte> nativeBytes = BuildFixture(ObjectType::Unit);
    NativeObjectCodec codec(ObjectType::Unit);
    auto converted = ConvertNativeObjectDataToLni(ObjectType::Unit, nativeBytes);
    Check(converted.Succeeded() && converted.value.has_value(),
          "write-back fixture conversion failed");
    LniObjectCodec lniCodec(ObjectType::Unit);
    auto lniParsed = lniCodec.Parse(*converted.value);
    Check(lniParsed.Succeeded(), "write-back fixture LNI did not parse");
    auto* lniDoc = dynamic_cast<LniObjectDocument*>(lniParsed.document.get());
    Check(lniDoc != nullptr, "write-back fixture LNI document type");
    if (lniDoc == nullptr) {
        return;
    }

    // 1) 无编辑写回：序列化必须与原字节一致。
    auto pristine = ParseOwned(codec, nativeBytes);
    Check(ApplyLniChangesToNative(*pristine, *lniDoc).Succeeded(),
          "no-op write-back must succeed");
    auto pristineOut = codec.Serialize(*pristine);
    Check(pristineOut.Succeeded(), "no-op write-back did not serialize");
    Check(pristineOut.Succeeded() && *pristineOut.value == nativeBytes,
          "no-op write-back changed native bytes");

    // 2) 编辑字段后写回：值必须生效，其余字节不变。
    const ObjectKey target{ObjectType::Unit, "hfoo"};
    const ModificationKey key = FixtureKey(ObjectType::Unit, "I001");
    Check(lniDoc->SetModification(target, key, Value{std::int32_t{99}}).Succeeded(),
          "LNI field edit failed");
    auto edited = ParseOwned(codec, nativeBytes);
    Check(ApplyLniChangesToNative(*edited, *lniDoc).Succeeded(),
          "edited write-back must succeed");
    auto editedOut = codec.Serialize(*edited);
    Check(editedOut.Succeeded() && !(*editedOut.value == nativeBytes),
          "edited write-back did not change native bytes");
    auto reparsed = ParseOwned(codec, *editedOut.value);
    const ObjectRecord* record = reparsed->FindObject(target);
    Check(record != nullptr, "edited object missing after write-back");
    if (record != nullptr) {
        const ObjectModification* mod = nullptr;
        for (const auto& candidate : record->modifications) {
            if (candidate.key == key) {
                mod = &candidate;
                break;
            }
        }
        Check(mod != nullptr, "edited field missing after write-back");
        if (mod != nullptr) {
            Check(std::holds_alternative<std::int32_t>(mod->value) &&
                std::get<std::int32_t>(mod->value) == 99,
                "edited value did not round-trip through write-back");
        }
    }
}

void TestOptionalWriteBack(const std::filesystem::path& root) {
    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };
    for (const ObjectType type : types) {
        const std::filesystem::path path = root / NativeFileName(type);
        const std::vector<std::byte> bytes = ReadFile(path);
        if (bytes.empty()) {
            continue;
        }
        NativeObjectCodec codec(type);
        auto converted = ConvertNativeObjectDataToLni(type, bytes);
        Check(converted.Succeeded() && converted.value.has_value(),
              "writeback golden native-to-LNI conversion");
        if (!converted.Succeeded() || !converted.value.has_value()) {
            continue;
        }
        LniObjectCodec lniCodec(type);
        auto lniParsed = lniCodec.Parse(*converted.value);
        Check(lniParsed.Succeeded(), "writeback golden LNI parse");
        if (!lniParsed.Succeeded()) {
            for (const auto& diagnostic : lniParsed.diagnostics) {
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
            continue;
        }
        auto* lniDoc = dynamic_cast<LniObjectDocument*>(lniParsed.document.get());
        Check(lniDoc != nullptr, "writeback golden LNI document type");
        if (lniDoc == nullptr) {
            continue;
        }
        auto nativeDoc = ParseOwned(codec, bytes);
        if (nativeDoc == nullptr) {
            continue;
        }
        const War3Map::Status applied =
            ApplyLniChangesToNative(*nativeDoc, *lniDoc);
        Check(applied.Succeeded(), "writeback golden apply");
        if (!applied.Succeeded()) {
            for (const auto& diagnostic : applied.diagnostics) {
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
            continue;
        }
        const auto serialized = codec.Serialize(*nativeDoc);
        Check(serialized.Succeeded(), "writeback golden serialize");
        if (serialized.value.has_value()) {
            Check(*serialized.value == bytes,
                  "writeback golden no-op byte fidelity");
        }
        std::cout << NativeFileName(type) << ": write-back ok ("
                  << nativeDoc->Objects().size() << " objects)\n";
    }
}

void TestResetFieldWriteBack() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Doodad}) {
        NativeObjectCodec codec(type);
        const auto originalBytes = BuildFixture(type);
        auto document = ParseOwned(codec, originalBytes);
        if (!document) continue;
        const ObjectKey object{type, "hfoo"};
        const auto removedKey = FixtureKey(type, "I001");
        Check(document->RemoveModification(object, removedKey).Succeeded(), "reset exact native field");
        Check(document->SetModification(object, FixtureKey(type, "R001"), 2.5f).Succeeded(),
            "neighbor index remains editable after resetting first field");
        const auto bytes = codec.Serialize(*document);
        Check(bytes.Succeeded(), "reset field serializes");
        if (!bytes.value) continue;
        const auto reopened = ParseOwned(codec, *bytes.value);
        if (!reopened) continue;
        const auto* record = reopened->FindObject(object);
        Check(record && record->modifications.size() == 3, "reset removes exactly one field");
        if (!record) continue;
        Check(std::none_of(record->modifications.begin(), record->modifications.end(),
            [&](const auto& field) { return field.key == removedKey; }), "reset field stays absent on reopen");
        const auto second = std::find_if(record->modifications.begin(), record->modifications.end(),
            [&](const auto& field) { return field.key == FixtureKey(type, "R001"); });
        Check(second != record->modifications.end() && second->value == Value{2.5f}, "neighbor change persists");
        const auto beforeFailure = *codec.Serialize(*document).value;
        Check(!document->RemoveModification(object, removedKey).Succeeded(), "missing field reset rejects");
        Check(*codec.Serialize(*document).value == beforeFailure, "failed reset preserves bytes");

        auto converted = ConvertNativeObjectDataToLni(type, originalBytes);
        if (!converted.value) continue;
        auto lniParsed = LniObjectCodec(type).Parse(*converted.value);
        auto* lni = dynamic_cast<LniObjectDocument*>(lniParsed.document.get());
        if (!lni) continue;
        // LNI 的无 level 占位规则保留，使用该文档实际键执行重置。
        const auto* lniRecord = lni->FindObject(object);
        if (!lniRecord || lniRecord->modifications.empty()) continue;
        const auto lniKey = lniRecord->modifications.front().key;
        Check(lni->RemoveModification(object, lniKey).Succeeded(), "reset LNI field");
        auto baseline = ParseOwned(codec, originalBytes);
        Check(ApplyLniChangesToNative(*baseline, *lni).Succeeded(), "LNI reset applies to native baseline");
        const auto saved = codec.Serialize(*baseline);
        if (!saved.value) continue;
        const auto finalDocument = ParseOwned(codec, *saved.value);
        Check(finalDocument && finalDocument->FindObject(object)->modifications.size() == 3,
            "LNI reset survives native roundtrip");
    }
}

void TestSchemaPreservesNewUnrealType() {
    const std::string table = "ID;PWXL;N;E\nB;X4;Y2\n"
        "C;X1;Y1;K\"ID\"\nC;X2;K\"field\"\nC;X3;K\"type\"\nC;X4;K\"slk\"\n"
        "C;X1;Y2;K\"U002\"\nC;X2;K\"blend\"\nC;X3;K\"unreal\"\nC;X4;K\"UnitUI\"\nE\n";
    ObjectFieldSchemaOptions options;
    options.sources = {{ObjectType::Unit, "test.slk"}};
    options.stringFiles.clear();
    auto schema = ObjectFieldSchema::Load([&](const std::string& path) {
        Result<std::vector<std::byte>> result;
        if (path == "test.slk") result.value = std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(table.data()), reinterpret_cast<const std::byte*>(table.data() + table.size()));
        return result;
    }, options);
    Check(schema.Succeeded() && schema.value, "synthetic unreal schema loads");
    if (!schema.value) return;
    const auto bytes = BuildFixture(ObjectType::Unit);
    auto converted = ConvertNativeObjectDataToLni(ObjectType::Unit, bytes);
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(*converted.value);
    auto* lni = dynamic_cast<LniObjectDocument*>(parsed.document.get());
    if (!lni) return;
    Check(lni->SetModification({ObjectType::Unit, "hfoo"}, {"U002"}, 0.5f).Succeeded(), "add synthetic unreal field");
    NativeObjectCodec codec(ObjectType::Unit);
    auto native = ParseOwned(codec, bytes);
    Check(ApplyLniChangesToNative(*native, *lni, &*schema.value).Succeeded(), "schema-aware writeback succeeds");
    const auto output = codec.Serialize(*native);
    std::vector<std::byte> needle;
    AppendId(needle, "U002");
    AppendInt32(needle, static_cast<int>(NativeValueType::Unreal));
    Check(output.value && std::search(output.value->begin(), output.value->end(), needle.begin(), needle.end()) != output.value->end(),
        "new unreal preserves wire tag 2 instead of inferred real tag 1");
}

void TestLniCloneKeepsActualWireTypes() {
    const std::string table = "ID;PWXL;N;E\nB;X4;Y4\n"
        "C;X1;Y1;K\"ID\"\nC;X2;K\"field\"\nC;X3;K\"type\"\nC;X4;K\"slk\"\n"
        "C;X1;Y2;K\"udef\"\nC;X2;K\"def\"\nC;X3;K\"int\"\nC;X4;K\"UnitBalance\"\n"
        "C;X1;Y3;K\"udup\"\nC;X2;K\"defup\"\nC;X3;K\"int\"\nC;X4;K\"UnitBalance\"\n"
        "C;X1;Y4;K\"unam\"\nC;X2;K\"Name\"\nC;X3;K\"string\"\nC;X4;K\"Profile\"\nE\n";
    ObjectFieldSchemaOptions options; options.sources = {{ObjectType::Unit, "test.slk"}}; options.stringFiles.clear();
    auto schema = ObjectFieldSchema::Load([&](const std::string& path) {
        Result<std::vector<std::byte>> result;
        if (path == "test.slk") result.value = std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(table.data()), reinterpret_cast<const std::byte*>(table.data() + table.size()));
        return result;
    }, options);
    Check(schema.Succeeded() && schema.value, "clone regression metadata loads");
    if (!schema.value) return;
    auto baseline = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    if (!baseline.value) return;
    Check(baseline.value->AddCustomObject("E000", "Eevi").Succeeded(), "actual clone source exists");
    for (const auto* id : {"udef", "udup"})
        Check(baseline.value->AddModification({ObjectType::Unit, "E000"}, {id}, NativeValueType::Unreal, 5.0f).Succeeded(),
            "source uses actual Unreal despite int metadata");
    Check(baseline.value->AddModification({ObjectType::Unit, "E000"}, {"ucam"}, NativeValueType::Int32, std::int32_t{0}).Succeeded(), "source bool wire type is int");
    Check(baseline.value->AddModification({ObjectType::Unit, "E000"}, {"usca"}, NativeValueType::Real, 0.8f).Succeeded(), "source Real wire type");
    NativeObjectCodec nativeCodec(ObjectType::Unit);
    const auto original = nativeCodec.Serialize(*baseline.value);
    if (!original.value) return;
    const auto text = ConvertNativeObjectDataToLni(ObjectType::Unit, *original.value);
    if (!text.value) return;
    const LniObjectCodec lniCodec(ObjectType::Unit);
    auto parsed = lniCodec.Parse(*text.value);
    auto* edited = dynamic_cast<LniObjectDocument*>(parsed.document.get());
    if (!edited) return;
    Check(edited->SetModification({ObjectType::Unit, "E000"}, {"udef"}, 7.0f).Succeeded(), "edit source before clone");
    Check(edited->CloneCustomObject({ObjectType::Unit, "E000"}, "E001").Succeeded(), "LNI records exact clone source");
    Check(edited->SetModification({ObjectType::Unit, "E001"}, {"unam"}, std::string("Clone")).Succeeded(), "clone permits a new name");
    Check(edited->CloneCustomObject({ObjectType::Unit, "E001"}, "E002").Succeeded(), "clone of new clone records dependency");
    Check(edited->SetModification({ObjectType::Unit, "E000"}, {"udef"}, 9.0f).Succeeded(), "later source edit does not change clone snapshot");
    const auto finalLni = lniCodec.Serialize(*edited);
    if (!finalLni.value) return;
    auto reopened = lniCodec.Parse(*finalLni.value);
    auto* restored = dynamic_cast<LniObjectDocument*>(reopened.document.get());
    if (!restored) return;
    Check(restored->CloneSources().empty() && restored->DocumentFingerprint() != edited->DocumentFingerprint(),
        "plain text cannot invent clone provenance or share its fingerprint");
    std::vector<LniObjectDocument::CloneSource> sources(edited->CloneSources().begin(), edited->CloneSources().end());
    std::reverse(sources.begin(), sources.end());
    Check(restored->RestoreCloneSources(sources, *baseline.value).Succeeded(), "restore sorts clone dependency, independent of manifest row order");
    Check(restored->DocumentFingerprint() == edited->DocumentFingerprint(), "canonical provenance fingerprint survives restore");
    auto classicBytes = *original.value;
    classicBytes[0] = std::byte{1};
    const auto classic = ParseOwned(nativeCodec, classicBytes);
    Check(classic && !restored->RestoreCloneSources(sources, *classic).Succeeded(), "clone provenance cannot widen classic v1 structure editing");
    auto candidate = *baseline.value;
    const auto applied = ApplyLniChangesToNative(candidate, *restored, &*schema.value);
    Check(applied.Succeeded(), "clone writeback does not coerce Unreal to metadata int");
    for (const auto* id : {"E000", "E001", "E002"}) {
        const ObjectKey key{ObjectType::Unit, id};
        Check(candidate.ModificationValueType(key, {"udef"}) == NativeValueType::Unreal
            && candidate.ModificationValueType(key, {"udup"}) == NativeValueType::Unreal
            && candidate.ModificationValueType(key, {"usca"}) == NativeValueType::Real
            && candidate.ModificationValueType(key, {"ucam"}) == NativeValueType::Int32, "all clone wire tags stay exact");
        const auto* record = candidate.FindObject(key);
        if (!record) { Check(false, "clone record exists"); continue; }
        const auto field = std::find_if(record->modifications.begin(), record->modifications.end(), [](const auto& mod) { return mod.key.fieldId == "udef"; });
        Check(field != record->modifications.end() && field->value == Value{std::string_view(id) == "E000" ? 9.0f : 7.0f}, "clone values are independent snapshots");
    }
    Check(nativeCodec.Serialize(*baseline.value).value == original.value, "source native baseline remains byte exact");
    const auto fingerprint = restored->DocumentFingerprint();
    std::vector<LniObjectDocument::CloneSource> overBudget(LniObjectDocument::kMaximumCloneSources + 1, {"E001", "E000"});
    Check(!restored->RestoreCloneSources(overBudget, *baseline.value).Succeeded()
        && restored->DocumentFingerprint() == fingerprint, "oversized clone manifest rejects before native cloning");
    for (const auto& invalid : std::vector<std::vector<LniObjectDocument::CloneSource>>{
        {{"E001", "E000"}, {"E001", "E000"}},
        {{"E001", "E002"}, {"E002", "E001"}},
        {{"E001", "E999"}}, {{"E000", "E001"}}, {{"E001", "Eevi"}}}) {
        Check(!restored->RestoreCloneSources(invalid, *baseline.value).Succeeded(), "duplicate/cycle/missing/native target/original source rejects");
        Check(restored->DocumentFingerprint() == fingerprint, "failed restore preserves complete clone identity");
        Check(nativeCodec.Serialize(*baseline.value).value == original.value, "failed restore leaves native source exact");
    }
    const std::string parents = "[E000]\n_parent=\"Eevi\"\n[E001]\n_parent=\"hfoo\"\n";
    auto wrongParent = lniCodec.Parse(std::as_bytes(std::span(parents.data(), parents.size())));
    const std::array<LniObjectDocument::CloneSource, 1> parentOrigin{{{"E001", "E000"}}};
    auto* parentDoc = dynamic_cast<LniObjectDocument*>(wrongParent.document.get());
    Check(parentDoc && !parentDoc->RestoreCloneSources(parentOrigin, *baseline.value).Succeeded(), "wrong clone parent rejects");
    const auto merged = MergeGeneratedNativeWithLniEdits(*baseline.value, *restored, candidate, &*schema.value);
    Check(merged.Succeeded() && merged.value && merged.value->CloneSources().size() == 2,
        "generated native merge restores clone origins before exact projection");
    auto bad = *restored;
    Check(bad.SetModification({ObjectType::Unit, "E002"}, {"unam"}, std::int32_t{123}).Succeeded(), "construct a late invalid name value");
    auto rejectedCandidate = *baseline.value;
    Check(!ApplyLniChangesToNative(rejectedCandidate, bad, &*schema.value).Succeeded()
        && nativeCodec.Serialize(rejectedCandidate).value == original.value, "late field failure rolls back all native clones and source edits");
    auto deleting = *restored;
    const auto deleteAudited = [&](std::string rawcode) {
        ReferenceAuditReport report;
        report.subject = {ObjectType::Unit, std::move(rawcode)};
        report.documentFingerprint = deleting.DocumentFingerprint();
        report.inspectedSources = RequiredReferenceSources();
        const auto token = ReferenceAuditAuthority::IssueDeletionToken(report, true);
        return token.value && deleting.DeleteObject(report.subject, *token.value).Succeeded();
    };
    Check(!deleteAudited("E000") && !deleteAudited("E001")
        && deleting.DocumentFingerprint() == fingerprint, "native root and intermediate clone cannot be deleted while depended on");
    Check(deleteAudited("E002") && deleting.CloneSources().size() == 1
        && deleteAudited("E001") && deleting.CloneSources().empty(), "deleting clone destinations removes only their own origins");
    auto undoCopy = *restored;
    deleting = undoCopy;
    Check(deleting.DocumentFingerprint() == fingerprint && deleting.CloneSources().size() == 2,
        "whole-document undo restores clone identities");
    const auto saved = nativeCodec.Serialize(candidate);
    restored->ClearCloneSources();
    Check(ApplyLniChangesToNative(candidate, *restored, &*schema.value).Succeeded()
        && nativeCodec.Serialize(candidate).value == saved.value, "rebased clone can save again without stale source metadata");
}

void TestLniCloneOfPendingCustomSource() {
    const std::string table = "ID;PWXL;N;E\nB;X4;Y2\n"
        "C;X1;Y1;K\"ID\"\nC;X2;K\"field\"\nC;X3;K\"type\"\nC;X4;K\"slk\"\n"
        "C;X1;Y2;K\"unam\"\nC;X2;K\"Name\"\nC;X3;K\"string\"\nC;X4;K\"Profile\"\nE\n";
    ObjectFieldSchemaOptions options; options.sources = {{ObjectType::Item, "items.slk"}}; options.stringFiles.clear();
    auto schema = ObjectFieldSchema::Load([&](const std::string& path) {
        Result<std::vector<std::byte>> result;
        if (path == "items.slk") result.value = std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(table.data()), reinterpret_cast<const std::byte*>(table.data() + table.size()));
        return result;
    }, options);
    Check(schema.Succeeded() && schema.value, "pending source field schema loads");
    if (!schema.value) return;
    auto baseline = NativeObjectDocument::CreateEmpty(ObjectType::Item);
    if (!baseline.value) return;
    const NativeObjectCodec nativeCodec(ObjectType::Item);
    const auto original = nativeCodec.Serialize(*baseline.value);
    const LniObjectCodec lniCodec(ObjectType::Item);
    auto parsed = lniCodec.Parse(std::span<const std::byte>{});
    auto* edited = dynamic_cast<LniObjectDocument*>(parsed.document.get());
    if (!edited) return;
    Check(edited->AddObject("I001", "itst").Succeeded()
        && edited->SetModification({ObjectType::Item, "I001"}, {"unam"}, std::string("Before clone")).Succeeded(),
        "ordinary custom source is new in this transaction");
    Check(edited->CloneCustomObject({ObjectType::Item, "I001"}, "I002").Succeeded()
        && edited->CloneCustomObject({ObjectType::Item, "I002"}, "I003").Succeeded(), "pending root supports clone topology");
    const auto undoCopy = *edited;
    Check(edited->SetModification({ObjectType::Item, "I001"}, {"unam"}, std::string("After clone")).Succeeded(),
        "later source value does not alter clone values");
    const auto text = lniCodec.Serialize(*edited);
    if (!text.value) return;
    auto reparsed = lniCodec.Parse(*text.value);
    auto* restored = dynamic_cast<LniObjectDocument*>(reparsed.document.get());
    if (!restored) return;
    std::vector<LniObjectDocument::CloneSource> sources(edited->CloneSources().begin(), edited->CloneSources().end());
    std::reverse(sources.begin(), sources.end());
    Check(restored->RestoreCloneSources(sources, *baseline.value, &*schema.value).Succeeded()
        && restored->DocumentFingerprint() == edited->DocumentFingerprint(), "pending root provenance restores without inventing a baseline object");
    auto candidate = *baseline.value;
    Check(ApplyLniChangesToNative(candidate, *restored, &*schema.value).Succeeded(),
        "new I001 from itst materializes before actual I002/I003 native cloning");
    Check(candidate.CustomObjectCount() == 3, "all pending custom objects survive one projection");
    for (const auto* id : {"I001", "I002", "I003"}) {
        const ObjectKey key{ObjectType::Item, id};
        const auto* record = candidate.FindObject(key);
        Check(record && record->parentRawcode == std::optional<std::string>{"itst"}
            && candidate.ModificationValueType(key, {"unam"}) == NativeValueType::String, "pending clone parent and typed name stay exact");
        if (!record) continue;
        const auto name = std::find_if(record->modifications.begin(), record->modifications.end(), [](const auto& mod) { return mod.key.fieldId == "unam"; });
        Check(name != record->modifications.end() && name->value == Value{std::string(std::string_view(id) == "I001" ? "After clone" : "Before clone")},
            "clones retain their own values while the pending source changes");
    }
    auto bad = *restored;
    Check(bad.SetModification({ObjectType::Item, "I001"}, {"unam"}, std::int32_t{3}).Succeeded(), "construct invalid pending source value");
    auto rejected = *baseline.value;
    Check(!ApplyLniChangesToNative(rejected, bad, &*schema.value).Succeeded()
        && nativeCodec.Serialize(rejected).value == original.value, "strict source encoding failure creates neither root nor clone");
    const auto badFingerprint = bad.DocumentFingerprint();
    Check(!bad.RestoreCloneSources(sources, *baseline.value, &*schema.value).Succeeded()
        && bad.DocumentFingerprint() == badFingerprint, "project restore cannot bypass pending source wire type checks or change the document on failure");
    auto undone = undoCopy;
    auto undoCandidate = *baseline.value;
    Check(ApplyLniChangesToNative(undoCandidate, undone, &*schema.value).Succeeded()
        && undone.CloneSources().size() == 2, "whole-document undo keeps pending clone provenance");
    const auto saved = nativeCodec.Serialize(candidate);
    restored->ClearCloneSources();
    Check(ApplyLniChangesToNative(candidate, *restored, &*schema.value).Succeeded()
        && nativeCodec.Serialize(candidate).value == saved.value, "rebased pending clone saves again without stale metadata");
    Check(nativeCodec.Serialize(*baseline.value).value == original.value, "all projections leave original empty source byte exact");
}

void TestNativeWireByteSizes() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        const NativeObjectCodec codec(type);
        const auto source = BuildFixture(type);
        auto document = ParseOwned(codec, source);
        if (!document) continue;
        const auto checkSizes = [&] {
            const auto encoded = codec.Serialize(*document);
            const auto bytes = document->SerializedByteSize();
            Check(encoded.Succeeded() && encoded.value && bytes && *bytes == encoded.value->size(),
                "read-only native size equals actual encoded bytes across seven types");
            std::size_t objectBytes = 12;
            for (const auto& object : document->Objects()) {
                const auto size = document->ObjectSerializedByteSize(object.key);
                Check(size.has_value(), "active native record has actual wire size");
                if (size) objectBytes += *size;
            }
            Check(bytes && *bytes == objectBytes, "document size includes each active record once plus the v2 header");
        };
        checkSizes(); // 未改字段必须使用原始 sourceLength。
        Check(document->SetModification({type, "hfoo"}, FixtureKey(type, "S001"),
            std::string("A longer changed string")).Succeeded(), "size fixture changes string length");
        checkSizes();
        Check(document->AddModification({type, "H001"}, FixtureKey(type, "NEWS", 4, 7),
            NativeValueType::String, std::string("New field")).Succeeded(), "size fixture adds a typed string field");
        checkSizes();
        Check(document->CloneCustomObject({type, "H001"}, "H002").Succeeded(), "size fixture changes copied end markers");
        checkSizes();
        Check(document->RemoveModification({type, "H001"}, FixtureKey(type, "ZZZZ", 4, 7)).Succeeded(),
            "size fixture removes an existing modification");
        Check(document->DeleteCustomObject({type, "H001"}).Succeeded(), "size fixture deletes a stored object");
        checkSizes();
        Check(!document->ObjectSerializedByteSize({type, "H001"})
            && !document->ObjectSerializedByteSize({type, "NONE"}), "deleted and missing records have no wire size");
    }
    const NativeObjectCodec codec(ObjectType::Unit);
    const auto classic = ParseOwned(codec, BuildFixture(ObjectType::Unit, 1.25f, 0.75f, 1));
    Check(classic && !classic->SerializedByteSize()
        && !classic->ObjectSerializedByteSize({ObjectType::Unit, "H001"}), "clone size API cannot widen v1 structural capability");
}

void TestNativeCloneProjectionBudget() {
    constexpr std::size_t payloadBytes = 1024 * 1024;
    const std::string payload(payloadBytes, 'x');
    const auto budgetFailure = [](const auto& result) {
        return !result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "lni-native-clone-projection-budget";
        });
    };
    const auto id = [](char prefix, std::size_t number) {
        const auto digits = std::to_string(number);
        return std::string(1, prefix) + std::string(3 - digits.size(), '0') + digits;
    };
    auto built = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    if (!built.value) return;
    Check(built.value->AddCustomObject("E000", "hfoo").Succeeded(), "budget fixture adds actual native source");
    Check(built.value->AddModification({ObjectType::Unit, "E000"}, {"unam"},
        NativeValueType::String, payload).Succeeded(), "budget source has a one MiB wire string");
    Check(built.value->AddModification({ObjectType::Unit, "E000"}, {"udup"},
        NativeValueType::Unreal, 5.0f).Succeeded(), "budget source retains a true Unreal wire tag");
    const NativeObjectCodec nativeCodec(ObjectType::Unit);
    const auto nativeBytes = nativeCodec.Serialize(*built.value);
    if (!nativeBytes.value) return;
    auto baseline = ParseOwned(nativeCodec, *nativeBytes.value);
    if (!baseline) return;
    const LniObjectCodec lniCodec(ObjectType::Unit);
    const std::string text = "[E000]\n_parent=\"hfoo\"\n";
    auto parsed = lniCodec.Parse(std::as_bytes(std::span(text.data(), text.size())));
    auto* skinny = dynamic_cast<LniObjectDocument*>(parsed.document.get());
    if (!skinny) return;
    for (std::size_t number = 1; number <= 64; ++number) {
        Check(skinny->CloneCustomObject({ObjectType::Unit, "E000"}, id('E', number)).Succeeded(),
            "skinny LNI clone creates valid provenance without copying the actual native payload");
    }
    const auto skinnyText = lniCodec.Serialize(*skinny);
    Check(skinnyText.value && skinnyText.value->size() < 4096, "malicious projection uses a genuinely small LNI document");
    const auto fingerprint = skinny->DocumentFingerprint();
    const std::vector<LniObjectDocument::CloneSource> sources(skinny->CloneSources().begin(), skinny->CloneSources().end());
    const auto recordBytes = baseline->ObjectSerializedByteSize({ObjectType::Unit, "E000"});
    Check(recordBytes && 12 + 65 * *recordBytes > LniObjectDocument::kMaximumNativeCloneProjectionBytes,
        "one actual source plus 64 clones exceeds the intermediate wire budget");
    Check(budgetFailure(skinny->RestoreCloneSources(sources, *baseline))
        && skinny->DocumentFingerprint() == fingerprint, "restore rejects wire expansion before native clone and preserves provenance fingerprint");
    auto staged = *baseline;
    Check(budgetFailure(StagePendingNativeCloneSources(staged, *skinny))
        && nativeCodec.Serialize(staged).value == nativeBytes.value, "direct Stage enforces budget even when there are no pending roots");
    Check(budgetFailure(skinny->PrepareNativeClones(staged))
        && nativeCodec.Serialize(staged).value == nativeBytes.value, "direct Prepare cannot bypass the first-clone budget");
    Check(budgetFailure(ApplyLniChangesToNative(staged, *skinny))
        && nativeCodec.Serialize(staged).value == nativeBytes.value
        && skinny->DocumentFingerprint() == fingerprint, "Apply rejects peak expansion even though the final diff would delete all payload fields");
    Check(nativeCodec.Serialize(*baseline).value == nativeBytes.value, "budget rejection leaves the actual native baseline byte exact");

    const std::string positiveText = "[E000]\n_parent=\"hfoo\"\nudup=5.0\n";
    auto positiveParsed = lniCodec.Parse(std::as_bytes(std::span(positiveText.data(), positiveText.size())));
    auto* positive = dynamic_cast<LniObjectDocument*>(positiveParsed.document.get());
    if (!positive) return;
    Check(positive->CloneCustomObject({ObjectType::Unit, "E000"}, "E001").Succeeded()
        && positive->CloneCustomObject({ObjectType::Unit, "E001"}, "E002").Succeeded(), "under-budget fixture includes a clone-of-clone");
    std::vector<LniObjectDocument::CloneSource> positiveSources(positive->CloneSources().begin(), positive->CloneSources().end());
    std::reverse(positiveSources.begin(), positiveSources.end());
    Check(positive->RestoreCloneSources(positiveSources, *baseline).Succeeded(), "under-budget same-wire provenance restores in arbitrary manifest order");
    auto copied = *baseline;
    Check(positive->PrepareNativeClones(copied).Succeeded()
        && recordBytes && copied.SerializedByteSize() == std::optional<std::size_t>{12 + 3 * *recordBytes},
        "under-budget topology materializes exact actual record sizes");
    for (const auto* rawcode : {"E000", "E001", "E002"}) {
        const auto* record = copied.FindObject({ObjectType::Unit, rawcode});
        Check(record && copied.ModificationValueType({ObjectType::Unit, rawcode}, {"udup"}) == NativeValueType::Unreal
            && std::any_of(record->modifications.begin(), record->modifications.end(), [&](const auto& field) {
                const auto* text = std::get_if<std::string>(&field.value);
                return field.key.fieldId == "unam" && text && *text == payload;
            }), "actual string bytes and Unreal tags survive under-budget native cloning");
    }
    auto applied = *baseline;
    Check(ApplyLniChangesToNative(applied, *positive).Succeeded()
        && applied.ModificationValueType({ObjectType::Unit, "E002"}, {"udup"}) == NativeValueType::Unreal
        && applied.SerializedByteSize() && *applied.SerializedByteSize() < 1024,
        "later field deletion stays allowed after a safe clone peak and preserves actual wire tags");

    // 根在克隆后才增加大字段，目标快照仍为空；首次 Stage 也会先复制当前根再删除目标字段。
    const NativeObjectCodec itemCodec(ObjectType::Item);
    auto empty = NativeObjectDocument::CreateEmpty(ObjectType::Item);
    if (!empty.value) return;
    const auto emptyBytes = itemCodec.Serialize(*empty.value);
    auto pendingParsed = LniObjectCodec(ObjectType::Item).Parse(std::span<const std::byte>{});
    auto* pending = dynamic_cast<LniObjectDocument*>(pendingParsed.document.get());
    if (!pending) return;
    Check(pending->AddObject("I001", "itst").Succeeded(), "pending budget fixture creates ordinary new root");
    for (std::size_t number = 2; number <= 64; ++number) {
        Check(pending->CloneCustomObject({ObjectType::Item, "I001"}, id('I', number)).Succeeded(),
            "pending budget target remains skinny with legitimate clone provenance");
    }
    Check(pending->SetModification({ObjectType::Item, "I001"}, {"unam"}, payload).Succeeded(), "pending root grows after clone snapshots");
    const auto pendingFingerprint = pending->DocumentFingerprint();
    const std::vector<LniObjectDocument::CloneSource> pendingSources(pending->CloneSources().begin(), pending->CloneSources().end());
    const auto pendingRecordBytes = payloadBytes + 25;
    Check(12 + 63 * pendingRecordBytes < LniObjectDocument::kMaximumNativeCloneProjectionBytes
        && 12 + 64 * pendingRecordBytes > LniObjectDocument::kMaximumNativeCloneProjectionBytes,
        "counting the pending root itself changes this fixture from under budget to over budget");
    Check(budgetFailure(pending->RestoreCloneSources(pendingSources, *empty.value))
        && pending->DocumentFingerprint() == pendingFingerprint, "restore counts pending root wire bytes before staging it");
    Check(budgetFailure(StagePendingNativeCloneSources(*empty.value, *pending))
        && budgetFailure(ApplyLniChangesToNative(*empty.value, *pending))
        && itemCodec.Serialize(*empty.value).value == emptyBytes.value
        && pending->DocumentFingerprint() == pendingFingerprint, "pending Stage and Apply reject atomically without materializing a root or clone");
}

std::unique_ptr<LniObjectDocument> LniFromRecords(ObjectType type, std::span<const ObjectRecord> records) {
    const auto rendered = RenderObjectRecordsToLni(type, records);
    Check(rendered.Succeeded(), "synthetic records render to LNI");
    if (!rendered.value) return {};
    auto parsed = LniObjectCodec(type).Parse(*rendered.value);
    Check(parsed.Succeeded(), "synthetic records LNI parses");
    return std::unique_ptr<LniObjectDocument>(dynamic_cast<LniObjectDocument*>(parsed.document.release()));
}

void TestClassicV1LniTransactions() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        const auto source = BuildFixture(type, 1.25f, 0.75f, 1);
        NativeObjectCodec codec(type);
        auto baseline = ParseOwned(codec, source);
        if (!baseline) continue;
        auto lni = LniFromRecords(type, baseline->Objects());
        if (!lni) continue;
        NativeObjectDocument edited = *baseline;
        Check(ApplyLniChangesToNative(edited, *lni).Succeeded()
            && codec.Serialize(edited).value == std::optional{source}, "v1 LNI no-op preserves original bytes across six categories");
        Check(lni->SetModification({type, "hfoo"}, FixtureKey(type, "I001", 2, 3, 1), std::int32_t{99}).Succeeded()
            && lni->SetModification({type, "H001"}, FixtureKey(type, "ZZZZ", 4, 7, 1), std::int32_t{8}).Succeeded(),
            "v1 LNI edits original and custom existing values");
        const auto expected = BuildFixture(type, 1.25f, 0.75f, 1, 99, std::string_view{"A\xd6\xd0", 3}, 8);
        Check(ApplyLniChangesToNative(edited, *lni).Succeeded()
            && codec.Serialize(edited).value == std::optional{expected},
            "v1 LNI writes values using version-specific wire addresses without adding index bytes");
        Check(ApplyLniChangesToNative(edited, *lni).Succeeded()
            && codec.Serialize(edited).value == std::optional{expected}, "v1 repeated LNI apply is idempotent");

        for (int operation = 0; operation < 5; ++operation) {
            std::vector<ObjectRecord> records(baseline->Objects().begin(), baseline->Objects().end());
            records[0].modifications[0].value = std::int32_t{999}; // 此合法修改先被差分应用。
            switch (operation) {
            case 0:
                records[1].modifications.push_back({FixtureKey(type, "NEW1", 4, 7, 1), std::int32_t{1}});
                break;
            case 1:
                records[1].modifications.clear();
                break;
            case 2:
                records[1].parentRawcode = "hrif";
                break;
            case 3:
                records.pop_back();
                break;
            case 4: {
                ObjectRecord added = records[1];
                added.key.rawcode = "H002";
                records.push_back(std::move(added));
                break;
            }
            }
            auto unsupported = LniFromRecords(type, records);
            NativeObjectDocument staged = *baseline;
            Check(unsupported && !ApplyLniChangesToNative(staged, *unsupported).Succeeded(),
                "v1 LNI field addition/deletion, reparent and object addition/deletion stay denied");
            Check(codec.Serialize(staged).value == std::optional{source}
                && staged.Objects()[0].modifications[0].value == Value{std::int32_t{42}},
                "late v1 structural rejection rolls back earlier legal value change and source state");
        }
        auto modern = ParseOwned(codec, BuildFixture(type));
        Check(modern && !MergeGeneratedNativeWithLniEdits(*baseline, *lni, *modern).Succeeded()
            && !MergeGeneratedNativeWithLniEdits(*modern, *lni, *baseline).Succeeded(),
            "v1 baseline and generated output both remain excluded from template whole-category replacement");
    }
}

void TestDoodadV1LniIndexIdentity() {
    constexpr auto type = ObjectType::Doodad;
    const auto source = BuildFixture(type, 1.25f, 0.75f, 1);
    NativeObjectCodec codec(type);
    auto baseline = ParseOwned(codec, source);
    if (!baseline) return;
    std::vector<ObjectRecord> records(baseline->Objects().begin(), baseline->Objects().end());
    for (auto& record : records) {
        for (auto& modification : record.modifications) {
            modification.key.level = 0;
            modification.key.dataIndex = 0;
            modification.key.variation = 0;
        }
    }
    records[0].modifications[0].value = std::int32_t{99};
    auto placeholders = LniFromRecords(type, records);
    NativeObjectDocument edited = *baseline;
    Check(placeholders && ApplyLniChangesToNative(edited, *placeholders).Succeeded()
        && codec.Serialize(edited).value == std::optional{BuildFixture(type, 1.25f, 0.75f, 1, 99)},
        "v1 doodad zero LNI placeholders collapse to simple source address");

    for (int component = 0; component < 3; ++component) {
        records.assign(baseline->Objects().begin(), baseline->Objects().end());
        records[0].modifications[0].value = std::int32_t{999};
        auto& key = records[1].modifications[0].key;
        if (component == 0) key.level = 1;
        if (component == 1) key.dataIndex = 1;
        if (component == 2) key.variation = 1;
        auto unsupported = LniFromRecords(type, records);
        NativeObjectDocument unchanged = *baseline;
        Check(unsupported && !ApplyLniChangesToNative(unchanged, *unsupported).Succeeded()
            && codec.Serialize(unchanged).value == std::optional{source},
            "v1 doodad nonzero level/data/variation is not silently erased or written as v2");
    }
    records.assign(baseline->Objects().begin(), baseline->Objects().end());
    auto alias = records[0].modifications[0];
    alias.key.level = 0; alias.key.dataIndex = 0; alias.key.variation = 0;
    records[0].modifications.push_back(alias);
    auto ambiguous = LniFromRecords(type, records);
    NativeObjectDocument unchanged = *baseline;
    const auto result = ambiguous ? ApplyLniChangesToNative(unchanged, *ambiguous) : Status{};
    Check(ambiguous && !result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const Diagnostic& diagnostic) { return diagnostic.code == "lni-native-field-alias"; })
        && codec.Serialize(unchanged).value == std::optional{source}, "v1 normalized LNI aliases refuse the whole transaction");
}

void TestCreateEmptyAndOriginalOverrides() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        NativeObjectCodec codec(type);
        auto created = NativeObjectDocument::CreateEmpty(type);
        Check(created.Succeeded(), "seven classic empty documents create");
        if (!created.value) continue;
        auto& document = *created.value;
        std::vector<std::byte> emptyBytes;
        AppendInt32(emptyBytes, 2); AppendInt32(emptyBytes, 0); AppendInt32(emptyBytes, 0);
        Check(document.Version() == 2 && document.Type() == type && document.IsEditable() && document.IsStructurallySafe(), "empty document identity is explicit classic v2");
        Check(codec.Serialize(document).value == std::optional{emptyBytes}, "empty document has exact twelve byte encoding");
        Check(document.AddOriginalObject("hfoo").Succeeded(), "add original override");
        Check(document.AddCustomObject("H001", "hfoo").Succeeded(), "add custom distinct table");
        Check(document.AddOriginalObject("HFOO").Succeeded(), "rawcode case remains distinct");
        const ObjectKey original{type, "hfoo"}, custom{type, "H001"};
        Check(document.OriginalObjectCount() == 2 && document.CustomObjectCount() == 1, "table counts after both additions");
        Check(document.FindObject(original) && !document.FindObject(original)->parentRawcode, "original override has no parent marker");
        Check(document.FindObject(custom) && document.FindObject(custom)->parentRawcode == "hfoo", "custom retains parent");
        const auto integerKey = FixtureKey(type, "I001");
        const auto realKey = FixtureKey(type, "R001");
        const auto stringKey = FixtureKey(type, "S001");
        Check(document.AddModification(original, integerKey, NativeValueType::Int32, std::int32_t{10}).Succeeded(), "original override initial field");
        Check(document.AddModification(original, realKey, NativeValueType::Real, 1.25f).Succeeded(), "original override real field");
        Check(document.AddModification(custom, stringKey, NativeValueType::String, std::string{"Custom initial fields"}).Succeeded(), "custom initial field");
        Check(document.SetModification(original, integerKey, std::int32_t{77}).Succeeded(), "edit original override");
        const auto output = codec.Serialize(document);
        Check(output.Succeeded(), "serialize new original and custom objects");
        if (!output.value) continue;
        auto reopened = ParseOwned(codec, *output.value);
        Check(reopened && reopened->OriginalObjectCount() == 2 && reopened->CustomObjectCount() == 1, "reparse retains Original/Custom split");
        if (!reopened) continue;
        Check(reopened->FindObject(original)->modifications.size() == 2 && reopened->FindObject(custom)->modifications.size() == 1, "all added fields retained");
        Check(codec.Serialize(*reopened).value == output.value, "new document no-op byte fidelity");
        const auto unchanged = *codec.Serialize(document).value;
        Check(!document.AddOriginalObject("hfoo").Succeeded() && !document.AddOriginalObject("H001").Succeeded(), "reject duplicate within or across tables");
        Check(!document.AddCustomObject("HFOO", "hfoo").Succeeded(), "custom cannot shadow an original override");
        Check(!document.AddCustomObject("hnew", "hnew").Succeeded(), "custom cannot inherit from itself or disguise an Original overlay");
        Check(!document.AddOriginalObject("bad").Succeeded() && !document.AddOriginalObject(std::string("h\0oo", 4)).Succeeded(), "reject invalid original IDs");
        Check(!document.AddCustomObject("abc", "hfoo").Succeeded() && !document.AddCustomObject("H002", std::string("h\0oo", 4)).Succeeded(), "reject invalid custom IDs");
        Check(!document.DeleteCustomObject(original).Succeeded() && !document.RemoveOriginalObjectOverrides(custom).Succeeded(), "table-specific removal cannot cross tables");
        Check(*codec.Serialize(document).value == unchanged, "rejected object operations leave byte state unchanged");

        // 清除覆盖恢复标准值；原版对象定义不在本地文档中，绝不会被删除。
        Check(document.RemoveOriginalObjectOverrides(original).Succeeded(), "remove exact original overlay record including fields");
        Check(!document.FindObject(original) && document.OriginalObjectCount() == 1 && document.FindObject(custom), "overlay remove preserves custom neighbor");
        Check(document.SetModification(custom, stringKey, std::string{"Neighbor survives"}).Succeeded(), "neighbor indexes remain editable after overlay remove");
        Check(document.AddOriginalObject("hfoo").Succeeded(), "removed original overlay can be re-added");
        Check(document.DeleteCustomObject(custom).Succeeded() && !document.FindObject(custom), "deleted custom removed from public diff view");
        Check(document.AddCustomObject("H001", "hfoo").Succeeded(), "custom undo can recreate same ID");
        const auto finalBytes = codec.Serialize(document);
        Check(finalBytes.Succeeded() && ParseOwned(codec, *finalBytes.value)->IsEditable(), "removal and recreation yields valid native document");
    }
    Check(!NativeObjectDocument::CreateEmpty(ObjectType::Unit, 1).Succeeded() &&
        !NativeObjectDocument::CreateEmpty(ObjectType::Unit, 3).Succeeded(), "empty creation rejects unsupported versions");
    Check(!NativeObjectDocument::CreateEmpty(static_cast<ObjectType>(255)).Succeeded(), "empty creation rejects unknown object kind");
}

void TestApplyNewObjectsAndUndoAcrossSevenTypes() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        auto target = NativeObjectDocument::CreateEmpty(type);
        if (!target.value) continue;
        ObjectRecord original;
        original.key = {type, "hfoo"};
        original.modifications.push_back({FixtureKey(type, "I001"), std::int32_t{55}});
        original.modifications.push_back({FixtureKey(type, "R001"), 1.5f});
        ObjectRecord custom;
        custom.key = {type, "H001"}; custom.parentRawcode = "hfoo";
        custom.modifications.push_back({FixtureKey(type, "S001"), std::string{"New custom"}});
        const std::array records{original, custom};
        auto lni = LniFromRecords(type, records);
        if (!lni) continue;
        Check(ApplyLniChangesToNative(*target.value, *lni).Succeeded(), "LNI adds original and custom with full initial fields");
        Check(target.value->OriginalObjectCount() == 1 && target.value->CustomObjectCount() == 1, "LNI addition preserves table identity");
        Check(target.value->FindObject(original.key) && target.value->FindObject(original.key)->modifications.size() == 2 &&
            target.value->FindObject(custom.key) && target.value->FindObject(custom.key)->modifications.size() == 1, "new-object initial fields are not omitted by object-only semantic diff");
        NativeObjectCodec codec(type);
        const auto first = codec.Serialize(*target.value);
        Check(first.Succeeded(), "LNI new object serialize");
        Check(ApplyLniChangesToNative(*target.value, *lni).Succeeded() && codec.Serialize(*target.value).value == first.value, "repeated apply is idempotent including doodad placeholders");
        if (first.value) {
            auto loaded = ParseOwned(codec, *first.value);
            Check(ApplyLniChangesToNative(*loaded, *lni).Succeeded() && codec.Serialize(*loaded).value == first.value, "saved/reopened baseline no-op preserves exact bytes");
            auto emptyLni = LniFromRecords(type, {});
            if (!emptyLni) continue;
            Check(ApplyLniChangesToNative(*loaded, *emptyLni).Succeeded(), "undo all additions removes original overlay and custom record");
            Check(loaded->Objects().empty() && loaded->OriginalObjectCount() == 0 && loaded->CustomObjectCount() == 0, "undo has no stale public object tombstones");
            Check(codec.Serialize(*loaded).value == codec.Serialize(*NativeObjectDocument::CreateEmpty(type).value).value, "second save after undo restores classic empty bytes");
            Check(ApplyLniChangesToNative(*loaded, *lni).Succeeded(), "redo restores both tables after saved undo");
        }
    }
}

void TestTransactionalApplyFailure() {
    NativeObjectCodec codec(ObjectType::Unit);
    const auto fixture = BuildFixture(ObjectType::Unit);
    auto target = ParseOwned(codec, fixture);
    std::vector<ObjectRecord> records(target->Objects().begin(), target->Objects().end());
    records[0].modifications[0].value = std::int32_t{999}; // 先发生合法修改。
    records[1].parentRawcode = "hrif"; // 后遇到不支持的重新指定父对象。
    auto badReparent = LniFromRecords(ObjectType::Unit, records);
    Check(badReparent && !ApplyLniChangesToNative(*target, *badReparent).Succeeded(), "late reparent rejects transaction");
    Check(codec.Serialize(*target).value == std::optional{fixture} && target->Objects()[0].modifications[0].value == Value{std::int32_t{42}}, "late failure rolls back earlier valid field changes and source bytes");

    records.assign(target->Objects().begin(), target->Objects().end());
    records.erase(records.begin() + 1); // 先移除合法 Custom 覆盖。
    ObjectRecord added;
    added.key = {ObjectType::Unit, "hnew"};
    added.modifications.push_back({{"I002"}, std::int32_t{12}});
    added.modifications.push_back({{"invalid_field_id"}, std::int32_t{13}});
    records.push_back(added);
    auto badAddedField = LniFromRecords(ObjectType::Unit, records);
    Check(badAddedField && !ApplyLniChangesToNative(*target, *badAddedField).Succeeded(), "new original late invalid field rejects");
    Check(codec.Serialize(*target).value == std::optional{fixture} && target->CustomObjectCount() == 1 && !target->FindObject(added.key), "failure rolls back prior deletion, object add and valid first new field");
    auto wrongType = LniFromRecords(ObjectType::Item, {});
    Check(wrongType && !ApplyLniChangesToNative(*target, *wrongType).Succeeded() && codec.Serialize(*target).value == std::optional{fixture}, "cross-type apply rejected before any mutation");

    records.assign(target->Objects().begin(), target->Objects().end());
    records[0].modifications.push_back({{"I001", 0}, std::int32_t{456}});
    auto aliases = LniFromRecords(ObjectType::Unit, records);
    Check(aliases && !ApplyLniChangesToNative(*target, *aliases).Succeeded(), "distinct LNI zero-index spellings cannot target one native field twice");
    Check(codec.Serialize(*target).value == std::optional{fixture}, "normalized alias rejection leaves native bytes unchanged");
}

void TestMultilineFieldsKeepExactNativeIdentity() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        NativeObjectCodec codec(type);
        auto document = ParseOwned(codec, BuildFixture(type));
        if (!document) continue;
        const ObjectKey original{type, "hfoo"};
        const auto textKey = FixtureKey(type, "S001");
        ModificationKey lniTextKey = textKey;
        if (type == ObjectType::Doodad) lniTextKey.level = 0; // 旧调用方的零占位仍归一到原生键。
        const Value originalText = std::string{"Line one\r\nLine two\n"};
        Check(document->SetModification(original, textKey, originalText).Succeeded(), "create multiline native field");
        const auto baseline = codec.Serialize(*document);
        if (!baseline.value) continue;
        const auto exportedLni = RenderObjectRecordsToLni(type, document->Objects());
        const std::string exportedText = exportedLni.value
            ? std::string(reinterpret_cast<const char*>(exportedLni.value->data()), exportedLni.value->size())
            : std::string{};
        Check(exportedLni.value && exportedText.find(" = [=[\r\nLine one\r\nLine two\n]=]") != std::string::npos
            && exportedText.find("FORGE_MULTILINE") == std::string::npos,
            "all seven native categories export multiline strings with w3x2lni syntax");
        auto lni = LniFromRecords(type, document->Objects());
        if (!lni) continue;
        const auto* record = lni->FindObject(original);
        Check(record && std::any_of(record->modifications.begin(), record->modifications.end(),
            [&](const auto& field) { return field.key.fieldId == "S001" && !field.valueWasOpaque && field.value == originalText; }),
            "marked multiline LNI field keeps CRLF and trailing newline as editable text");
        Check(ApplyLniChangesToNative(*document, *lni).Succeeded() && codec.Serialize(*document).value == baseline.value,
            "multiline no-op preserves exact native bytes across all seven types");

        const auto integerKey = FixtureKey(type, "I001");
        ModificationKey lniIntegerKey = integerKey;
        Check(lni->SetModification(original, lniIntegerKey, std::int32_t{123}).Succeeded(), "edit neighbor of multiline field");
        Check(ApplyLniChangesToNative(*document, *lni).Succeeded(), "multiline field does not block safe neighbor write");
        const auto edited = codec.Serialize(*document);
        if (!edited.value) continue;
        auto reopened = ParseOwned(codec, *edited.value);
        const auto* restored = reopened ? reopened->FindObject(original) : nullptr;
        Check(restored && std::any_of(restored->modifications.begin(), restored->modifications.end(),
            [&](const auto& field) { return field.key == textKey && field.value == originalText; }),
            "neighbor edit retains exact multiline value including newline bytes and doodad variation identity");

        const Value changedText = std::string{"Edited\\literal\r\nsecond line\n"};
        Check(lni->SetModification(original, lniTextKey, changedText).Succeeded(), "edit existing multiline field");
        const auto lniBytes = LniObjectCodec(type).Serialize(*lni);
        LniObjectDocument cancelled = *lni;
        Check(cancelled.SetModification(original, lniTextKey, std::string{"Cancelled\nvalue\n"}).Succeeded() &&
            LniObjectCodec(type).Serialize(*lni).value == lniBytes.value,
            "discarding a tentative multiline edit leaves project document unchanged");
        auto rereadLni = lniBytes.value ? LniObjectCodec(type).Parse(*lniBytes.value) : CodecParseResult{};
        const auto* restoredLni = dynamic_cast<const LniObjectDocument*>(rereadLni.document.get());
        const auto* editedField = restoredLni ? restoredLni->FindObject(original) : nullptr;
        Check(editedField && std::any_of(editedField->modifications.begin(), editedField->modifications.end(),
            [&](const auto& field) { return field.key.fieldId == "S001" && !field.valueWasOpaque && field.value == changedText; }),
            "edited multiline value remains exact after project reopen");
        auto* mutableReopened = dynamic_cast<LniObjectDocument*>(rereadLni.document.get());
        const Value editedAgain = std::string{"Second edit\r\nwith end\n"};
        Check(mutableReopened && mutableReopened->SetModification(original, lniTextKey, editedAgain).Succeeded(),
            "reopened multiline value remains editable");
        const auto secondLni = mutableReopened ? LniObjectCodec(type).Serialize(*mutableReopened)
            : Result<std::vector<std::byte>>{};
        auto rereadSecond = secondLni.value ? LniObjectCodec(type).Parse(*secondLni.value) : CodecParseResult{};
        const auto* secondDocument = dynamic_cast<const LniObjectDocument*>(rereadSecond.document.get());
        const auto* secondRecord = secondDocument ? secondDocument->FindObject(original) : nullptr;
        Check(secondRecord && std::any_of(secondRecord->modifications.begin(), secondRecord->modifications.end(),
            [&](const auto& field) { return field.key.fieldId == "S001" && !field.valueWasOpaque && field.value == editedAgain; }),
            "second multiline edit survives another project reopen");
        Check(ApplyLniChangesToNative(*document, *lni).Succeeded(), "edited multiline writes to native");
        const auto changed = codec.Serialize(*document);
        auto changedNative = changed.value ? ParseOwned(codec, *changed.value) : nullptr;
        const auto* changedRecord = changedNative ? changedNative->FindObject(original) : nullptr;
        Check(changedRecord && std::any_of(changedRecord->modifications.begin(), changedRecord->modifications.end(),
            [&](const auto& field) { return field.key == textKey && field.value == changedText; }),
            "native reopen retains edited CRLF, trailing newline and literal backslash");

        const Value addedText = std::string{"A\nB\n"};
        ModificationKey newLniKey = FixtureKey(type, "S002");
        Check(lni->SetModification(original, newLniKey, addedText).Succeeded(),
            "new multiline field can be added");
        Check(ApplyLniChangesToNative(*document, *lni).Succeeded(), "new multiline field writes to native");
        const auto added = codec.Serialize(*document);
        auto addedNative = added.value ? ParseOwned(codec, *added.value) : nullptr;
        const auto* addedRecord = addedNative ? addedNative->FindObject(original) : nullptr;
        Check(addedRecord && std::any_of(addedRecord->modifications.begin(), addedRecord->modifications.end(),
            [&](const auto& field) { return field.key.fieldId == "S002" && field.value == addedText; }),
            "new multiline field survives native reopen");
    }
}

void TestLniLongBracketInteroperability() {
    const ObjectKey object{ObjectType::Unit, "hfoo"};
    const ModificationKey textKey{"S001"};
    LniObjectCodec codec(ObjectType::Unit);
    const std::string fixture = "[hfoo]\r\nS001 = [=[\r\nfirst\r\nsecond\n]=]\r\nI001 = 3\r\n";
    auto parsed = codec.Parse(std::as_bytes(std::span(fixture.data(), fixture.size())));
    auto* document = dynamic_cast<LniObjectDocument*>(parsed.document.get());
    const auto* record = document ? document->FindObject(object) : nullptr;
    Check(parsed.Succeeded() && record && record->modifications.size() == 2
        && record->modifications[0].value == Value{std::string{"first\r\nsecond\n"}}
        && !record->modifications[0].valueWasOpaque,
        "w3x2lni long bracket decodes CRLF and trailing newline as editable text");
    if (!document) return;
    const auto unchanged = codec.Serialize(*document);
    Check(unchanged.value && std::string(reinterpret_cast<const char*>(unchanged.value->data()),
        unchanged.value->size()) == fixture,
        "unchanged external long bracket retains original bytes");

    const std::string firstEdit = "quote \" and \\ plus ]=] and ]==]\r\nsecond\n";
    Check(document->SetModification(object, textKey, firstEdit).Succeeded(),
        "external long bracket can be edited in place");
    const auto edited = codec.Serialize(*document);
    const std::string editedText = edited.value
        ? std::string(reinterpret_cast<const char*>(edited.value->data()), edited.value->size())
        : std::string{};
    Check(edited.value && editedText.find("S001 = [===[\r\n") != std::string::npos
        && editedText.find("-- FORGE_MULTILINE_") == std::string::npos
        && editedText.find("I001 = 3\r\n") != std::string::npos,
        "new multiline output selects a non-colliding Lua delimiter and keeps the neighbor");
    auto reopened = edited.value ? codec.Parse(*edited.value) : CodecParseResult{};
    auto* again = dynamic_cast<LniObjectDocument*>(reopened.document.get());
    const auto* reopenedRecord = again ? again->FindObject(object) : nullptr;
    Check(reopened.Succeeded() && reopenedRecord
        && reopenedRecord->modifications[0].value == Value{firstEdit},
        "edited long bracket reopens without changing content");
    if (!again) return;
    const std::string secondEdit = "again\r\nwith final newline\n";
    Check(again->SetModification(object, textKey, secondEdit).Succeeded(),
        "reopened long bracket remains editable");
    const auto second = codec.Serialize(*again);
    auto secondReopen = second.value ? codec.Parse(*second.value) : CodecParseResult{};
    auto* secondDocument = dynamic_cast<LniObjectDocument*>(secondReopen.document.get());
    const auto* secondRecord = secondDocument ? secondDocument->FindObject(object) : nullptr;
    Check(secondReopen.Succeeded() && secondRecord
        && secondRecord->modifications[0].value == Value{secondEdit},
        "second edit and reopen preserve exact multiline bytes");
    if (secondDocument) {
        Check(secondDocument->RemoveModification(object, textKey).Succeeded(),
            "long bracket field can be reset without orphaning continuation lines");
        const auto removed = codec.Serialize(*secondDocument);
        auto removedReopen = removed.value ? codec.Parse(*removed.value) : CodecParseResult{};
        const auto* removedDocument = dynamic_cast<const LniObjectDocument*>(removedReopen.document.get());
        const auto* remaining = removedDocument ? removedDocument->FindObject(object) : nullptr;
        Check(removedReopen.Succeeded() && remaining && remaining->modifications.size() == 1
            && remaining->modifications[0].key.fieldId == "I001",
            "reset long bracket leaves only the independent neighbor");
    }

    const std::string oldPrivate =
        "[hfoo]\nS001 = \"old\\r\\nline\\n\" -- FORGE_MULTILINE_ESCAPED_V1\n";
    auto legacy = codec.Parse(std::as_bytes(std::span(oldPrivate.data(), oldPrivate.size())));
    const auto* legacyDocument = dynamic_cast<const LniObjectDocument*>(legacy.document.get());
    const auto converted = legacyDocument ? codec.Serialize(*legacyDocument)
        : Result<std::vector<std::byte>>{};
    const std::string convertedText = converted.value
        ? std::string(reinterpret_cast<const char*>(converted.value->data()), converted.value->size())
        : std::string{};
    Check(legacy.Succeeded() && converted.value
        && convertedText.find("S001 = [=[\r\nold\r\nline\n]=]") != std::string::npos
        && convertedText.find("FORGE_MULTILINE") == std::string::npos,
        "legacy private multiline marker migrates on serialize to w3x2lni syntax");
    auto migrated = converted.value ? codec.Parse(*converted.value) : CodecParseResult{};
    const auto* migratedDocument = dynamic_cast<const LniObjectDocument*>(migrated.document.get());
    const auto* migratedRecord = migratedDocument ? migratedDocument->FindObject(object) : nullptr;
    Check(migrated.Succeeded() && migratedRecord
        && migratedRecord->modifications[0].value == Value{std::string{"old\r\nline\n"}},
        "migrated private value reopens with exact original content");

    const std::string withNote = "[hfoo]\nS001 = \"old\" -- note\n";
    auto noted = codec.Parse(std::as_bytes(std::span(withNote.data(), withNote.size())));
    auto* notedDocument = dynamic_cast<LniObjectDocument*>(noted.document.get());
    Check(notedDocument && notedDocument->SetModification(object, textKey,
        std::string{"new\nvalue"}).Succeeded(),
        "scalar field with Lua comment can become multiline");
    const auto notedBytes = notedDocument ? codec.Serialize(*notedDocument)
        : Result<std::vector<std::byte>>{};
    auto notedReopen = notedBytes.value ? codec.Parse(*notedBytes.value) : CodecParseResult{};
    const auto* notedReopenDocument = dynamic_cast<const LniObjectDocument*>(notedReopen.document.get());
    const auto* notedRecord = notedReopenDocument ? notedReopenDocument->FindObject(object) : nullptr;
    Check(notedReopen.Succeeded() && notedRecord
        && notedRecord->modifications[0].value == Value{std::string{"new\nvalue"}}
        && notedBytes.value
        && std::string(reinterpret_cast<const char*>(notedBytes.value->data()),
            notedBytes.value->size()).find("]=] -- note") != std::string::npos,
        "long bracket preserves an inline Lua comment after the closing delimiter");

    const std::string ambiguous = "[hfoo]\nS001 = [=[ \nvalue]=]\n";
    auto opaque = codec.Parse(std::as_bytes(std::span(ambiguous.data(), ambiguous.size())));
    auto* opaqueDocument = dynamic_cast<LniObjectDocument*>(opaque.document.get());
    const auto* opaqueRecord = opaqueDocument ? opaqueDocument->FindObject(object) : nullptr;
    Check(opaque.Succeeded() && opaqueRecord && opaqueRecord->modifications[0].valueWasOpaque
        && !opaqueDocument->SetModification(object, textKey, std::string{"replacement\n"}).Succeeded(),
        "non-canonical long bracket opening stays lossless and read-only");
}

void TestLegacyOpaqueBlockStaysReadOnly() {
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    Check(native.value && native.value->AddOriginalObject("hfoo").Succeeded(), "legacy block fixture created");
    if (!native.value) return;
    Check(native.value->AddModification({ObjectType::Unit, "hfoo"}, {"S001"},
        NativeValueType::String, std::string{"Original\r\ntext\n"}).Succeeded(), "legacy native text created");
    NativeObjectCodec nativeCodec(ObjectType::Unit);
    const auto baseline = nativeCodec.Serialize(*native.value).value;
    const std::string legacyText = "[hfoo]\nS001 = {\n\"legacy\",\n}\n";
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(legacyText.data()), legacyText.size()));
    auto* lni = dynamic_cast<LniObjectDocument*>(parsed.document.get());
    const auto* record = lni ? lni->FindObject({ObjectType::Unit, "hfoo"}) : nullptr;
    Check(lni && record && record->modifications.size() == 1 && record->modifications[0].valueWasOpaque,
        "legacy table block remains opaque");
    if (!lni) return;
    Check(!lni->SetModification({ObjectType::Unit, "hfoo"}, {"S001"}, std::string{"Changed\n"}).Succeeded(),
        "legacy block cannot be edited through ambiguous line semantics");
    Check(ApplyLniChangesToNative(*native.value, *lni).Succeeded() &&
        nativeCodec.Serialize(*native.value).value == baseline,
        "legacy block no-op preserves original native bytes");

    const std::string malformed = "[hfoo]\nS001 = \"bad\\q\" -- FORGE_MULTILINE_ESCAPED_V1\n";
    auto unsafe = LniObjectCodec(ObjectType::Unit).Parse(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(malformed.data()), malformed.size()));
    const auto* unsafeLni = dynamic_cast<const LniObjectDocument*>(unsafe.document.get());
    Check(unsafeLni && !unsafeLni->IsStructurallySafe() &&
        !ApplyLniChangesToNative(*native.value, *unsafeLni).Succeeded() &&
        nativeCodec.Serialize(*native.value).value == baseline,
        "malformed marked multiline field rejects native write without changing baseline");

    const std::string unknownMarker = "[hfoo]\nS001 = \"some\\ntext\" -- FORGE_MULTILINE_ESCAPED_V2\n";
    auto unknown = LniObjectCodec(ObjectType::Unit).Parse(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(unknownMarker.data()), unknownMarker.size()));
    const auto* unknownLni = dynamic_cast<const LniObjectDocument*>(unknown.document.get());
    Check(unknownLni && !unknownLni->IsStructurallySafe(),
        "unknown multiline marker version does not silently reinterpret text");
}

void TestCloneCustomObjectAcrossSevenTypes() {
    for (const auto type : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        NativeObjectCodec codec(type);
        auto document = ParseOwned(codec, BuildCloneFixture(type, false));
        if (!document) continue;
        const ObjectKey source{type, "H001"}, clone{type, "H002"};
        Check(document->CloneCustomObject(source, clone.rawcode).Succeeded(), "native custom clone succeeds");
        const auto encoded = codec.Serialize(*document);
        Check(encoded.Succeeded() && encoded.value == std::optional{BuildCloneFixture(type, true)},
            "native clone copies complete wire values, levels, data, variation and source/zero/parent end markers");
        if (!encoded.value) continue;
        auto reopened = ParseOwned(codec, *encoded.value);
        const auto* copied = reopened ? reopened->FindObject(clone) : nullptr;
        const auto* original = reopened ? reopened->FindObject(source) : nullptr;
        Check(copied && original && copied->parentRawcode == "hfoo"
            && copied->modifications.size() == 4 && original->modifications.size() == 4,
            "native clone reopens with all source fields and original remains unchanged");
        if (copied && original) {
            for (std::size_t index = 0; index < 4; ++index) {
                Check(copied->modifications[index].key == original->modifications[index].key
                    && copied->modifications[index].value == original->modifications[index].value,
                    "native cloned field identity and value match source");
            }
        }
        const auto baseline = codec.Serialize(*document).value;
        Check(!document->CloneCustomObject(source, clone.rawcode).Succeeded()
            && !document->CloneCustomObject(source, "hfoo").Succeeded()
            && !document->CloneCustomObject(source, "bad").Succeeded()
            && !document->CloneCustomObject({type, "MISS"}, "H004").Succeeded(),
            "native clone rejects duplicate, self-parent, invalid ID and missing source");
        Check(codec.Serialize(*document).value == baseline, "native failed clone leaves bytes unchanged");
        Check(document->AddOriginalObject("H003").Succeeded(), "native original override added for clone boundary");
        const auto withOverride = codec.Serialize(*document).value;
        Check(!document->CloneCustomObject({type, "H003"}, "H004").Succeeded(),
            "native original override cannot become a custom clone");
        Check(codec.Serialize(*document).value == withOverride, "native rejected original clone leaves bytes unchanged");

        Check(document->SetModification(source, FixtureKey(type, "I001", 4, 7), std::int32_t{99}).Succeeded(),
            "native source field edited before second clone");
        Check(document->AddModification(source, FixtureKey(type, "X001", 8, 11), NativeValueType::String,
            std::string{"new field"}).Succeeded(), "native source gains new field before second clone");
        Check(document->CloneCustomObject(source, "H004").Succeeded(), "native edited custom clone succeeds");
        const auto editedBytes = codec.Serialize(*document);
        auto edited = editedBytes.value ? ParseOwned(codec, *editedBytes.value) : nullptr;
        const auto* editedClone = edited ? edited->FindObject({type, "H004"}) : nullptr;
        Check(editedClone && editedClone->modifications.size() == 5
            && editedClone->modifications.front().value == Value{std::int32_t{99}}
            && editedClone->modifications.back().value == Value{std::string{"new field"}},
            "native clone includes edited and newly added fields");
    }
}

void TestLniClonePreservesOpaqueSection() {
    const std::string text = "; retained preamble\r\n"
        "[H001] ; custom note\r\n"
        "_parent = \"hfoo\"\r\n"
        "name = \"Original\"\r\n"
        "opaque[2,3] = {\r\n"
        "\"first\",\r\n"
        "\"second\",\r\n"
        "}\r\n"
        "[hfoo]\r\n"
        "name = \"standard override\"\r\n";
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    LniObjectCodec codec(ObjectType::Unit);
    auto parsed = codec.Parse(std::span<const std::byte>(data, text.size()));
    Check(parsed.Succeeded(), "LNI opaque clone fixture parses");
    auto* document = dynamic_cast<LniObjectDocument*>(parsed.document.get());
    if (!document) return;
    const ObjectKey source{ObjectType::Unit, "H001"};
    Check(document->CloneCustomObject(source, "H002").Succeeded(), "LNI custom clone succeeds");
    const auto encoded = codec.Serialize(*document);
    const std::string rendered = encoded.value
        ? std::string(reinterpret_cast<const char*>(encoded.value->data()), encoded.value->size())
        : std::string{};
    Check(encoded.Succeeded() && rendered.starts_with(text), "LNI clone leaves complete source prefix unchanged");
    Check(rendered.find("[H002] ; custom note\r\n_parent = \"hfoo\"\r\nname = \"Original\"\r\n"
        "opaque[2,3] = {\r\n\"first\",\r\n\"second\",\r\n}\r\n") != std::string::npos,
        "LNI clone copies entire section including opaque continuation, comments and CRLF");
    auto reopened = encoded.value ? codec.Parse(*encoded.value) : CodecParseResult{};
    const auto* roundtrip = dynamic_cast<const LniObjectDocument*>(reopened.document.get());
    const auto* cloned = roundtrip ? roundtrip->FindObject({ObjectType::Unit, "H002"}) : nullptr;
    Check(reopened.Succeeded() && cloned && cloned->parentRawcode == "hfoo"
        && cloned->modifications.size() == 2 && cloned->modifications[1].valueWasOpaque,
        "LNI cloned opaque field remains present and read-only after reopen");
    const auto baseline = codec.Serialize(*document).value;
    Check(!document->CloneCustomObject(source, "H002").Succeeded()
        && !document->CloneCustomObject(source, "hfoo").Succeeded()
        && !document->CloneCustomObject({ObjectType::Unit, "hfoo"}, "H003").Succeeded()
        && !document->CloneCustomObject(source, "bad").Succeeded(),
        "LNI clone rejects duplicate, self-parent, original override and invalid ID");
    Check(codec.Serialize(*document).value == baseline, "LNI failed clone preserves complete bytes");

    const std::string malformed = "[H001]\n_parent = \"hfoo\"\nunknown structure\n";
    auto unsafe = codec.Parse(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(malformed.data()), malformed.size()));
    auto* unsafeDocument = dynamic_cast<LniObjectDocument*>(unsafe.document.get());
    Check(unsafeDocument && !unsafeDocument->CloneCustomObject(source, "H002").Succeeded(),
        "LNI clone refuses unknown section boundary rather than losing text");
}

} // namespace

int main(int argc, char** argv) {
    TestSevenTypesRoundTrip();
    TestSingleFieldEdit();
    TestStringEditPreservesNeighbors();
    TestFailClosedInputs();
    TestAmbiguousIdentityIsReadOnly();
    TestClassicV1ExistingValueEdits();
    TestClassicV1DuplicateIdentityDenied();
    TestVersion3ReadOnlyAcrossSevenTypes();
    TestIndependentSkinMembersStayReadOnly();
    TestNativeToLniConversion();
    TestDoodadVariationLniRoundTrip();
    TestLniWriteBackToNative();
    TestResetFieldWriteBack();
    TestSchemaPreservesNewUnrealType();
    TestLniCloneKeepsActualWireTypes();
    TestLniCloneOfPendingCustomSource();
    TestNativeWireByteSizes();
    TestNativeCloneProjectionBudget();
    TestClassicV1LniTransactions();
    TestDoodadV1LniIndexIdentity();
    TestCreateEmptyAndOriginalOverrides();
    TestApplyNewObjectsAndUndoAcrossSevenTypes();
    TestTransactionalApplyFailure();
    TestMultilineFieldsKeepExactNativeIdentity();
    TestLniLongBracketInteroperability();
    TestLegacyOpaqueBlockStaysReadOnly();
    TestCloneCustomObjectAcrossSevenTypes();
    TestLniClonePreservesOpaqueSection();
    if (argc == 2) {
        TestOptionalGoldenRoot(std::filesystem::path(argv[1]));
    }
    if (argc == 3 && std::string_view(argv[1]) == "--writeback") {
        TestOptionalWriteBack(std::filesystem::path(argv[2]));
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " native object-data test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "All native object-data tests passed.\n";
    return EXIT_SUCCESS;
}
