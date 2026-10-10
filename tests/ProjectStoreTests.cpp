#include "War3MapCore/ProjectStore.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using namespace War3Map;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
void RequireSuccess(const Result<T>& result, const std::string& context) {
    if (result.Succeeded()) return;
    std::string message = context;
    for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
    throw std::runtime_error(message);
}

template <typename T>
void RequireCode(const Result<T>& result, std::string_view code) {
    Require(!result.Succeeded(), "expected rejected save");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [code](const auto& diagnostic) {
        return diagnostic.code == code;
    }), "missing diagnostic " + std::string(code));
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path() / ("Forge-project-store-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
            + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directories(path_), "create original test fixture directory");
    }
    ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    ~FileHandle() { if (Valid()) CloseHandle(handle_); }
    bool Valid() const { return handle_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE handle_;
};

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

std::string Hash(const std::filesystem::path& path) {
    const auto result = Sha256File(path);
    RequireSuccess(result, "hash fixture");
    return *result.value;
}

std::vector<MapArchiveMember> Project(std::string_view revision = "first") {
    return {{"project.json", Bytes(R"({"format":"forge-project","version":1,"sourceKind":"directory","recovery":false,"edits":[{"label":"修改名称"}]})")},
        {"baseline/workspace/map/war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")},
        {"baseline/workspace/opaque.bin", {std::byte{0}, std::byte{0xFF}, std::byte{0x88}}},
        {"overlay/table/unit.ini", Bytes(revision)},
        {"definitions/TriggerData.txt", Bytes("[TriggerTypes]\nnothing=0,0,0\n")},
        {"imports/0", Bytes("authored-test-resource")}};
}

void RequireMembers(const std::filesystem::path& path, const std::vector<MapArchiveMember>& expected) {
    const auto archive = MapArchiveSession::Open(path);
    RequireSuccess(archive, "reopen saved project");
    Require(archive.value->Prefix().empty(), "project has no game-map prefix");
    Require(archive.value->Members().size() == expected.size(), "complete member count");
    for (const auto& member : expected) {
        Require(archive.value->ReadMember(member.name).value == std::optional(member.data), "round-trip member " + member.name);
    }
}

void RequireNoStaging(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().wstring();
        Require(!name.starts_with(L".forge-project-stage-") && !name.starts_with(L".forge-candidate-"), "uncommitted staging leaked");
    }
}

void TestFirstSaveAndVerifiedUpdates() {
    TempDirectory temp;
    const auto path = temp.Path() / "document.forge";
    const auto first = ProjectStore::Save(path, Project());
    RequireSuccess(first, "first independent save");
    Require(std::filesystem::equivalent(first.value->path, path), "first receipt identifies saved file");
    Require(!first.value->backupPath, "first receipt has no backup");
    Require(first.value->memberCount == Project().size() && first.value->sha256 == Hash(path), "first receipt verifies bytes");
    RequireMembers(path, Project());

    ProjectSaveOptions options;
    options.expectedTargetSha256 = first.value->sha256;
    std::transform(options.expectedTargetSha256->begin(), options.expectedTargetSha256->end(), options.expectedTargetSha256->begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const auto second = ProjectStore::Save(path, Project("second"), options);
    RequireSuccess(second, "same-file update with receipt");
    Require(second.value->backupPath.has_value(), "update returns prior snapshot path");
    Require(second.value->backupPath->filename().wstring().starts_with(L"document.forge.backup-"), "backup naming contract");
    Require(Hash(*second.value->backupPath) == first.value->sha256, "backup preserves exact previous archive bytes");
    RequireMembers(*second.value->backupPath, Project());
    RequireMembers(path, Project("second"));
    Require(second.value->sha256 == Hash(path) && second.value->sha256 != first.value->sha256, "new save receipt");

    options.expectedTargetSha256 = second.value->sha256;
    const auto third = ProjectStore::Save(path, Project("third"), options);
    RequireSuccess(third, "second update");
    Require(third.value->backupPath != second.value->backupPath, "independent backup names");
    Require(Hash(*second.value->backupPath) == first.value->sha256 && Hash(*third.value->backupPath) == second.value->sha256,
        "success never deletes earlier history");
    RequireMembers(path, Project("third"));
    RequireNoStaging(temp.Path());
}

void TestConflictAndMissingTargetNeverChangeBaseline() {
    TempDirectory temp;
    const auto path = temp.Path() / "document.forge";
    const auto first = ProjectStore::Save(path, Project());
    RequireSuccess(first, "create conflict fixture");
    Require(!ProjectStore::Save(path, Project("other")).Succeeded(), "first-save request never replaces existing project");
    ProjectSaveOptions options;
    options.expectedTargetSha256 = std::string(64, '0');
    RequireCode(ProjectStore::Save(path, Project("other"), options), "PROJECT_TARGET_CHANGED");
    Require(Hash(path) == first.value->sha256, "conflict preserves actual previous file");
    options.expectedTargetSha256 = "not-a-hash";
    RequireCode(ProjectStore::Save(path, Project(), options), "EXPECTED_SHA_INVALID");
    options.expectedTargetSha256 = first.value->sha256;
    const auto moved = temp.Path() / "moved.forge";
    std::filesystem::rename(path, moved);
    RequireCode(ProjectStore::Save(path, Project("other"), options), "PROJECT_TARGET_UNAVAILABLE");
    Require(!std::filesystem::exists(path) && Hash(moved) == first.value->sha256, "missing target does not become a first save");
    RequireNoStaging(temp.Path());
}

void TestManifestIdentityAndEnvelope() {
    TempDirectory temp;
    const std::vector<std::string> invalid{
        "not json", "{}", R"({"format":"other","version":1,"sourceKind":"directory"})",
        R"({"format":"forge-project","version":2,"sourceKind":"directory"})",
        R"({"format":"forge-project","version":"1","sourceKind":"directory"})",
        R"({"format":"forge-project","version":1,"sourceKind":"other"})",
        R"({"format":"forge-project","version":1,"sourceKind":"directory",})",
        R"({"format":"forge-project","version":1,"sourceKind":"directory"} trailing)",
        R"({"format":"forge-project","version":1,"sourceKind":"directory","\u0066ormat":"other"})",
        R"({"format":"forge-project","version":1,"sourceKind":"directory","label":"\uD800"})",
        R"({"format":"forge-project","version":1,"sourceKind":"directory","value":01})",
        R"({"format":"forge-project","version":1,"sourceKind":"directory","value":1.})",
        R"({"format":"forge-project","version":1,"sourceKind":"directory","value":1e})",
        R"({"format":"forge-project","version":1,"sourceKind":"directory","value":{"x":1,"x":2}})"
    };
    unsigned index = 0;
    for (const auto& manifest : invalid) {
        auto members = Project();
        members[0].data = Bytes(manifest);
        const auto path = temp.Path() / ("invalid-" + std::to_string(index++) + ".forge");
        RequireCode(ProjectStore::Save(path, members), "PROJECT_MANIFEST_INVALID");
        Require(!std::filesystem::exists(path), "invalid JSON never published");
    }
    auto members = Project();
    members[0].data = Bytes(R"({"format":"forge-project","version":1,"sourceKind":"directory","label":"\uD83D\uDE80","value":[null,true,false,-2.5e+3]})");
    RequireSuccess(ProjectStore::Save(temp.Path() / "valid-unicode.forge", members), "valid escaped unicode and nested JSON");

    members = Project();
    members[0].data.push_back(std::byte{0xFF});
    RequireCode(ProjectStore::Save(temp.Path() / "invalid-utf8.forge", members), "PROJECT_MANIFEST_INVALID");
    members[0].data.assign(1024 * 1024 + 1, std::byte{' '});
    RequireCode(ProjectStore::Save(temp.Path() / "large.forge", members), "PROJECT_MANIFEST_INVALID");
    members = Project();
    members[0].data = Bytes("{\"format\":\"forge-project\",\"version\":1,\"sourceKind\":\"directory\",\"value\":"
        + std::string(70, '[') + "0" + std::string(70, ']') + "}");
    RequireCode(ProjectStore::Save(temp.Path() / "deep.forge", members), "PROJECT_MANIFEST_INVALID");

    members = Project();
    members[0].data = Bytes(R"({"format":"forge-project","version":1,"sourceKind":"archive"})");
    RequireCode(ProjectStore::Save(temp.Path() / "wrong-baseline.forge", members), "PROJECT_ENVELOPE_INVALID");
    members = Project();
    members.push_back({"war3map.j", Bytes("not a project root")});
    RequireCode(ProjectStore::Save(temp.Path() / "map-member.forge", members), "PROJECT_ENVELOPE_INVALID");
    members = Project();
    members.push_back({"BASELINE/SOURCE.W3X", Bytes("mixed-source")});
    RequireCode(ProjectStore::Save(temp.Path() / "mixed-baseline.forge", members), "PROJECT_ENVELOPE_INVALID");
    RequireNoStaging(temp.Path());
}

void TestNonProjectUpdateAndInvalidCandidate() {
    TempDirectory temp;
    const auto path = temp.Path() / "pretending.forge";
    auto invalid = Project();
    invalid[0].data = Bytes("arbitrary non-project text");
    RequireSuccess(WriteVerifiedArchive(path, invalid), "generic archive fixture");
    const auto originalHash = Hash(path);
    ProjectSaveOptions options;
    options.expectedTargetSha256 = originalHash;
    RequireCode(ProjectStore::Save(path, Project("other"), options), "PROJECT_TARGET_NOT_PROJECT");
    Require(Hash(path) == originalHash, "non-project unchanged");

    const auto projectPath = temp.Path() / "real.forge";
    const auto first = ProjectStore::Save(projectPath, Project());
    RequireSuccess(first, "valid candidate failure fixture");
    options.expectedTargetSha256 = first.value->sha256;
    auto badCandidate = Project("changed");
    badCandidate.push_back({"overlay/../escaped.bin", Bytes("invalid path")});
    Require(!ProjectStore::Save(projectPath, badCandidate, options).Succeeded(), "archive writer failure stops update");
    Require(Hash(projectPath) == first.value->sha256, "failed candidate leaves old target bytes intact");
    RequireMembers(projectPath, Project());
    RequireNoStaging(temp.Path());

    const auto map = temp.Path() / "map.forge";
    const std::vector<MapArchiveMember> mapMembers{{"war3map.j", Bytes("authored script")}};
    RequireSuccess(WriteVerifiedArchive(map, mapMembers), "renamed-map fixture");
    options.expectedTargetSha256 = Hash(map);
    RequireCode(ProjectStore::Save(map, Project(), options), "PROJECT_TARGET_NOT_PROJECT");
    Require(Hash(map) == *options.expectedTargetSha256, "renamed map untouched");
}

void TestProtectedSourcesAndAliases() {
    TempDirectory temp;
    const auto sourceDirectory = temp.Path() / "source";
    std::filesystem::create_directory(sourceDirectory);
    ProjectSaveOptions options;
    options.protectedSourcePaths = {sourceDirectory};
    const auto protectedTarget = sourceDirectory / "output.forge";
    RequireCode(ProjectStore::Save(protectedTarget, Project(), options), "PROTECTED_SOURCE_TARGET");
    Require(!std::filesystem::exists(protectedTarget), "protected directory remains unchanged");
    options.protectedSourcePaths = {sourceDirectory / "missing"};
    RequireCode(ProjectStore::Save(temp.Path() / "outside.forge", Project(), options), "PROTECTED_SOURCE_UNAVAILABLE");

    const auto source = temp.Path() / "source.forge";
    const auto first = ProjectStore::Save(source, Project());
    RequireSuccess(first, "protected project source fixture");
    options.expectedTargetSha256 = first.value->sha256;
    options.protectedSourcePaths = {source};
    RequireCode(ProjectStore::Save(source, Project("change"), options), "PROTECTED_SOURCE_TARGET");
    Require(Hash(source) == first.value->sha256, "same source target unchanged");
    const auto alias = temp.Path() / "hardlink.forge";
    Require(CreateHardLinkW(alias.c_str(), source.c_str(), nullptr) != FALSE, "create hard-link identity fixture");
    Require(!ProjectStore::Save(alias, Project("change"), options).Succeeded(), "hard-link source alias must be rejected");
    Require(Hash(source) == first.value->sha256 && Hash(alias) == first.value->sha256, "hard-link contents unchanged");

    const auto directoryAlias = temp.Path() / "directory-link";
    if (CreateSymbolicLinkW(directoryAlias.c_str(), sourceDirectory.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
        options.expectedTargetSha256.reset();
        options.protectedSourcePaths = {sourceDirectory};
        RequireCode(ProjectStore::Save(directoryAlias / "linked.forge", Project(), options), "PROTECTED_SOURCE_TARGET");
        Require(!std::filesystem::exists(sourceDirectory / "linked.forge"), "resolved directory alias protected");
        std::filesystem::remove(directoryAlias);
    } else {
        std::cout << "ProjectStore: directory symlink scenario unavailable (Windows error " << GetLastError() << ")\n";
    }
    RequireNoStaging(temp.Path());
}

void TestLockedReadonlyAndInvalidPaths() {
    TempDirectory temp;
    const auto path = temp.Path() / "document.forge";
    const auto first = ProjectStore::Save(path, Project());
    RequireSuccess(first, "lock fixture");
    ProjectSaveOptions options;
    options.expectedTargetSha256 = first.value->sha256;
    {
        FileHandle lock(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(lock.Valid(), "hold external file reader");
        RequireCode(ProjectStore::Save(path, Project("change"), options), "PROJECT_TARGET_LOCKED");
    }
    Require(Hash(path) == first.value->sha256, "locked target unchanged");
    Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "set readonly fixture");
    const auto readonlyResult = ProjectStore::Save(path, Project("change"), options);
    Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "restore fixture attributes");
    Require(!readonlyResult.Succeeded() && Hash(path) == first.value->sha256, "readonly target unchanged");
    for (const auto* name : {L"output.w3x", L"CON.forge", L"trailing.forge.", L"alternate:stream.forge"}) {
        RequireCode(ProjectStore::Save(temp.Path() / name, Project()), "TARGET_PATH_INVALID");
    }
    Require(!ProjectStore::Save(temp.Path() / "missing" / "output.forge", Project()).Succeeded(), "missing parent rejected");
    RequireNoStaging(temp.Path());
}

void TestConcurrentPublicationPreservesWinner() {
    TempDirectory temp;
    const auto path = temp.Path() / "concurrent.forge";
    auto left = Project("left"), right = Project("right");
    Result<ProjectSaveResult> leftResult, rightResult;
    std::barrier start(3);
    std::thread first([&] { start.arrive_and_wait(); leftResult = ProjectStore::Save(path, left); });
    std::thread second([&] { start.arrive_and_wait(); rightResult = ProjectStore::Save(path, right); });
    start.arrive_and_wait();
    first.join();
    second.join();
    Require(leftResult.Succeeded() != rightResult.Succeeded(), "exactly one independent save may publish");
    const auto& winner = leftResult.Succeeded() ? leftResult : rightResult;
    Require(Hash(path) == winner.value->sha256, "loser never deletes or replaces concurrent winner");
    RequireMembers(path, leftResult.Succeeded() ? left : right);

    ProjectSaveOptions options;
    options.expectedTargetSha256 = winner.value->sha256;
    const auto priorMembers = leftResult.Succeeded() ? left : right;
    std::barrier updateStart(3);
    std::thread leftUpdate([&] { updateStart.arrive_and_wait(); leftResult = ProjectStore::Save(path, Project("update-left"), options); });
    std::thread rightUpdate([&] { updateStart.arrive_and_wait(); rightResult = ProjectStore::Save(path, Project("update-right"), options); });
    updateStart.arrive_and_wait();
    leftUpdate.join();
    rightUpdate.join();
    Require(!(leftResult.Succeeded() && rightResult.Succeeded()), "stale expected SHA cannot authorize two updates");
    if (leftResult.Succeeded() || rightResult.Succeeded()) {
        const auto& updated = leftResult.Succeeded() ? leftResult : rightResult;
        Require(Hash(path) == updated.value->sha256, "update loser preserves winner");
        Require(updated.value->backupPath.has_value() && Hash(*updated.value->backupPath) == *options.expectedTargetSha256,
            "concurrent update retains old complete project");
        RequireMembers(*updated.value->backupPath, priorMembers);
    } else {
        Require(Hash(path) == *options.expectedTargetSha256, "mutual lock rejection preserves previous baseline");
    }
    RequireNoStaging(temp.Path());
}

} // namespace

int main() {
    try {
        TestFirstSaveAndVerifiedUpdates();
        TestConflictAndMissingTargetNeverChangeBaseline();
        TestManifestIdentityAndEnvelope();
        TestNonProjectUpdateAndInvalidCandidate();
        TestProtectedSourcesAndAliases();
        TestLockedReadonlyAndInvalidPaths();
        TestConcurrentPublicationPreservesWinner();
        std::cout << "ProjectStore: 7 scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
