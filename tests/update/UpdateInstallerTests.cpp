#include "War3MapCore/Update/UpdateInstaller.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "Core/Update/UpdateInstallerInternal.h"

#include <miniz.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>

namespace {

using namespace War3Map;
using namespace War3Map::Update;
using Json = nlohmann::json;
int checks = 0, failures = 0;
void Check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<typename T>
bool Code(const Result<T>& result, std::string_view code) {
    return !result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& value) { return value.code == code; });
}
template<typename T>
void Diagnostics(const Result<T>& result, std::string_view name) {
    if (result.Succeeded()) return;
    for (const auto& diagnostic : result.diagnostics) std::cerr << name << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
}
void ReceiptDiagnostics(const Result<UpdateInstallReceipt>& result, std::string_view name) {
    Diagnostics(result, name);
    if (!result.value) return;
    const auto& receipt = *result.value;
    std::cerr << name << ": " << Json{{"outcome", static_cast<int>(receipt.outcome)}, {"reason_code", receipt.reasonCode},
        {"reason", receipt.reason}, {"install_exists", std::filesystem::exists(receipt.installDirectory)},
        {"backup_exists", std::filesystem::exists(receipt.backupDirectory)}, {"transaction", receipt.transactionId}}.dump() << '\n';
}
std::span<const std::byte> Bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }
std::string Hash(std::span<const std::byte> bytes) {
    const auto hash = Sha256(bytes);
    if (!hash.Succeeded()) throw std::runtime_error("fixture hash failed");
    return *hash.value;
}
std::string Hash(std::string_view text) { return Hash(Bytes(text)); }
std::vector<std::byte> Read(const std::filesystem::path& path) {
    const auto read = IO::ReadFileBytes(path, 64 * 1024 * 1024);
    if (!read.Succeeded()) throw std::runtime_error("fixture read failed");
    return *read.value;
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    if (!IO::WriteFileBytes(path, bytes).Succeeded()) throw std::runtime_error("fixture write failed");
}
void Write(const std::filesystem::path& path, std::string_view text) { Write(path, Bytes(text)); }
std::filesystem::path Self() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) throw std::runtime_error("fixture self path failed");
    return std::filesystem::path(std::wstring(path.data(), count));
}
struct Temp {
    std::filesystem::path parent, path;
    Detail::RootIdentity owned;
    bool retain = false;
    Temp() {
        const auto original = std::filesystem::temp_directory_path();
        Detail::NativeHandle handle(CreateFileW(original.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        std::array<wchar_t, 32768> actual{};
        const auto count = handle.Valid() ? GetFinalPathNameByHandleW(handle.Get(), actual.data(), static_cast<DWORD>(actual.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
        std::wstring resolved(actual.data(), count < actual.size() ? count : 0);
        if (!count || count >= actual.size() || !resolved.starts_with(L"\\\\?\\") || resolved.starts_with(L"\\\\?\\UNC\\"))
            throw std::runtime_error("fixture actual TEMP failed");
        parent = std::filesystem::path(resolved.substr(4)).lexically_normal();
        while (parent.filename().empty() && parent != parent.root_path()) parent = parent.parent_path();
        path = parent / (L"AudastraInstallerTests-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!path.is_absolute() || path.parent_path() != parent || !std::filesystem::create_directory(path))
            throw std::runtime_error("fixture exclusive TEMP failed");
        const auto identity = Detail::ReadRootIdentity(path);
        if (!identity.Succeeded()) throw std::runtime_error("fixture own root identity failed");
        owned = *identity.value;
    }
    ~Temp() {
        if (retain) return; // 新故障回归只保留自己的独占TEMP供root核验，不触碰其他输入。
        Sleep(100); // 所有test child只正常返回；给已启动旧版fixture完成自然退出。
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (path.is_absolute() && path.parent_path() == parent && path.filename().native().starts_with(L"AudastraInstallerTests-")
            && attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            const auto identity = Detail::ReadRootIdentity(path);
            if (!identity.Succeeded() || *identity.value != owned) return;
            // Windows不能直接删除readonly。只在精确自己创建/回读的夹具根内清除该属性。
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                const auto flags = GetFileAttributesW(entry.path().c_str());
                if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return;
                if (flags & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(entry.path().c_str(), flags & ~FILE_ATTRIBUTE_READONLY);
            }
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};
struct Signer {
    TrustedUpdateKey key;
    std::array<unsigned char, 64> secret{};
    Signer() {
        std::array<unsigned char, 32> seed{};
        for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<unsigned char>(0x51 + i);
        if (crypto_sign_seed_keypair(reinterpret_cast<unsigned char*>(key.bytes.data()), secret.data(), seed.data()) != 0)
            throw std::runtime_error("fixture key failed");
        key.configured = true; // 仅原创测试信任，不进入产品BuildInfo/CLI。
    }
    std::vector<std::byte> Sign(std::string_view text) const {
        std::vector<std::byte> signature(64);
        unsigned long long count = 0;
        if (crypto_sign_detached(reinterpret_cast<unsigned char*>(signature.data()), &count,
            reinterpret_cast<const unsigned char*>(text.data()), text.size(), secret.data()) != 0 || count != 64)
            throw std::runtime_error("fixture sign failed");
        return signature;
    }
};
using Payload = std::vector<std::pair<std::string, std::vector<std::byte>>>;
std::vector<std::byte> Data(std::string_view text) { const auto bytes = Bytes(text); return {bytes.begin(), bytes.end()}; }
std::vector<std::byte> Zip(const Payload& payload) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) throw std::runtime_error("fixture ZIP init failed");
    for (const auto& [name, contents] : payload) {
        if (!mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(), MZ_BEST_COMPRESSION))
            throw std::runtime_error("fixture ZIP add failed");
    }
    void* memory = nullptr; size_t bytes = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &memory, &bytes)) throw std::runtime_error("fixture ZIP finish failed");
    const auto* begin = static_cast<const std::byte*>(memory);
    std::vector<std::byte> result(begin, begin + bytes);
    mz_free(memory); mz_zip_writer_end(&zip);
    return result;
}
std::string Manifest(std::string_view version, const Payload& payload, std::span<const std::byte> zip = {}) {
    Json files = Json::array();
    for (const auto& [name, bytes] : payload) files.push_back({{"path", name}, {"bytes", bytes.size()}, {"sha256", Hash(bytes)}});
    return Json{{"format", "audastra-update"}, {"schema", 1}, {"product", "Audastra"}, {"platform", "windows-x64"},
        {"version", version}, {"minimum_direct_upgrade", "0.1.0"}, {"source_commit", std::string(40, 'a')},
        {"release_notes", "原创临时安装与自然退出恢复"},
        {"zip", {{"filename", "fixture.zip"}, {"bytes", zip.empty() ? 1 : zip.size()}, {"sha256", zip.empty() ? Hash("unused old ZIP") : Hash(zip)}}},
        {"files", std::move(files)}}.dump();
}
std::vector<std::byte> OriginalDllImage(std::vector<std::byte> image) {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) throw std::runtime_error("fixture DOS header missing");
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) throw std::runtime_error("fixture DOS header invalid");
    const auto offset = static_cast<std::size_t>(dos.e_lfanew);
    if (offset > image.size() || image.size() - offset < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))
        throw std::runtime_error("fixture PE header missing");
    DWORD signature = 0;
    std::memcpy(&signature, image.data() + offset, sizeof(signature));
    if (signature != IMAGE_NT_SIGNATURE) throw std::runtime_error("fixture PE signature invalid");
    IMAGE_FILE_HEADER header{};
    std::memcpy(&header, image.data() + offset + sizeof(DWORD), sizeof(header));
    header.Characteristics |= IMAGE_FILE_DLL;
    std::memcpy(image.data() + offset + sizeof(DWORD), &header, sizeof(header));
    return image; // 只改本测试自身原创PE的DLL标志，不读取外部DLL。
}
struct Fixture {
    Temp temp;
    Signer signer;
    UpdateInstallRequest request;
    std::vector<std::byte> image;
    Detail::TreeInventory original;
    std::string targetRaw;
    std::vector<std::byte> targetSignature;
    explicit Fixture(bool invalidNewImage = false, bool invalidOldImage = false) {
        image = Read(Self());
        const auto oldImage = invalidOldImage ? Data("original invalid old PE fixture") : image;
        request.installDirectory = temp.path / L"install";
        request.packagePath = temp.path / L"inputs/fixture.zip";
        request.manifestPath = temp.path / L"inputs/target.json";
        request.signaturePath = temp.path / L"inputs/target.sig";
        request.baselineManifestPath = temp.path / L"inputs/old.json";
        request.baselineSignaturePath = temp.path / L"inputs/old.sig";
        request.currentVersion = "0.1.0"; request.executableRelativePath = "app.exe"; request.healthTimeoutMs = 3000;
        Write(request.installDirectory / L"app.exe", oldImage);
        Write(request.installDirectory / L"program.txt", "old program");
        Write(request.installDirectory / L"obsolete.dll", "preserved obsolete program");
        Write(request.installDirectory / L"settings.json", "original settings");
        Write(request.installDirectory / L"logs/local.log", "original log");
        Write(request.installDirectory / L"feedback/local.json", "original private feedback");
        Write(request.installDirectory / L"作者数据/地图.w3x", "original private author bytes");
        Write(request.installDirectory / L"readonly-user.txt", "original readonly user bytes");
        SetFileAttributesW((request.installDirectory / L"readonly-user.txt").c_str(), FILE_ATTRIBUTE_READONLY);
        std::filesystem::create_directories(request.installDirectory / L"user-empty/inner");
        const auto old = Manifest("0.1.0", {{"app.exe", oldImage}, {"program.txt", Data("old program")},
            {"obsolete.dll", Data("preserved obsolete program")}});
        const auto signature = signer.Sign(old);
        Write(request.baselineManifestPath, old); Write(request.baselineSignaturePath, signature);
        Write(request.installDirectory / L"update-manifest.json", old);
        Write(request.installDirectory / L"update-manifest.json.sig", signature);
        Prepare("0.2.0", invalidNewImage ? Data("original invalid PE fixture") : image);
        const auto inventory = Detail::InspectUpdateTree(request.installDirectory);
        if (!inventory.Succeeded()) throw std::runtime_error("fixture inventory failed");
        original = *inventory.value;
    }
    void Prepare(std::string_view version, const std::vector<std::byte>& newImage) {
        Payload payload{{"app.exe", newImage}, {"program.txt", Data("new program " + std::string(version))},
            {"assets/new.txt", Data("new signed asset " + std::string(version))}};
        const auto archive = Zip(payload);
        targetRaw = Manifest(version, payload, archive); targetSignature = signer.Sign(targetRaw);
        Write(request.packagePath, archive); Write(request.manifestPath, targetRaw); Write(request.signaturePath, targetSignature);
        request.startupArguments = {"--fixture-child", "good", "--fixture-version", std::string(version)};
    }
    void Next(std::string_view from, std::string_view to) {
        Write(request.baselineManifestPath, targetRaw); Write(request.baselineSignaturePath, targetSignature);
        request.currentVersion = from;
        Prepare(to, image);
    }
    bool Unchanged() const {
        const auto tree = Detail::InspectUpdateTree(request.installDirectory);
        return tree.Succeeded() && tree.value->sha256 == original.sha256;
    }
    bool FileIs(std::wstring_view path, std::string_view text) const {
        const auto hash = Sha256File(request.installDirectory / std::filesystem::path(path));
        return hash.Succeeded() && *hash.value == Hash(text);
    }
    void Preserved(std::string_view context) const {
        for (const auto& [key, file] : original.files) {
            if (key == L"app.exe" || key == L"program.txt") continue;
            const auto hash = Sha256File(request.installDirectory / std::filesystem::path(key));
            Check(hash.Succeeded() && *hash.value == file.sha256, context);
        }
        Check(std::filesystem::is_directory(request.installDirectory / L"user-empty/inner"), "empty nested user directories retained");
        Check((GetFileAttributesW((request.installDirectory / L"readonly-user.txt").c_str()) & FILE_ATTRIBUTE_READONLY) != 0,
            "readonly user attribute retained");
    }
};
void WaitNatural(const std::optional<UpdateProcessIdentity>& identity) {
    if (!identity) return;
    Detail::NativeHandle process(OpenProcess(SYNCHRONIZE, FALSE, identity->processId));
    if (!process.Valid()) return;
    Check(WaitForSingleObject(process.Get(), 5000) == WAIT_OBJECT_0, "owned test child exits naturally within bound");
}
struct WaitingInstance {
    Detail::NativeHandle release, process, thread;
    explicit WaitingInstance(const std::filesystem::path& executable) {
        const auto eventName = L"Local\\AudastraInstallerExisting-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
        release = Detail::NativeHandle(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
        if (!release.Valid() || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("fixture original release event failed");
        auto command = L"\"" + executable.native() + L"\" --fixture-child wait-existing --fixture-exit-event \"" + eventName + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            executable.parent_path().c_str(), &startup, &child)) {
            process = Detail::NativeHandle(child.hProcess); thread = Detail::NativeHandle(child.hThread);
        }
    }
    ~WaitingInstance() { if (process.Valid()) { SetEvent(release.Get()); WaitForSingleObject(process.Get(), 5000); } }
    [[nodiscard]] bool Running() const {
        DWORD exit = 0;
        return process.Valid() && GetExitCodeProcess(process.Get(), &exit) && exit == STILL_ACTIVE;
    }
    [[nodiscard]] bool ReleaseNaturally() const {
        return process.Valid() && SetEvent(release.Get()) && WaitForSingleObject(process.Get(), 5000) == WAIT_OBJECT_0;
    }
};

} // namespace

namespace {

void BaselineTests() {
    Fixture fixture;
    const auto discovered = DiscoverInstalledUpdateBaseline(fixture.request.installDirectory, "0.1.0", fixture.signer.key);
    Diagnostics(discovered, "first portable baseline");
    Check(discovered.Succeeded() && discovered.value->transactionDirectory.empty()
        && discovered.value->manifestPath == fixture.request.installDirectory / L"update-manifest.json", "first signed portable sidecar discovered");
    Check(fixture.Unchanged(), "baseline discovery is read only");
    Check(Code(DiscoverInstalledUpdateBaseline(fixture.request.installDirectory, "0.1.1", fixture.signer.key),
        "UPDATE_BASELINE_IDENTITY_MISMATCH"), "sidecar exact version required");
    Write(fixture.request.installDirectory / L"program.txt", "modified original program");
    Check(!DiscoverInstalledUpdateBaseline(fixture.request.installDirectory, "0.1.0", fixture.signer.key).Succeeded(),
        "sidecar discovery verifies signed current program SHA");
    Write(fixture.request.installDirectory / L"program.txt", "old program");
    auto signature = Read(fixture.request.installDirectory / L"update-manifest.json.sig");
    signature[0] ^= std::byte{1};
    Write(fixture.request.installDirectory / L"update-manifest.json.sig", signature);
    Check(Code(DiscoverInstalledUpdateBaseline(fixture.request.installDirectory, "0.1.0", fixture.signer.key),
        "UPDATE_SIGNATURE_INVALID"), "unsigned or altered sidecar does not grant ownership");
}

void GuardTests() {
    Fixture fixture;
    const auto noKey = InstallUpdate(fixture.request, {});
    Check(Code(noKey, "UPDATE_TRUST_NOT_CONFIGURED") && fixture.Unchanged(), "no trusted release key means no mutation");
    Detail::NativeHandle locked(CreateFileW((fixture.request.installDirectory / L"settings.json").c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    Check(locked.Valid(), "original user write lease fixture acquired");
    const auto busy = InstallUpdate(fixture.request, fixture.signer.key);
    Check(!busy.Succeeded(), "actual locked writing user file refuses update");
    locked = Detail::NativeHandle{};
    Check(fixture.Unchanged(), "busy user file refusal preserves original inventory");
    auto inside = fixture.request;
    inside.baselineManifestPath = fixture.request.installDirectory / L"update-manifest.json";
    inside.baselineSignaturePath = fixture.request.installDirectory / L"update-manifest.json.sig";
    Check(Code(InstallUpdate(inside, fixture.signer.key), "UPDATE_INPUT_INSIDE_INSTALL") && fixture.Unchanged(),
        "signed baseline input inside rename tree must first be copied outside");
}

void CommitAndCleanupTests() {
    Fixture fixture;
    auto first = InstallUpdate(fixture.request, fixture.signer.key);
    Diagnostics(first, "first commit");
    Check(first.Succeeded() && first.value->outcome == UpdateInstallOutcome::Committed && first.value->healthVerified,
        "actual test process bound health pipe commits signed mirror");
    if (!first.Succeeded() || first.value->outcome != UpdateInstallOutcome::Committed) {
        if (first.value) std::cerr << "first commit outcome reason=" << first.value->reasonCode << ": " << first.value->reason << '\n';
        return;
    }
    WaitNatural(first.value->childProcess);
    Check(fixture.FileIs(L"program.txt", "new program 0.2.0"), "signed target bytes replace old program");
    fixture.Preserved("commit preserves all original user/sidecar/obsolete bytes");
    const auto backup = Detail::InspectUpdateTree(first.value->backupDirectory);
    Check(backup.Succeeded() && backup.value->sha256 == fixture.original.sha256, "whole original backup inventory retained");
    const auto baseline = DiscoverInstalledUpdateBaseline(fixture.request.installDirectory, "0.2.0", fixture.signer.key);
    Check(baseline.Succeeded() && baseline.value->transactionDirectory == first.value->transactionDirectory,
        "committed raw signed baseline supersedes old root sidecar");
    const auto recovered = RecoverUpdate(first.value->transactionDirectory, fixture.signer.key);
    Check(recovered.Succeeded() && recovered.value->outcome == UpdateInstallOutcome::Committed, "committed recovery is idempotent read validation");
    Write(first.value->backupDirectory / L"late-author-file.txt", "unknown backup user addition");
    fixture.Next("0.2.0", "0.3.0");
    auto second = InstallUpdate(fixture.request, fixture.signer.key);
    Diagnostics(second, "second commit");
    Check(second.Succeeded() && second.value->outcome == UpdateInstallOutcome::Committed, "second upgrade consumes authenticated old release baseline");
    if (!second.Succeeded() || second.value->outcome != UpdateInstallOutcome::Committed) return;
    WaitNatural(second.value->childProcess);
    fixture.Next("0.3.0", "0.4.0");
    auto third = InstallUpdate(fixture.request, fixture.signer.key);
    Diagnostics(third, "third commit");
    Check(third.Succeeded() && third.value->outcome == UpdateInstallOutcome::Committed, "third signed upgrade commits independently");
    if (!third.Succeeded() || third.value->outcome != UpdateInstallOutcome::Committed) return;
    WaitNatural(third.value->childProcess);
    const auto unknown = fixture.temp.path / L".audastra-update-author-backup";
    Write(unknown / L"original.txt", "author unrelated directory");
    const auto cleanup = CleanupUpdateBackups(fixture.request.installDirectory, fixture.signer.key);
    Diagnostics(cleanup, "cleanup");
    Check(cleanup.Succeeded() && cleanup.value->removedBackups == 1 && cleanup.value->retainedBackups == 1
        && cleanup.value->retainedUnknown >= 1, "next startup deletes only unchanged older owned backup and keeps most recent one");
    Check(std::filesystem::exists(first.value->backupDirectory / L"late-author-file.txt"), "changed old backup is retained");
    Check(!std::filesystem::exists(second.value->backupDirectory) && std::filesystem::exists(third.value->backupDirectory),
        "exact older backup removed while newest backup remains");
    Check(Read(unknown / L"original.txt") == Data("author unrelated directory"), "cleanup does not use a broad backup glob");
    Check(std::filesystem::exists(second.value->transactionDirectory / L"target-manifest.json"), "cleanup retains raw signed baseline metadata");
    fixture.Preserved("multiple upgrades preserve original user files and signed sidecars");
}

void CleanupRootReplacementTests() {
    Fixture fixture;
    const auto first = InstallUpdate(fixture.request, fixture.signer.key);
    Diagnostics(first, "cleanup replacement first commit");
    Check(first.Succeeded() && first.value->outcome == UpdateInstallOutcome::Committed,
        "cleanup replacement fixture first signed upgrade commits");
    if (!first.Succeeded() || first.value->outcome != UpdateInstallOutcome::Committed) return;
    WaitNatural(first.value->childProcess);
    fixture.Next("0.2.0", "0.3.0");
    const auto second = InstallUpdate(fixture.request, fixture.signer.key);
    Diagnostics(second, "cleanup replacement second commit");
    Check(second.Succeeded() && second.value->outcome == UpdateInstallOutcome::Committed,
        "cleanup replacement fixture second signed upgrade commits");
    if (!second.Succeeded() || second.value->outcome != UpdateInstallOutcome::Committed) return;
    WaitNatural(second.value->childProcess);
    const auto original = Detail::InspectUpdateTree(first.value->backupDirectory), current = Detail::InspectUpdateTree(fixture.request.installDirectory);
    const auto originalId = Detail::ReadRootIdentity(first.value->backupDirectory);
    if (!original.Succeeded() || !current.Succeeded() || !originalId.Succeeded()) throw std::runtime_error("fixture cleanup original inventory failed");
    const auto held = fixture.temp.path / L"verified-original-backup-held";
    Detail::BackupCleanupTestHooks hooks;
    int checkpoints = 0;
    hooks.beforeDelete = [&](const std::filesystem::path& candidate) {
        ++checkpoints;
        Check(candidate == first.value->backupDirectory, "cleanup seam runs after exact older backup candidate identity is validated");
        const auto moved = Detail::RenameOwnedRoot(candidate, held, *originalId.value);
        if (!moved.Succeeded() || !std::filesystem::create_directory(candidate)) throw std::runtime_error("fixture cleanup equal-bytes root replacement failed");
        const auto copied = Detail::CopyInventory(held, candidate, *original.value);
        const auto contents = Detail::InspectUpdateTree(candidate);
        const auto replacementId = Detail::ReadRootIdentity(candidate);
        Check(copied.Succeeded() && contents.Succeeded() && replacementId.Succeeded()
            && contents.value->sha256 == original.value->sha256 && *replacementId.value != *originalId.value,
            "cleanup candidate is replaced by actual equal-bytes independent native root");
        return Status::Ok();
    };
    const auto result = Detail::CleanupUpdateBackupsWithHooks(fixture.request.installDirectory, fixture.signer.key, hooks);
    Diagnostics(result, "cleanup replaced native root");
    Check(checkpoints == 1 && result.Succeeded() && result.value->removedBackups == 0 && result.value->retainedBackups == 1
        && result.value->retainedUnknown == 1, "cleanup refuses equal-bytes unknown native root after outer validation and keeps latest owned backup");
    const auto replacement = Detail::InspectUpdateTree(first.value->backupDirectory), heldContents = Detail::InspectUpdateTree(held),
        after = Detail::InspectUpdateTree(fixture.request.installDirectory);
    const auto heldId = Detail::ReadRootIdentity(held);
    Check(replacement.Succeeded() && heldContents.Succeeded() && after.Succeeded() && heldId.Succeeded()
        && replacement.value->sha256 == original.value->sha256 && heldContents.value->sha256 == original.value->sha256
        && after.value->sha256 == current.value->sha256 && *heldId.value == *originalId.value,
        "cleanup identity race refusal preserves every replacement/original/current byte and original root identity");
    Check(std::filesystem::exists(second.value->backupDirectory), "latest exact owned backup survives replacement refusal");
    const auto retry = CleanupUpdateBackups(fixture.request.installDirectory, fixture.signer.key);
    Check(retry.Succeeded() && retry.value->removedBackups == 0 && retry.value->retainedUnknown == 1,
        "normal cleanup remains fail-closed for replaced backup root on later startup");
    fixture.Preserved("cleanup identity race regression retains original author data");
}

void CleanupBusyLastFileTests() {
    Fixture fixture;
    fixture.temp.retain = true;
    const auto first = InstallUpdate(fixture.request, fixture.signer.key);
    Diagnostics(first, "cleanup busy-last first commit");
    Check(first.Succeeded() && first.value->outcome == UpdateInstallOutcome::Committed,
        "cleanup busy-last first signed upgrade commits");
    if (!first.Succeeded() || first.value->outcome != UpdateInstallOutcome::Committed) return;
    WaitNatural(first.value->childProcess);
    fixture.Next("0.2.0", "0.3.0");
    const auto second = InstallUpdate(fixture.request, fixture.signer.key);
    Diagnostics(second, "cleanup busy-last second commit");
    if (!second.Succeeded() || second.value->outcome != UpdateInstallOutcome::Committed)
        ReceiptDiagnostics(second, "cleanup busy-last second commit outcome");
    Check(second.Succeeded() && second.value->outcome == UpdateInstallOutcome::Committed,
        "cleanup busy-last second signed upgrade commits");
    if (!second.Succeeded() || second.value->outcome != UpdateInstallOutcome::Committed) return;
    WaitNatural(second.value->childProcess);

    const auto backup = Detail::InspectUpdateTree(first.value->backupDirectory);
    const auto latest = Detail::InspectUpdateTree(second.value->backupDirectory);
    const auto installed = Detail::InspectUpdateTree(fixture.request.installDirectory);
    const auto backupId = Detail::ReadRootIdentity(first.value->backupDirectory);
    if (!backup.Succeeded() || !latest.Succeeded() || !installed.Succeeded() || !backupId.Succeeded()
        || backup.value->files.size() < 2) throw std::runtime_error("fixture cleanup busy-last inventory failed");
    // 精确使用删除循环的同一Windows排序：末文件只读可盘点，但不共享DELETE。
    const auto firstPath = first.value->backupDirectory / std::filesystem::path(backup.value->files.begin()->first);
    const auto lastPath = first.value->backupDirectory / std::filesystem::path(backup.value->files.rbegin()->first);
    std::map<std::wstring, DWORD> originalAttributes;
    originalAttributes.emplace(L"", GetFileAttributesW(first.value->backupDirectory.c_str()));
    for (const auto& [relative, file] : backup.value->files) {
        (void)file;
        originalAttributes.emplace(relative, GetFileAttributesW((first.value->backupDirectory / relative).c_str()));
    }
    for (const auto& relative : backup.value->directories)
        originalAttributes.emplace(relative, GetFileAttributesW((first.value->backupDirectory / relative).c_str()));
    Check(std::none_of(originalAttributes.begin(), originalAttributes.end(), [](const auto& entry) {
        return entry.second == INVALID_FILE_ATTRIBUTES;
    }), "cleanup busy-last snapshots every original file and directory attribute");

    Detail::NativeHandle busyFile;
    Detail::BackupCleanupTestHooks hooks;
    int checkpoints = 0;
    hooks.beforeDelete = [&](const std::filesystem::path& candidate) {
        ++checkpoints;
        Check(candidate == first.value->backupDirectory, "cleanup busy-last hook binds exact older backup after outer validation");
        busyFile = Detail::NativeHandle(CreateFileW(lastPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!busyFile.Valid()) throw std::runtime_error("fixture cleanup busy-last native lock failed");
        const auto readable = Detail::InspectUpdateTree(candidate);
        Check(readable.Succeeded() && readable.value->sha256 == backup.value->sha256,
            "cleanup busy-last native lock still allows complete unchanged read inventory");
        return Status::Ok();
    };
    const auto cleaned = Detail::CleanupUpdateBackupsWithHooks(fixture.request.installDirectory, fixture.signer.key, hooks);
    Diagnostics(cleaned, "cleanup busy-last refusal");
    Check(checkpoints == 1 && cleaned.Succeeded() && cleaned.value->removedBackups == 0
        && cleaned.value->retainedBackups == 1 && cleaned.value->retainedUnknown == 1,
        "cleanup busy-last occupied older backup is refused while newest exact backup stays retained");
    Check(cleaned.value && cleaned.value->partiallyCleanedBackups == 0 && cleaned.value->partialBackupDirectories.empty()
        && cleaned.value->filesMarkedForDeletion == 0 && cleaned.value->directoriesMarkedForDeletion == 0
        && cleaned.value->fileAttributeChanges == 0,
        "cleanup busy-last preflight refusal reports zero accepted deletion and attribute operations");

    const auto after = Detail::InspectUpdateTree(first.value->backupDirectory);
    const auto latestAfter = Detail::InspectUpdateTree(second.value->backupDirectory);
    const auto installedAfter = Detail::InspectUpdateTree(fixture.request.installDirectory);
    const auto afterId = Detail::ReadRootIdentity(first.value->backupDirectory);
    const bool allBytes = after.Succeeded() && after.value->sha256 == backup.value->sha256;
    const bool allAttributes = std::all_of(originalAttributes.begin(), originalAttributes.end(), [&](const auto& entry) {
        return GetFileAttributesW((first.value->backupDirectory / entry.first).c_str()) == entry.second;
    });
    Check(allBytes, "cleanup busy-last refusal deletes zero files and preserves the complete original backup inventory");
    Check(std::filesystem::exists(firstPath), "cleanup busy-last refusal preserves the lexically first file before the locked last file");
    Check(allAttributes, "cleanup busy-last refusal preserves every original file and directory attribute");
    Check(afterId.Succeeded() && *afterId.value == *backupId.value, "cleanup busy-last refusal preserves the exact backup root identity");
    Check(latestAfter.Succeeded() && latestAfter.value->sha256 == latest.value->sha256
        && installedAfter.Succeeded() && installedAfter.value->sha256 == installed.value->sha256,
        "cleanup busy-last refusal preserves newest backup and current install completely");
    const auto lastHash = Sha256File(lastPath);
    Check(lastHash.Succeeded() && *lastHash.value == backup.value->files.rbegin()->second.sha256,
        "cleanup busy-last occupied file remains unchanged and readable");
    const auto receiptPath = fixture.temp.path / L"cleanup-busy-last.receipt.json";
    const Json receipt{{"format", "audastra-original-cleanup-busy-last-regression"}, {"fixture_only", true},
        {"fixture_retained", true}, {"fixture_directory", IO::PathUtf8(fixture.temp.path)},
        {"backup_directory", IO::PathUtf8(first.value->backupDirectory)}, {"hook_count", checkpoints},
        {"locked_file", IO::PathUtf8(lastPath)}, {"first_file_exists_after", std::filesystem::exists(firstPath)},
        {"before_file_count", backup.value->files.size()}, {"after_file_count", after.Succeeded() ? after.value->files.size() : 0},
        {"before_sha256", backup.value->sha256}, {"after_sha256", after.Succeeded() ? after.value->sha256 : ""},
        {"all_backup_bytes_preserved", allBytes}, {"all_backup_attributes_preserved", allAttributes},
        {"cleanup_succeeded", cleaned.Succeeded()},
        {"removed_backups", cleaned.Succeeded() ? cleaned.value->removedBackups : 0},
        {"retained_unknown", cleaned.Succeeded() ? cleaned.value->retainedUnknown : 0}};
    Write(receiptPath, receipt.dump(2) + "\n");
    std::cout << "cleanup-busy-last receipt=" << IO::PathUtf8(receiptPath) << " before_files=" << backup.value->files.size()
        << " after_files=" << (after.Succeeded() ? after.value->files.size() : 0) << " all_bytes=" << allBytes
        << " all_attributes=" << allAttributes << std::endl;
    busyFile = Detail::NativeHandle{};
    const auto retried = CleanupUpdateBackups(fixture.request.installDirectory, fixture.signer.key);
    Diagnostics(retried, "cleanup busy-last unlocked retry");
    Check(retried.Succeeded() && retried.value->removedBackups == 1 && retried.value->retainedBackups == 1
        && retried.value->retainedUnknown == 0 && retried.value->partiallyCleanedBackups == 0
        && retried.value->partialBackupDirectories.empty(),
        "cleanup busy-last normal retry after closing native busy handle deletes only older exact backup");
    const auto readonlyFiles = static_cast<std::size_t>(std::count_if(backup.value->files.begin(), backup.value->files.end(),
        [&](const auto& entry) { return (originalAttributes.at(entry.first) & FILE_ATTRIBUTE_READONLY) != 0; }));
    Check(retried.value && retried.value->filesMarkedForDeletion == backup.value->files.size()
        && retried.value->directoriesMarkedForDeletion == backup.value->directories.size() + 1
        && retried.value->fileAttributeChanges == readonlyFiles,
        "cleanup busy-last successful retry reports exact accepted file directory and readonly operations");
    Check(!std::filesystem::exists(first.value->backupDirectory), "cleanup busy-last successful retry confirms older backup root disappeared");
    const auto latestRetried = Detail::InspectUpdateTree(second.value->backupDirectory);
    const auto installedRetried = Detail::InspectUpdateTree(fixture.request.installDirectory);
    Check(latestRetried.Succeeded() && latestRetried.value->sha256 == latest.value->sha256
        && installedRetried.Succeeded() && installedRetried.value->sha256 == installed.value->sha256,
        "cleanup busy-last unlocked retry preserves newest backup and current install complete inventory");
    const auto retryReceiptPath = fixture.temp.path / L"cleanup-busy-last.retry.receipt.json";
    const Json retryReceipt{{"format", "audastra-original-cleanup-busy-last-retry"}, {"fixture_only", true},
        {"cleanup_succeeded", retried.Succeeded()}, {"older_root_removed", !std::filesystem::exists(first.value->backupDirectory)},
        {"removed_backups", retried.value ? retried.value->removedBackups : 0},
        {"retained_backups", retried.value ? retried.value->retainedBackups : 0},
        {"retained_unknown", retried.value ? retried.value->retainedUnknown : 0},
        {"partially_cleaned_backups", retried.value ? retried.value->partiallyCleanedBackups : 0},
        {"files_marked_for_deletion", retried.value ? retried.value->filesMarkedForDeletion : 0},
        {"directories_marked_for_deletion", retried.value ? retried.value->directoriesMarkedForDeletion : 0},
        {"file_attribute_changes", retried.value ? retried.value->fileAttributeChanges : 0},
        {"latest_sha256", latestRetried.Succeeded() ? latestRetried.value->sha256 : ""},
        {"install_sha256", installedRetried.Succeeded() ? installedRetried.value->sha256 : ""}};
    Write(retryReceiptPath, retryReceipt.dump(2) + "\n");
    std::cout << "cleanup-busy-last retry_receipt=" << IO::PathUtf8(retryReceiptPath) << std::endl;
    fixture.Preserved("cleanup busy-last retains original user bytes in active install");
}

void CleanupMutationTests() {
    for (const std::string_view scenario : {"preflight-insertion", "after-delete-insertion", "directory-busy", "directory-replacement"}) {
        Fixture fixture;
        fixture.temp.retain = true;
        const auto first = InstallUpdate(fixture.request, fixture.signer.key);
        Diagnostics(first, "cleanup mutation first commit");
        Check(first.Succeeded() && first.value->outcome == UpdateInstallOutcome::Committed,
            "cleanup mutation first signed upgrade commits");
        if (!first.Succeeded() || first.value->outcome != UpdateInstallOutcome::Committed) continue;
        WaitNatural(first.value->childProcess);
        fixture.Next("0.2.0", "0.3.0");
        const auto second = InstallUpdate(fixture.request, fixture.signer.key);
        Diagnostics(second, "cleanup mutation second commit");
        Check(second.Succeeded() && second.value->outcome == UpdateInstallOutcome::Committed,
            "cleanup mutation second signed upgrade commits");
        if (!second.Succeeded() || second.value->outcome != UpdateInstallOutcome::Committed) continue;
        WaitNatural(second.value->childProcess);
        const auto backup = Detail::InspectUpdateTree(first.value->backupDirectory);
        const auto latest = Detail::InspectUpdateTree(second.value->backupDirectory);
        const auto installed = Detail::InspectUpdateTree(fixture.request.installDirectory);
        const auto rootId = Detail::ReadRootIdentity(first.value->backupDirectory);
        if (!backup.Succeeded() || !latest.Succeeded() || !installed.Succeeded() || !rootId.Succeeded()
            || backup.value->files.empty() || backup.value->directories.empty())
            throw std::runtime_error("fixture cleanup mutation inventory failed");
        std::vector<std::wstring> directoryOrder(backup.value->directories.begin(), backup.value->directories.end());
        std::sort(directoryOrder.begin(), directoryOrder.end(), [](const auto& a, const auto& b) {
            return a.size() != b.size() ? a.size() > b.size() : Detail::WindowsPathLess{}(a, b);
        });
        const auto directoryPath = first.value->backupDirectory / directoryOrder.front();
        const auto directoryId = Detail::ReadRootIdentity(directoryPath);
        if (!directoryId.Succeeded() || !std::filesystem::is_empty(directoryPath))
            throw std::runtime_error("fixture cleanup mutation empty leaf identity failed");
        const auto firstFilePath = first.value->backupDirectory / backup.value->files.begin()->first;
        const auto lateFile = directoryPath / L"late-original-author.txt";
        const std::string lateBytes = "original unknown author bytes inserted during cleanup " + std::string(scenario);
        const auto heldDirectory = fixture.temp.path / L"original-cleanup-leaf-held";
        std::map<std::wstring, DWORD> originalAttributes;
        originalAttributes.emplace(L"", GetFileAttributesW(first.value->backupDirectory.c_str()));
        for (const auto& [relative, file] : backup.value->files) {
            (void)file;
            originalAttributes.emplace(relative, GetFileAttributesW((first.value->backupDirectory / relative).c_str()));
        }
        for (const auto& relative : backup.value->directories)
            originalAttributes.emplace(relative, GetFileAttributesW((first.value->backupDirectory / relative).c_str()));
        Check(std::none_of(originalAttributes.begin(), originalAttributes.end(), [](const auto& entry) {
            return entry.second == INVALID_FILE_ATTRIBUTES;
        }), "cleanup mutation snapshots every original native attribute before any checkpoint");
        Detail::NativeHandle busyDirectory;
        Detail::BackupCleanupTestHooks hooks;
        int checkpointCount = 0;
        bool injected = false;
        bool nativeRenameAttempted = false, nativeRenameCompleted = false;
        DWORD nativeRenameError = ERROR_SUCCESS, injectionError = ERROR_SUCCESS;
        std::string injectionFailureCode;
        hooks.checkpoint = [&](std::string_view phase, const std::filesystem::path& root, const std::filesystem::path& path) {
            ++checkpointCount;
            Check(root == first.value->backupDirectory, "cleanup mutation checkpoint is confined to exact original older backup");
            if (injected) return Status::Ok();
            if (scenario == "preflight-insertion" && phase == "preflight-complete") {
                Write(lateFile, lateBytes);
                injected = true;
            } else if ((scenario == "after-delete-insertion" || scenario == "directory-busy") && phase == "file-delete-accepted") {
                Check(path == firstFilePath && !std::filesystem::exists(path),
                    "cleanup mutation injects only after exact first file deletion accepted and own handle closed");
                if (scenario == "after-delete-insertion") {
                    Write(lateFile, lateBytes);
                } else {
                    busyDirectory = Detail::NativeHandle(CreateFileW(directoryPath.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                    if (!busyDirectory.Valid()) throw std::runtime_error("fixture cleanup mutation native directory lease failed");
                }
                injected = true;
            } else if (scenario == "directory-replacement" && phase == "directory-lease-released" && path == directoryPath) {
                const auto refuse = [&](std::string code, DWORD error) {
                    injectionFailureCode = std::move(code);
                    injectionError = error;
                    return Status::Failure("OriginalFixture", injectionFailureCode,
                        "原创目录替换actor未完成，Win32=" + std::to_string(error));
                };
                const auto fixtureId = Detail::ReadRootIdentity(fixture.temp.path);
                if (!Detail::Within(directoryPath, first.value->backupDirectory) || !Detail::Within(heldDirectory, fixture.temp.path)
                    || Detail::Within(heldDirectory, first.value->backupDirectory) || heldDirectory.parent_path() != fixture.temp.path
                    || !fixtureId.Succeeded() || *fixtureId.value != fixture.temp.owned
                    || directoryId.value->volume != fixture.temp.owned.volume)
                    return refuse("FIXTURE_RENAME_SCOPE_INVALID", ERROR_INVALID_PARAMETER);
                // 竞争actor只移动独占夹具的精确空叶；产品helper仅允许同parent，不能用它模拟跨parent外部actor。
                Detail::NativeHandle source(CreateFileW(directoryPath.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (!source.Valid()) return refuse("FIXTURE_RENAME_SOURCE_OPEN_FAILED", GetLastError());
                const auto safe = Detail::CheckOpenedPath(source.Get(), directoryPath, true);
                BY_HANDLE_FILE_INFORMATION info{};
                if (!safe.Succeeded() || !GetFileInformationByHandle(source.Get(), &info))
                    return refuse("FIXTURE_RENAME_SOURCE_IDENTITY_FAILED", ERROR_INVALID_DATA);
                const Detail::RootIdentity sourceId{info.dwVolumeSerialNumber,
                    (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
                if (sourceId != *directoryId.value || !std::filesystem::is_empty(directoryPath))
                    return refuse("FIXTURE_RENAME_SOURCE_CHANGED", ERROR_INVALID_DATA);
                if (GetFileAttributesW(heldDirectory.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return refuse("FIXTURE_RENAME_DESTINATION_EXISTS", ERROR_ALREADY_EXISTS);
                const auto destinationError = GetLastError();
                if (destinationError != ERROR_FILE_NOT_FOUND)
                    return refuse("FIXTURE_RENAME_DESTINATION_UNKNOWN", destinationError);
                const auto name = heldDirectory.native();
                std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
                auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
                rename->ReplaceIfExists = FALSE; // 原子CREATE_NEW语义，目标在检查后出现也绝不覆盖。
                rename->RootDirectory = nullptr;
                rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
                std::copy(name.begin(), name.end(), rename->FileName);
                nativeRenameAttempted = true;
                if (!SetFileInformationByHandle(source.Get(), FileRenameInfo, rename, static_cast<DWORD>(buffer.size()))) {
                    nativeRenameError = GetLastError();
                    return refuse("FIXTURE_NATIVE_RENAME_FAILED", nativeRenameError);
                }
                nativeRenameCompleted = true;
                const auto renamed = Detail::CheckOpenedPath(source.Get(), heldDirectory, true);
                if (!renamed.Succeeded() || GetFileAttributesW(directoryPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return refuse("FIXTURE_NATIVE_RENAME_VERIFY_FAILED", ERROR_INVALID_DATA);
                source = Detail::NativeHandle{};
                if (!CreateDirectoryW(directoryPath.c_str(), nullptr))
                    return refuse("FIXTURE_REPLACEMENT_CREATE_FAILED", GetLastError());
                injected = true;
            }
            return Status::Ok();
        };
        const auto result = Detail::CleanupUpdateBackupsWithHooks(fixture.request.installDirectory, fixture.signer.key, hooks);
        Diagnostics(result, "cleanup mutation result");
        Check(injected && checkpointCount > 0, "cleanup mutation deterministically reached the requested native checkpoint");
        const bool preflight = scenario == "preflight-insertion";
        if (preflight) {
            Check(result.Succeeded() && result.value->removedBackups == 0 && result.value->retainedBackups == 1
                && result.value->retainedUnknown == 1 && result.value->partiallyCleanedBackups == 0
                && result.value->partialBackupDirectories.empty(),
                "cleanup mutation before deletion is unknown retained and never counted as partial");
            Check(result.value && result.value->filesMarkedForDeletion == 0 && result.value->directoriesMarkedForDeletion == 0
                && result.value->fileAttributeChanges == 0,
                "cleanup mutation preflight insertion performs zero native deletion or attribute operations");
            for (const auto& [relative, file] : backup.value->files) {
                const auto hash = Sha256File(first.value->backupDirectory / relative);
                Check(hash.Succeeded() && *hash.value == file.sha256, "cleanup mutation preflight insertion preserves every original file SHA");
            }
            Check(std::all_of(originalAttributes.begin(), originalAttributes.end(), [&](const auto& entry) {
                return GetFileAttributesW((first.value->backupDirectory / entry.first).c_str()) == entry.second;
            }), "cleanup mutation preflight insertion preserves every original file and directory attribute");
        } else {
            Check(Code(result, "UPDATE_CLEANUP_PARTIAL") && result.value && result.value->removedBackups == 0
                && result.value->retainedBackups == 1 && result.value->retainedUnknown == 0
                && result.value->partiallyCleanedBackups == 1
                && result.value->partialBackupDirectories == std::vector<std::filesystem::path>{first.value->backupDirectory},
                "cleanup mutation after native deletion returns Error plus exact partial receipt without unknown retained misclassification");
            const auto readonlyFiles = static_cast<std::size_t>(std::count_if(backup.value->files.begin(), backup.value->files.end(),
                [&](const auto& entry) { return (originalAttributes.at(entry.first) & FILE_ATTRIBUTE_READONLY) != 0; }));
            Check(result.value && result.value->filesMarkedForDeletion == backup.value->files.size()
                && result.value->directoriesMarkedForDeletion == 0 && result.value->fileAttributeChanges == readonlyFiles,
                "cleanup mutation reports all accepted file deletes but zero accepted directory deletes after exact first-directory conflict");
            Check(!std::filesystem::exists(firstFilePath), "cleanup mutation partial receipt corresponds to actual earlier file disappearance");
            for (const auto& [relative, file] : backup.value->files) {
                (void)file;
                Check(!std::filesystem::exists(first.value->backupDirectory / relative),
                    "cleanup mutation deletion counts correspond to all original files absent");
            }
        }
        const auto remaining = Detail::InspectUpdateTree(first.value->backupDirectory);
        const auto afterRootId = Detail::ReadRootIdentity(first.value->backupDirectory);
        Check(remaining.Succeeded() && afterRootId.Succeeded() && *afterRootId.value == *rootId.value,
            "cleanup mutation keeps remaining exact original backup root and readable evidence");
        Check(remaining.Succeeded() && remaining.value->directories == backup.value->directories,
            "cleanup mutation first-directory refusal preserves the complete original directory name set");
        if (scenario == "preflight-insertion" || scenario == "after-delete-insertion") {
            const auto lateHash = Sha256File(lateFile);
            Check(lateHash.Succeeded() && *lateHash.value == Hash(lateBytes),
                "cleanup mutation unknown author file inserted at requested phase remains byte-identical");
            Check(remaining.Succeeded() && remaining.value->files.size() == (preflight ? backup.value->files.size() + 1 : 1),
                "cleanup mutation remaining inventory matches original-files-plus-insertion or only-insertion exactly");
        } else if (scenario == "directory-replacement") {
            const auto replacementId = Detail::ReadRootIdentity(directoryPath);
            const auto heldId = Detail::ReadRootIdentity(heldDirectory);
            Check(replacementId.Succeeded() && *replacementId.value != *directoryId.value
                && heldId.Succeeded() && *heldId.value == *directoryId.value
                && std::filesystem::is_empty(directoryPath) && std::filesystem::is_empty(heldDirectory),
                "cleanup mutation refuses same-bytes replacement leaf and preserves both unknown and original native identities");
        } else {
            Check(busyDirectory.Valid(), "cleanup mutation preserves externally held directory and never closes foreign leases");
        }
        busyDirectory = Detail::NativeHandle{};
        const auto latestAfter = Detail::InspectUpdateTree(second.value->backupDirectory);
        const auto installedAfter = Detail::InspectUpdateTree(fixture.request.installDirectory);
        Check(latestAfter.Succeeded() && latestAfter.value->sha256 == latest.value->sha256
            && installedAfter.Succeeded() && installedAfter.value->sha256 == installed.value->sha256,
            "cleanup mutation preserves newest backup and current installed tree complete inventory");
        const auto retry = CleanupUpdateBackups(fixture.request.installDirectory, fixture.signer.key);
        Check(retry.Succeeded() && retry.value->removedBackups == 0 && retry.value->retainedBackups == 1
            && retry.value->retainedUnknown == 1 && retry.value->partiallyCleanedBackups == 0
            && retry.value->filesMarkedForDeletion == 0 && retry.value->directoriesMarkedForDeletion == 0
            && retry.value->fileAttributeChanges == 0 && retry.value->partialBackupDirectories.empty(),
            "cleanup mutation subsequent startup retains changed remainder and does not invent another partial operation");
        const auto retriedRemaining = Detail::InspectUpdateTree(first.value->backupDirectory);
        Check(remaining.Succeeded() && retriedRemaining.Succeeded() && retriedRemaining.value->sha256 == remaining.value->sha256,
            "cleanup mutation retry leaves all remaining evidence byte-identical");
        Json receipt{{"format", "audastra-original-cleanup-mutation-regression"}, {"fixture_only", true}, {"fixture_retained", true},
            {"scenario", scenario}, {"fixture_directory", IO::PathUtf8(fixture.temp.path)},
            {"backup_directory", IO::PathUtf8(first.value->backupDirectory)}, {"checkpoint_count", checkpointCount},
            {"injected", injected}, {"cleanup_succeeded", result.Succeeded()},
            {"native_rename_attempted", nativeRenameAttempted}, {"native_rename_completed", nativeRenameCompleted},
            {"native_rename_error", nativeRenameError}, {"injection_error", injectionError}, {"injection_failure_code", injectionFailureCode},
            {"before_sha256", backup.value->sha256}, {"remaining_sha256", remaining.Succeeded() ? remaining.value->sha256 : ""},
            {"before_files", backup.value->files.size()}, {"remaining_files", remaining.Succeeded() ? remaining.value->files.size() : 0},
            {"diagnostics", Json::array()}, {"partial_backup_directories", Json::array()}};
        for (const auto& diagnostic : result.diagnostics)
            receipt["diagnostics"].push_back({{"code", diagnostic.code}, {"message", diagnostic.message}});
        if (result.value) {
            receipt["removed_backups"] = result.value->removedBackups;
            receipt["retained_backups"] = result.value->retainedBackups;
            receipt["retained_unknown"] = result.value->retainedUnknown;
            receipt["partially_cleaned_backups"] = result.value->partiallyCleanedBackups;
            receipt["files_marked_for_deletion"] = result.value->filesMarkedForDeletion;
            receipt["directories_marked_for_deletion"] = result.value->directoriesMarkedForDeletion;
            receipt["file_attribute_changes"] = result.value->fileAttributeChanges;
            for (const auto& path : result.value->partialBackupDirectories)
                receipt["partial_backup_directories"].push_back(IO::PathUtf8(path));
        }
        const auto receiptPath = fixture.temp.path / ("cleanup-" + std::string(scenario) + ".receipt.json");
        Write(receiptPath, receipt.dump(2) + "\n");
        std::cout << "cleanup-mutation scenario=" << scenario << " receipt=" << IO::PathUtf8(receiptPath)
            << " succeeded=" << result.Succeeded() << " partial=" << (result.value ? result.value->partiallyCleanedBackups : 0)
            << " delete_marks=" << (result.value ? result.value->filesMarkedForDeletion : 0) << std::endl;
        fixture.Preserved("cleanup mutation retains original active author data");
    }
}

void RenameAndRecoveryTests() {
    {
        Fixture fixture;
        Detail::NativeHandle stageLock;
        Detail::InstallerTestHooks hooks;
        hooks.checkpoint = [&](std::string_view phase) {
            if (phase == "before-activation") {
                for (const auto& entry : std::filesystem::directory_iterator(fixture.temp.path)) {
                    if (entry.path().filename().native().ends_with(L".next")) stageLock = Detail::NativeHandle(CreateFileW(entry.path().c_str(),
                        FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                }
            }
            return Status::Ok();
        };
        const auto result = Detail::InstallUpdateWithHooks(fixture.request, fixture.signer.key, hooks);
        Diagnostics(result, "second rename locked");
        if (!result.Succeeded() || result.value->outcome != UpdateInstallOutcome::RolledBack) ReceiptDiagnostics(result, "second rename locked receipt");
        Check(stageLock.Valid(), "exclusive-delete-share stage directory lease acquired");
        Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RolledBack,
            "actual second native rename failure restores whole old tree");
        stageLock = Detail::NativeHandle{};
        Check(fixture.Unchanged(), "failed second rename preserves exact original inventory");
    }
    for (const auto* interruptedPhase : {"after-old-rename", "after-activation"}) {
        Fixture fixture;
        Detail::InstallerTestHooks hooks;
        hooks.leaveForRecovery = true;
        hooks.checkpoint = [=](std::string_view phase) {
            return phase == interruptedPhase ? Status::Failure("UpdateTest", "UPDATE_TEST_INTERRUPTED", "原创夹具模拟持久阶段后退出。") : Status::Ok();
        };
        const auto stopped = Detail::InstallUpdateWithHooks(fixture.request, fixture.signer.key, hooks);
        Check(Code(stopped, "UPDATE_TEST_INTERRUPTED"), "original fixture stops after durable rename boundary");
        std::filesystem::path transaction;
        for (const auto& entry : std::filesystem::directory_iterator(fixture.temp.path))
            if (entry.path().filename().native().ends_with(L".transaction")) transaction = entry.path();
        Check(!transaction.empty(), "durable owned transaction discovered after interrupted fixture");
        const auto recovered = RecoverUpdate(transaction, fixture.signer.key);
        Diagnostics(recovered, interruptedPhase);
        Check(recovered.Succeeded() && recovered.value->outcome == UpdateInstallOutcome::RolledBack,
            "recover resolves interrupted install rename boundary");
        Check(fixture.FileIs(L"program.txt", "old program"), "recovery restores old signed program bytes");
        fixture.Preserved("recovery preserves original user bytes and empty dirs");
    }
    {
        Fixture fixture;
        Detail::NativeHandle backupLock;
        Detail::InstallerTestHooks hooks;
        hooks.checkpoint = [&](std::string_view phase) {
            if (phase == "after-old-rename") {
                for (const auto& entry : std::filesystem::directory_iterator(fixture.temp.path)) {
                    if (entry.path().filename().native().ends_with(L".backup")) backupLock = Detail::NativeHandle(CreateFileW(entry.path().c_str(),
                        FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                }
                return Status::Failure("UpdateTest", "UPDATE_TEST_RENAME_FAILURE", "原创恢复锁夹具。");
            }
            return Status::Ok();
        };
        const auto pending = Detail::InstallUpdateWithHooks(fixture.request, fixture.signer.key, hooks);
        if (!pending.Succeeded() || pending.value->outcome != UpdateInstallOutcome::RollbackPending) ReceiptDiagnostics(pending, "backup restore lock receipt");
        Check(pending.Succeeded() && pending.value->outcome == UpdateInstallOutcome::RollbackPending,
            "actual rollback rename failure is reported pending without fake restoration");
        if (!pending.Succeeded()) return;
        Check(!std::filesystem::exists(fixture.request.installDirectory) && std::filesystem::exists(pending.value->backupDirectory),
            "pending restoration preserves old backup while install path is absent");
        backupLock = Detail::NativeHandle{};
        const auto recovered = RecoverUpdate(pending.value->transactionDirectory, fixture.signer.key);
        Check(recovered.Succeeded() && recovered.value->outcome == UpdateInstallOutcome::RolledBack && fixture.Unchanged(),
            "recover retries after original lock naturally released");
    }
}

Detail::InstallJournal InterruptAfterRollbackActivation(Fixture& fixture,
    std::string_view stoppedPhase = "after-rollback-activation") {
    Detail::InstallerTestHooks initial;
    initial.leaveForRecovery = true;
    initial.checkpoint = [](std::string_view phase) {
        return phase == "after-activation"
            ? Status::Failure("UpdateTest", "UPDATE_TEST_INTERRUPTED", "原创夹具在新树激活后退出。") : Status::Ok();
    };
    const auto activated = Detail::InstallUpdateWithHooks(fixture.request, fixture.signer.key, initial);
    Check(Code(activated, "UPDATE_TEST_INTERRUPTED"), "original fixture interrupts activated tree before health launch");
    std::filesystem::path transaction;
    for (const auto& entry : std::filesystem::directory_iterator(fixture.temp.path))
        if (entry.path().filename().native().ends_with(L".transaction")) transaction = entry.path();
    auto journal = Detail::ReadJournal(transaction);
    auto target = Detail::ReadSignedInput(transaction / L"target-manifest.json", transaction / L"target-manifest.sig", fixture.signer.key);
    auto baseline = Detail::ReadSignedInput(transaction / L"baseline-manifest.json", transaction / L"baseline-manifest.sig", fixture.signer.key);
    auto mutex = Detail::AcquireInstallMutex(fixture.request.installDirectory);
    if (!journal.Succeeded() || !target.Succeeded() || !baseline.Succeeded() || !mutex.Succeeded())
        throw std::runtime_error("fixture frozen rollback inputs failed");
    Detail::InstallerTestHooks rollback;
    rollback.leaveForRecovery = true;
    rollback.checkpoint = [=](std::string_view phase) {
        return phase == stoppedPhase
            ? Status::Failure("UpdateTest", "UPDATE_TEST_ROLLBACK_INTERRUPTED", "原创夹具在最后回滚改名后、终态日志前退出。") : Status::Ok();
    };
    const auto stopped = Detail::RollbackJournal(*journal.value, target.value->verified, &baseline.value->verified, rollback);
    if (!Code(stopped, "UPDATE_TEST_ROLLBACK_INTERRUPTED")) Diagnostics(stopped, "original rollback boundary");
    if (stoppedPhase == "after-rollback-current-rename")
        Check(Code(stopped, "UPDATE_TEST_ROLLBACK_INTERRUPTED"), "actual current-to-failed rollback rename completes before simulated interruption");
    else Check(Code(stopped, "UPDATE_TEST_ROLLBACK_INTERRUPTED"), "actual final rollback rename completes before simulated interruption");
    auto persisted = Detail::ReadJournal(transaction);
    if (!persisted.Succeeded()) throw std::runtime_error("fixture persisted rollback journal failed");
    if (stoppedPhase == "after-rollback-current-rename") {
        Check(persisted.value->phase == "rollback-current-renamed" && !std::filesystem::exists(persisted.value->install),
            "rollback-current-renamed durable boundary retains absent install path");
        const auto failed = Detail::ReadRootIdentity(persisted.value->failed), backup = Detail::ReadRootIdentity(persisted.value->backup),
            prepared = Detail::ReadRootIdentity(persisted.value->rollback);
        Check(failed.Succeeded() && backup.Succeeded() && prepared.Succeeded() && *failed.value == persisted.value->nextId
            && *backup.value == persisted.value->originalId && *prepared.value == persisted.value->rollbackId,
            "between rollback renames all three prepared/failed/backup native identities remain exact");
        return *persisted.value;
    }
    Check(persisted.value->phase == "rollback-activate-pending", "last durable journal remains rollback-activate-pending");
    const auto installed = Detail::ReadRootIdentity(persisted.value->install), failed = Detail::ReadRootIdentity(persisted.value->failed),
        backup = Detail::ReadRootIdentity(persisted.value->backup);
    Check(installed.Succeeded() && failed.Succeeded() && backup.Succeeded()
        && *installed.value == persisted.value->rollbackId && *failed.value == persisted.value->nextId
        && *backup.value == persisted.value->originalId, "interrupted final rename has exact three owned native root identities");
    return *persisted.value;
}

void RollbackTerminalBoundaryTests() {
    {
        Fixture fixture;
        const auto journal = InterruptAfterRollbackActivation(fixture);
        const auto recovered = RecoverUpdate(journal.transaction, fixture.signer.key);
        Diagnostics(recovered, "final rollback rename recovery");
        Check(recovered.Succeeded() && recovered.value->outcome == UpdateInstallOutcome::RolledBack && recovered.value->userFilesPreserved,
            "public recover proves completed final rollback rename from exact identities and three full inventories");
        const auto finalJournal = Detail::ReadJournal(journal.transaction);
        Check(finalJournal.Succeeded() && finalJournal.value->phase == "rolled-back", "inferred restored terminal state is durably recorded");
        Check(fixture.FileIs(L"program.txt", "old program"), "terminal boundary restores signed original program");
        fixture.Preserved("terminal boundary preserves original unsigned and sidecar user bytes");
        const auto again = RecoverUpdate(journal.transaction, fixture.signer.key);
        Check(again.Succeeded() && again.value->outcome == UpdateInstallOutcome::RolledBack && !again.value->oldVersionRestarted,
            "durable restored terminal recovery is idempotent and does not restart again");
    }
    for (const auto* changed : {"install-content", "backup-content", "failed-content", "install-id", "backup-id", "failed-id"}) {
        Fixture fixture;
        const auto journal = InterruptAfterRollbackActivation(fixture);
        const std::string_view scenario(changed);
        const auto root = scenario.starts_with("install") ? journal.install : scenario.starts_with("backup") ? journal.backup : journal.failed;
        if (scenario.ends_with("-content")) {
            Write(root / L"settings.json", std::string("late author bytes at ") + changed);
        } else {
            const auto contents = Detail::InspectUpdateTree(root);
            const auto identity = Detail::ReadRootIdentity(root);
            if (!contents.Succeeded() || !identity.Succeeded()) throw std::runtime_error("fixture replacement identity failed");
            const auto held = fixture.temp.path / L"original-owned-root-held";
            const auto moved = Detail::RenameOwnedRoot(root, held, *identity.value);
            if (!moved.Succeeded() || !std::filesystem::create_directory(root)) throw std::runtime_error("fixture independent replacement root failed");
            const auto copied = Detail::CopyInventory(held, root, *contents.value);
            const auto replacement = Detail::ReadRootIdentity(root);
            const auto sameContents = Detail::InspectUpdateTree(root);
            Check(copied.Succeeded() && replacement.Succeeded() && sameContents.Succeeded()
                && *replacement.value != *identity.value && sameContents.value->sha256 == contents.value->sha256,
                "independent root with equal bytes cannot impersonate native owned identity");
        }
        const auto beforeInstall = Detail::InspectUpdateTree(journal.install), beforeBackup = Detail::InspectUpdateTree(journal.backup),
            beforeFailed = Detail::InspectUpdateTree(journal.failed);
        const auto beforeInstallId = Detail::ReadRootIdentity(journal.install), beforeBackupId = Detail::ReadRootIdentity(journal.backup),
            beforeFailedId = Detail::ReadRootIdentity(journal.failed);
        const auto result = RecoverUpdate(journal.transaction, fixture.signer.key);
        Diagnostics(result, changed);
        Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RollbackPending && !result.value->oldVersionRestarted,
            "unknown identity or any changed full inventory prevents terminal rollback inference");
        const auto afterInstall = Detail::InspectUpdateTree(journal.install), afterBackup = Detail::InspectUpdateTree(journal.backup),
            afterFailed = Detail::InspectUpdateTree(journal.failed);
        const auto afterInstallId = Detail::ReadRootIdentity(journal.install), afterBackupId = Detail::ReadRootIdentity(journal.backup),
            afterFailedId = Detail::ReadRootIdentity(journal.failed);
        Check(beforeInstall.Succeeded() && beforeBackup.Succeeded() && beforeFailed.Succeeded()
            && afterInstall.Succeeded() && afterBackup.Succeeded() && afterFailed.Succeeded()
            && beforeInstall.value->sha256 == afterInstall.value->sha256 && beforeBackup.value->sha256 == afterBackup.value->sha256
            && beforeFailed.value->sha256 == afterFailed.value->sha256 && beforeInstallId.Succeeded() && beforeBackupId.Succeeded()
            && beforeFailedId.Succeeded() && afterInstallId.Succeeded() && afterBackupId.Succeeded() && afterFailedId.Succeeded()
            && *beforeInstallId.value == *afterInstallId.value && *beforeBackupId.value == *afterBackupId.value
            && *beforeFailedId.value == *afterFailedId.value, "pending terminal refusal preserves all three tree inventories and native identities");
    }
}

void BetweenRollbackRenamesTests() {
    {
        Fixture fixture;
        const auto journal = InterruptAfterRollbackActivation(fixture, "after-rollback-current-rename");
        const auto beforeBackup = Detail::InspectUpdateTree(journal.backup), beforeFailed = Detail::InspectUpdateTree(journal.failed),
            beforePrepared = Detail::InspectUpdateTree(journal.rollback);
        WaitingInstance reopened(journal.failed / L"app.exe");
        Check(reopened.process.Valid(), "actual failed-root fixture instance starts in between-rename recovery gap");
        if (!reopened.process.Valid()) return;
        const auto pending = RecoverUpdate(journal.transaction, fixture.signer.key);
        ReceiptDiagnostics(pending, "failed-root still running receipt");
        Check(pending.Succeeded() && pending.value->outcome == UpdateInstallOutcome::RollbackPending && !pending.value->oldVersionRestarted
            && pending.value->reasonCode == "UPDATE_CHILD_STILL_RUNNING" && !std::filesystem::exists(journal.install),
            "between-rename recovery refuses activation while actual failed-root instance is alive");
        const auto backup = Detail::InspectUpdateTree(journal.backup), failed = Detail::InspectUpdateTree(journal.failed),
            prepared = Detail::InspectUpdateTree(journal.rollback);
        const auto backupId = Detail::ReadRootIdentity(journal.backup), failedId = Detail::ReadRootIdentity(journal.failed),
            preparedId = Detail::ReadRootIdentity(journal.rollback);
        Check(beforeBackup.Succeeded() && beforeFailed.Succeeded() && beforePrepared.Succeeded() && backup.Succeeded() && failed.Succeeded()
            && prepared.Succeeded() && backup.value->sha256 == beforeBackup.value->sha256 && failed.value->sha256 == beforeFailed.value->sha256
            && prepared.value->sha256 == beforePrepared.value->sha256 && backupId.Succeeded() && failedId.Succeeded() && preparedId.Succeeded()
            && *backupId.value == journal.originalId && *failedId.value == journal.nextId && *preparedId.value == journal.rollbackId,
            "running failed-root refusal preserves all three full inventories and native identities");
        Check(reopened.Running(), "failed-root fixture remains alive after refused recovery without termination");
        Check(reopened.ReleaseNaturally(), "failed-root fixture exits only via its original owner's own release event");
        const auto recovered = RecoverUpdate(journal.transaction, fixture.signer.key);
        Diagnostics(recovered, "failed-root naturally exited recover");
        Check(recovered.Succeeded() && recovered.value->outcome == UpdateInstallOutcome::RolledBack && recovered.value->userFilesPreserved
            && fixture.FileIs(L"program.txt", "old program"), "between-rename recovery succeeds after failed-root instance naturally exits");
        fixture.Preserved("between-rename process gate recovery retains original author files");
    }
    {
        Fixture fixture;
        const auto journal = InterruptAfterRollbackActivation(fixture, "after-rollback-current-rename");
        Write(journal.backup / L"settings.json", "late backup author bytes at interrupted rollback");
        const auto beforeBackup = Detail::InspectUpdateTree(journal.backup), beforeFailed = Detail::InspectUpdateTree(journal.failed),
            beforePrepared = Detail::InspectUpdateTree(journal.rollback);
        const auto pending = RecoverUpdate(journal.transaction, fixture.signer.key);
        ReceiptDiagnostics(pending, "changed original backup receipt");
        Check(pending.Succeeded() && pending.value->outcome == UpdateInstallOutcome::RollbackPending && !pending.value->oldVersionRestarted
            && pending.value->reasonCode == "UPDATE_ROLLBACK_CONTENT_UNKNOWN" && !std::filesystem::exists(journal.install),
            "between-rename recovery refuses changed original backup before mirror activation");
        const auto backup = Detail::InspectUpdateTree(journal.backup), failed = Detail::InspectUpdateTree(journal.failed),
            prepared = Detail::InspectUpdateTree(journal.rollback);
        Check(beforeBackup.Succeeded() && beforeFailed.Succeeded() && beforePrepared.Succeeded() && backup.Succeeded() && failed.Succeeded()
            && prepared.Succeeded() && backup.value->sha256 == beforeBackup.value->sha256 && failed.value->sha256 == beforeFailed.value->sha256
            && prepared.value->sha256 == beforePrepared.value->sha256, "changed backup refusal keeps every byte of the three owned recovery trees");
        Write(journal.backup / L"settings.json", "original settings");
        const auto recovered = RecoverUpdate(journal.transaction, fixture.signer.key);
        Check(recovered.Succeeded() && recovered.value->outcome == UpdateInstallOutcome::RolledBack && fixture.FileIs(L"program.txt", "old program"),
            "between-rename recovery can continue once fixture restores its own exact original backup bytes");
        fixture.Preserved("between-rename backup gate recovery retains original author files");
    }
}

void OldRestartGuardsTests() {
    {
        Fixture fixture;
        const auto journal = InterruptAfterRollbackActivation(fixture);
        WaitingInstance author(journal.install / L"app.exe");
        Check(author.process.Valid(), "original author-reopened old-version fixture process is created");
        if (!author.process.Valid()) return;
        const auto result = RecoverUpdate(journal.transaction, fixture.signer.key);
        Diagnostics(result, "existing old version recover");
        Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RolledBack && !result.value->oldVersionRestarted
            && result.value->reason.find("UPDATE_OLD_RESTART_ALREADY_RUNNING") != std::string::npos,
            "receipt explains skipped restart while an actual instance in the same install root is running");
        Check(author.Running(), "author-reopened fixture remains alive and is never terminated");
        Check(author.ReleaseNaturally(),
            "author-reopened fixture exits naturally only after its original owner release");
    }
    {
        Fixture fixture;
        auto journal = InterruptAfterRollbackActivation(fixture);
        auto target = Detail::ReadSignedInput(journal.transaction / L"target-manifest.json", journal.transaction / L"target-manifest.sig", fixture.signer.key);
        auto baseline = Detail::ReadSignedInput(journal.transaction / L"baseline-manifest.json", journal.transaction / L"baseline-manifest.sig", fixture.signer.key);
        if (!target.Succeeded() || !baseline.Succeeded()) throw std::runtime_error("fixture unknown-running inputs failed");
        Detail::InstallerTestHooks hooks;
        int probes = 0;
        hooks.restartProcessProbe = [&](const Detail::InstallJournal&) {
            ++probes;
            return Result<bool>::Failure("UpdateTest", "UPDATE_TEST_PROCESS_UNKNOWN", "原创夹具模拟完整进程枚举不可确认。");
        };
        const auto result = Detail::RollbackJournal(journal, target.value->verified, &baseline.value->verified, hooks);
        Check(probes == 1 && result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RolledBack
            && !result.value->oldVersionRestarted && result.value->reason.find("UPDATE_OLD_RESTART_RUNNING_UNKNOWN") != std::string::npos,
            "simulated unknown process enumeration skips restart and explicitly explains the reason in receipt");
        fixture.Preserved("unknown-running restart refusal preserves original user bytes");
    }
    {
        Fixture fixture(false, true);
        const auto journal = InterruptAfterRollbackActivation(fixture);
        const auto result = RecoverUpdate(journal.transaction, fixture.signer.key);
        Diagnostics(result, "non-native old version recover");
        Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RolledBack && !result.value->oldVersionRestarted
            && result.value->reason.find("UPDATE_OLD_RESTART_EXECUTABLE_INVALID") != std::string::npos,
            "restored signed non-native old entry is rejected before automatic restart");
        Check(fixture.FileIs(L"app.exe", "original invalid old PE fixture") && fixture.FileIs(L"program.txt", "old program"),
            "skipped non-native old restart keeps exact restored signed bytes");
        fixture.Preserved("non-native old restart refusal retains original author files");
    }
}

void InventoryChangeTests() {
    Fixture fixture;
    Detail::InstallerTestHooks hooks;
    hooks.checkpoint = [&](std::string_view phase) {
        if (phase == "after-stage") Write(fixture.request.installDirectory / L"settings.json", "parallel author settings");
        return Status::Ok();
    };
    const auto result = Detail::InstallUpdateWithHooks(fixture.request, fixture.signer.key, hooks);
    Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::Abandoned
        && result.value->reasonCode == "UPDATE_INVENTORY_CHANGED", "source change after staging refuses directory switch");
    Check(fixture.FileIs(L"settings.json", "parallel author settings") && fixture.FileIs(L"program.txt", "old program"),
        "parallel user change preserved with original program");
}

std::filesystem::path ZonePath(const std::filesystem::path& path) {
    return std::filesystem::path(path.native() + Detail::kZoneIdentifierSuffix);
}
bool HasZone(const std::filesystem::path& path) {
    WIN32_FIND_STREAM_DATA data{};
    const auto handle = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &data, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_HANDLE_EOF) return false;
        throw std::runtime_error("original fixture stream enumeration failed");
    }
    bool found = false;
    do { found |= std::wstring_view(data.cStreamName) == Detail::kZoneIdentifierSuffix; } while (FindNextStreamW(handle, &data));
    const auto error = GetLastError(); FindClose(handle);
    if (error != ERROR_HANDLE_EOF) throw std::runtime_error("original fixture incomplete stream enumeration");
    return found;
}
constexpr std::string_view kOriginalZone = "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://original.invalid/fixture\r\n";
constexpr std::string_view kChangedZone = "[ZoneTransfer]\r\nZoneId=4\r\nReferrerUrl=https://original.invalid/changed\r\n";

void OriginInventoryCopyTests() {
    Temp temp;
    const auto source = temp.path / L"source", copy = temp.path / L"copy";
    Write(source / L"user.txt", "original user bytes");
    std::filesystem::create_directories(source / L"empty"); std::filesystem::create_directory(copy);
    const auto plain = Detail::InspectUpdateTree(source);
    Check(plain.Succeeded(), "original no-stream tree can be inventoried");
    if (!plain.Succeeded()) return;
    Json legacy{{"directories", Json::array({"empty"})}, {"files", Json::array({
        {{"path", "user.txt"}, {"bytes", 19}, {"sha256", Hash("original user bytes")}}})}};
    Check(plain.value->origins.empty() && plain.value->sha256 == Hash(legacy.dump()), "no-Zone inventory keeps exact legacy canonical hash");
    for (const auto& path : {source, source / L"empty", source / L"user.txt"}) Write(ZonePath(path), kOriginalZone);
    const auto original = Detail::InspectUpdateTree(source);
    Check(original.Succeeded() && original.value->origins.size() == 3 && original.value->sha256 != plain.value->sha256,
        "root, empty directory and file Zone presence/raw SHA all belong to inventory identity");
    if (!original.Succeeded()) return;
    Check(Detail::CopyInventory(source, copy, *original.value).Succeeded(), "owned mirror copies original root/directory/file download metadata");
    for (const auto& path : {copy, copy / L"empty", copy / L"user.txt"})
        Check(Read(ZonePath(path)) == Data(kOriginalZone), "copied origin metadata preserves exact original bytes");
    Write(ZonePath(source), kChangedZone);
    Check(DeleteFileW(ZonePath(source / L"user.txt").c_str()) != FALSE, "only original source fixture file marker removed");
    const auto latest = Detail::InspectUpdateTree(source);
    Check(latest.Succeeded() && latest.value->sha256 != original.value->sha256, "metadata-only change/absence changes inventory identity");
    if (!latest.Succeeded()) return;
    Check(Detail::CopyInventory(source, copy, *latest.value, true).Succeeded(), "rollback-style copy overlays latest metadata including absence");
    Check(Read(ZonePath(copy)) == Data(kChangedZone) && !HasZone(copy / L"user.txt"),
        "latest user removal does not resurrect backup Zone and root metadata stays latest");
    Check(Read(ZonePath(copy / L"empty")) == Data(kOriginalZone), "empty user directory retains its own metadata");
    Write(ZonePath(copy / L"user.txt"), kOriginalZone);
    const auto tampered = Detail::InspectUpdateTree(copy);
    Check(tampered.Succeeded() && !Detail::PreservedOriginsMatch(*latest.value, *tampered.value),
        "post-copy metadata injection cannot satisfy the final preservation proof");
    const auto stale = Detail::CopyInventory(source, copy, *original.value, true);
    Check(!stale.Succeeded(), "copy refuses source metadata which no longer matches captured expected bytes");
    Check(DeleteFileW(ZonePath(source).c_str()) != FALSE && DeleteFileW(ZonePath(source / L"empty").c_str()) != FALSE,
        "only original source root and empty-directory markers removed for absence overlay");
    const auto unmarked = Detail::InspectUpdateTree(source);
    Check(unmarked.Succeeded() && Detail::CopyInventory(source, copy, *unmarked.value, true).Succeeded(),
        "owned mirror handles root directory and file marker removal without conflicting with its own strict base leases");
    Check(!HasZone(copy) && !HasZone(copy / L"empty") && !HasZone(copy / L"user.txt")
        && Read(copy / L"user.txt") == Data("original user bytes"),
        "absence overlay preserves default bytes and removes only the three captured mirror markers");
}

void OriginCommitRollbackTests() {
    {
        Fixture fixture;
        for (const auto& path : {fixture.request.installDirectory, fixture.request.installDirectory / L"user-empty/inner",
                 fixture.request.installDirectory / L"settings.json", fixture.request.installDirectory / L"program.txt",
                 fixture.request.installDirectory / L"update-manifest.json", fixture.request.installDirectory / L"update-manifest.json.sig"})
            Write(ZonePath(path), kOriginalZone);
        const auto original = Detail::InspectUpdateTree(fixture.request.installDirectory);
        Check(original.Succeeded(), "marked original install inventory is complete");
        if (!original.Succeeded()) return;
        fixture.original = *original.value;
        Check(DiscoverInstalledUpdateBaseline(fixture.request.installDirectory, "0.1.0", fixture.signer.key).Succeeded(),
            "normal portable download marks do not reject an otherwise exact signed installed baseline");
        auto first = InstallUpdate(fixture.request, fixture.signer.key);
        Diagnostics(first, "MOTW commit");
        Check(first.Succeeded() && first.value->outcome == UpdateInstallOutcome::Committed, "signed update commits with normal origin metadata");
        if (!first.Succeeded() || first.value->outcome != UpdateInstallOutcome::Committed) return;
        WaitNatural(first.value->childProcess);
        const auto backup = Detail::InspectUpdateTree(first.value->backupDirectory);
        Check(backup.Succeeded() && backup.value->sha256 == fixture.original.sha256, "original program and user MOTW remain exact in renamed backup");
        Check(Read(ZonePath(fixture.request.installDirectory / L"settings.json")) == Data(kOriginalZone)
            && Read(ZonePath(fixture.request.installDirectory / L"user-empty/inner")) == Data(kOriginalZone),
            "committed user file and empty directory retain their exact download marks");
        Check(!HasZone(fixture.request.installDirectory / L"program.txt"),
            "new signed program is new payload; old program URL is not fabricated as its origin");
        fixture.Next("0.2.0", "0.3.0");
        auto second = InstallUpdate(fixture.request, fixture.signer.key);
        Check(second.Succeeded() && second.value->outcome == UpdateInstallOutcome::Committed, "subsequent signed update retains marked user tree");
        if (!second.Succeeded() || second.value->outcome != UpdateInstallOutcome::Committed) return;
        WaitNatural(second.value->childProcess);
        const auto cleanup = CleanupUpdateBackups(fixture.request.installDirectory, fixture.signer.key);
        Diagnostics(cleanup, "MOTW cleanup");
        Check(cleanup.Succeeded() && cleanup.value->removedBackups == 1 && !std::filesystem::exists(first.value->backupDirectory)
            && std::filesystem::exists(second.value->backupDirectory), "only verified older marked backup is removed; latest backup remains");
    }
    {
        Fixture fixture(true);
        Write(ZonePath(fixture.request.installDirectory / L"settings.json"), kOriginalZone);
        Write(ZonePath(fixture.request.installDirectory / L"user-empty/inner"), kOriginalZone);
        Detail::InstallerTestHooks hooks;
        bool changed = false;
        hooks.checkpoint = [&](std::string_view phase) {
            if (phase == "after-activation") {
                changed = true;
                Check(DeleteFileW(ZonePath(fixture.request.installDirectory / L"settings.json").c_str()) != FALSE,
                    "late original user fixture removes its mark");
                Write(ZonePath(fixture.request.installDirectory / L"logs/local.log"), kChangedZone);
                Write(ZonePath(fixture.request.installDirectory / L"user-empty/inner"), kChangedZone);
            }
            return Status::Ok();
        };
        const auto result = Detail::InstallUpdateWithHooks(fixture.request, fixture.signer.key, hooks);
        ReceiptDiagnostics(result, "MOTW rollback");
        Check(changed && result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RolledBack
            && result.value->reasonCode == "UPDATE_ACTIVATION_CHANGED" && !result.value->childProcess,
            "changed activated inventory restores original program before health launch through existing rollback transaction");
        if (!changed || !result.Succeeded() || result.value->outcome != UpdateInstallOutcome::RolledBack) return;
        Check(!HasZone(fixture.request.installDirectory / L"settings.json")
            && Read(ZonePath(fixture.request.installDirectory / L"logs/local.log")) == Data(kChangedZone)
            && Read(ZonePath(fixture.request.installDirectory / L"user-empty/inner")) == Data(kChangedZone),
            "rollback preserves latest user marker removal, insertion and directory change without resurrecting backup metadata");
    }
    {
        Fixture fixture(true);
        for (const auto& path : {fixture.request.installDirectory, fixture.request.installDirectory / L"settings.json",
                 fixture.request.installDirectory / L"user-empty/inner"}) Write(ZonePath(path), kOriginalZone);
        const auto result = InstallUpdate(fixture.request, fixture.signer.key);
        Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RolledBack
            && result.value->reasonCode == "UPDATE_EXECUTABLE_TYPE_INVALID" && !result.value->healthVerified && !result.value->childProcess,
            "actual invalid native image health-start gate restores marked tree without granting health or launching child");
        Check(fixture.FileIs(L"program.txt", "old program")
            && Read(ZonePath(fixture.request.installDirectory)) == Data(kOriginalZone)
            && Read(ZonePath(fixture.request.installDirectory / L"settings.json")) == Data(kOriginalZone)
            && Read(ZonePath(fixture.request.installDirectory / L"user-empty/inner")) == Data(kOriginalZone),
            "native health-start refusal preserves root user-file and empty-directory origin bytes through rollback");
    }
}

void OriginMetadataBudgetTests() {
    Temp temp;
    const auto root = temp.path / L"origin-budget";
    std::filesystem::create_directory(root);
    const std::string prefix = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=";
    const auto limit = static_cast<std::size_t>(Detail::kMaxZoneIdentifierBytes);
    const auto full = prefix + std::string(limit - prefix.size(), 'a');
    const auto count = Detail::kMaxOriginMetadataBytes / Detail::kMaxZoneIdentifierBytes;
    Check(count == 1024 && count * limit == Detail::kMaxOriginMetadataBytes, "original aggregate boundary fixture matches declared budgets");
    Write(ZonePath(root), full);
    std::filesystem::path last;
    for (std::uint64_t i = 1; i < count; ++i) {
        last = root / (L"original-" + std::to_wstring(i) + L".txt");
        Write(last, "original bounded user bytes"); Write(ZonePath(last), full);
    }
    const auto exact = Detail::InspectUpdateTree(root);
    Check(exact.Succeeded() && exact.value->origins.size() == count,
        "root and file origin streams at exactly 16MiB aggregate are accepted without a partial inventory");
    const auto directory = root / L"original-empty";
    std::filesystem::create_directory(directory);
    const std::string minimal = "[ZoneTransfer]\nZoneId=3\n";
    Write(ZonePath(last), std::string_view(full).substr(0, full.size() - minimal.size() + 1));
    Write(ZonePath(directory), minimal);
    const auto oversized = Detail::InspectUpdateTree(root);
    Check(Code(oversized, "UPDATE_ORIGIN_METADATA_BUDGET") && !oversized.value,
        "root file and empty-directory origin streams at aggregate limit plus one fail closed without a partial inventory");
    Check(Read(ZonePath(root)) == Data(full) && Read(ZonePath(directory)) == Data(minimal),
        "aggregate refusal preserves original fixture metadata bytes");
}

void OriginMutationAndCleanupTests() {
    {
        Fixture fixture;
        Detail::InstallerTestHooks hooks;
        hooks.checkpoint = [&](std::string_view phase) {
            if (phase == "after-stage") Write(ZonePath(fixture.request.installDirectory / L"settings.json"), kOriginalZone);
            return Status::Ok();
        };
        const auto result = Detail::InstallUpdateWithHooks(fixture.request, fixture.signer.key, hooks);
        Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::Abandoned
            && result.value->reasonCode == "UPDATE_INVENTORY_CHANGED" && fixture.FileIs(L"program.txt", "old program"),
            "metadata-only author change after staging rejects switch and retains original program");
        Check(Read(ZonePath(fixture.request.installDirectory / L"settings.json")) == Data(kOriginalZone), "author's changed marker is not cleared on refusal");
    }
    for (const bool unknown : {false, true}) {
        Temp temp;
        const auto root = temp.path / L"owned-old-tree";
        std::filesystem::create_directory(root);
        Write(ZonePath(root), kOriginalZone);
        const auto inventory = Detail::InspectUpdateTree(root); const auto identity = Detail::ReadRootIdentity(root);
        if (!inventory.Succeeded() || !identity.Succeeded()) throw std::runtime_error("original cleanup marker fixture failed");
        Detail::BackupCleanupTestHooks hooks;
        bool injected = false;
        hooks.checkpoint = [&](std::string_view phase, const auto&, const auto& path) {
            if (phase == "directory-lease-released" && path == root) {
                injected = true;
                if (unknown) Write(std::filesystem::path(root.native() + L":original-unknown"), "original unknown metadata");
                else Write(ZonePath(root), kChangedZone);
            }
            return Status::Ok();
        };
        const auto removed = Detail::DeleteOwnedTree(root, *inventory.value, *identity.value, hooks);
        Check(injected && !removed.status.Succeeded() && !removed.MutationStarted() && std::filesystem::exists(root),
            "final directory lease-release race rejects changed Zone or newly unknown ADS before any deletion");
        Check(Read(ZonePath(root)) == Data(unknown ? kOriginalZone : kChangedZone), "cleanup refusal preserves changed origin bytes");
    }
}

void HealthFailureTests() {
    {
        Fixture fixture(true);
        std::cout << "STAGE health-invalid-pe signed native format gate begin" << std::endl;
        const auto result = InstallUpdate(fixture.request, fixture.signer.key);
        std::cout << "STAGE health-invalid-pe signed native format gate returned" << std::endl;
        Diagnostics(result, "invalid PE");
        Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RolledBack
            && !result.value->healthVerified, "rejected non-native signed entry restores old version");
        Check(result.Succeeded() && result.value->reasonCode == "UPDATE_EXECUTABLE_TYPE_INVALID"
            && !result.value->childProcess.has_value(), "text or DOS format is rejected before recording a new child process");
        Check(fixture.FileIs(L"program.txt", "old program"), "start failure restores original program bytes");
        fixture.Preserved("start failure retains user/sidecar content");
    }
    {
        Fixture fixture;
        fixture.Prepare("0.2.0", OriginalDllImage(fixture.image));
        const auto result = InstallUpdate(fixture.request, fixture.signer.key);
        Diagnostics(result, "signed DLL entry");
        Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RolledBack && !result.value->healthVerified
            && result.value->reasonCode == "UPDATE_EXECUTABLE_TYPE_INVALID" && !result.value->childProcess.has_value(),
            "signed PE with DLL characteristic is refused before native health launch");
        Check(fixture.FileIs(L"program.txt", "old program"), "DLL format refusal restores exact old program bytes");
        fixture.Preserved("DLL format refusal retains original user/sidecar content");
    }
    {
        Fixture fixture;
        fixture.request.startupArguments = {"--fixture-child", "bad", "--fixture-version", "0.2.0"};
        auto result = InstallUpdate(fixture.request, fixture.signer.key);
        Check(result.Succeeded() && result.value->outcome != UpdateInstallOutcome::Committed && !result.value->healthVerified,
            "wrong compiled version in bound pipe never grants health");
        if (!result.Succeeded()) return;
        WaitNatural(result.value->childProcess);
        if (result.value->outcome == UpdateInstallOutcome::RollbackPending) result = RecoverUpdate(result.value->transactionDirectory, fixture.signer.key);
        if (!result.Succeeded() || result.value->outcome != UpdateInstallOutcome::RolledBack)
            ReceiptDiagnostics(result, "mismatched health recovery after natural exit");
        Check(result.Succeeded() && result.value->outcome == UpdateInstallOutcome::RolledBack && fixture.FileIs(L"program.txt", "old program"),
            "mismatched health recovers after child exits naturally");
    }
    {
        Fixture fixture;
        fixture.request.healthTimeoutMs = 30;
        fixture.request.startupArguments = {"--fixture-child", "late", "--fixture-version", "0.2.0"};
        const auto pending = InstallUpdate(fixture.request, fixture.signer.key);
        Check(pending.Succeeded() && pending.value->outcome == UpdateInstallOutcome::RollbackPending && !pending.value->healthVerified,
            "timeout while new child runs retains current and backup with pending outcome");
        if (!pending.Succeeded()) return;
        Check(std::filesystem::exists(pending.value->backupDirectory) && fixture.FileIs(L"program.txt", "new program 0.2.0"),
            "running failed-health child is not killed or overwritten");
        WaitNatural(pending.value->childProcess);
        const auto recovered = RecoverUpdate(pending.value->transactionDirectory, fixture.signer.key);
        Diagnostics(recovered, "late user recovery");
        Check(recovered.Succeeded() && recovered.value->outcome == UpdateInstallOutcome::RolledBack && recovered.value->userFilesPreserved,
            "naturally exited child allows verified late rollback");
        Check(fixture.FileIs(L"settings.json", "late child settings") && fixture.FileIs(L"new-user/private.txt", "late child user file"),
            "late rollback merges modified and newly created unsigned user files");
        Check(std::filesystem::is_directory(fixture.request.installDirectory / L"late-empty"), "late rollback retains newly empty user directory");
        Check(fixture.FileIs(L"program.txt", "old program") && std::filesystem::exists(pending.value->backupDirectory)
            && std::filesystem::exists(pending.value->failedDirectory), "old program restored with original/failed evidence retained");
    }
}

void ParentWaitTests() {
    Fixture fixture;
    auto command = L"\"" + Self().native() + L"\" --fixture-child sleep-parent --fixture-version 0.1.0";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(Self().c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        nullptr, &startup, &process) != FALSE, "original parent wait process created");
    if (!process.hProcess) return;
    Detail::NativeHandle parent(process.hProcess), thread(process.hThread);
    const auto identity = ReadUpdateProcessIdentity(process.dwProcessId);
    Check(identity.Succeeded(), "real parent PID/creation identity queried");
    if (!identity.Succeeded()) return;
    auto wrong = fixture.request;
    wrong.parentProcess = *identity.value; ++wrong.parentProcess->creationTime100ns;
    Check(Code(InstallUpdate(wrong, fixture.signer.key), "UPDATE_PARENT_IDENTITY_MISMATCH"), "PID alone does not authorize wait");
    fixture.request.parentProcess = *identity.value; fixture.request.parentExitTimeoutMs = 20;
    const auto timedOut = InstallUpdate(fixture.request, fixture.signer.key);
    Check(Code(timedOut, "UPDATE_PARENT_EXIT_TIMEOUT") && fixture.Unchanged(), "parent timeout abandons before any install tree mutation");
    DWORD exit = 0;
    Check(GetExitCodeProcess(parent.Get(), &exit) && exit == STILL_ACTIVE, "parent process remains alive after timeout");
    Check(WaitForSingleObject(parent.Get(), 5000) == WAIT_OBJECT_0, "parent fixture exits naturally without termination");
}

int Child(const std::map<std::wstring, std::wstring>& arguments) {
    const auto mode = arguments.at(L"--fixture-child");
    if (mode == L"sleep-parent") { Sleep(1500); return 0; }
    if (mode == L"wait-existing") {
        Detail::NativeHandle release(OpenEventW(SYNCHRONIZE, FALSE, arguments.at(L"--fixture-exit-event").c_str()));
        return release.Valid() && WaitForSingleObject(release.Get(), 10000) == WAIT_OBJECT_0 ? 0 : 6;
    }
    if (!arguments.contains(L"--update-health-pipe")) return 0; // 正常旧版restart夹具立即自然退出。
    if (mode == L"late") {
        Sleep(120);
        Write(std::filesystem::current_path() / L"settings.json", "late child settings");
        Write(std::filesystem::current_path() / L"new-user/private.txt", "late child user file");
        std::filesystem::create_directory(std::filesystem::current_path() / L"late-empty");
        Sleep(250); return 0;
    }
    const auto utf8 = [](const std::wstring& value) { return IO::PathUtf8(std::filesystem::path(value)); };
    const auto handleText = utf8(arguments.at(L"--update-health-pipe"));
    std::uintptr_t handle = 0;
    const auto parsed = std::from_chars(handleText.data(), handleText.data() + handleText.size(), handle);
    if (parsed.ec != std::errc{} || parsed.ptr != handleText.data() + handleText.size()) return 4;
    const auto version = mode == L"bad" ? "9.9.9" : utf8(arguments.at(L"--fixture-version"));
    const auto reply = ReplyUpdateHealth(handle, utf8(arguments.at(L"--update-health-transaction")),
        utf8(arguments.at(L"--update-health-challenge")), version, std::string(40, 'a'));
    Sleep(20);
    return reply.Succeeded() ? 0 : 5;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (sodium_init() < 0) throw std::runtime_error("fixture sodium init failed");
        if (argc > 1) {
            std::map<std::wstring, std::wstring> arguments;
            for (int index = 1; index + 1 < argc; index += 2) arguments.emplace(argv[index], argv[index + 1]);
            if (arguments.contains(L"--fixture-child")) return Child(arguments);
            return 3;
        }
        const auto suite = [](std::string_view name, auto test) {
            const auto started = std::chrono::steady_clock::now();
            std::cout << "RUN " << name << std::endl;
            test();
            std::cout << "DONE " << name << " ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count() << std::endl;
        };
        suite("baseline", BaselineTests); suite("guards", GuardTests); suite("commit-cleanup", CommitAndCleanupTests);
        suite("cleanup-root-replacement", CleanupRootReplacementTests); suite("cleanup-busy-last", CleanupBusyLastFileTests);
        suite("cleanup-mutation", CleanupMutationTests);
        suite("rename-recovery", RenameAndRecoveryTests);
        suite("rollback-terminal", RollbackTerminalBoundaryTests); suite("between-rollback-renames", BetweenRollbackRenamesTests);
        suite("old-restart-guards", OldRestartGuardsTests); suite("inventory-change", InventoryChangeTests);
        suite("origin-inventory-copy", OriginInventoryCopyTests); suite("origin-commit-rollback", OriginCommitRollbackTests);
        suite("origin-metadata-budget", OriginMetadataBudgetTests); suite("origin-mutation-cleanup", OriginMutationAndCleanupTests);
        suite("health-failure", HealthFailureTests); suite("parent-wait", ParentWaitTests);
    } catch (const std::exception& error) {
        ++failures; std::cerr << "FAIL: unexpected original fixture error: " << error.what() << '\n';
    }
    std::cout << "Update Installer checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
