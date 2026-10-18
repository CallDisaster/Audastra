#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using War3Map::Resources::CanonicalImportPath;
using War3Map::Resources::ImportRecord;
using War3Map::Resources::ImportTable;
using War3Map::Resources::ResolveArchivePath;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::vector<std::byte> Fixture(const std::vector<ImportRecord>& entries,
    const std::vector<std::byte>& tail = {}) {
    std::vector<std::byte> bytes;
    auto u32 = [&](std::uint32_t value) {
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        }
    };
    u32(1);
    u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        bytes.push_back(static_cast<std::byte>(entry.flags));
        for (const auto character : entry.path) bytes.push_back(static_cast<std::byte>(character));
        bytes.push_back(std::byte{0});
    }
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
}

void TestClassicFlags() {
    for (const auto flags : {5u, 8u}) {
        const auto resolved = ResolveArchivePath({static_cast<std::uint8_t>(flags), "Models/Test.mdx"});
        Require(resolved.Succeeded() && *resolved.value == "war3mapImported\\Models\\Test.mdx",
            "standard flags must add the standard directory");
    }
    for (const auto flags : {10u, 13u}) {
        const auto resolved = ResolveArchivePath({static_cast<std::uint8_t>(flags), "Models/Test.mdx"});
        Require(resolved.Succeeded() && *resolved.value == "Models\\Test.mdx",
            "custom flags must preserve the full archive path");
    }
    auto table = ImportTable::Parse(Fixture({{5, "Test.mdx"}, {13, "Textures/Map.blp"}}));
    Require(table.Succeeded(), "classic fixture must parse");
    Require(table.value->Upsert("WAR3MAPIMPORTED/test.MDX").Succeeded(), "standard canonical match failed");
    Require(table.value->Upsert("textures\\MAP.BLP").Succeeded(), "custom canonical match failed");
    Require(!table.value->IsModified() && table.value->Entries().size() == 2,
        "canonical upsert must not rewrite classic flags or add duplicates");
}

void TestLosslessAndAppend() {
    const std::vector<std::byte> tail{std::byte{0xaa}, std::byte{0}, std::byte{0x7f}};
    const std::vector<ImportRecord> entries{{5, "One.mdx"}, {10, "UI/ICON.blp"}, {13, "ui\\icon.BLP"}};
    const auto original = Fixture(entries, tail);
    auto parsed = ImportTable::Parse(original);
    Require(parsed.Succeeded(), "lossless fixture must parse");
    Require(parsed.value->OriginalBytes() == original, "original bytes must be retained");
    auto bytes = parsed.value->Serialize();
    Require(bytes.Succeeded() && *bytes.value == original, "no-op must be byte-identical");
    Require(parsed.value->Upsert("UI/icon.blp").Succeeded(), "existing duplicate is a valid no-op");
    Require(parsed.value->Entries() == entries, "old duplicate entries and original spelling must remain");
    Require(!parsed.value->IsModified(), "no-op must not dirty the document");
    Require(parsed.value->Upsert("Sounds/New.wav").Succeeded(), "new custom import must register");
    Require(parsed.value->IsModified(), "new import must dirty the document");
    Require(parsed.value->Entries().back() == ImportRecord{13, "Sounds\\New.wav"},
        "new custom paths must use classic flag 13 and full path");
    bytes = parsed.value->Serialize();
    auto expected = entries;
    expected.push_back({13, "Sounds\\New.wav"});
    Require(bytes.Succeeded() && *bytes.value == Fixture(expected, tail),
        "append must preserve old flags, paths, order and extension tail");
    const auto reopened = ImportTable::Parse(*bytes.value);
    Require(reopened.Succeeded() && reopened.value->Entries() == expected,
        "modified import table must reopen with the new entry");
    Require(parsed.value->OriginalBytes() == original, "editing must not replace the source baseline");
}

void TestCreationAndFailureAtomicity() {
    auto table = ImportTable::CreateEmpty();
    Require(table.OriginalBytes() == Fixture({}), "empty table must be an explicit v1 header");
    Require(table.Upsert("Art/Portrait.blp").Succeeded(), "first insert must succeed");
    const auto before = table.Serialize();
    const std::vector<std::string> invalid{
        "", "../x", "Art/../x", "/root/x", "\\\\server\\x", "C:\\x", "a:b",
        "a//b", "a\\", "a/./b", "x. ", "x.", "nul.txt", "A/COM1.blp", "A/LPT9.bin",
        "x?", "x*", "x|", "x\"", "x<", "x>", std::string("a\0b", 3), "a\nb",
    };
    for (const auto& path : invalid) {
        Require(!table.Upsert(path).Succeeded(), "unsafe path must fail");
        const auto after = table.Serialize();
        Require(after.Succeeded() && *after.value == *before.value,
            "failed insertion must not mutate an already edited table");
    }
    const auto canonical = CanonicalImportPath("Models/Test Model.mdx");
    Require(canonical.Succeeded() && *canonical.value == "MODELS\\TEST MODEL.MDX",
        "path comparison must retain allowed spaces and normalize slash/case");
}

void TestMalformedAndUnknownVersion() {
    for (std::size_t count = 0; count < 8; ++count) {
        Require(!ImportTable::Parse(std::vector<std::byte>(count)).Succeeded(),
            "short header must fail instead of constructing an empty table");
    }
    auto bytes = Fixture({});
    bytes[0] = std::byte{2};
    Require(!ImportTable::Parse(bytes).Succeeded(), "unknown version must fail");
    bytes = Fixture({});
    for (std::size_t i = 4; i < 8; ++i) bytes[i] = std::byte{0xff};
    Require(!ImportTable::Parse(bytes).Succeeded(), "unbounded count must fail before allocation");
    bytes = Fixture({{13, "truncated.blp"}});
    bytes.pop_back();
    Require(!ImportTable::Parse(bytes).Succeeded(), "unterminated path must fail");
    bytes = Fixture({{13, "first.blp"}});
    bytes[4] = std::byte{2};
    Require(!ImportTable::Parse(bytes).Succeeded(), "missing second entry must fail");
}

void TestUnrecognizedAndArchiveDisambiguation() {
    const auto original = Fixture({{99, "legacy.blp"}});
    auto table = ImportTable::Parse(original);
    Require(table.Succeeded(), "unknown flag with known boundary must be readable");
    Require(table.value->Serialize().value == original, "unknown flag must roundtrip without edits");
    Require(!table.value->Upsert("new.blp").Succeeded(), "unknown path semantics must block mutation");
    Require(!table.value->IsModified() && table.value->OriginalBytes() == original,
        "unknown record rejection must be atomic");
    const auto directOnly = [](std::string_view path) { return path == "legacy.blp"; };
    Require(table.value->Upsert("LEGACY.BLP", directOnly).Succeeded(),
        "unique actual archive path must disambiguate unknown flag");
    Require(!table.value->IsModified(), "disambiguation must not rewrite original flag");
    Require(table.value->Upsert("new.blp", directOnly).Succeeded(), "proven unknown record permits new import");
    Require(table.value->Entries().front().flags == 99, "unknown flags must remain untouched");

    ImportRecord prefixedStandard{8, "war3mapImported\\Asset.blp"};
    Require(!ResolveArchivePath(prefixedStandard).Succeeded(), "prefixed standard spelling needs archive evidence");
    const auto resolved = ResolveArchivePath(prefixedStandard,
        [](std::string_view path) { return path == "war3mapImported\\Asset.blp"; });
    Require(resolved.Succeeded() && *resolved.value == prefixedStandard.path,
        "actual archive may prove a full path stored with standard flags");
    Require(!ResolveArchivePath(prefixedStandard, [](std::string_view) { return true; }).Succeeded(),
        "two actual candidates must remain ambiguous");

    // 悬空记录：归档证明两种可能路径都不存在时，不同路径的新导入可以追加，记录原样保留；
    // 与其可能路径相同的导入仍然拒绝。
    const auto dangling = Fixture({{0, "[effe][0058].mdx"}, {13, "Keep.blp"}});
    auto withDangling = ImportTable::Parse(dangling);
    Require(withDangling.Succeeded(), "dangling fixture must parse");
    const auto noDanglingMember = [](std::string_view path) { return path == "Keep.blp"; };
    Require(!withDangling.value->Upsert("new.blp").Succeeded(), "dangling record without archive evidence must still block");
    Require(withDangling.value->Upsert("ReplaceableTextures/new.blp", noDanglingMember).Succeeded(),
        "proven dangling record must not block unrelated imports");
    Require(withDangling.value->Entries().front() == ImportRecord{0, "[effe][0058].mdx"}, "dangling record must stay verbatim");
    // 补回悬空记录指向的成员：保存后只有标准前缀路径存在，记录唯一指向本次导入，原样沿用、不追加。
    {
        auto restored = ImportTable::Parse(dangling);
        Require(restored.value->Upsert("war3mapImported/[effe][0058].mdx", noDanglingMember).Succeeded(),
            "restoring the member a dangling record names must resolve that record");
        Require(!restored.value->IsModified() && restored.value->Serialize().value == dangling,
            "restored dangling record must stay verbatim without a duplicate entry");
        Require(!restored.value->Upsert("war3mapImported/[effe][0058].mdx").Succeeded(),
            "without archive evidence the dangling record must still block its own paths");
        auto direct = ImportTable::Parse(dangling);
        Require(direct.value->Upsert("[effe][0058].mdx", noDanglingMember).Succeeded()
            && !direct.value->IsModified() && direct.value->Serialize().value == dangling,
            "restoring the unprefixed candidate also resolves the record verbatim");
        const auto prefixedOnly = [](std::string_view path) { return path == "war3mapImported\\Darion.blp"; };
        auto standard = ImportTable::Parse(Fixture({{5, "Darion.blp"}}));
        Require(standard.value->Upsert("Darion.blp", prefixedOnly).Succeeded() && standard.value->Entries().size() == 2,
            "standard records keep resolving from the real archive, a root-level file of the same name still appends");
    }
    Require(withDangling.value->Remove("Keep.blp", noDanglingMember).Succeeded() && withDangling.value->Entries().size() == 2,
        "removal next to a dangling record must succeed");

    const auto nonAscii = Fixture({{13, std::string("icon-") + static_cast<char>(0x80) + ".blp"}});
    auto legacy = ImportTable::Parse(nonAscii);
    Require(legacy.Succeeded() && legacy.value->Serialize().value == nonAscii,
        "unresolved legacy path bytes must still roundtrip");
    Require(!legacy.value->Upsert("new.blp").Succeeded(), "unsupported legacy encoding must not be guessed");
}

void TestUnrelatedAmbiguousRecordsPreserveBytesAndRelatedRefusals() {
    const std::vector<std::byte> tail{std::byte{0xde}, std::byte{0xad}, std::byte{0x7f}};
    const auto archiveEvidence = [](std::string_view path) {
        const auto canonical = CanonicalImportPath(path);
        return canonical.Succeeded() && (*canonical.value == "DARION.BLP" || *canonical.value == "WAR3MAPIMPORTED\\DARION.BLP"
            || *canonical.value == "KEEP.BLP");
    };
    for (const std::uint8_t flag : {std::uint8_t{5}, std::uint8_t{8}, std::uint8_t{99}}) {
        const std::vector<ImportRecord> records{{13, "Darion.blp"}, {flag, "Darion.blp"}, {13, "Keep.blp"}};
        const auto original = Fixture(records, tail);
        Require(HasCode(ResolveArchivePath(records[1], archiveEvidence), "IMPORT_PATH_AMBIGUOUS"),
            "both real candidate members remain ambiguous; unrelated mutation never chooses one");
        auto parsed = ImportTable::Parse(original); Require(parsed.Succeeded(), "complete ambiguous fixture reads without rewriting");
        Require(parsed.value->Upsert("KEEP.BLP", archiveEvidence).Succeeded() && !parsed.value->IsModified()
            && parsed.value->Serialize().value == original, "unrelated existing import is a complete-byte no-op despite ambiguous siblings");
        Require(parsed.value->Remove("Absent.blp", archiveEvidence).Succeeded() && !parsed.value->IsModified()
            && parsed.value->Entries() == records && parsed.value->OriginalBytes() == original
            && parsed.value->Serialize().value == original, "unrelated missing removal preserves all record identities and complete original bytes");
        Require(parsed.value->Upsert("ReplaceableTextures/CommandButtonsDisabled/DISBTNPortrait.blp", archiveEvidence).Succeeded(),
            "unrelated disabled-icon import is not blocked by Darion dual paths");
        auto appended = records; appended.push_back({13, "ReplaceableTextures\\CommandButtonsDisabled\\DISBTNPortrait.blp"});
        Require(parsed.value->Entries() == appended && parsed.value->OriginalBytes() == original
            && parsed.value->Serialize().value == Fixture(appended, tail), "append preserves original flags, paths, order and opaque tail exactly");
        parsed = ImportTable::Parse(original);
        Require(parsed.value->Remove("keep.blp", archiveEvidence).Succeeded()
            && parsed.value->Entries() == std::vector<ImportRecord>{records[0], records[1]}
            && parsed.value->OriginalBytes() == original && parsed.value->Serialize().value == Fixture({records[0], records[1]}, tail),
            "unrelated removal retains both Darion records and exact opaque tail");
        for (const auto* target : {"dArIoN.BLP", "WAR3MAPIMPORTED/Darion.blp"}) {
            parsed = ImportTable::Parse(original);
            Require(HasCode(parsed.value->Upsert(target, archiveEvidence), "IMPORT_PATH_AMBIGUOUS")
                && !parsed.value->IsModified() && parsed.value->Entries() == records && parsed.value->Serialize().value == original,
                "related direct or prefixed upsert still refuses atomically after earlier matching records");
            Require(HasCode(parsed.value->Remove(target, archiveEvidence), "IMPORT_PATH_AMBIGUOUS")
                && !parsed.value->IsModified() && parsed.value->Entries() == records && parsed.value->Serialize().value == original,
                "related direct or prefixed removal still refuses atomically with no partial record deletion");
        }
    }
    const auto unproven = Fixture({{8, "war3mapImported\\Darion.blp"}}, tail);
    auto parsed = ImportTable::Parse(unproven); Require(parsed.Succeeded(), "unproven prefixed-standard fixture reads");
    Require(HasCode(parsed.value->Upsert("New.blp"), "IMPORT_PATH_AMBIGUOUS")
        && HasCode(parsed.value->Remove("Absent.blp"), "IMPORT_PATH_AMBIGUOUS") && !parsed.value->IsModified()
        && parsed.value->Serialize().value == unproven, "no archive evidence cannot bypass even an unrelated ambiguous record");
    const auto unsafe = Fixture({{8, "../Darion.blp"}}, tail);
    parsed = ImportTable::Parse(unsafe); Require(parsed.Succeeded(), "unsafe path remains parseable for byte preservation");
    Require(HasCode(parsed.value->Upsert("New.blp", archiveEvidence), "IMPORT_PATH_UNSAFE")
        && HasCode(parsed.value->Remove("Absent.blp", archiveEvidence), "IMPORT_PATH_UNSAFE") && !parsed.value->IsModified()
        && parsed.value->Serialize().value == unsafe, "other diagnostics never gain an unrelated-target bypass");
}

} // namespace

void TestEditorManagedMemberPaths() {
    for (const auto* managed : {"war3map.j", "WAR3MAP.W3U", "war3mapUnits.doo", "war3mapSkin.txt", "war3map.shd",
            "war3campaign.w3f", "(listfile)", "Scripts\\war3map.j", "scripts/WAR3MAP.LUA"})
        Require(War3Map::Resources::IsEditorManagedMemberPath(managed), "map data members are editor managed");
    for (const auto* asset : {"war3mapImported\\hero.mdx", "Textures\\war3map.blp", "Scripts\\main.j", "main.lua", "readme.txt"})
        Require(!War3Map::Resources::IsEditorManagedMemberPath(asset), "ordinary imports are not editor managed");
}

int main() {
    try {
        TestClassicFlags();
        TestEditorManagedMemberPaths();
        TestLosslessAndAppend();
        TestCreationAndFailureAtomicity();
        TestMalformedAndUnknownVersion();
        TestUnrecognizedAndArchiveDisambiguation();
        TestUnrelatedAmbiguousRecordsPreserveBytesAndRelatedRefusals();
        std::cout << "ImportTable: 6 scenario groups passed (with dangling and unrelated-ambiguity cases)\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ImportTable: " << exception.what() << '\n';
        return 1;
    }
}
