#include "War3MapCore/MapBackupMigration.h"
#include "War3MapCore/Hash.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void Good(const Result<T>& r) {
    if (r.Succeeded() && r.value) return;
    std::string text;
    for (const auto& d : r.diagnostics) text += d.code + ":" + d.message + "\n";
    throw std::runtime_error(text);
}
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data()); return {first, first + text.size()};
}
void Write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc); out << text; out.close(); Require(out.good(), "write owned fixture");
}
nlohmann::json Json(const std::filesystem::path& path) {
    std::ifstream in(path); nlohmann::json value; in >> value; return value;
}
struct Fixture {
    std::filesystem::path parent, root, old, target, source, backup, receipt;
    Fixture() {
        parent = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
        root = parent / ("Audastra-backup-migration-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        for (int suffix = 0; std::filesystem::exists(root); ++suffix) root = parent / ("Audastra-backup-migration-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(suffix));
        Require(root.is_absolute() && root.parent_path() == parent && std::filesystem::create_directory(root), "create own absolute directory");
        old = root / L"旧备份"; target = root / L"新备份"; source = root / L"原创地图.w3x";
        const auto created = WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"authored.bin", Bytes("original fixture")}}); Good(created);
        Save();
    }
    ~Fixture() {
        std::error_code error;
        const auto checked = std::filesystem::weakly_canonical(root, error);
        if (!error && checked == root && checked.parent_path() == parent && checked.filename().wstring().starts_with(L"Audastra-backup-migration-")) {
            // 只回收本测试创建且重新确认边界的目录，包含只读负例的属性恢复。
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error))
                if (entry.is_regular_file()) SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
            std::filesystem::remove_all(root, error);
        }
    }
    void Save() {
        auto opened = MapArchiveSession::Open(source); Good(opened);
        InPlaceSaveOptions options; options.backupDirectory = old; options.keepBackups = 0;
        auto saved = opened.value->SaveInPlace({}, {}, options); Good(saved);
        backup = saved.value->backup; receipt = old / L".forge-backup-owners" / (backup.filename().wstring() + L".json");
    }
    MapBackupMigrationOptions Options() const { MapBackupMigrationOptions o; o.keepBackups = 0; return o; }
    std::filesystem::path NewBackup() const { return target / backup.filename(); }
    std::filesystem::path NewReceipt() const { return target / L".forge-backup-owners" / receipt.filename(); }
    void Target() const { std::filesystem::create_directories(target); }
};
void SuccessAndOwnership() {
    Fixture f;
    Write(f.old / L"用户文件.txt", "do not move");
    Write(f.old / L"unowned.w3x", "not an editor backup");
    auto before = Sha256File(f.backup); Good(before);
    auto r = MigrateMapBackups(f.old, f.target, f.Options()); Good(r);
    Require(r.value->moved == 1 && r.value->skipped == 0 && r.value->complete, "one owned backup migrated");
    Require(!std::filesystem::exists(f.backup) && !std::filesystem::exists(f.receipt), "source pair removed");
    Require(std::filesystem::exists(f.old / L"用户文件.txt") && std::filesystem::exists(f.old / L"unowned.w3x"), "unowned files untouched");
    Require(!std::filesystem::exists(f.old / L".forge-backup-owners"), "empty own index removed");
    Require(Sha256File(f.NewBackup()).value == before.value, "copied content preserved");
    auto checked = InspectMapBackupDirectory(f.target); Good(checked);
    Require(checked.value->state == MapBackupInspectionState::ConfirmedAtLeastOne, "destination native identity rewritten correctly");
    const auto sourceHash = Sha256File(f.source); Good(sourceHash);
    Require(sourceHash.value == before.value, "source map never opened for mutation");
}
void EmptyAndSameDirectory() {
    Fixture f;
    auto r = MigrateMapBackups(f.old, f.old / L".", f.Options()); Good(r);
    Require(r.value->moved == 0 && std::filesystem::exists(f.backup), "same normalized path no-op");
    r = MigrateMapBackups(f.root / L"missing", f.target, f.Options()); Good(r);
    Require(!std::filesystem::exists(f.target), "missing source does not create destination");
    r = MigrateMapBackups(f.old, f.target, f.Options()); Good(r);
    Require(!std::filesystem::exists(f.old), "empty source backup root removed");
    r = MigrateMapBackups(f.target, f.old, f.Options()); Good(r);
    Require(r.value->moved == 1, "migrated owner remains migratable");
    const auto noOwner = f.root / L"unowned"; std::filesystem::create_directory(noOwner); Write(noOwner / L"private.w3x", "unowned");
    r = MigrateMapBackups(noOwner, f.target, f.Options()); Good(r);
    Require(r.value->moved == 0 && std::filesystem::exists(noOwner / L"private.w3x"), "no owner no movement");
    const auto emptyIndex = f.root / L"unconfirmed-empty-index";
    std::filesystem::create_directories(emptyIndex / L".forge-backup-owners");
    r = MigrateMapBackups(emptyIndex, f.root / L"empty-target", f.Options()); Good(r);
    Require(std::filesystem::exists(emptyIndex / L".forge-backup-owners"), "unproven empty index directory is not claimed or removed");
}
void ConflictsAndDigest() {
    for (bool same : {false, true}) {
        Fixture f; f.Target();
        if (same) std::filesystem::copy_file(f.backup, f.NewBackup()); else Write(f.NewBackup(), "user conflict");
        auto r = MigrateMapBackups(f.old, f.target, f.Options()); Good(r);
        Require(r.value->moved == (same ? 1u : 0u) && r.value->skipped == (same ? 0u : 1u), "same name content rule");
        Require(std::filesystem::exists(f.backup) != same, "source preserved on conflict");
        if (same) {
            Require(r.value->alreadyPresent == 1, "identical existing destination reused");
            const auto checked = InspectMapBackupDirectory(f.target); Good(checked);
            Require(checked.value->state == MapBackupInspectionState::ConfirmedAtLeastOne, "reused file has correct ownership");
        }
    }
    Fixture f; Write(f.backup, "changed after backup");
    auto r = MigrateMapBackups(f.old, f.target, f.Options()); Good(r);
    Require(r.value->moved == 0 && r.value->skipped == 1 && std::filesystem::exists(f.backup), "SHA mismatch preserved");
    Fixture g; auto record = Json(g.receipt); record["fileId"] = record["fileId"].get<std::uint64_t>() + 1; Write(g.receipt, record.dump());
    r = MigrateMapBackups(g.old, g.target, g.Options()); Good(r);
    Require(r.value->moved == 0 && std::filesystem::exists(g.backup), "file identity mismatch preserved");
}
void LockedAndRollback() {
    Fixture f;
    HANDLE held = CreateFileW(f.backup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(held != INVALID_HANDLE_VALUE, "open own occupied fixture");
    auto r = MigrateMapBackups(f.old, f.target, f.Options()); CloseHandle(held); Good(r);
    Require(r.value->moved == 0 && r.value->skipped == 1 && std::filesystem::exists(f.receipt), "locked source remains indexed");
    Fixture g; Require(SetFileAttributesW(g.backup.c_str(), FILE_ATTRIBUTE_READONLY), "set own fixture readonly");
    r = MigrateMapBackups(g.old, g.target, g.Options()); Good(r);
    Require(r.value->moved == 0 && std::filesystem::exists(g.backup) && std::filesystem::exists(g.receipt), "source delete failure retains pair");
    Require(!std::filesystem::exists(g.NewBackup()) && !std::filesystem::exists(g.NewReceipt()), "delete failure rolls back target pair");
    Fixture lockedRecord; Require(SetFileAttributesW(lockedRecord.receipt.c_str(), FILE_ATTRIBUTE_READONLY), "set own receipt readonly");
    r = MigrateMapBackups(lockedRecord.old, lockedRecord.target, lockedRecord.Options()); Good(r);
    Require(r.value->moved == 0 && std::filesystem::exists(lockedRecord.backup) && std::filesystem::exists(lockedRecord.receipt), "second source deletion failure restores first delete marker");
    Require(!std::filesystem::exists(lockedRecord.NewBackup()) && !std::filesystem::exists(lockedRecord.NewReceipt()), "receipt delete failure rolls back target pair");
    Fixture h; h.Target(); std::filesystem::create_directory(h.target / L".forge-backup-owners"); Write(h.NewReceipt(), "user receipt conflict");
    r = MigrateMapBackups(h.old, h.target, h.Options()); Good(r);
    Require(r.value->moved == 0 && std::filesystem::exists(h.backup) && !std::filesystem::exists(h.NewBackup()), "receipt conflict rolls back new copy");
    Require(Json(h.receipt).is_object(), "old record remains valid");
}
void RetentionAndBudgets() {
    Fixture f; f.Save(); f.Save();
    auto options = f.Options(); options.keepBackups = 1;
    auto r = MigrateMapBackups(f.old, f.target, options); Good(r);
    Require(r.value->moved == 3 && r.value->pruned == 2, "existing source-key retention reused");
    Require(std::filesystem::exists(f.NewBackup()), "retention preserves original newest receipt order");
    Fixture g;
    options = g.Options(); options.maxReadBytes = 1;
    r = MigrateMapBackups(g.old, g.target, options); Good(r);
    Require(!r.value->complete && r.value->moved == 0 && r.value->bytesRead <= 1 && std::filesystem::exists(g.backup), "bounded byte accounting");
    options = g.Options(); options.maxEntries = 0;
    r = MigrateMapBackups(g.old, g.target, options); Good(r);
    Require(!r.value->complete && r.value->examinedEntries == 0 && std::filesystem::exists(g.backup), "bounded entry accounting");
    std::stop_source stop; stop.request_stop(); options = g.Options(); options.stop = stop.get_token();
    r = MigrateMapBackups(g.old, g.target, options); Good(r);
    Require(!r.value->complete && r.value->moved == 0, "cancel before mutation");
    options = g.Options(); options.deadline = std::chrono::steady_clock::now();
    r = MigrateMapBackups(g.old, g.target, options); Good(r); Require(!r.value->complete, "expired deadline");
    Require(!MigrateMapBackups(L"relative", g.target).Succeeded(), "reject relative root");
}
}
int main() {
    try { SuccessAndOwnership(); EmptyAndSameDirectory(); ConflictsAndDigest(); LockedAndRollback(); RetentionAndBudgets(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "backup migration ownership/conflict/rollback/retention/budgets passed\n";
}
