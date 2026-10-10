#include "War3MapCore/ObjectData/LniObjectCodec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
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

[[nodiscard]] std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result(text.size());
    std::transform(text.begin(), text.end(), result.begin(), [](unsigned char value) {
        return static_cast<std::byte>(value);
    });
    return result;
}

[[nodiscard]] std::string Text(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
}

[[nodiscard]] std::unique_ptr<LniObjectDocument> ParseOwned(
    const LniObjectCodec& codec,
    std::string_view source,
    bool expectSuccess = true) {
    const std::vector<std::byte> bytes = Bytes(source);
    CodecParseResult parsed = codec.Parse(bytes);
    Check(parsed.Succeeded() == expectSuccess, "parse success state");
    auto* raw = dynamic_cast<LniObjectDocument*>(parsed.document.release());
    Check(raw != nullptr, "parsed document type");
    return std::unique_ptr<LniObjectDocument>(raw);
}

void TestSevenObjectTypes() {
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
        "unit.ini",
        "ability.ini",
        "item.ini",
        "buff.ini",
        "upgrade.ini",
        "doodad.ini",
        "destructable.ini",
    };

    for (std::size_t index = 0; index < types.size(); ++index) {
        Check(LniFileName(types[index]) == names[index], "LNI filename mapping");
        Check(ObjectTypeFromLniFileName(names[index]) == types[index], "reverse mapping");
        Check(ObjectTypeFromLniFileName(std::string(names[index])) == types[index], "owned reverse mapping");

        LniObjectCodec codec(types[index]);
        const std::string rawcode = "X" + std::to_string(index) + "A0";
        const std::string fixture = '[' + rawcode + "]\nname = \"Synthetic\"\n";
        auto document = ParseOwned(codec, fixture);
        Check(document != nullptr && document->Objects().size() == 1, "one synthetic object");
        Check(document != nullptr && document->Type() == types[index], "document object type");
        if (document != nullptr) {
            const auto serialized = codec.Serialize(*document);
            Check(serialized.Succeeded(), "seven-type serialization");
            if (serialized.value.has_value()) {
                Check(Text(*serialized.value) == fixture, "seven-type byte roundtrip");
            }
        }
    }
}

void TestRoundTripAndSemanticEdit() {
    const std::string fixture =
        "\xEF\xBB\xBF// synthetic, redistributable fixture\r\n"
        "[H000] ; first object\r\n"
        "_parent = \"H001\"\r\n"
        "name = \"Synthetic Unit\" ; keep this comment\r\n"
        "hitPoints = 420\r\n"
        "moveSpeed = 2.5\r\n"
        "enabled = true\r\n"
        "unknownPayload = { alpha = 1, beta = \"x\" }\r\n"
        "indexed[2,1,3] = 9\r\n"
        "\r\n"
        "[H002]\r\n"
        "name = \"Second\"\r\n";

    LniObjectCodec codec(ObjectType::Unit);
    auto original = ParseOwned(codec, fixture);
    auto edited = ParseOwned(codec, fixture);
    Check(original != nullptr && edited != nullptr, "roundtrip documents");
    if (original == nullptr || edited == nullptr) {
        return;
    }

    Check(original->Objects().size() == 2, "object count");
    const ObjectRecord& first = original->Objects().front();
    Check(first.parentRawcode == "H001", "parent rawcode");
    Check(first.modifications.size() == 6, "modification count");
    const auto opaque = std::find_if(
        first.modifications.begin(),
        first.modifications.end(),
        [](const ObjectModification& modification) {
            return modification.key.fieldId == "unknownPayload";
        });
    Check(opaque != first.modifications.end() && opaque->valueWasOpaque, "opaque framed value");
    const auto indexed = std::find_if(
        first.modifications.begin(),
        first.modifications.end(),
        [](const ObjectModification& modification) {
            return modification.key.fieldId == "indexed";
        });
    Check(indexed != first.modifications.end(), "indexed field found");
    if (indexed != first.modifications.end()) {
        Check(indexed->key.level == std::uint32_t{2}, "indexed field level");
        Check(indexed->key.dataIndex == std::uint32_t{1}, "indexed field data index");
        Check(indexed->key.variation == std::uint32_t{3}, "indexed field variation");
    }

    const auto originalBytes = codec.Serialize(*original);
    Check(originalBytes.Succeeded(), "unchanged serialize succeeds");
    if (originalBytes.value.has_value()) {
        Check(Text(*originalBytes.value) == fixture, "BOM/CRLF/comment exact roundtrip");
    }

    const ObjectKey key{ObjectType::Unit, "H000"};
    Status changed = edited->SetModification(
        key,
        ModificationKey{"name", std::nullopt, std::nullopt, std::nullopt},
        LocalizedStringValue{"Updated Unit"});
    Check(changed.Succeeded(), "change existing field");
    changed = edited->SetModification(
        key,
        ModificationKey{"armor", std::nullopt, std::nullopt, std::nullopt},
        std::int32_t{7});
    Check(changed.Succeeded(), "add field");

    const auto editedBytes = codec.Serialize(*edited);
    Check(editedBytes.Succeeded(), "edited serialize succeeds");
    if (editedBytes.value.has_value()) {
        const std::string output = Text(*editedBytes.value);
        Check(
            output.find("name = \"Updated Unit\" ; keep this comment") != std::string::npos,
            "inline comment preserved after edit");
        Check(
            output.find("unknownPayload = { alpha = 1, beta = \"x\" }") != std::string::npos,
            "unknown framed value preserved");
        const std::size_t armor = output.find("armor = 7");
        const std::size_t nextSection = output.find("[H002]");
        Check(armor != std::string::npos && armor < nextSection, "new field stays in object section");
    }

    const SemanticDiff diff = codec.Diff(*original, *edited);
    Check(diff.changes.size() == 2, "semantic diff reports edit and add");
}

void TestFailClosedParsing() {
    LniObjectCodec codec(ObjectType::Ability);
    auto unknown = ParseOwned(codec, "[A000]\nnot an assignment\n", false);
    Check(unknown != nullptr && !unknown->IsStructurallySafe(), "unknown structure unsafe");
    if (unknown != nullptr) {
        Check(!codec.Serialize(*unknown).Succeeded(), "unsafe structure cannot serialize");
        Check(
            !unknown->SetModification(
                ObjectKey{ObjectType::Ability, "A000"},
                ModificationKey{"name", std::nullopt, std::nullopt, std::nullopt},
                std::string{"No"}).Succeeded(),
            "unsafe structure cannot edit");
    }

    auto duplicate = ParseOwned(codec, "[A000]\nname = \"a\"\nname = \"b\"\n", false);
    Check(duplicate != nullptr && !duplicate->IsStructurallySafe(), "duplicate field unsafe");

    const std::array<std::byte, 4> utf16{
        std::byte{0xff}, std::byte{0xfe}, std::byte{'['}, std::byte{0x00}};
    const CodecParseResult utf16Result = codec.Parse(utf16);
    Check(!utf16Result.Succeeded(), "UTF-16 fails closed");
}

void TestMultilineValuesAreFramedReadOnly() {
    const std::string fixture =
        "[A000]\r\n"
        "_parent = \"AHbz\"\r\n"
        "-- LNI table value\r\n"
        "Tip = {\r\n"
        "\"Level 1\",\r\n"
        "\"Level 2\",\r\n"
        "}\r\n"
        "Ubertip = [=[\r\n"
        "line one\r\n"
        "line two]=]\r\n"
        "levels = 2\r\n";

    LniObjectCodec codec(ObjectType::Ability);
    auto document = ParseOwned(codec, fixture);
    Check(document != nullptr && document->IsStructurallySafe(), "multiline LNI values framed");
    if (document == nullptr) {
        return;
    }
    const auto serialized = codec.Serialize(*document);
    Check(serialized.Succeeded(), "multiline LNI serializes");
    if (serialized.value.has_value()) {
        Check(Text(*serialized.value) == fixture, "multiline LNI byte roundtrip");
    }
    Check(
        !document->SetModification(
            ObjectKey{ObjectType::Ability, "A000"},
            ModificationKey{"Tip", std::nullopt, std::nullopt, std::nullopt},
            std::string{"unsafe replacement"}).Succeeded(),
        "opaque table field remains read-only");
    Check(
        document->SetModification(
            ObjectKey{ObjectType::Ability, "A000"},
            ModificationKey{"levels", std::nullopt, std::nullopt, std::nullopt},
            std::int32_t{3}).Succeeded(),
        "scalar field beside multiline value remains editable");
}

void TestDeletionAuditGate() {
    const std::string fixture =
        "[H100]\nname = \"Delete Candidate\"\n"
        "[H101]\nname = \"Keep\"\n";
    LniObjectCodec codec(ObjectType::Unit);
    auto document = ParseOwned(codec, fixture);
    Check(document != nullptr, "deletion document");
    if (document == nullptr) {
        return;
    }

    const ObjectKey subject{ObjectType::Unit, "H100"};
    ReferenceAuditReport report;
    report.subject = subject;
    report.documentFingerprint = document->DocumentFingerprint();
    report.inspectedSources = ReferenceSourceBit(ReferenceSource::ObjectData);
    Check(
        !ReferenceAuditAuthority::IssueDeletionToken(report, true).Succeeded(),
        "partial static audit cannot authorize deletion");

    report.inspectedSources = RequiredReferenceSources();
    report.references.push_back(ReferenceEvidence{
        ReferenceSource::Trigger,
        "synthetic-trigger",
        "synthetic blocking reference",
        true,
    });
    Check(
        !ReferenceAuditAuthority::IssueDeletionToken(report, true).Succeeded(),
        "blocking reference prevents deletion");
    report.references.clear();
    Check(
        !ReferenceAuditAuthority::IssueDeletionToken(report, false).Succeeded(),
        "explicit approval required");

    auto tokenResult = ReferenceAuditAuthority::IssueDeletionToken(report, true);
    Check(tokenResult.Succeeded(), "complete audit issues token");
    if (!tokenResult.value.has_value()) {
        return;
    }

    Check(
        document->SetModification(
            subject,
            ModificationKey{"name", std::nullopt, std::nullopt, std::nullopt},
            std::string{"Changed"}).Succeeded(),
        "mutation before stale-token test");
    Check(
        !document->DeleteObject(subject, *tokenResult.value).Succeeded(),
        "token is invalid after document changes");

    report.documentFingerprint = document->DocumentFingerprint();
    tokenResult = ReferenceAuditAuthority::IssueDeletionToken(report, true);
    Check(tokenResult.Succeeded(), "fresh audit token");
    if (tokenResult.value.has_value()) {
        Check(document->DeleteObject(subject, *tokenResult.value).Succeeded(), "audited deletion");
        Check(document->FindObject(subject) == nullptr, "deleted object absent");
        const auto serialized = codec.Serialize(*document);
        Check(serialized.Succeeded(), "serialize after deletion");
        if (serialized.value.has_value()) {
            const std::string output = Text(*serialized.value);
            Check(output.find("[H100]") == std::string::npos, "deleted section removed");
            Check(output.find("[H101]") != std::string::npos, "following section retained");
        }
    }
}

} // namespace

int main() {
    TestSevenObjectTypes();
    TestRoundTripAndSemanticEdit();
    TestFailClosedParsing();
    TestMultilineValuesAreFramedReadOnly();
    TestDeletionAuditGate();

    if (g_failures != 0) {
        std::cerr << g_failures << " object-data test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "All synthetic object-data tests passed.\n";
    return EXIT_SUCCESS;
}
