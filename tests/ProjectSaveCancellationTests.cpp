#include "War3MapCore/ProjectStore.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace War3Map;
constexpr std::size_t kMiB = 1024 * 1024;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void Success(const Result<T>& result) {
    if (result.Succeeded() && result.value) return;
    std::string error;
    for (const auto& item : result.diagnostics) error += item.code + ":" + item.message + "\n";
    throw std::runtime_error(error);
}
template<class T> bool Code(const Result<T>& result, std::string_view code) {
    return !result.Succeeded() && !result.value && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& item) { return item.code == code; });
}
std::vector<std::byte> Bytes(std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text));
    return {bytes.begin(), bytes.end()};
}
std::vector<std::byte> Body() {
    std::vector<std::byte> result(8 * kMiB);
    std::uint32_t seed = 0x5193447u;
    for (auto& value : result) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; value = std::byte(seed & 0xff); }
    return result;
}
std::vector<MapArchiveMember> Members(bool large = false) {
    return {{"project.json", Bytes(R"({"format":"forge-project","version":1,"sourceKind":"directory","recovery":true})")},
        {"baseline/workspace/source.bin", large ? Body() : Bytes("original source")}};
}
std::string Hash(const std::filesystem::path& path) { auto result = Sha256File(path); Success(result); return *result.value; }
class Fixture {
public:
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() / (L"Forge-cancel-owned-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(serial.fetch_add(1)));
        Require(std::filesystem::create_directory(root), "create fresh owned fixture");
        target = root / L"recovery.forge";
        auto saved = ProjectStore::Save(target, Members()); Success(saved); oldSha = saved.value->sha256;
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    ProjectSaveOptions Options(const std::shared_ptr<ProjectSaveControl>& control) const {
        ProjectSaveOptions result; result.control = control; result.expectedTargetSha256 = oldSha; return result;
    }
    void OldPoint() const {
        Require(Hash(target) == oldSha, "previous recovery exact SHA retained");
        auto opened = MapArchiveSession::Open(target); Success(opened);
        Require(opened.value->ReadMember("baseline/workspace/source.bin").value == std::optional(Bytes("original source")), "old point reopens exact original payload");
    }
    void NoTemporary() const {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            const auto name = entry.path().filename().wstring();
            Require(!name.starts_with(L".forge-") && name.find(L".backup-") == std::wstring::npos, "cancelled prepublication leaves no verified stage or backup");
        }
    }
    std::filesystem::path root, target;
    std::string oldSha;
};

void ReadAndHashCancellation() {
    Fixture fixture;
    const auto path = fixture.root / "input.bin";
    const auto body = Body();
    { std::ofstream output(path, std::ios::binary); output.write(reinterpret_cast<const char*>(body.data()), body.size()); Require(output.good(), "write synthetic source"); }
    const auto sha = Hash(path);
    ProjectSaveControl* active = nullptr;
    auto read = std::make_shared<ProjectSaveControl>([&](auto stage, auto bytes) {
        if (stage == ProjectSaveStage::Read && bytes >= kMiB) Require(active->RequestCancellation(), "cancel accepted during read");
    }); active = read.get();
    Require(Code(ReadVerifiedFile(path, sha, 16 * kMiB, read.get()), kProjectSaveCancelledCode), "read cancellation returns no partial payload");
    Require(read->Complete(), "read cancellation terminal");
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "cancelled trusted read released source HANDLE"); CloseHandle(handle);
    Require(Hash(path) == sha, "read cancellation source unchanged");
    auto hash = std::make_shared<ProjectSaveControl>([&](auto stage, auto bytes) {
        if (stage == ProjectSaveStage::Hash && bytes >= kMiB) Require(active->RequestCancellation(), "cancel accepted during hash");
    }); active = hash.get();
    Require(Code(Sha256File(path, hash.get()), kProjectSaveCancelledCode), "hash cancellation not partial digest");
    Require(Hash(path) == sha, "hash cancellation source unchanged"); fixture.OldPoint();
}
void HandleHashCallbackExceptionRestoresBorrowedCursor() {
    Fixture fixture; const auto path = fixture.root / "cursor-source.bin"; const auto body = Body();
    { std::ofstream output(path, std::ios::binary); output.write(reinterpret_cast<const char*>(body.data()), body.size()); Require(output.good(), "write cursor fixture"); }
    const auto sha = Hash(path);
    struct OwnedHandle {
        HANDLE handle;
        ~OwnedHandle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } input{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
    Require(input.handle != INVALID_HANDLE_VALUE, "open original borrowed-handle fixture");
    LARGE_INTEGER start{}; start.QuadPart = 137;
    Require(SetFilePointerEx(input.handle, start, nullptr, FILE_BEGIN) != FALSE, "position original cursor");
    ProjectSaveControl control([](auto stage, auto bytes) {
        if (stage == ProjectSaveStage::Hash && bytes >= kMiB) throw std::runtime_error("owned hash callback exception");
    });
    bool observed = false;
    try { (void)Sha256Handle(input.handle, &control); }
    catch (const std::runtime_error& error) { observed = std::string_view(error.what()) == "owned hash callback exception"; }
    Require(observed, "original callback exception propagated");
    LARGE_INTEGER zero{}, actual{};
    Require(SetFilePointerEx(input.handle, zero, &actual, FILE_CURRENT) != FALSE && actual.QuadPart == start.QuadPart,
        "exception path restores exact original borrowed HANDLE cursor");
    std::byte next[16]{}; DWORD count = 0;
    Require(ReadFile(input.handle, next, sizeof(next), &count, nullptr) != FALSE && count == sizeof(next)
        && std::equal(std::begin(next), std::end(next), body.begin() + start.QuadPart), "caller next read remains at original position");
    Require(SetFilePointerEx(input.handle, start, nullptr, FILE_BEGIN) != FALSE, "reset normal-path cursor");
    const auto normal = Sha256Handle(input.handle); Success(normal);
    Require(*normal.value == sha && SetFilePointerEx(input.handle, zero, &actual, FILE_CURRENT) != FALSE
        && actual.QuadPart == start.QuadPart, "normal hash still restores cursor and original digest");
    Require(Hash(path) == sha, "throwing callback never changes source bytes"); fixture.OldPoint();
}
void ChunkBuildCopyWriteAndVerifyCancellation() {
    for (const auto stage : {ProjectSaveStage::Build, ProjectSaveStage::Copy, ProjectSaveStage::Write, ProjectSaveStage::Verify}) {
        Fixture fixture; ProjectSaveControl* active = nullptr; bool reached = false, wrote = false;
        auto control = std::make_shared<ProjectSaveControl>([&](auto observed, auto bytes) {
            if (observed == ProjectSaveStage::Write) wrote = true;
            if (observed == stage && bytes >= (stage == ProjectSaveStage::Build ? 4096u : kMiB)
                && (stage != ProjectSaveStage::Verify || wrote)) {
                reached = true; Require(active->RequestCancellation(), "chunk cancellation accepted");
            }
        }); active = control.get();
        auto result = ProjectStore::Save(fixture.target, Members(true), fixture.Options(control));
        Require(reached && Code(result, kProjectSaveCancelledCode), "selected real chunk cancelled");
        Require(!control->PublicationStarted() && !control->RequestCancellation(), "cancel terminal is prepublication and closes requests");
        fixture.OldPoint(); fixture.NoTemporary();
    }
}
void BeforePublicationBothRoutes() {
    for (const bool update : {false, true}) {
        Fixture fixture; ProjectSaveControl* active = nullptr; bool reached = false;
        auto control = std::make_shared<ProjectSaveControl>([&](auto stage, auto) {
            if (stage == ProjectSaveStage::BeforePublication) { reached = true; Require(active->RequestCancellation(), "final boundary cancel accepted"); }
        }); active = control.get();
        auto options = fixture.Options(control); if (!update) options.expectedTargetSha256.reset();
        const auto target = update ? fixture.target : fixture.root / "first.forge";
        Require(Code(ProjectStore::Save(target, Members(true), options), kProjectSaveCancelledCode) && reached, "both final publish routes honor cancellation");
        Require(!control->PublicationStarted(), "publication did not begin");
        if (!update) Require(!std::filesystem::exists(target), "initial cancelled target absent");
        fixture.OldPoint(); fixture.NoTemporary();
    }
}
void LateCancellationCannotUndoPublication() {
    for (const bool update : {false, true}) {
        Fixture fixture; ProjectSaveControl* active = nullptr; bool reached = false;
        auto control = std::make_shared<ProjectSaveControl>([&](auto stage, auto) {
            if (stage == ProjectSaveStage::Publication) { reached = true; Require(!active->RequestCancellation(), "late cancellation rejected"); }
        }); active = control.get();
        auto options = fixture.Options(control); if (!update) options.expectedTargetSha256.reset();
        const auto target = update ? fixture.target : fixture.root / "first.forge";
        auto result = ProjectStore::Save(target, Members(true), options); Success(result);
        Require(reached && control->PublicationStarted() && !control->RequestCancellation(), "publication sticky and closed after completion");
        Require(Hash(target) == result.value->sha256, "new exact recovery receipt");
        auto opened = MapArchiveSession::Open(target); Success(opened);
        Require(opened.value->ReadMember("baseline/workspace/source.bin").value == std::optional(Body()), "new candidate payload reopens");
        if (update) Require(result.value->backupPath && Hash(*result.value->backupPath) == fixture.oldSha, "late cancel preserves exact prior backup");
        else fixture.OldPoint();
    }
}
void PrepublicationExceptionPreservesOldPoint() {
    Fixture fixture; ProjectSaveControl* active = nullptr;
    auto control = std::make_shared<ProjectSaveControl>([&](auto stage, auto) {
        if (stage == ProjectSaveStage::Publication) {
            Require(!active->RequestCancellation(), "publication failure still cannot accept late cancel");
            throw std::runtime_error("owned injected pre-rename failure");
        }
    }); active = control.get();
    auto result = ProjectStore::Save(fixture.target, Members(true), fixture.Options(control));
    Require(Code(result, "PROJECT_SAVE_FAILED") && !Code(result, kProjectSaveCancelledCode), "publication exception is failure not false cancellation");
    fixture.OldPoint(); fixture.NoTemporary();
    Fixture rollbackFixture; bool reachedBackup = false;
    auto rollbackControl = std::make_shared<ProjectSaveControl>([&](auto stage, auto step) {
        if (stage == ProjectSaveStage::Publication && step == 1) {
            reachedBackup = true;
            Require(!active->RequestCancellation(), "cancel rejected after moving old point to backup");
            throw std::runtime_error("owned injected failure after backup rename");
        }
    }); active = rollbackControl.get();
    auto rollbackResult = ProjectStore::Save(rollbackFixture.target, Members(true), rollbackFixture.Options(rollbackControl));
    Require(reachedBackup && Code(rollbackResult, "PROJECT_SAVE_FAILED") && !Code(rollbackResult, kProjectSaveCancelledCode), "post-backup failure rolls back without false cancelled outcome");
    rollbackFixture.OldPoint(); rollbackFixture.NoTemporary();
}
void UnverifiedStageCancellationIsRetainedHonestly() {
    Fixture fixture; ProjectSaveControl* active = nullptr; unsigned hashesAfterWrite = 0; bool wrote = false;
    auto control = std::make_shared<ProjectSaveControl>([&](auto stage, auto bytes) {
        if (stage == ProjectSaveStage::Write) wrote = true;
        if (wrote && stage == ProjectSaveStage::Hash && bytes == 0 && ++hashesAfterWrite == 2)
            Require(active->RequestCancellation(), "cancel during stage re-hash accepted");
    }); active = control.get();
    auto result = ProjectStore::Save(fixture.target, Members(true), fixture.Options(control));
    Require(Code(result, kProjectSaveCancelledCode) && Code(result, "PROJECT_CANDIDATE_RETAINED"), "unverified stage is not silently deleted");
    unsigned stages = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fixture.root))
        if (entry.path().filename().wstring().starts_with(L".forge-project-stage-")) ++stages;
    Require(stages == 1, "only owned unverified stage retained"); fixture.OldPoint();
}
void CancellationAndPublicationCASAreExclusive() {
    std::barrier rendezvous(2); ProjectSaveControl* active = nullptr;
    ProjectSaveControl control([&](auto stage, auto) { if (stage == ProjectSaveStage::BeforePublication) rendezvous.arrive_and_wait(); }); active = &control;
    bool published = false;
    std::jthread worker([&] { published = active->BeginPublication(); });
    rendezvous.arrive_and_wait(); const bool accepted = control.RequestCancellation(); worker.join();
    Require(accepted != published, "cancel and publication cannot both win CAS");
    Require(control.Complete() == accepted, "terminal follows unique winner");
    Require(control.PublicationStarted() == published && !control.RequestCancellation(), "sticky publication and late rejection");
}
void ManualDefaultsAndCancelledControlCannotBeReused() {
    Fixture fixture; auto control = std::make_shared<ProjectSaveControl>();
    Require(control->RequestCancellation(), "prepared cancellation accepted");
    auto result = ProjectStore::Save(fixture.target, Members(true), fixture.Options(control));
    Require(Code(result, kProjectSaveCancelledCode), "pre-cancel rejected before read/build");
    Require(Code(ProjectStore::Save(fixture.target, Members(), fixture.Options(control)), "PROJECT_SAVE_CONTROL_USED"), "finished control cannot publish another snapshot");
    ProjectSaveOptions options; options.expectedTargetSha256 = fixture.oldSha;
    auto manual = ProjectStore::Save(fixture.target, Members(true), options); Success(manual);
    Require(manual.value->backupPath && Hash(*manual.value->backupPath) == fixture.oldSha, "manual default preserves previous backup behavior");
}
} // namespace
int main() {
    try {
        ReadAndHashCancellation(); HandleHashCallbackExceptionRestoresBorrowedCursor();
        ChunkBuildCopyWriteAndVerifyCancellation(); BeforePublicationBothRoutes();
        LateCancellationCannotUndoPublication(); PrepublicationExceptionPreservesOldPoint();
        UnverifiedStageCancellationIsRetainedHonestly(); CancellationAndPublicationCASAreExclusive();
        ManualDefaultsAndCancelledControlCannotBeReused();
        std::cout << "9 recovery cancellation contracts passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
