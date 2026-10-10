#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using State = MapBackupInspectionState;

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename T>
void RequireSuccess(const Result<T>& result, std::string_view context) {
    if (result.Succeeded() && result.value) return;
    std::string message(context);
    for (const auto& item : result.diagnostics) message += " | " + item.code + ": " + item.message;
    throw std::runtime_error(message);
}

void RequireState(const Result<MapBackupInspection>& result, State state, std::string_view context) {
    RequireSuccess(result, context);
    Require(result.value->state == state, context);
    Require(result.value->confirmedBackup.has_value() == (state == State::ConfirmedAtLeastOne), "confirmation path has exact tri-state meaning");
    if (state == State::Unknown) Require(!result.diagnostics.empty(), "unknown retains a diagnostic value");
}

bool HasCode(const Result<MapBackupInspection>& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [code](const auto& item) { return item.code == code; });
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        parent_ = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
        path_ = (parent_ / ("Forge-backup-inspection-" + std::to_string(GetCurrentProcessId()) + "-"
            + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)))).lexically_normal();
        Require(path_.is_absolute() && path_.parent_path() == parent_, "owned temporary target stays inside named parent");
        Require(std::filesystem::create_directory(path_), "create owned temporary directory");
    }
    ~TempDirectory() {
        // 只移除本测试实际创建的绝对目录；不对路径拼接结果盲目递归删除。
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path_, error);
        if (!error && resolved == path_ && resolved.parent_path() == parent_
            && resolved.filename().wstring().starts_with(L"Forge-backup-inspection-")) std::filesystem::remove_all(resolved, error);
    }
    const std::filesystem::path& Path() const noexcept { return path_; }
private:
    std::filesystem::path parent_, path_;
};

class ReadLease final {
public:
    explicit ReadLease(const std::filesystem::path& path, bool directory = false)
        : handle_(CreateFileW(path.c_str(), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr)) {
        Require(handle_ != INVALID_HANDLE_VALUE, "hold readonly fixture lease that forbids DELETE/write sharing");
    }
    ~ReadLease() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
private:
    HANDLE handle_;
};

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

std::vector<std::byte> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(file.good(), "read authored fixture");
    const auto size = file.tellg();
    Require(size >= 0, "fixture size is known");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(file.good(), "read complete fixture");
    return bytes;
}

void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Require(file.good(), "open authored fixture output");
    if (!bytes.empty()) file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    Require(file.good(), "write complete fixture");
}

using DirectoryImage = std::map<std::filesystem::path, std::vector<std::byte>>;
DirectoryImage Image(const std::filesystem::path& directory) {
    DirectoryImage image;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        // 只用于测试自己的无链接夹具；目录也入账，发现意外创建。
        image.emplace(entry.path().lexically_relative(directory), entry.is_directory() ? std::vector<std::byte>{} : Read(entry.path()));
    }
    return image;
}

struct Fixture {
    TempDirectory directory;
    std::filesystem::path source, root, backup, receipt;
    std::vector<std::byte> receiptBytes, backupBytes;
    nlohmann::json record;

    Fixture() {
        source = directory.Path() / L"原创 地图.w3x";
        root = directory.Path() / L"备份 目录";
        const std::vector<MapArchiveMember> members{{"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")},
            {"imports/authored.bin", Bytes("authored backup inspection payload")}};
        RequireSuccess(WriteVerifiedArchive(source, members), "write original MPQ fixture");
        const auto session = MapArchiveSession::Open(source);
        RequireSuccess(session, "open authored map snapshot");
        InPlaceSaveOptions options;
        options.backupDirectory = root;
        options.keepBackups = 0;
        const auto saved = session.value->SaveInPlace({}, {}, options);
        RequireSuccess(saved, "actual save creates native v1 owner record");
        backup = saved.value->backup;
        root = backup.parent_path();
        receipt = root / L".forge-backup-owners" / (backup.filename().wstring() + L".json");
        receiptBytes = Read(receipt);
        backupBytes = Read(backup);
        record = nlohmann::json::parse(reinterpret_cast<const char*>(receiptBytes.data()),
            reinterpret_cast<const char*>(receiptBytes.data() + receiptBytes.size()));
    }
    void Record(const nlohmann::json& changed) { Write(receipt, Bytes(changed.dump())); }
    std::uint64_t CompleteCost() const { return static_cast<std::uint64_t>(receiptBytes.size() + backupBytes.size()); }
};

void Inventory() {
    TempDirectory temp;
    const auto missing = temp.Path() / L"未创建";
    const auto before = Image(temp.Path());
    RequireState(InspectMapBackupDirectory(missing), State::Absent, "missing known directory is absent");
    Require(Image(temp.Path()) == before && !std::filesystem::exists(missing), "missing directory check never creates it");
    RequireState(InspectMapBackupDirectory(temp.Path()), State::Absent, "complete empty root without owner index is absent");
    const auto rootAbsent = temp.Path() / "root-with-orphan";
    std::filesystem::create_directory(rootAbsent);
    Write(rootAbsent / "forge-map-orphan.w3x", Bytes("marker alone never proves ownership"));
    const auto missingOwner = InspectMapBackupDirectory(rootAbsent);
    RequireState(missingOwner, State::Unknown, "missing owner directory plus orphan marker is not proven absence");
    Require(missingOwner.value->bytesRead == 0, "unowned marker payload is not hashed");
    Write(temp.Path() / "source.backup.w3x", Bytes("ordinary user file, never a confirmed backup"));
    RequireState(InspectMapBackupDirectory(temp.Path()), State::Absent, "name-only legacy file is not program ownership proof");
    std::filesystem::create_directory(temp.Path() / L".forge-backup-owners");
    Write(temp.Path() / L".forge-backup-owners" / "notes.json", Bytes("{}"));
    RequireState(InspectMapBackupDirectory(temp.Path()), State::Absent, "fully inspected index has only unrelated notes");
    const auto file = temp.Path() / "not-a-directory";
    Write(file, Bytes("file"));
    RequireState(InspectMapBackupDirectory(file), State::Unknown, "file root cannot establish directory absence");

    Fixture fixture;
    const auto unchanged = Image(fixture.directory.Path());
    const auto checked = InspectMapBackupDirectory(fixture.root);
    RequireState(checked, State::ConfirmedAtLeastOne, "actual owner/native identity/full SHA confirms one backup");
    Require(*checked.value->confirmedBackup == fixture.backup && checked.value->readReceipts == 1
        && checked.value->examinedEntries == 1 && checked.value->bytesRead == fixture.CompleteCost(), "positive receipt has exact source path and charged bytes");
    Require(Image(fixture.directory.Path()) == unchanged, "positive inspection never mutates source, backup, owner or directory membership");
    const auto rootImage = Image(fixture.root);
    {
        ReadLease root(fixture.root, true), index(fixture.receipt.parent_path(), true);
        ReadLease receipt(fixture.receipt), backup(fixture.backup);
        const HANDLE deniedDelete = CreateFileW(fixture.backup.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(deniedDelete == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "fixture genuinely refuses DELETE access");
        RequireState(InspectMapBackupDirectory(fixture.root), State::ConfirmedAtLeastOne, "inspection only requests permissions compatible with readonly leases");
    }
    Require(Image(fixture.root) == rootImage, "readonly compatibility retains all exact backup bytes");
    std::cout << "inventory confirmed=1 receipt_bytes=" << fixture.receiptBytes.size()
        << " backup_bytes=" << fixture.backupBytes.size() << " charged=" << fixture.CompleteCost() << '\n';
}

void Identity() {
    Fixture fixture;
    const auto unchanged = Image(fixture.directory.Path());
    const auto changedRecord = [&](std::string_view field, nlohmann::json value) {
        auto bad = fixture.record;
        bad[std::string(field)] = std::move(value);
        fixture.Record(bad);
        const auto before = Image(fixture.directory.Path());
        RequireState(InspectMapBackupDirectory(fixture.root), State::Unknown, "invalid owner field must remain unknown");
        Require(Image(fixture.directory.Path()) == before, "invalid owner never changes files");
        Write(fixture.receipt, fixture.receiptBytes);
    };
    changedRecord("schema", "forge.map-backup.v0");
    changedRecord("sourceKey", std::string(64, 'a'));
    changedRecord("sourceKey", "short");
    changedRecord("backup", "../outside.w3x");
    changedRecord("sourceName", 123);
    changedRecord("volume", fixture.record.at("volume").get<std::uint64_t>() + (1ull << 32));
    changedRecord("volume", static_cast<double>(fixture.record.at("volume").get<std::uint64_t>()));
    changedRecord("fileId", -1);
    changedRecord("size", static_cast<double>(fixture.backupBytes.size()));
    changedRecord("sha256", std::string(64, 'z'));
    changedRecord("sha256", std::string(64, '0'));
    changedRecord("createdUtc", -1);
    Write(fixture.receipt, Bytes("{broken"));
    RequireState(InspectMapBackupDirectory(fixture.root), State::Unknown, "damaged JSON is unknown rather than absent");
    Write(fixture.receipt, fixture.receiptBytes);
    auto modified = fixture.backupBytes;
    modified.back() ^= std::byte{1};
    Write(fixture.backup, modified);
    const auto contentChanged = InspectMapBackupDirectory(fixture.root);
    RequireState(contentChanged, State::Unknown, "same file identity and length still require full content SHA");
    Require(contentChanged.value->bytesRead == fixture.CompleteCost(), "failed complete SHA still retains its full reserved reading charge");
    Write(fixture.backup, fixture.backupBytes);
    const auto original = fixture.directory.Path() / "held-original.w3x";
    std::filesystem::rename(fixture.backup, original);
    std::filesystem::copy_file(original, fixture.backup);
    const auto replaced = InspectMapBackupDirectory(fixture.root);
    RequireState(replaced, State::Unknown, "byte-identical replacement with different fileId is not a confirmed backup");
    Require(replaced.value->bytesRead == fixture.receiptBytes.size(), "unowned file is never read as map content");
    std::filesystem::remove(fixture.backup);
    std::filesystem::rename(original, fixture.backup);
    {
        const HANDLE writer = CreateFileW(fixture.backup.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(writer != INVALID_HANDLE_VALUE, "hold authored backup writer");
        const auto locked = InspectMapBackupDirectory(fixture.root);
        CloseHandle(writer);
        RequireState(locked, State::Unknown, "occupied backup is unknown with readonly data lease failure");
    }
    std::filesystem::remove(fixture.receipt);
    const auto orphan = InspectMapBackupDirectory(fixture.root);
    RequireState(orphan, State::Unknown, "orphan named backup without receipt is unknown rather than absence or ownership proof");
    Require(orphan.value->bytesRead == 0, "orphan files are not opened as map content");
    Write(fixture.receipt, fixture.receiptBytes);
    Require(Image(fixture.directory.Path()) == unchanged, "all identity negatives preserve the authored source and restore only test-authored modifications");
    std::cout << "identity invalid_fields=12 damaged=1 changed_sha=1 replacement=1 occupied=1\n";
}

void Budgets() {
    Fixture fixture;
    const auto before = Image(fixture.directory.Path());
    MapBackupInspectionOptions options;
    options.maxReadBytes = fixture.CompleteCost() - 1;
    const auto low = InspectMapBackupDirectory(fixture.root, options);
    RequireState(low, State::Unknown, "one byte below full record/content capacity stays unknown");
    Require(HasCode(low, "BACKUP_INSPECTION_BYTE_LIMIT") && low.value->bytesRead == fixture.receiptBytes.size(), "failed pre-hash capacity only charges receipt actually read");
    options.maxReadBytes = fixture.CompleteCost();
    RequireState(InspectMapBackupDirectory(fixture.root, options), State::ConfirmedAtLeastOne, "exact full reading budget is admitted");
    ++options.maxReadBytes;
    RequireState(InspectMapBackupDirectory(fixture.root, options), State::ConfirmedAtLeastOne, "one byte above full reading budget is admitted");
    options = {};
    options.maxReceiptBytes = fixture.receiptBytes.size() - 1;
    RequireState(InspectMapBackupDirectory(fixture.root, options), State::Unknown, "one byte below receipt size rejects without false absence");
    options.maxReceiptBytes = fixture.receiptBytes.size();
    RequireState(InspectMapBackupDirectory(fixture.root, options), State::ConfirmedAtLeastOne, "exact receipt byte capacity is admitted");
    options = {};
    options.maxEntries = 0;
    const auto entries = InspectMapBackupDirectory(fixture.root, options);
    RequireState(entries, State::Unknown, "zero entries performs no scan");
    Require(entries.value->examinedEntries == 0 && entries.value->bytesRead == 0 && HasCode(entries, "BACKUP_INSPECTION_ENTRY_LIMIT"), "entry guard is real and uncharged");
    options = {};
    options.maxReceipts = 0;
    const auto receipts = InspectMapBackupDirectory(fixture.root, options);
    RequireState(receipts, State::Unknown, "zero receipt capacity is not absence");
    Require(receipts.value->readReceipts == 0 && receipts.value->bytesRead == 0 && HasCode(receipts, "BACKUP_INSPECTION_RECEIPT_LIMIT"), "receipt guard precedes reading");
    {
        TempDirectory indexBudget;
        const auto index = indexBudget.Path() / L".forge-backup-owners";
        std::filesystem::create_directory(index);
        Write(index / "note1.json", Bytes("{}"));
        Write(index / "note2.json", Bytes("{}"));
        options = {}; options.maxEntries = 2;
        const auto truncated = InspectMapBackupDirectory(indexBudget.Path(), options);
        RequireState(truncated, State::Unknown, "index completeness alone does not bypass total root-entry capacity");
        Require(truncated.value->examinedEntries == 2 && HasCode(truncated, "BACKUP_INSPECTION_ENTRY_LIMIT"), "all index and root directory entries consume one combined budget");
        options.maxEntries = 3;
        RequireState(InspectMapBackupDirectory(indexBudget.Path(), options), State::Absent, "exact complete index plus root entry capacity proves absence");
    }
    {
        auto bad = fixture.record; bad["schema"] = "invalid";
        fixture.Record(bad);
        const auto key = fixture.record.at("sourceKey").get<std::string>();
        const auto extra = fixture.receipt.parent_path() / ("forge-map-" + key + "-1-2-3.w3x.json");
        Write(extra, Bytes("{broken"));
        options = {}; options.maxReceipts = 1;
        const auto limited = InspectMapBackupDirectory(fixture.root, options);
        RequireState(limited, State::Unknown, "both unconfirmed real-name receipts consume finite reading allowance");
        Require(limited.value->readReceipts == 1 && HasCode(limited, "BACKUP_INSPECTION_RECEIPT_LIMIT"), "receipt cap counts invalid JSON rather than skipping it for free");
        std::filesystem::remove(extra);
        Write(fixture.receipt, fixture.receiptBytes);
    }
    options = {};
    options.deadline = std::chrono::steady_clock::now();
    const auto late = InspectMapBackupDirectory(fixture.root, options);
    RequireState(late, State::Unknown, "expired shared absolute deadline stays unknown");
    Require(HasCode(late, "BACKUP_INSPECTION_DEADLINE") && late.value->bytesRead == 0, "expired deadline does not reserve unread backup");
    options = {};
    std::stop_source stop;
    stop.request_stop();
    options.stop = stop.get_token();
    const auto cancelled = InspectMapBackupDirectory(fixture.root, options);
    RequireState(cancelled, State::Unknown, "pre-cancelled shared stop token stays unknown");
    Require(HasCode(cancelled, "BACKUP_INSPECTION_CANCELLED") && cancelled.value->bytesRead == 0, "pre-cancellation does not charge unstarted work");
    const auto invalid = [&](MapBackupInspectionOptions bad) {
        const auto result = InspectMapBackupDirectory(fixture.root, bad);
        Require(!result.Succeeded() && !result.value && HasCode(result, "BACKUP_INSPECTION_OPTIONS_INVALID"), "invalid options fail explicitly without partial state");
    };
    options = {}; options.maxReceiptBytes = 4097; invalid(options);
    options = {}; options.maxEntries = 4097; invalid(options);
    options = {}; options.maxReceipts = 4097; invalid(options);
    options = {}; options.maxReadBytes = 1024ull * 1024 * 1024 + 1; invalid(options);
    options = {}; options.deadline = std::chrono::steady_clock::time_point::max(); invalid(options);
    const auto empty = InspectMapBackupDirectory({});
    Require(!empty.Succeeded() && !empty.value && HasCode(empty, "BACKUP_INSPECTION_OPTIONS_INVALID"), "empty path is an invalid caller argument");
    const auto relative = InspectMapBackupDirectory("relative-backups");
    Require(!relative.Succeeded() && !relative.value, "inspection only uses a frozen absolute known location");
    Require(Image(fixture.directory.Path()) == before, "every budget/cancellation/argument result leaves source, backups, receipts and directories untouched");
    std::cout << "budgets threshold=" << fixture.CompleteCost() << " low=" << fixture.CompleteCost() - 1
        << " high=" << fixture.CompleteCost() + 1 << " receipt=" << fixture.receiptBytes.size() << '\n';
}

bool Reparse() {
    Fixture fixture;
    const auto linkedRoot = fixture.directory.Path() / "linked-backups";
    constexpr DWORD allowUnprivileged = 0x2;
    if (!CreateSymbolicLinkW(linkedRoot.c_str(), fixture.root.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | allowUnprivileged)) {
        std::cout << "reparse unavailable_win32=" << GetLastError() << " (other inspection scenarios still run)\n";
        return false;
    }
    RequireState(InspectMapBackupDirectory(linkedRoot), State::Unknown, "root reparse cannot confirm ownership");
    std::filesystem::remove(linkedRoot);
    const auto index = fixture.receipt.parent_path(), heldIndex = fixture.root / "held-owner-index";
    std::filesystem::rename(index, heldIndex);
    Require(CreateSymbolicLinkW(index.c_str(), heldIndex.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | allowUnprivileged) != FALSE, "create authored linked owner index");
    RequireState(InspectMapBackupDirectory(fixture.root), State::Unknown, "index reparse cannot confirm ownership");
    std::filesystem::remove(index);
    std::filesystem::rename(heldIndex, index);
    const auto original = fixture.directory.Path() / "held-backup.w3x";
    std::filesystem::rename(fixture.backup, original);
    Require(CreateSymbolicLinkW(fixture.backup.c_str(), original.c_str(), allowUnprivileged) != FALSE, "create authored linked backup");
    RequireState(InspectMapBackupDirectory(fixture.root), State::Unknown, "backup reparse cannot confirm even when it resolves to correct original file");
    std::filesystem::remove(fixture.backup);
    std::filesystem::rename(original, fixture.backup);
    const auto heldReceipt = fixture.directory.Path() / "held-receipt.json";
    std::filesystem::rename(fixture.receipt, heldReceipt);
    Require(CreateSymbolicLinkW(fixture.receipt.c_str(), heldReceipt.c_str(), allowUnprivileged) != FALSE, "create authored linked receipt");
    RequireState(InspectMapBackupDirectory(fixture.root), State::Unknown, "receipt reparse cannot confirm even when it resolves to correct owner record");
    std::filesystem::remove(fixture.receipt);
    std::filesystem::rename(heldReceipt, fixture.receipt);
    RequireState(InspectMapBackupDirectory(fixture.root), State::ConfirmedAtLeastOne, "restored actual objects remain admissible after all reparse negatives");
    std::cout << "reparse root=1 index=1 backup=1 receipt=1\n";
    return true;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view group = argc > 1 ? argv[1] : "all";
        if (group == "all" || group == "inventory") Inventory();
        if (group == "all" || group == "identity") Identity();
        if (group == "all" || group == "budgets") Budgets();
        if ((group == "all" || group == "reparse") && !Reparse() && group == "reparse") return 77;
        Require(group == "all" || group == "inventory" || group == "identity" || group == "budgets" || group == "reparse", "unknown inspection test group");
        std::cout << "MapBackupInspection: " << group << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
