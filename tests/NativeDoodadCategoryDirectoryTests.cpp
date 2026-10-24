#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Game/NativeDoodadCategoryDirectory.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <Windows.h>

namespace {
using namespace War3Map;
using namespace War3Map::Game;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBudget = 8 * 1024 * 1024;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Check(Status result) {
    if (result.Succeeded()) return;
    std::string message;
    for (const auto& d : result.diagnostics) message += d.code + ": " + d.message + "\n";
    throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    Check({result.diagnostics}); Require(result.value.has_value(), "missing value"); return std::move(*result.value);
}
Bytes Text(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        static std::atomic<unsigned> sequence{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-startup-category-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(root), "private source directory");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void Archive(std::string_view name, std::vector<MapArchiveMember> members) {
        if (members.empty()) members.push_back({"unrelated.bin", Text("unrelated")});
        const auto destination = root / IO::PathFromUtf8(name);
        // 测试自己的临时归档可替换；不写作者来源或固定安装目录。
        std::error_code error; std::filesystem::remove(destination, error);
        Take(WriteVerifiedArchive(destination, members));
    }
    void Loose(std::string_view name, std::string_view text) {
        const auto path = root / IO::PathFromUtf8(name);
        std::filesystem::create_directories(path.parent_path());
        Check(IO::WriteFileBytes(path, Text(text)));
    }
    NativeDoodadCategoryDirectory Capture(std::size_t budget = kBudget) const {
        return Take(NativeDoodadCategoryDirectory::Capture(root, NativeEditorStartupRules::Classic127aExpansion, budget));
    }
};

void FirstSpellingAndRawEmptyValues() {
    const std::pair<std::string_view, unsigned> cases[]{
        {"[Other]\nC=ignored\n", 2}, {"[DoodadCategories]\nC=custom\n", 4},
        {"[DoodadCategories]\nc=custom\n", 2}, {"[DoodadCategories]\nc=x\nC=y\n", 2},
        {"[DoodadCategories]\nc=\nC=y\n", 4}, {"[DoodadCategories]\nC=\n", 2},
        {"[DoodadCategories]\nC=\"\"\n", 4}, {"[DoodadCategories]\nC=,\n", 4},
        {"[DoodadCategories]\nCustom=custom\n", 4}, {"[DoodadCategories]\ncustom=x\nCustom=y\n", 2},
        {"[DoodadCategories]\nC=x\nC=\n", 4}, {"[DoodadCategories]\n C=x\n", 2},
        {"[ DoodadCategories]\nC=x\n", 2}, {"[DOODADCATEGORIES]\nC=x", 4},
        {"\xef\xbb\xbf[DoodadCategories]\r\n//C=no\r\nC=x", 4},
        {"[DoodadCategories]\n[unfinished\nC=x\n", 4}
    };
    for (const auto& [profile, expected] : cases) {
        Fixture f; f.Archive("war3.mpq", {{"UI/WorldEditData.txt", Text(profile)}});
        const auto owner = f.Capture(); Require(owner.CategoryCSnapCells() == expected, "native first-spelling/raw-value semantics");
        Check(owner.VerifySources(kBudget));
        const auto exact = owner.CaptureBudgetBytes();
        Require(f.Capture(exact).CategoryCSnapCells() == expected, "exact aggregate source/parser budget");
        Require(!NativeDoodadCategoryDirectory::Capture(f.root, NativeEditorStartupRules::Classic127aExpansion, exact - 1).Succeeded(),
            "budget minus one never issues a partial directory");
    }
}
void FixedProfileOrderAndFivePackageWinners() {
    Fixture f;
    // flag=0的Units/MiscData先于flag=1的UI/WorldEditData，editor mode=1两者都加载。
    f.Archive("war3.mpq", {{"Units/MiscData.txt", Text("[DoodadCategories]\nc=first\n")},
        {"UI/WorldEditData.txt", Text("[DoodadCategories]\nC=later\n")},
        {"Custom_V1/UI/WorldEditData.txt", Text("[DoodadCategories]\nC=not-startup\n")},
        {"L.mpq", Text("not a startup provider")}});
    Require(f.Capture().CategoryCSnapCells() == 2, "fixed startup order preserves the first effective spelling");
    for (const auto name : GameDataSource::CoreArchiveNames()) {
        Fixture priority;
        priority.Archive("war3.mpq", {{"UI/WorldEditData.txt", Text("[DoodadCategories]\nc=base\n")}});
        priority.Archive(name, {{"UI/WorldEditData.txt", Text("[DoodadCategories]\nC=winner\n")}});
        Require(priority.Capture().CategoryCSnapCells() == 4, "each catalog archive is a real expansion-startup source");
    }
    Fixture precedence;
    for (const auto name : GameDataSource::CoreArchiveNames())
        precedence.Archive(name, {{"UI/WorldEditData.txt", Text(name == "war3patch.mpq"
            ? "[DoodadCategories]\nc=patch\n" : "[DoodadCategories]\nC=lower\n")}});
    Require(precedence.Capture().CategoryCSnapCells() == 2, "patch beats both local packages and expansion");
}
void LooseSourcesAndAbsenceCas() {
    Fixture f; f.Archive("war3.mpq", {{"UI/WorldEditData.txt", Text("[DoodadCategories]\nC=mpq\n")}});
    const auto mpq = f.Capture();
    f.Loose("UI/WorldEditData.txt", "[DoodadCategories]\nc=relative\n");
    Require(!mpq.VerifySources(kBudget).Succeeded(), "previously absent full relative loose source invalidates the snapshot");
    const auto relative = f.Capture(); Require(relative.CategoryCSnapCells() == 2, "loose relative wins over MPQ");
    f.Loose("WorldEditData.txt", "[DoodadCategories]\nC=basename\n");
    Require(!relative.VerifySources(kBudget).Succeeded(), "previously absent basename invalidates the snapshot");
    const auto basename = f.Capture(); Require(basename.CategoryCSnapCells() == 4, "basename beats full relative path");
    f.Loose("WorldEditData.txt", "[DoodadCategories]\nC=same-grid-different-bytes\n");
    Require(!basename.VerifySources(kBudget).Succeeded(), "same final grid cannot hide changed source bytes");
    const auto beforeDelete = f.Capture(); std::filesystem::remove(f.root / "WorldEditData.txt");
    Require(!beforeDelete.VerifySources(kBudget).Succeeded(), "source removal invalidates the snapshot");
    Fixture higher; higher.Archive("war3.mpq", {{"UI/WorldEditData.txt", Text("[DoodadCategories]\nC=unchanged\n")}});
    const auto absentArchive = higher.Capture();
    higher.Archive("war3patch.mpq", {{"UI/WorldEditData.txt", Text("[DoodadCategories]\nC=unchanged\n")}});
    Require(!absentArchive.VerifySources(kBudget).Succeeded(), "same bytes from a new higher archive change the source identity");
}
void UnknownSourcesAndUnsupportedBytesFailClosed() {
    Require(!NativeDoodadCategoryDirectory::Capture({}, NativeEditorStartupRules::Classic127aExpansion, kBudget).Succeeded(),
        "unconfigured exeDir cannot prove no C");
    Fixture f;
    Require(f.Capture().CategoryCSnapCells() == 2, "known-absent complete startup inputs select two cells");
    for (const auto bytes : {std::string("[DoodadCategories]\nC=x") + '\0' + "ignored",
            std::string("[DoodadCategories]\nC\xc3\xa9=x\n"), std::string("\xff\xfe[\0", 4)}) {
        f.Archive("war3.mpq", {{"UI/WorldEditData.txt", Text(bytes)}});
        Require(!NativeDoodadCategoryDirectory::Capture(f.root, NativeEditorStartupRules::Classic127aExpansion, kBudget).Succeeded(),
            "unsupported encoding/key bytes never become no C");
    }
    Fixture directory; std::filesystem::create_directory(directory.root / "WorldEditData.txt");
    Require(!NativeDoodadCategoryDirectory::Capture(directory.root, NativeEditorStartupRules::Classic127aExpansion, kBudget).Succeeded(),
        "a directory at a candidate file is unknown, not absent");
    Fixture invalidArchive; invalidArchive.Loose("war3patch.mpq", "not an MPQ");
    Require(!NativeDoodadCategoryDirectory::Capture(invalidArchive.root, NativeEditorStartupRules::Classic127aExpansion, kBudget).Succeeded(),
        "an unreadable higher MPQ is never ignored");
}

void NamedLookupDoesNotRequireTheListfile() {
    Fixture f;
    const auto payload = Text("[DoodadCategories]\nC=actual-named-member\n");
    f.Archive("war3.mpq", {{"UI/WorldEditData.txt", payload}});
    const auto path = f.root / "war3.mpq";
    const auto normal = Take(Mpq::MpqArchive::OpenReadOnly(path));
    Require(!normal.RawListfileNames().empty(), "default MPQ behavior still loads the writer listfile");
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "borrowed read-only MPQ lease");
    Mpq::MpqOpenLimits limits; limits.loadListfile = false; limits.maxArchivePrefixBytes = 0;
    auto opened = Mpq::MpqArchive::OpenReadOnlyHandle(handle, path, limits);
    CloseHandle(handle);
    auto named = Take(std::move(opened));
    Require(named.RawListfileNames().empty() && named.ListFiles().empty(), "skip-listfile performs no implicit name enumeration");
    const auto info = named.GetFileInfo("UI/WorldEditData.txt");
    Require(info && info->fileSize == payload.size(), "named member metadata retains the actual decoded size");
    const auto readBudget = std::max<std::size_t>(info->compressedSize, info->fileSize);
    Require(named.HasFile("UI/WorldEditData.txt")
        && Take(named.ReadFileBounded("UI/WorldEditData.txt", readBudget)) == payload,
        "named hash lookup and bounded content reads remain independent of listfile loading");
    Require(!named.ReadFileBounded("UI/WorldEditData.txt", readBudget - 1).Succeeded(),
        "skip-listfile keeps both the stored and decoded member budgets");
    handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "second borrowed MPQ lease");
    limits.maxHashTableEntries = 1;
    const auto refused = Mpq::MpqArchive::OpenReadOnlyHandle(handle, path, limits);
    CloseHandle(handle);
    Require(!refused.Succeeded(), "skip-listfile keeps header/table allocation gates");
}

void StoredAndDecodedBudgetsNeverFallBack() {
    Fixture small;
    const auto shortPayload = Text("[Other]\nC=ignored\n");
    small.Archive("war3.mpq", {{"UI/WorldEditData.txt", shortPayload}});
    const auto archive = Take(Mpq::MpqArchive::OpenReadOnly(small.root / "war3.mpq"));
    const auto info = archive.GetFileInfo("UI/WorldEditData.txt");
    Require(info && info->fileSize == shortPayload.size() && info->compressedSize > info->fileSize,
        "short sector member includes its real offset-table storage overhead");
    const auto captured = small.Capture();
    Require(small.Capture(captured.CaptureBudgetBytes()).CategoryCSnapCells() == 2,
        "exact aggregate budget includes stored bytes larger than the decoded profile");
    Require(!NativeDoodadCategoryDirectory::Capture(small.root, NativeEditorStartupRules::Classic127aExpansion,
        captured.CaptureBudgetBytes() - 1).Succeeded(), "stored-byte accounting keeps the exact minus-one refusal");

    Fixture higher;
    higher.Archive("war3.mpq", {{"UI/WorldEditData.txt", Text("[DoodadCategories]\nC=lower\n")}});
    const auto largePayload = Text(std::string("[Other]\n") + std::string(256 * 1024, 'x') + "\n");
    higher.Archive("war3patch.mpq", {{"UI/WorldEditData.txt", largePayload}});
    const auto winner = higher.Capture();
    Require(winner.CategoryCSnapCells() == 2, "higher profile genuinely overrides a lower C directory");
    const auto insufficient = winner.CaptureBudgetBytes() - largePayload.size();
    Require(!NativeDoodadCategoryDirectory::Capture(higher.root, NativeEditorStartupRules::Classic127aExpansion,
        insufficient).Succeeded(), "an over-budget higher decoded winner never falls back to the lower C profile");
}
} // namespace
int main() {
    try {
        FirstSpellingAndRawEmptyValues(); FixedProfileOrderAndFivePackageWinners();
        LooseSourcesAndAbsenceCas(); UnknownSourcesAndUnsupportedBytesFailClosed();
        NamedLookupDoesNotRequireTheListfile();
        StoredAndDecodedBudgetsNeverFallBack();
        std::cout << "Native startup doodad category directory checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
