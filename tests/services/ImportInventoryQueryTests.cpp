#include "War3MapCore/Services/ImportInventoryQuery.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "../../src/Core/Mpq/MpqCrypt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> bool Code(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [code](const auto& value) { return value.code == code; });
}
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* data = reinterpret_cast<const std::byte*>(text.data()); return {data, data + text.size()};
}
void U32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 255U);
}
std::vector<std::byte> Imp(const std::vector<Resources::ImportRecord>& records) {
    std::vector<std::byte> bytes(8); U32(bytes, 0, 1); U32(bytes, 4, static_cast<std::uint32_t>(records.size()));
    for (const auto& record : records) {
        bytes.push_back(static_cast<std::byte>(record.flags));
        const auto path = Bytes(record.path); bytes.insert(bytes.end(), path.begin(), path.end()); bytes.push_back(std::byte{});
    }
    bytes.push_back(std::byte{0x91}); bytes.push_back(std::byte{0x37}); // 原未知尾部不丢失。
    return bytes;
}
struct Temp {
    std::filesystem::path root;
    Temp() {
        static std::atomic<unsigned> sequence{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-import-query-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directories(root), "create owned fixture directory");
    }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root, error); }
    void Put(const std::filesystem::path& path, const std::vector<std::byte>& bytes) const {
        std::filesystem::create_directories((root / path).parent_path());
        Require(IO::WriteFileBytes(root / path, bytes).Succeeded(), "write owned fixture bytes");
    }
};
ImportInventoryBinding Binding(const std::string& sha) { return {"inventory-fixture", 3, "rev:7", sha, "assets:2", "test-current-assets"}; }
const ImportInventoryRow& Find(const ImportInventoryPage& page, std::string_view kind, std::string_view raw, unsigned ordinal = 0) {
    for (const auto& row : page.rows) if (row.kind == kind && row.rawPath == raw) { if (!ordinal--) return row; }
    throw std::runtime_error("expected inventory row missing");
}

void ArchiveFlagsAndRawIdentity() {
    Temp temporary;
    const std::string invalidName("raw-\x81.bin", 9);
    const auto imp = Imp({{5, "standard.bin"}, {8, "legacy.bin"}, {10, "direct.bin"}, {13, "direct.bin"},
        {13, "direct.bin"}, {99, "unknown.bin"}, {5, "both.bin"}, {99, "absent.bin"}, {13, "empty.bin"}, {13, invalidName}});
    const std::vector<MapArchiveMember> members{{"war3map.imp", imp}, {"war3mapImported/standard.bin", Bytes("standard")},
        {"legacy.bin", Bytes("legacy")}, {"direct.bin", Bytes("direct")}, {"unknown.bin", Bytes("identified")},
        {"both.bin", Bytes("first")}, {"war3mapImported/both.bin", Bytes("second")}, {"empty.bin", {}},
        {"payload.ai", {std::byte{0xFF}, std::byte{0}, std::byte{0x91}}}};
    const auto written = WriteVerifiedArchive(temporary.root / "source.w3x", members);
    Require(written.Succeeded(), "create real archive fixture");
    const auto opened = MapArchiveSession::Open(written.value->path); Require(opened.Succeeded(), "open source archive");
    const auto binding = Binding(opened.value->SourceSha256());
    const auto captured = CaptureImportInventory(*opened.value, binding); Require(captured.Succeeded(), "capture archive inventory");
    const auto page = (*captured.value)->Query(binding, {}, {}, 128); Require(page.Succeeded(), "query real archive identities");
    Require(page.value->description.impRecordCount == 10 && page.value->description.impSha256 == Sha256(imp).value,
        "IMP original records and bytes digest preserved");
    const auto& standard = Find(*page.value, "imp-record", "standard.bin");
    Require(standard.flags == 5 && standard.sourcePath == "war3mapImported\\standard.bin" && standard.sha256 == Sha256(Bytes("standard")).value,
        "flags5 resolves actual standard member through original service");
    Require(Find(*page.value, "imp-record", "legacy.bin").sourcePath == "legacy.bin", "flags8 respects real direct-path evidence");
    Require(Find(*page.value, "imp-record", "unknown.bin").state == "present", "unknown flags needs actual unique identity");
    const auto& ambiguous = Find(*page.value, "imp-record", "both.bin");
    Require(!ambiguous.sourcePath && !ambiguous.archivePath && ambiguous.candidates.size() == 2
        && ambiguous.candidates[0].present && ambiguous.candidates[1].present
        && Code(ambiguous, "IMPORT_PATH_AMBIGUOUS"), "dual paths remain explicit with both identities and no chosen member");
    Require(Code(Find(*page.value, "imp-record", "absent.bin"), "IMPORT_FLAGS_UNSUPPORTED"), "unknown flag missing is unresolved not guessed");
    const auto& empty = Find(*page.value, "imp-record", "empty.bin");
    Require(empty.state == "present" && empty.bytes == 0 && empty.sha256 == Sha256(std::vector<std::byte>{}).value,
        "real empty member differs from missing member");
    Require(Find(*page.value, "imp-record", invalidName).rawPath == invalidName
        && Code(Find(*page.value, "imp-record", invalidName), "IMPORT_PATH_UNSAFE"), "nonUTF raw name preserved without unsafe member lookup");
    const auto& first = Find(*page.value, "imp-record", "direct.bin", 1);
    const auto& second = Find(*page.value, "imp-record", "direct.bin", 2);
    Require(first.flags == 13 && second.flags == 13 && first.importId != second.importId, "duplicate identical IMP records retain separate stable IDs");
    const auto detail = (*captured.value)->Get(binding, page.value->description.inventorySha256, second.importId);
    Require(detail.Succeeded() && detail.value->rawPath == second.rawPath && detail.value->flags == second.flags, "get locates exact original record");
    Require(Find(*page.value, "physical-member", "payload.ai").bytes == 3, "arbitrary binary extension treated only as member identity");
    Require(Sha256File(written.value->path).value == std::optional(written.value->sha256), "all queries leave actual source unchanged");
}

void DirectoryAliasOverlayAndStaleness() {
    Temp temporary;
    const auto originalImp = Imp({{13, "assets/one.bin"}, {13, "assets/two.bin"}, {13, "missing.bin"}});
    temporary.Put("map/WAR3MAP.IMP", originalImp); temporary.Put("assets/one.bin", Bytes("one")); temporary.Put("assets/two.bin", Bytes("two"));
    const auto directory = DirectoryWorkspace::Open(temporary.root); Require(directory.Succeeded(), "open real directory");
    const auto binding = Binding(directory.value->Snapshot().aggregateSha256);
    ImportInventoryOverlay overlay{true, {{"ASSETS\\one.bin", "assets/renamed.bin"}, {"assets/two.bin", ""}},
        {{"queued.bin", 600}, {"queued.bin", 900}}};
    const auto captured = CaptureImportInventory(*directory.value, binding, overlay); Require(captured.Succeeded(), "capture live asset metadata overlay");
    const auto page = (*captured.value)->Query(binding, {}, {}, 128); Require(page.Succeeded(), "query overlay source identities");
    Require(page.value->description.impSourcePath == "map/WAR3MAP.IMP", "original root/map IMP path retained");
    const auto& moved = Find(*page.value, "imp-record", "assets/one.bin");
    Require(moved.overlayState == "moved" && moved.archivePath == "assets/renamed.bin" && moved.bytes == 3
        && moved.sha256 == Sha256(Bytes("one")).value, "moved path has original source digest not invented target bytes");
    const auto& removed = Find(*page.value, "imp-record", "assets/two.bin");
    Require(removed.overlayState == "removed" && !removed.archivePath && removed.state == "present", "removed overlay retains original physical evidence");
    const auto& pending = Find(*page.value, "queued-unfrozen", "queued.bin");
    Require(!pending.bytes && !pending.sha256 && !pending.sourcePath && pending.reportedBytes == 600, "pending not represented as frozen content");
    Require(pending.importId != Find(*page.value, "queued-unfrozen", "queued.bin", 1).importId, "duplicate pending rows do not alias IDs");
    const auto& missing = Find(*page.value, "imp-record", "missing.bin");
    Require(missing.state == "missing" && !missing.bytes && !missing.sha256, "full directory snapshot proves actual missing");
    auto badBinding = binding; badBinding.assetRevision = "assets:3";
    Require(Code((*captured.value)->Query(badBinding), "IMPORT_REVISION_CONFLICT"), "asset revision CAS");
    badBinding = binding; ++badBinding.generation;
    Require(Code((*captured.value)->Query(badBinding), "IMPORT_REVISION_CONFLICT"), "document generation CAS");
    badBinding = binding; badBinding.sourceSha256 = std::string(64, '0');
    Require(Code((*captured.value)->Query(badBinding), "IMPORT_REVISION_CONFLICT"), "full source SHA CAS");
    const auto firstPage = (*captured.value)->Query(binding, {}, {}, 1); Require(firstPage.Succeeded() && firstPage.value->next, "get overlay cursor");
    overlay.pending[0].reportedBytes = 601;
    const auto nextSnapshot = CaptureImportInventory(*directory.value, binding, overlay); Require(nextSnapshot.Succeeded(), "new queue metadata snapshot");
    Require(Code((*nextSnapshot.value)->Query(binding, {}, firstPage.value->next), "IMPORT_CURSOR_STALE"), "queue changed without host revision still invalidates cursor");
    Require(Code((*nextSnapshot.value)->Get(binding, page.value->description.inventorySha256, moved.importId), "IMPORT_REVISION_CONFLICT"), "get binds inventory SHA");
    Require(Code((*captured.value)->Get(binding, page.value->description.inventorySha256, "0"), "IMPORT_ID_MISSING"), "UI row number not accepted as import ID");
    Require(IO::ReadFileBytes(temporary.root / "map/WAR3MAP.IMP").value == std::optional(originalImp), "metadata never rewrites IMP");
    temporary.Put("assets/one.bin", Bytes("ONE"));
    Require(!(*captured.value)->Query(binding).Succeeded(), "same-size source change detected by original full snapshot verification");
}

void MissingDamagedAndAmbiguousImp() {
    Temp temporary; temporary.Put("plain.bin", Bytes("plain"));
    auto directory = DirectoryWorkspace::Open(temporary.root); Require(directory.Succeeded(), "missing IMP directory");
    auto captured = CaptureImportInventory(*directory.value, Binding(directory.value->Snapshot().aggregateSha256));
    Require(captured.Succeeded() && (*captured.value)->Description().impState == "missing", "missing IMP explicit, physical members still visible");
    temporary.Put("war3map.imp", Bytes("broken"));
    directory = DirectoryWorkspace::Open(temporary.root);
    Require(!CaptureImportInventory(*directory.value, Binding(directory.value->Snapshot().aggregateSha256)).Succeeded(), "damaged IMP never empty successful table");
    std::vector<std::byte> oversizedCount(8); U32(oversizedCount, 0, 1); U32(oversizedCount, 4, 262145);
    temporary.Put("war3map.imp", oversizedCount); directory = DirectoryWorkspace::Open(temporary.root);
    Require(Code(CaptureImportInventory(*directory.value, Binding(directory.value->Snapshot().aggregateSha256)), "IMPORT_CAPTURE_BUDGET"), "declared IMP count rejected before parser allocation");
    temporary.Put("war3map.imp", Imp({})); temporary.Put("map/war3map.imp", Imp({}));
    directory = DirectoryWorkspace::Open(temporary.root);
    Require(Code(CaptureImportInventory(*directory.value, Binding(directory.value->Snapshot().aggregateSha256)), "IMPORT_IMP_AMBIGUOUS"), "root/map IMP ambiguity rejected");
}

void PaginationAndAtomicBoundaries() {
    Temp temporary;
    const auto directory = DirectoryWorkspace::Open(temporary.root); Require(directory.Succeeded(), "empty directory snapshot");
    const auto binding = Binding(directory.value->Snapshot().aggregateSha256);
    ImportInventoryOverlay overlay; overlay.includesUnsavedChanges = true;
    for (unsigned index = 0; index < 129; ++index) overlay.pending.push_back({"Item" + std::to_string(index) + ".bin", index});
    auto captured = CaptureImportInventory(*directory.value, binding, overlay); Require(captured.Succeeded(), "129 original queue metadata records");
    std::optional<ImportInventoryCursor> cursor;
    std::set<std::string> ids;
    do {
        const auto page = (*captured.value)->Query(binding, "Item", cursor, 64);
        Require(page.Succeeded() && page.value->matchedTotal == 129, "bounded full predicate total");
        for (const auto& row : page.value->rows) Require(ids.insert(row.importId).second, "pagination duplicates no record");
        cursor = page.value->next;
    } while (cursor);
    Require(ids.size() == 129, "129 records not truncated or skipped");
    const auto first = (*captured.value)->Query(binding, "Item", {}, 64);
    Require(Code((*captured.value)->Query(binding, "item", first.value->next), "IMPORT_CURSOR_STALE"), "cursor binds case-sensitive query");
    Require((*captured.value)->Query(binding, "item").value->matchedTotal == 0, "filter does not lowercase original byte path");
    Require(Code((*captured.value)->Query(binding, {}, {}, 129), "IMPORT_QUERY_BUDGET"), "result count budget");
    Require(Code((*captured.value)->Query(binding, {}, {}, 1, 4097), "IMPORT_QUERY_BUDGET"), "scan budget cannot expand");
    overlay.pending.clear();
    for (unsigned index = 0; index < 4096; ++index) overlay.pending.push_back({"ordinary" + std::to_string(index), {}});
    overlay.pending.push_back({"UniqueLastMatch", {}});
    captured = CaptureImportInventory(*directory.value, binding, overlay); Require(captured.Succeeded(), "large original queue metadata snapshot");
    const auto empty = (*captured.value)->Query(binding, "UniqueLastMatch");
    Require(empty.Succeeded() && empty.value->rows.empty() && !empty.value->matchedTotal && empty.value->next
        && empty.value->examined == 4096 && empty.value->scanBudgetExhausted, "empty scanned prefix keeps unknown total and continuation");
    const auto last = (*captured.value)->Query(binding, "UniqueLastMatch", empty.value->next);
    Require(last.Succeeded() && last.value->rows.size() == 1 && !last.value->next && !last.value->matchedTotal, "final large-table continuation finds only last match");
    overlay.includesUnsavedChanges = false;
    Require(Code(CaptureImportInventory(*directory.value, binding, overlay), "IMPORT_OVERLAY_SCOPE"), "unreported overlay refused");
    overlay = {true, {{"same/path.bin", "a.bin"}, {"SAME\\PATH.BIN", "b.bin"}}, {}};
    Require(Code(CaptureImportInventory(*directory.value, binding, overlay), "IMPORT_OVERLAY_AMBIGUOUS"), "canonical duplicate source edit rejected");
}

// 原创三成员MPQ夹具：listfile只列IMP，payload不列名；IMP以明确名字引用payload。
// 此处独立构造wire作为oracle，不修改产品MPQ实现，不使用任何作者资源。
void OpaqueArchiveExactEvidence() {
    Temp temporary;
    const auto imp = Imp({{13, "hidden-payload.bin"}}), payload = Bytes("\x01\x7f\x02\x11");
    const auto list = Bytes("war3map.imp\r\n");
    constexpr std::uint32_t hashCount = 8, blockCount = 3;
    const std::uint32_t impAt = 32, payloadAt = impAt + static_cast<std::uint32_t>(imp.size());
    const std::uint32_t listAt = payloadAt + static_cast<std::uint32_t>(payload.size());
    const std::uint32_t hashAt = listAt + static_cast<std::uint32_t>(list.size()), blockAt = hashAt + hashCount * 16;
    std::vector<std::byte> bytes(blockAt + blockCount * 16);
    U32(bytes, 0, 0x1A51504D); U32(bytes, 4, 32); U32(bytes, 8, static_cast<std::uint32_t>(bytes.size())); U32(bytes, 12, 0x00030000);
    U32(bytes, 16, hashAt); U32(bytes, 20, blockAt); U32(bytes, 24, hashCount); U32(bytes, 28, blockCount);
    std::copy(imp.begin(), imp.end(), bytes.begin() + impAt); std::copy(payload.begin(), payload.end(), bytes.begin() + payloadAt);
    std::copy(list.begin(), list.end(), bytes.begin() + listAt); std::fill(bytes.begin() + hashAt, bytes.begin() + blockAt, std::byte{0xff});
    const auto& crypt = Mpq::MpqCrypt::Instance();
    const auto entry = [&](std::string_view name, std::uint32_t index, std::uint32_t at, std::uint32_t size) {
        auto bucket = crypt.HashString(name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        while (bytes[hashAt + bucket * 16 + 12] != std::byte{0xff}) bucket = (bucket + 1) & (hashCount - 1);
        const auto offset = hashAt + bucket * 16;
        U32(bytes, offset, crypt.HashString(name, Mpq::MpqCrypt::HashType::NameA)); U32(bytes, offset + 4, crypt.HashString(name, Mpq::MpqCrypt::HashType::NameB));
        U32(bytes, offset + 8, 0); U32(bytes, offset + 12, index);
        U32(bytes, blockAt + index * 16, at); U32(bytes, blockAt + index * 16 + 4, size);
        U32(bytes, blockAt + index * 16 + 8, size); U32(bytes, blockAt + index * 16 + 12, Mpq::kFileExists | Mpq::kFileSingleUnit);
    };
    entry("war3map.imp", 0, impAt, static_cast<std::uint32_t>(imp.size())); entry("hidden-payload.bin", 1, payloadAt, static_cast<std::uint32_t>(payload.size()));
    entry("(listfile)", 2, listAt, static_cast<std::uint32_t>(list.size()));
    crypt.EncryptBlock(bytes.data() + hashAt, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockAt, blockCount * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    temporary.Put("opaque.w3x", bytes);
    const auto source = MapArchiveSession::Open(temporary.root / "opaque.w3x"); Require(source.Succeeded() && source.value->HasOpaqueMembers(), "real partial-names archive");
    const auto binding = Binding(source.value->SourceSha256());
    const auto captured = CaptureImportInventory(*source.value, binding); Require(captured.Succeeded(), "capture opaque metadata");
    const auto page = (*captured.value)->Query(binding); Require(page.Succeeded() && page.value->description.hasOpaqueArchiveMembers, "opaque coverage remains partial");
    const auto& row = Find(*page.value, "imp-record", "hidden-payload.bin");
    Require(row.state == "present" && row.bytes == payload.size() && row.sha256 == Sha256(payload).value, "original exact service finds unlisted member and proves SHA");
    Require(std::none_of(page.value->rows.begin(), page.value->rows.end(), [](const auto& value) {
        return value.kind == "physical-member" && value.rawPath == "hidden-payload.bin";
    }), "exact lookup does not fabricate complete listfile inventory");
    Require(Sha256File(temporary.root / "opaque.w3x").value == Sha256(bytes).value, "opaque source bytes unchanged");
}
// 一页成员总字节超过单次精确核对预算时分批核对；单个超过单项预算的成员列为 oversized，不让整页失败。
void LargeMembersDoNotFailPage() {
    Temp temporary;
    const auto filled = [](std::size_t size, unsigned char value) { return std::vector<std::byte>(size, std::byte{value}); };
    const ArchiveExactMemberLimits budget;
    const auto medium = filled(budget.totalBytes / 2 - 1024, 0x11), medium2 = filled(budget.totalBytes / 2 - 1024, 0x22);
    const auto huge = filled(budget.memberBytes + 4096, 0x33);
    const std::vector<MapArchiveMember> members{{"a.mp3", medium}, {"b.mp3", medium2},
        {"huge.mp3", huge}, {"small.blp", Bytes("small")}};
    const auto written = WriteVerifiedArchive(temporary.root / "large.w3x", members);
    Require(written.Succeeded(), "create large-member archive");
    const auto opened = MapArchiveSession::Open(written.value->path); Require(opened.Succeeded(), "open large-member archive");
    const auto binding = Binding(opened.value->SourceSha256());
    const auto captured = CaptureImportInventory(*opened.value, binding); Require(captured.Succeeded(), "capture large-member inventory");
    const auto page = (*captured.value)->Query(binding, {}, {}, 128);
    Require(page.Succeeded(), "page larger than one exact budget still lists");
    const auto& a = Find(*page.value, "physical-member", "a.mp3");
    const auto& b = Find(*page.value, "physical-member", "b.mp3");
    Require(a.state == "present" && a.sha256 == Sha256(medium).value && b.state == "present" && b.sha256 == Sha256(medium2).value,
        "batched members keep exact identities");
    const auto& large = Find(*page.value, "physical-member", "huge.mp3");
    Require(large.state == "oversized" && !large.sha256 && large.bytes == huge.size() && Code(large, "IMPORT_MEMBER_OVER_IDENTITY_BUDGET"),
        "oversized member is listed without fabricated digest");
    Require(Find(*page.value, "physical-member", "small.blp").state == "present", "small member unaffected");
}
} // namespace

int main() {
    try {
        ArchiveFlagsAndRawIdentity(); DirectoryAliasOverlayAndStaleness(); MissingDamagedAndAmbiguousImp();
        PaginationAndAtomicBoundaries(); OpaqueArchiveExactEvidence(); LargeMembersDoNotFailPage();
        std::cout << "PASS import inventory: flags/identities, directory overlay, source gates, pagination, opaque exact evidence\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
