#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "../src/Core/Mpq/MpqCrypt.h"

#include <Windows.h>

#include <algorithm>
#include <ranges>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {

using namespace War3Map;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
void RequireSuccess(const Result<T>& result, const std::string& context) {
    if (result.Succeeded()) return;
    std::string diagnostic = context;
    for (const auto& item : result.diagnostics) diagnostic += " | " + item.code + ": " + item.message;
    throw std::runtime_error(diagnostic);
}

class TempDirectory {
public:
    explicit TempDirectory(const std::filesystem::path& parent = std::filesystem::temp_directory_path()) {
        static std::atomic<unsigned> sequence{0};
        path_ = parent / ("Forge-archive-session-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
            + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directories(path_), "temporary directory");
    }
    ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(file.good(), "read fixture bytes");
    const auto size = file.tellg();
    Require(size >= 0, "fixture size");
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!result.empty()) file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(size));
    Require(file.good(), "complete fixture read");
    return result;
}

std::vector<std::byte> Prefix() {
    std::vector<std::byte> result(1024, std::byte{0xC4});
    std::memcpy(result.data(), "HM3W", 4);
    return result;
}

std::vector<MapArchiveMember> Members() {
    return {{"war3map.wtg", Bytes("original-wtg")}, {"war3map.wct", Bytes("original-wct")},
        {"war3map.j", Bytes("existing-script")}, {"unknown/opaque.bin", {std::byte{0}, std::byte{0xFF}, std::byte{0x91}}},
        {"imports/empty.bin", {}}};
}

std::filesystem::path CreateSource(const TempDirectory& directory) {
    const auto path = directory.Path() / "source.w3x";
    const auto written = WriteVerifiedArchive(path, Members(), Prefix());
    RequireSuccess(written, "write original fixture");
    return path;
}

void RequireNoTemporary(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        Require(!entry.path().filename().wstring().starts_with(L".forge-candidate-"), "failed save leaked its temporary file");
    }
}

void TestCompleteRoundTripAndIndependentBaseline() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    const auto originalHash = Sha256File(source);
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open session");
    auto& session = *opened.value;
    Require(session.Members().size() == 5, "snapshot excludes generated listfile");
    Require(session.HasMember("UNKNOWN\\OPAQUE.BIN"), "canonical lookup");
    Require(session.HasMember("(listfile)"), "raw listfile available for reading");
    Require(!session.ReadMember("missing.bin").Succeeded(), "missing must remain missing");
    const std::vector<MemberReplacement> edits{{"WAR3MAP.WTG", Bytes("edited-wtg")},
        {"war3map.wct", Bytes("edited-wct")}, {"war3map.w3t", Bytes("new-object-member")}};
    const auto candidatePath = directory.Path() / "candidate.w3x";
    const auto saved = session.SaveCandidate(candidatePath, edits);
    RequireSuccess(saved, "save candidate");
    Require(saved.value->memberCount == 6, "new member included in receipt");
    Require(saved.value->sha256 == *Sha256File(candidatePath).value, "receipt hash verifies candidate");
    Require(session.SourceSha256() == *originalHash.value, "successful save does not silently commit original baseline");
    Require(session.ReadMember("war3map.wtg").value == std::optional(Bytes("original-wtg")), "session remains original snapshot");
    Require(session.VerifySourceUnchanged().Succeeded(), "source unchanged after save");
    auto reopened = MapArchiveSession::Open(candidatePath);
    RequireSuccess(reopened, "reopen candidate");
    Require(std::equal(reopened.value->Prefix().begin(), reopened.value->Prefix().end(), Prefix().begin()), "1024-byte prefix preserved");
    Require(reopened.value->ReadMember("war3map.wtg").value == std::optional(Bytes("edited-wtg")), "WTG replaced");
    Require(reopened.value->ReadMember("war3map.wct").value == std::optional(Bytes("edited-wct")), "WCT replaced");
    for (const auto& member : Members()) {
        if (member.name == "war3map.wtg" || member.name == "war3map.wct") continue;
        Require(reopened.value->ReadMember(member.name).value == std::optional(member.data), "untouched member bytes preserved");
    }
    Require(reopened.value->ReadMember("war3map.w3t").value == std::optional(Bytes("new-object-member")), "new member persists");
    RequireNoTemporary(directory.Path());
}

void TestRejectedChangesLeaveSessionAndFilesUntouched() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open rejection fixture");
    auto& session = *opened.value;
    const auto sourceHash = session.SourceSha256();
    const std::vector<std::vector<MemberReplacement>> rejected{
        {{"war3map.wtg", Bytes("single-half")}},
        {{"War3Map.w3u", Bytes("first")}, {"WAR3MAP.W3U", Bytes("second")}},
        {{"a/b.txt", Bytes("first")}, {"a\\b.txt", Bytes("second")}},
        {{"../escape.bin", Bytes("invalid")}},
        {{"folder//empty.bin", Bytes("invalid")}},
        {{"C:/absolute.bin", Bytes("invalid")}},
        {{"folder/NUL.bin", Bytes("invalid")}},
        {{"(LISTFILE)", Bytes("replacement")}}
    };
    for (std::size_t i = 0; i < rejected.size(); ++i) {
        const auto target = directory.Path() / ("rejected-" + std::to_string(i) + ".w3x");
        Require(!session.SaveCandidate(target, rejected[i]).Succeeded(), "invalid replacement must fail");
        Require(!std::filesystem::exists(target), "invalid replacement must not publish candidate");
    }
    Require(!session.SaveCandidate(source, {}).Succeeded(), "cannot overwrite source");
    const auto occupied = directory.Path() / "occupied.w3x";
    { std::ofstream file(occupied, std::ios::binary); file << "keep-me"; }
    const auto occupiedHash = Sha256File(occupied);
    Require(!session.SaveCandidate(occupied, {}).Succeeded(), "cannot overwrite other existing target");
    Require(Sha256File(occupied).value == occupiedHash.value, "existing target remains untouched");
    Require(!session.SaveCandidate(directory.Path() / "missing" / "candidate.w3x", {}).Succeeded(), "missing parent is rejected");
    Require(session.SourceSha256() == sourceHash && Sha256File(source).value == std::optional(sourceHash), "failure does not change baseline");
    RequireNoTemporary(directory.Path());
}

void TestExternalSourceChangeAndReplacement() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open external-change fixture");
    {
        std::fstream file(source, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(20); file.put('x');
    }
    Require(!opened.value->VerifySourceUnchanged().Succeeded(), "in-place mutation detected");
    const auto staleCapture = opened.value->CaptureExactMembers(std::vector<std::string>{"war3map.j"});
    Require(!staleCapture.Succeeded() && !staleCapture.value && staleCapture.diagnostics.front().code == "SOURCE_CHANGED",
        "exact capture rejects a stale source without returning partial bytes");
    const auto staleIdentity = opened.value->DescribeExactMemberIdentities(opened.value->SourceSha256(), std::vector<std::string>{"war3map.j"});
    Require(!staleIdentity.value && staleIdentity.diagnostics.front().code == "SOURCE_CHANGED",
        "member identities cannot convert an actual stale-source read failure into absence");
    const auto target = directory.Path() / "changed-source.w3x";
    Require(!opened.value->SaveCandidate(target, {}).Succeeded() && !std::filesystem::exists(target), "changed source blocks publication");
    Require(opened.value->ReadMember("war3map.wtg").value == std::optional(Bytes("original-wtg")), "external changes never enter snapshot");

    const auto independent = directory.Path() / "identity.w3x";
    RequireSuccess(WriteVerifiedArchive(independent, Members()), "write identity fixture");
    auto identitySession = MapArchiveSession::Open(independent);
    RequireSuccess(identitySession, "open identity fixture");
    const auto moved = directory.Path() / "original-file-object.w3x";
    std::filesystem::rename(independent, moved);
    std::filesystem::copy_file(moved, independent);
    Require(Sha256File(independent).value == std::optional(identitySession.value->SourceSha256()), "replacement has equal bytes");
    Require(!identitySession.value->VerifySourceUnchanged().Succeeded(), "equal-byte replacement must still invalidate identity");
    Require(!identitySession.value->CaptureExactMembers(std::vector<std::string>{"war3map.j"}).value,
        "equal-byte replacement also invalidates exact capture");
}

void PutU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::filesystem::path CreateOpaqueSource(const TempDirectory& directory, bool aliasHash = false) {
    const auto& crypt = Mpq::MpqCrypt::Instance();
    const auto known = Bytes("old-known");
    const auto opaque = Bytes("secret-raw-12345");
    const auto listfile = Bytes("known.bin\r\n");
    constexpr std::uint32_t hashCount = 8;
    constexpr std::uint32_t blockCount = 3;
    const std::uint32_t knownPos = 32;
    const std::uint32_t opaquePos = knownPos + static_cast<std::uint32_t>(known.size());
    const std::uint32_t listPos = opaquePos + static_cast<std::uint32_t>(opaque.size());
    const std::uint32_t hashPos = listPos + static_cast<std::uint32_t>(listfile.size());
    const std::uint32_t blockPos = hashPos + hashCount * 16;
    const std::uint32_t archiveSize = blockPos + blockCount * 16;
    std::vector<std::byte> bytes(archiveSize);
    PutU32(bytes, 0, 0x1A51504D); PutU32(bytes, 4, 32); PutU32(bytes, 8, archiveSize);
    PutU32(bytes, 12, 0x00030000); PutU32(bytes, 16, hashPos); PutU32(bytes, 20, blockPos);
    PutU32(bytes, 24, hashCount); PutU32(bytes, 28, blockCount);
    std::copy(known.begin(), known.end(), bytes.begin() + knownPos);
    std::copy(opaque.begin(), opaque.end(), bytes.begin() + opaquePos);
    crypt.EncryptBlock(bytes.data() + opaquePos, opaque.size(),
        crypt.HashString("opaque.bin", Mpq::MpqCrypt::HashType::FileKey));
    std::copy(listfile.begin(), listfile.end(), bytes.begin() + listPos);
    std::fill(bytes.begin() + hashPos, bytes.begin() + blockPos, std::byte{0xFF});
    const auto putHash = [&](std::string_view name, std::uint32_t blockIndex) {
        std::uint32_t bucket = crypt.HashString(name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        while (true) {
            const auto at = hashPos + bucket * 16;
            std::uint32_t current = 0;
            std::memcpy(&current, bytes.data() + at + 12, 4);
            if (current == 0xFFFFFFFF) {
                PutU32(bytes, at, crypt.HashString(name, Mpq::MpqCrypt::HashType::NameA));
                PutU32(bytes, at + 4, crypt.HashString(name, Mpq::MpqCrypt::HashType::NameB));
                PutU32(bytes, at + 8, 0); PutU32(bytes, at + 12, blockIndex);
                return;
            }
            bucket = (bucket + 1) & (hashCount - 1);
        }
    };
    putHash("known.bin", 0);
    putHash("opaque.bin", aliasHash ? 0 : 1);
    putHash("(listfile)", 2);
    crypt.EncryptBlock(bytes.data() + hashPos, hashCount * 16,
        crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    PutU32(bytes, blockPos, knownPos); PutU32(bytes, blockPos + 4, static_cast<std::uint32_t>(known.size()));
    PutU32(bytes, blockPos + 8, static_cast<std::uint32_t>(known.size()));
    PutU32(bytes, blockPos + 12, Mpq::kFileExists | Mpq::kFileSingleUnit);
    PutU32(bytes, blockPos + 16, opaquePos); PutU32(bytes, blockPos + 20, static_cast<std::uint32_t>(opaque.size()));
    PutU32(bytes, blockPos + 24, static_cast<std::uint32_t>(opaque.size()));
    PutU32(bytes, blockPos + 28, Mpq::kFileExists | Mpq::kFileSingleUnit | Mpq::kFileEncrypted);
    PutU32(bytes, blockPos + 32, listPos); PutU32(bytes, blockPos + 36, static_cast<std::uint32_t>(listfile.size()));
    PutU32(bytes, blockPos + 40, static_cast<std::uint32_t>(listfile.size()));
    PutU32(bytes, blockPos + 44, Mpq::kFileExists | Mpq::kFileSingleUnit);
    crypt.EncryptBlock(bytes.data() + blockPos, blockCount * 16,
        crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    const auto path = directory.Path() / (aliasHash ? "opaque-alias.w3x" : "opaque-source.w3x");
    { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    return path;
}

void TestOpaqueMemberPreservedByAppendOnlyPatch() {
    TempDirectory directory;
    const auto source = CreateOpaqueSource(directory);
    const auto originalBytes = ReadBytes(source);
    auto originalArchive = Mpq::MpqArchive::OpenReadOnly(source);
    RequireSuccess(originalArchive, "open opaque fixture");
    Require(!originalArchive.value->ListFilesStrict().Succeeded(), "full rewrite must reject anonymous block");
    Require(originalArchive.value->ReadFile("opaque.bin").value == std::optional(Bytes("secret-raw-12345")),
        "opaque encrypted member has stable fixture identity");
    auto session = MapArchiveSession::Open(source);
    RequireSuccess(session, "open opaque-preserving session");
    Require(session.value->HasOpaqueMembers(), "session exposes incomplete inventory");
    Require(session.value->OpaqueMemberCount() == 1, "session counts anonymous members");
    Require(session.value->Members().size() == 1, "only listed named member is available to editor");
    const auto target = directory.Path() / "opaque-candidate.w3x";
    const std::vector<MemberReplacement> edits{{"KNOWN.BIN", Bytes("new-known-longer")}};
    const auto saved = session.value->SaveCandidate(target, edits);
    RequireSuccess(saved, "append-only opaque save");
    Require(saved.value->memberCount == 2, "receipt counts named and anonymous members");
    auto candidate = Mpq::MpqArchive::OpenReadOnly(target);
    RequireSuccess(candidate, "reopen opaque candidate");
    Require(candidate.value->ReadFile("known.bin").value == std::optional(Bytes("new-known-longer")),
        "named replacement is readable");
    Require(candidate.value->ReadFile("opaque.bin").value == std::optional(Bytes("secret-raw-12345")),
        "anonymous encrypted file remains readable with original name");
    const auto candidateBytes = ReadBytes(target);
    Require(candidateBytes.size() > originalBytes.size(), "patch appended data and block table");
    for (std::size_t i = 0; i < originalBytes.size(); ++i) {
        if ((i >= 8 && i < 12) || (i >= 0x14 && i < 0x18)) continue;
        Require(originalBytes[i] == candidateBytes[i], "all source bytes outside two header fields preserved");
    }
    auto reopened = MapArchiveSession::Open(target);
    RequireSuccess(reopened, "candidate remains editable with opaque-preserving mode");
    Require(reopened.value->HasOpaqueMembers(), "anonymous member still intentionally anonymous");
    auto rebased = session.value->AfterSave(*saved.value, edits);
    RequireSuccess(rebased, "opaque save rebases after actual-file verification");
    Require(rebased.value->HasOpaqueMembers() && rebased.value->SourceSha256() == reopened.value->SourceSha256(),
        "opaque rebase keeps the verified file baseline");
    auto forgedReceipt = *saved.value;
    forgedReceipt.sha256[0] = forgedReceipt.sha256[0] == '0' ? '1' : '0';
    const auto forged = session.value->AfterSave(forgedReceipt, edits);
    Require(!forged.Succeeded() && !forged.value && forged.diagnostics.front().code == "SAVED_FILE_CHANGED",
        "opaque path refuses a forged receipt digest");
    const auto rejected = directory.Path() / "opaque-new-member.w3x";
    const std::vector<MemberReplacement> newMember{{"new.bin", Bytes("forbidden")}};
    Require(!session.value->SaveCandidate(rejected, newMember).Succeeded(),
        "new name rejected when anonymous blocks exist");
    Require(!std::filesystem::exists(rejected), "failed append-only patch leaves no candidate");
    Require(!MapArchiveSession::Open(CreateOpaqueSource(directory, true)).Succeeded(),
        "aliased hash-to-block identity rejects opaque session");
    RequireNoTemporary(directory.Path());
}

void TestExactMemberCaptureDistinguishesOpaqueMissingAndEmpty() {
    TempDirectory directory;
    auto session = MapArchiveSession::Open(CreateOpaqueSource(directory));
    RequireSuccess(session, "open exact opaque fixture");
    const std::vector<std::string> names{"OPAQUE.BIN", "known.bin", "absent.bin"};
    const auto captured = session.value->CaptureExactMembers(names);
    RequireSuccess(captured, "capture known hashes beyond incomplete listfile");
    Require(captured.value->size() == names.size() && (*captured.value)[0].name == names[0] &&
        (*captured.value)[0].present && (*captured.value)[0].bytes == Bytes("secret-raw-12345") &&
        (*captured.value)[1].present && (*captured.value)[1].bytes == Bytes("old-known") &&
        !(*captured.value)[2].present && (*captured.value)[2].bytes.empty(), "exact batch freezes presence and owned bytes");
    Require(!session.value->HasMember("opaque.bin") && !session.value->ReadMember("opaque.bin").Succeeded() &&
        session.value->OpaqueMemberCount() == 1, "capture does not promote anonymous bytes into the named inventory");
    const auto identities = session.value->DescribeExactMemberIdentities(session.value->SourceSha256(), names);
    RequireSuccess(identities, "describe exact known hashes omitted from listfile");
    Require(identities.value->sourceSha256 == session.value->SourceSha256() && identities.value->members.size() == 3 &&
        identities.value->members[0].name == names[0] && identities.value->members[0].present &&
        identities.value->members[0].byteSize == 16 && identities.value->members[0].sha256 == Sha256Text("secret-raw-12345").value &&
        !identities.value->members[2].present && !identities.value->members[2].byteSize && !identities.value->members[2].sha256,
        "identities own order, bytes and digest while Missing has no invented size or digest");
    for (const auto& expected : {std::string{}, std::string(64, 'x'), std::string(64, '0')})
        Require(!session.value->DescribeExactMemberIdentities(expected, names).value, "invalid or stale expected source cannot describe members");
    const std::vector<std::vector<std::string>> invalid{{"known.bin", "KNOWN.BIN"},
        {"a/b", "a\\b"}, {"../x"}, {"(listfile)"}, {"(attributes)"}, {"(signature)"}};
    for (const auto& request : invalid) Require(!session.value->CaptureExactMembers(request).value,
        "unsafe duplicate or reserved query must not produce a partial capture");
    ArchiveExactMemberLimits limits;
    limits.totalBytes = 20;
    Require(!session.value->CaptureExactMembers(names, limits).value, "cumulative bytes checked before reading next member");
    Require(!session.value->DescribeExactMemberIdentities(session.value->SourceSha256(), names, limits).value,
        "identity query propagates a later member budget failure without returning earlier partial identities");
    limits = {}; limits.memberBytes = 15;
    Require(!session.value->CaptureExactMembers(names, limits).value, "single-member input/output budget enforced");
    limits = {}; limits.memberCount = 2;
    Require(!session.value->CaptureExactMembers(names, limits).value, "count budget enforced");
    limits = {}; limits.pathBytes = 9;
    Require(!session.value->CaptureExactMembers(names, limits).value, "path budget enforced");
    limits = {}; limits.totalPathBytes = 10;
    Require(!session.value->CaptureExactMembers(names, limits).value, "cumulative path budget enforced");
    limits = {}; ++limits.memberCount;
    Require(!session.value->CaptureExactMembers(names, limits).value, "hard budget cannot be widened");
    auto complete = MapArchiveSession::Open(CreateSource(directory));
    RequireSuccess(complete, "open exact complete fixture");
    limits = {}; limits.totalBytes = 0; limits.memberBytes = 0;
    const auto empty = complete.value->CaptureExactMembers(std::vector<std::string>{"imports/empty.bin", "absent.bin"}, limits);
    RequireSuccess(empty, "zero budget captures only empty or missing members");
    Require((*empty.value)[0].present && (*empty.value)[0].bytes.empty() && !(*empty.value)[1].present,
        "present zero-length member remains distinct from true absence");
    const auto emptyIdentity = complete.value->DescribeExactMemberIdentities(complete.value->SourceSha256(),
        std::vector<std::string>{"imports/empty.bin", "absent.bin"}, limits);
    RequireSuccess(emptyIdentity, "zero retained budget still describes a true empty file");
    Require(emptyIdentity.value->members[0].present && emptyIdentity.value->members[0].byteSize == 0 &&
        emptyIdentity.value->members[0].sha256 == Sha256Text("").value && !emptyIdentity.value->members[1].sha256,
        "empty contents have their standard SHA-256, distinct from unknown Missing identity");
    RequireSuccess(complete.value->CaptureExactMembers(std::vector<std::string>{"war3map.j"}),
        "complete named archive uses the same exact capture path");
}

// 手工构造的原创归档：哈希项 locale/platform 字段取给定值，(attributes) 与 (signature)
// 存在但不写进 (listfile)，与世界编辑器保存的地图形态一致。
std::filesystem::path CreateMetadataSource(
    const TempDirectory& directory, std::uint32_t localePlatform, std::string_view fileName) {
    const auto& crypt = Mpq::MpqCrypt::Instance();
    const std::vector<MapArchiveMember> blocks{{"known.bin", Bytes("old-known")},
        {"(attributes)", Bytes("stale-crc-table")}, {"(signature)", Bytes("weak-signature-bytes")},
        {"(listfile)", Bytes("known.bin\r\n")}};
    constexpr std::uint32_t hashCount = 8;
    const auto blockCount = static_cast<std::uint32_t>(blocks.size());
    std::uint32_t dataSize = 0;
    for (const auto& block : blocks) dataSize += static_cast<std::uint32_t>(block.data.size());
    const std::uint32_t hashPos = 32 + dataSize;
    const std::uint32_t blockPos = hashPos + hashCount * 16;
    const std::uint32_t archiveSize = blockPos + blockCount * 16;
    std::vector<std::byte> bytes(archiveSize);
    PutU32(bytes, 0, 0x1A51504D); PutU32(bytes, 4, 32); PutU32(bytes, 8, archiveSize);
    PutU32(bytes, 12, 0x00030000); PutU32(bytes, 16, hashPos); PutU32(bytes, 20, blockPos);
    PutU32(bytes, 24, hashCount); PutU32(bytes, 28, blockCount);
    std::fill(bytes.begin() + hashPos, bytes.begin() + blockPos, std::byte{0xFF});
    std::uint32_t position = 32;
    for (std::uint32_t index = 0; index < blockCount; ++index) {
        const auto& block = blocks[index];
        std::copy(block.data.begin(), block.data.end(), bytes.begin() + position);
        std::uint32_t bucket = crypt.HashString(block.name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        for (;; bucket = (bucket + 1) & (hashCount - 1)) {
            const auto at = hashPos + bucket * 16;
            std::uint32_t current = 0;
            std::memcpy(&current, bytes.data() + at + 12, 4);
            if (current != 0xFFFFFFFF) continue;
            PutU32(bytes, at, crypt.HashString(block.name, Mpq::MpqCrypt::HashType::NameA));
            PutU32(bytes, at + 4, crypt.HashString(block.name, Mpq::MpqCrypt::HashType::NameB));
            PutU32(bytes, at + 8, localePlatform); PutU32(bytes, at + 12, index);
            break;
        }
        const auto size = static_cast<std::uint32_t>(block.data.size());
        PutU32(bytes, blockPos + index * 16, position); PutU32(bytes, blockPos + index * 16 + 4, size);
        PutU32(bytes, blockPos + index * 16 + 8, size);
        PutU32(bytes, blockPos + index * 16 + 12, Mpq::kFileExists | Mpq::kFileSingleUnit);
        position += size;
    }
    crypt.EncryptBlock(bytes.data() + hashPos, hashCount * 16,
        crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockPos, blockCount * 16,
        crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    const auto path = directory.Path() / std::string(fileName);
    { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    return path;
}

void TestReservedPlatformByteAndMetadataMembers() {
    TempDirectory directory;
    // platform 字节 0、reserved 字节 0xFF：locale/platform 字段按小端 u32 为 0xFF000000。
    const auto source = CreateMetadataSource(directory, 0xFF000000u, "metadata-source.w3x");
    auto archive = Mpq::MpqArchive::OpenReadOnly(source);
    RequireSuccess(archive, "open metadata fixture");
    const auto strict = archive.value->ListFilesStrict();
    RequireSuccess(strict, "reserved byte and unlisted metadata pass strict inventory");
    Require(strict.value->size() == 4, "strict inventory names every block including metadata");
    auto session = MapArchiveSession::Open(source);
    RequireSuccess(session, "open metadata session");
    Require(!session.value->HasOpaqueMembers(), "metadata members are named, not opaque");
    Require(session.value->Members().size() == 1, "metadata excluded from editable members");
    Require(session.value->MetadataMembers().size() == 2, "metadata members reported");
    Require(!session.value->HasMember("(attributes)"), "metadata not exposed as editable member");
    const auto target = directory.Path() / "metadata-candidate.w3x";
    const std::vector<MemberReplacement> edits{{"known.bin", Bytes("new-known")}};
    const auto saved = session.value->SaveCandidate(target, edits);
    RequireSuccess(saved, "rebuild drops stale metadata");
    Require(saved.value->memberCount == 1, "receipt counts only rebuilt members");
    auto candidate = Mpq::MpqArchive::OpenReadOnly(target);
    RequireSuccess(candidate, "reopen metadata candidate");
    Require(!candidate.value->HasFile("(attributes)") && !candidate.value->HasFile("(signature)"),
        "stale metadata is not written");
    Require(candidate.value->ReadFile("known.bin").value == std::optional(Bytes("new-known")), "replacement readable");
    const auto rejected = directory.Path() / "metadata-replace.w3x";
    const std::vector<MemberReplacement> metadataEdit{{"(ATTRIBUTES)", Bytes("forged")}};
    Require(!session.value->SaveCandidate(rejected, metadataEdit).Succeeded() && !std::filesystem::exists(rejected),
        "metadata members cannot be replaced");
    // platform 字节非零仍然 fail-closed。
    const auto platformSource = CreateMetadataSource(directory, 0x00010000u, "platform-source.w3x");
    auto platformArchive = Mpq::MpqArchive::OpenReadOnly(platformSource);
    RequireSuccess(platformArchive, "open platform fixture");
    const auto platformStrict = platformArchive.value->ListFilesStrict();
    Require(!platformStrict.Succeeded() && std::any_of(platformStrict.diagnostics.begin(), platformStrict.diagnostics.end(),
        [](const Diagnostic& item) { return item.code == "HASH_ENTRY_UNSUPPORTED"; }), "non-zero platform byte rejected");
    RequireNoTemporary(directory.Path());
}

void TestIncompleteListAndTrailingData() {
    TempDirectory directory;
    // 有效可读取的单成员 MPQ，但没有 listfile，编辑会话必须拒绝。
    const auto& crypt = Mpq::MpqCrypt::Instance();
    constexpr std::uint32_t hashPosition = 36, blockPosition = 100, archiveSize = 116;
    std::vector<std::byte> bytes(archiveSize);
    PutU32(bytes, 0, 0x1A51504D); PutU32(bytes, 4, 32); PutU32(bytes, 8, archiveSize);
    PutU32(bytes, 12, 0x00030000); PutU32(bytes, 16, hashPosition); PutU32(bytes, 20, blockPosition);
    PutU32(bytes, 24, 4); PutU32(bytes, 28, 1); PutU32(bytes, 32, 0x11223344);
    std::fill(bytes.begin() + hashPosition, bytes.begin() + blockPosition, std::byte{0xFF});
    const auto bucket = crypt.HashString("opaque.bin", Mpq::MpqCrypt::HashType::TableOffset) & 3;
    const auto entry = hashPosition + bucket * 16;
    PutU32(bytes, entry, crypt.HashString("opaque.bin", Mpq::MpqCrypt::HashType::NameA));
    PutU32(bytes, entry + 4, crypt.HashString("opaque.bin", Mpq::MpqCrypt::HashType::NameB));
    PutU32(bytes, entry + 8, 0); PutU32(bytes, entry + 12, 0);
    crypt.EncryptBlock(bytes.data() + hashPosition, 64, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    PutU32(bytes, blockPosition, 32); PutU32(bytes, blockPosition + 4, 4);
    PutU32(bytes, blockPosition + 8, 4); PutU32(bytes, blockPosition + 12, Mpq::kFileExists | Mpq::kFileSingleUnit);
    crypt.EncryptBlock(bytes.data() + blockPosition, 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    const auto path = directory.Path() / "no-listfile.w3x";
    { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    auto loose = Mpq::MpqArchive::OpenReadOnly(path);
    RequireSuccess(loose, "loose reader accepts valid no-list fixture");
    Require(loose.value->ReadFile("opaque.bin").Succeeded(), "fixture member can be decoded");
    Require(!MapArchiveSession::Open(path).Succeeded(), "incomplete inventory cannot become editable snapshot");
    loose.value.reset();

    const auto source = CreateSource(directory);
    { std::ofstream file(source, std::ios::binary | std::ios::app); file << "unknown-trailer"; }
    Require(!MapArchiveSession::Open(source).Succeeded(), "unrecognized physical trailer must not be silently dropped");
}

void TestProjectWriterAndInvalidPrefix() {
    TempDirectory directory;
    const std::vector<MapArchiveMember> project{{"project.json", Bytes("{\"version\":1}")},
        {"baseline/source.w3x", Bytes("embedded-original")}, {"overlay/table/unit.ini", Bytes("[hfoo]\nname=\"test\"\n")}};
    const auto path = directory.Path() / "document.forge";
    RequireSuccess(WriteVerifiedArchive(path, project), "generic verified project writer");
    auto opened = MapArchiveSession::Open(path);
    RequireSuccess(opened, "open standalone project archive");
    for (const auto& member : project) Require(opened.value->ReadMember(member.name).value == std::optional(member.data), "project snapshot member preserved");
    const auto invalidPath = directory.Path() / "invalid-prefix.forge";
    Require(!WriteVerifiedArchive(invalidPath, project, Bytes("bad-prefix")).Succeeded(), "unaligned prefix must fail");
    Require(!std::filesystem::exists(invalidPath), "invalid writer input must never publish");
    RequireNoTemporary(directory.Path());
}

// 原地保存会在源目录写出 ".<主名>.saving-*" 临时候选；任何结果下都不得残留。
void RequireNoSavingTemporary(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().wstring();
        Require(name.find(L".saving-") == std::wstring::npos, "in-place save leaked its temporary candidate");
    }
    RequireNoTemporary(directory);
}

std::vector<std::filesystem::path> DirectoryFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(directory)) return files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) files.push_back(entry.path().filename());
    }
    std::sort(files.begin(), files.end());
    return files;
}

void TestSaveInPlaceReplacesSourceAndKeepsBackup() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    const auto originalBytes = ReadBytes(source);
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open in-place fixture");
    const std::vector<MemberReplacement> edits{{"war3map.wtg", Bytes("in-place-wtg")},
        {"war3map.wct", Bytes("in-place-wct")}};
    const std::vector<std::string> removals{"imports/empty.bin"};
    const auto saved = opened.value->SaveInPlace(edits, removals);
    RequireSuccess(saved, "save in place");
    // 会话保存的是规范化后的最终路径（可能是长文件名而测试目录是 8.3 短名），按同一文件比较。
    Require(std::filesystem::equivalent(saved.value->written.path, source), "receipt names the source path");
    Require(saved.value->prunedBackups.empty(), "first save prunes nothing");
    const auto backupDirectory = directory.Path() / L"War3MapEditor Backups";
    Require(std::filesystem::equivalent(saved.value->backup.parent_path(), backupDirectory), "backup lands in default backup directory");
    Require(ReadBytes(saved.value->backup) == originalBytes, "backup holds the exact original bytes");
    Require(saved.value->written.sha256 == *Sha256File(source).value, "receipt hash matches replaced source");

    auto reopened = MapArchiveSession::Open(source);
    RequireSuccess(reopened, "reopen saved source");
    Require(reopened.value->ReadMember("war3map.wtg").value == std::optional(Bytes("in-place-wtg")), "WTG written back");
    Require(reopened.value->ReadMember("war3map.wct").value == std::optional(Bytes("in-place-wct")), "WCT written back");
    Require(!reopened.value->HasMember("imports/empty.bin"), "removal applied");
    Require(reopened.value->ReadMember("unknown/opaque.bin").value
        == std::optional(std::vector<std::byte>{std::byte{0}, std::byte{0xFF}, std::byte{0x91}}), "untouched member preserved");
    Require(std::equal(reopened.value->Prefix().begin(), reopened.value->Prefix().end(), Prefix().begin()), "prefix preserved");
    // 旧会话的基线指向被替换掉的文件对象，不能再用于保存。
    Require(!opened.value->VerifySourceUnchanged().Succeeded(), "stale session detects replaced source");
    RequireNoSavingTemporary(directory.Path());

    // 第二次写回（在新会话上）：备份目录依次保留原图与第一次保存的结果。
    const auto firstSavedBytes = ReadBytes(source);
    const std::vector<MemberReplacement> second{{"war3map.j", Bytes("second-script")}};
    const auto again = reopened.value->SaveInPlace(second);
    RequireSuccess(again, "second in-place save");
    Require(again.value->backup != saved.value->backup, "second backup gets a distinct name");
    Require(ReadBytes(again.value->backup) == firstSavedBytes, "second backup holds first saved bytes");
    Require(DirectoryFiles(backupDirectory).size() == 2, "two backups kept");
    RequireNoSavingTemporary(directory.Path());
}

void TestSaveInPlaceRejectsExternallyChangedSource() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open external-change in-place fixture");
    {
        std::fstream file(source, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(20); file.put('x');
    }
    const auto changedBytes = ReadBytes(source);
    const std::vector<MemberReplacement> edits{{"war3map.j", Bytes("must-not-land")}};
    Require(!opened.value->SaveInPlace(edits).Succeeded(), "externally changed source blocks write-back");
    Require(ReadBytes(source) == changedBytes, "external bytes left untouched");
    Require(DirectoryFiles(directory.Path() / L"War3MapEditor Backups").empty(), "no backup created on rejection");
    RequireNoSavingTemporary(directory.Path());
}

void TestSaveInPlacePrunesOnlyOwnBackups() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    const auto backupDirectory = directory.Path() / L"custom-backups";
    std::filesystem::create_directories(backupDirectory);
    const auto touch = [&backupDirectory](const std::wstring& name) {
        std::ofstream file(backupDirectory / name, std::ios::binary);
        file << "old";
    };
    // 历史名称无法证明是本服务拥有的文件，必须全部保留。
    for (int day = 1; day <= 4; ++day) touch(L"source.2020010" + std::to_wstring(day) + L"-000000.w3x");
    touch(L"SOURCE.20200104-000000-2.W3X");
    // 不符合规则的文件：主名不同、时间戳格式不对、扩展名不同、后缀不是数字。
    const std::vector<std::wstring> foreign{L"other.20200101-000000.w3x", L"source.2020-01-01.w3x",
        L"source.20200101-000000.w3m", L"source.20200101-000000-x.w3x", L"source.20200101000000.w3x",
        L"source.backup.w3x", L"notes.txt"};
    for (const auto& name : foreign) touch(name);

    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open prune fixture");
    InPlaceSaveOptions options;
    options.backupDirectory = backupDirectory;
    options.keepBackups = 2;
    const std::vector<MemberReplacement> edits{{"war3map.j", Bytes("pruned-script")}};
    std::vector<std::filesystem::path> owned;
    for (int index = 0; index < 4; ++index) {
        const auto saved = opened.value->SaveInPlace(edits, {}, options);
        RequireSuccess(saved, "save with pruning");
        Require(std::filesystem::equivalent(saved.value->backup.parent_path(), backupDirectory), "custom backup directory honoured");
        owned.push_back(saved.value->backup);
        Require(saved.value->prunedBackups.size() == (index >= 2 ? 1u : 0u), "only oldest owned backup pruned");
        if (index >= 2) Require(saved.value->prunedBackups.front() == owned[static_cast<std::size_t>(index - 2)], "oldest owned identity selected");
        opened = MapArchiveSession::Open(source);
        RequireSuccess(opened, "reopen pruning source");
    }
    const auto remaining = DirectoryFiles(backupDirectory);
    const auto has = [&remaining](const std::wstring& name) {
        return std::any_of(remaining.begin(), remaining.end(),
            [&name](const std::filesystem::path& item) { return item.wstring() == name; });
    };
    Require(has(owned[2].filename().wstring()) && has(owned[3].filename().wstring()), "two latest owned backups kept");
    Require(!std::filesystem::exists(owned[0]) && !std::filesystem::exists(owned[1]), "older owned backups removed");
    for (int day = 1; day <= 4; ++day) Require(has(L"source.2020010" + std::to_wstring(day) + L"-000000.w3x"), "legacy named file kept");
    Require(has(L"SOURCE.20200104-000000-2.W3X"), "legacy suffixed name kept");
    for (const auto& name : foreign) Require(has(name), "foreign file must never be pruned");
    RequireNoSavingTemporary(directory.Path());
}

void TestSaveInPlaceSeparatesSameNamedSourcesAndSupportsUnlimitedRetention() {
    TempDirectory firstDirectory, secondDirectory, backups;
    const auto first = CreateSource(firstDirectory), second = CreateSource(secondDirectory);
    InPlaceSaveOptions options;
    options.backupDirectory = backups.Path();
    options.keepBackups = 0;
    const std::vector<MemberReplacement> edits{{"war3map.j", Bytes("same-name-edited")}};
    std::vector<std::filesystem::path> firstBackups, secondBackups;
    const auto save = [&options, &edits](const std::filesystem::path& source) {
        const auto opened = MapArchiveSession::Open(source);
        RequireSuccess(opened, "open same-name source");
        const auto saved = opened.value->SaveInPlace(edits, {}, options);
        RequireSuccess(saved, "save same-name source");
        return *saved.value;
    };
    for (int index = 0; index < 3; ++index) {
        const auto savedFirst = save(first), savedSecond = save(second);
        Require(savedFirst.prunedBackups.empty() && savedSecond.prunedBackups.empty(), "zero retention count means unlimited");
        firstBackups.push_back(savedFirst.backup);
        secondBackups.push_back(savedSecond.backup);
    }
    Require(DirectoryFiles(backups.Path()).size() == 6, "all six unlimited backups retained");
    options.keepBackups = 1;
    const auto saved = save(first);
    Require(saved.prunedBackups.size() == 3, "retention applies only to first source identity");
    for (const auto& path : firstBackups) Require(!std::filesystem::exists(path), "first-source old backup pruned");
    for (const auto& path : secondBackups) Require(std::filesystem::exists(path), "same-named second source preserved");
    RequireNoSavingTemporary(firstDirectory.Path());
    RequireNoSavingTemporary(secondDirectory.Path());
}

void TestSaveInPlacePreservesChangedBackupsAndInvalidOwnership() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    InPlaceSaveOptions options;
    options.backupDirectory = directory.Path() / L"owned-backups";
    options.keepBackups = 0;
    const auto save = [&] {
        const auto opened = MapArchiveSession::Open(source);
        RequireSuccess(opened, "open changed backup fixture");
        const auto saved = opened.value->SaveInPlace({}, {}, options);
        RequireSuccess(saved, "save changed backup fixture");
        return saved;
    };
    const auto changed = save().value->backup;
    {
        std::fstream file(changed, std::ios::binary | std::ios::in | std::ios::out);
        Require(file.good(), "open backup for user edit");
        file.seekp(20); file.put('x');
    }
    const auto changedBytes = ReadBytes(changed);
    const auto invalid = save().value->backup;
    const auto invalidReceipt = options.backupDirectory / L".forge-backup-owners" / (invalid.filename().wstring() + L".json");
    {
        std::ofstream file(invalidReceipt, std::ios::binary | std::ios::trunc);
        file << "{}";
    }
    const auto invalidBytes = ReadBytes(invalid);
    const auto replaced = save().value->backup;
    const auto replacedBytes = ReadBytes(replaced);
    const auto held = directory.Path() / L"user-moved-backup.w3x";
    std::filesystem::rename(replaced, held);
    std::filesystem::copy_file(held, replaced); // 相同内容，但已经不是归属记录绑定的文件对象。
    options.keepBackups = 1;
    const auto last = save();
    Require(last.value->prunedBackups.empty(), "changed or invalid-ownership backups never pruned");
    Require(ReadBytes(changed) == changedBytes && ReadBytes(invalid) == invalidBytes
        && ReadBytes(replaced) == replacedBytes, "all unconfirmed user backup objects preserved");
    Require(std::any_of(last.diagnostics.begin(), last.diagnostics.end(),
        [](const Diagnostic& item) { return item.severity == DiagnosticSeverity::Warning && item.code == "BACKUP_PRUNE_SKIPPED"; }),
        "uncertain ownership cleanup returns a warning after successful save");
    RequireNoSavingTemporary(directory.Path());
}

void TestSaveInPlaceRejectsBackupFilePathBeforeSourceChange() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    const auto bytes = ReadBytes(source);
    const auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open invalid backup directory fixture");
    InPlaceSaveOptions options;
    options.backupDirectory = source;
    Require(!opened.value->SaveInPlace({}, {}, options).Succeeded(), "a file cannot serve as backup directory");
    Require(ReadBytes(source) == bytes, "invalid backup path leaves source unchanged");
    RequireNoSavingTemporary(directory.Path());
}

void TestSaveInPlaceCrossVolumeBackup() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    const auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open cross-volume source");
    // CTest 在构建目录执行；TEMP 与构建目录异卷时覆盖真正跨卷写入。
    // 同卷环境明确跳过，不能把普通自选目录测试当成跨卷证据。
    const auto otherRoot = std::filesystem::current_path();
    const HANDLE sourceHandle = CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const HANDLE directoryHandle = CreateFileW(otherRoot.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION sourceInfo{}, directoryInfo{};
    const bool known = sourceHandle != INVALID_HANDLE_VALUE && directoryHandle != INVALID_HANDLE_VALUE
        && GetFileInformationByHandle(sourceHandle, &sourceInfo) && GetFileInformationByHandle(directoryHandle, &directoryInfo);
    if (sourceHandle != INVALID_HANDLE_VALUE) CloseHandle(sourceHandle);
    if (directoryHandle != INVALID_HANDLE_VALUE) CloseHandle(directoryHandle);
    Require(known, "cross-volume fixture root identities available");
    if (sourceInfo.dwVolumeSerialNumber == directoryInfo.dwVolumeSerialNumber) {
        std::cout << "SKIP cross-volume backup: TEMP and current build directory share one volume\n";
        return;
    }
    TempDirectory backups(otherRoot);
    InPlaceSaveOptions options;
    options.backupDirectory = backups.Path();
    const auto original = ReadBytes(source);
    const std::vector<MemberReplacement> edits{{"war3map.j", Bytes("cross-volume-edited")}};
    const auto saved = opened.value->SaveInPlace(edits, {}, options);
    RequireSuccess(saved, "save to a genuinely different backup volume");
    Require(ReadBytes(saved.value->backup) == original, "cross-volume backup matches original bytes");
    Require(std::filesystem::equivalent(saved.value->backup.parent_path(), backups.Path()), "cross-volume directory honored");
    Require(*Sha256File(source).value == saved.value->written.sha256, "cross-volume save commits verified candidate");
    RequireNoSavingTemporary(directory.Path());
    std::cout << "PASS cross-volume backup: sourceVolume=" << sourceInfo.dwVolumeSerialNumber
        << " backupVolume=" << directoryInfo.dwVolumeSerialNumber << " source=" << source
        << " backupRoot=" << backups.Path() << '\n';
}

void TestSaveInPlaceWithLongBackupOwnershipPaths() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open short source with long backup path");
    Require(opened.value->SourcePath().wstring().size() < MAX_PATH, "long-path fixture keeps original source short");
    auto backups = opened.value->SourcePath().parent_path() / L"long-custom-backups";
    while (backups.wstring().size() < 220) backups /= L"backup-component-0123456789";
    const auto api = [](const std::filesystem::path& path) {
        auto normalized = path.lexically_normal(); normalized.make_preferred();
        const auto name = normalized.wstring();
        return std::filesystem::path(name.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + name.substr(2) : L"\\\\?\\" + name);
    };
    InPlaceSaveOptions options;
    options.backupDirectory = backups;
    options.keepBackups = 1;
    std::vector<std::filesystem::path> savedBackups;
    for (int index = 0; index < 3; ++index) {
        const auto original = ReadBytes(source);
        const std::vector<MemberReplacement> edits{{"war3map.j", Bytes("long-path-script-" + std::to_string(index))}};
        const auto saved = opened.value->SaveInPlace(edits, {}, options);
        RequireSuccess(saved, "save with backup and ownership receipt beyond MAX_PATH");
        const auto receipt = saved.value->backup.parent_path() / L".forge-backup-owners" / (saved.value->backup.filename().wstring() + L".json");
        Require(saved.value->backup.wstring().size() >= MAX_PATH && receipt.wstring().size() >= MAX_PATH,
            "actual backup and ownership receipt really exceed MAX_PATH");
        Require(!saved.value->backup.wstring().starts_with(L"\\\\?\\"), "user backup receipt preserves ordinary canonical path semantics");
        Require(ReadBytes(api(saved.value->backup)) == original, "long backup path holds exact original bytes");
        Require(!ReadBytes(api(receipt)).empty(), "long ownership receipt created and flushed");
        Require(saved.value->prunedBackups.size() == (index == 0 ? 0u : 1u), "long ownership paths support retention pruning");
        if (index != 0) Require(saved.value->prunedBackups.front() == savedBackups.back(), "long path pruning reports original canonical backup path");
        savedBackups.push_back(saved.value->backup);
        Require(*Sha256File(source).value == saved.value->written.sha256, "long-backup save commits verified candidate");
        opened = MapArchiveSession::Open(source);
        RequireSuccess(opened, "reopen long-backup saved source");
    }
    Require(!std::filesystem::exists(api(savedBackups[0])) && !std::filesystem::exists(api(savedBackups[1]))
        && std::filesystem::exists(api(savedBackups[2])), "long backup retention removes only older owned objects");
    RequireNoSavingTemporary(directory.Path());
    std::error_code cleanupError;
    std::filesystem::remove_all(api(backups), cleanupError);
    Require(!cleanupError, "clean long backup fixture through extended absolute path");
}

void TestSaveInPlaceCleansRecoveryWhileOriginalReaderIsHeld() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    const auto original = ReadBytes(source);
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open held-reader in-place fixture");
    struct Reader {
        HANDLE handle;
        ~Reader() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } reader{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Require(reader.handle != INVALID_HANDLE_VALUE, "hold original read handle with delete sharing");
    const std::vector<MemberReplacement> edits{{"war3map.j", Bytes("held-reader-new-script")}};
    const auto saved = opened.value->SaveInPlace(edits);
    RequireSuccess(saved, "save while original reader remains open");
    Require(ReadBytes(saved.value->backup) == original, "held-reader save retains byte-exact original backup");
    Require(*Sha256File(source).value == saved.value->written.sha256, "held-reader save commits verified new source");
    auto reopened = MapArchiveSession::Open(source);
    RequireSuccess(reopened, "reopen new source while old reader is held");
    Require(reopened.value->ReadMember("war3map.j").value == std::optional(Bytes("held-reader-new-script")), "new source contains requested member");
    std::vector<std::byte> heldBytes(original.size());
    LARGE_INTEGER start{};
    Require(SetFilePointerEx(reader.handle, start, nullptr, FILE_BEGIN) != 0, "seek held original reader after save");
    DWORD read = 0;
    Require(ReadFile(reader.handle, heldBytes.data(), static_cast<DWORD>(heldBytes.size()), &read, nullptr) != 0
        && read == heldBytes.size() && heldBytes == original, "existing reader still reads original bytes after save");
    for (const auto& diagnostic : saved.diagnostics) {
        if (diagnostic.code.starts_with("RECOVERY_")) std::cout << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    RequireNoSavingTemporary(directory.Path()); // reader仍存活；恢复目录不能依赖外部reader先关闭。
}

void TestSaveInPlaceKeepsSourceWhenLocked() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    const auto originalBytes = ReadBytes(source);
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open locked fixture");
    // 模拟 World Editor 打开地图：允许读取，但不允许写入、删除或改名。
    const HANDLE lock = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "lock source");
    const std::vector<MemberReplacement> edits{{"war3map.j", Bytes("locked-script")}};
    const auto saved = opened.value->SaveInPlace(edits);
    CloseHandle(lock);
    Require(!saved.Succeeded(), "locked source cannot be replaced");
    Require(std::any_of(saved.diagnostics.begin(), saved.diagnostics.end(),
        [](const Diagnostic& item) { return item.code == "REPLACE_FAILED"; }), "failure reported as replace failure");
    Require(ReadBytes(source) == originalBytes, "locked source bytes unchanged");
    Require(DirectoryFiles(directory.Path() / L"War3MapEditor Backups").empty(), "no backup left behind");
    Require(opened.value->VerifySourceUnchanged().Succeeded(), "session baseline still valid after failure");
    RequireNoSavingTemporary(directory.Path());
    // 解除占用后同一会话可以正常写回。
    RequireSuccess(opened.value->SaveInPlace(edits), "save after lock released");
}

// 可压缩的多扇区成员（原创合成文本）。
std::vector<std::byte> LargeMember() {
    std::string text;
    for (int line = 0; text.size() < 3 * Mpq::kMpqWriterSectorSize + 123; ++line) {
        text += "function Synthetic_" + std::to_string(line) + " takes nothing returns nothing\r\nendfunction\r\n";
    }
    return Bytes(text);
}

std::map<std::string, std::vector<std::byte>> MemberMap(const MapArchiveSession& session) {
    std::map<std::string, std::vector<std::byte>> result;
    for (const auto& member : session.Members()) result.emplace(member.name, member.data);
    return result;
}

// 未改动成员原样复用源块：候选中的原始块与源完全相同；改动成员重新压缩且内容正确。
void TestUnchangedMembersReuseSourceBlocks() {
    TempDirectory directory;
    auto members = Members();
    members.push_back({"war3map.w3u", LargeMember()});
    const auto source = directory.Path() / "large.w3x";
    RequireSuccess(WriteVerifiedArchive(source, members, Prefix()), "write large fixture");
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open large fixture");
    const std::vector<MemberReplacement> edits{{"war3map.j", LargeMember()}};
    const auto candidatePath = directory.Path() / "reused.w3x";
    RequireSuccess(opened.value->SaveCandidate(candidatePath, edits), "save reusing blocks");
    auto sourceArchive = Mpq::MpqArchive::OpenReadOnly(source);
    auto candidate = Mpq::MpqArchive::OpenReadOnly(candidatePath);
    RequireSuccess(sourceArchive, "open source archive");
    RequireSuccess(candidate, "open candidate archive");
    for (const auto& member : members) {
        if (member.name == "war3map.j") continue;
        const auto original = sourceArchive.value->ReadRawBlock(member.name);
        const auto copied = candidate.value->ReadRawBlock(member.name);
        RequireSuccess(original, "source raw block " + member.name);
        RequireSuccess(copied, "candidate raw block " + member.name);
        Require(original.value->bytes == copied.value->bytes && original.value->flags == copied.value->flags,
            "unchanged member keeps its original compressed block: " + member.name);
        Require(candidate.value->ReadFile(member.name).value == std::optional(member.data), "reused block decodes: " + member.name);
    }
    const auto large = sourceArchive.value->ReadRawBlock("war3map.w3u");
    Require(large.value->bytes.size() < LargeMember().size() && (large.value->flags & Mpq::kFileCompress) != 0,
        "fixture member is really compressed");
    Require(candidate.value->ReadFile("war3map.j").value == std::optional(LargeMember()), "edited member recompressed");
}

// 源归档扇区大小与默认不同（YDWE 保存的地图为 64 KB）：候选沿用源归档的扇区大小，未改动成员的原始块照样复用；
// 写出器拒绝把分扇区块写进扇区大小不同的归档。
void TestSourceSectorSizeIsKept() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    {
        // 前置头 1024 字节之后是 MPQ 头；0x0E 处的扇区位移由 3（4096）改为 4（8192）。
        // 夹具成员都小于一个扇区，改后仍是合法归档。
        std::fstream file(source, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(1024 + 0x0E);
        file.put(static_cast<char>(4));
    }
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open 8192-sector fixture");
    auto sourceArchive = Mpq::MpqArchive::OpenReadOnly(source);
    RequireSuccess(sourceArchive, "open 8192-sector archive");
    Require(sourceArchive.value->SectorSize() == 8192, "fixture sector size");
    const auto raw = sourceArchive.value->ReadRawBlock("war3map.j");
    RequireSuccess(raw, "raw block from 8192-sector archive");
    Require(raw.value->sectorSize == 8192, "raw block records its sector size");

    const auto candidatePath = directory.Path() / "resectored.w3x";
    RequireSuccess(opened.value->SaveCandidate(candidatePath, {}), "save keeping the source sector size");
    auto reopened = MapArchiveSession::Open(candidatePath);
    RequireSuccess(reopened, "reopen candidate");
    Require(MemberMap(*reopened.value) == MemberMap(*opened.value), "candidate keeps every member");
    const auto rewritten = Mpq::MpqArchive::OpenReadOnly(candidatePath);
    RequireSuccess(rewritten, "open candidate archive");
    Require(rewritten.value->SectorSize() == 8192, "candidate keeps the source sector size");
    const auto copied = rewritten.value->ReadRawBlock("war3map.j");
    RequireSuccess(copied, "candidate raw block");
    Require(copied.value->bytes == raw.value->bytes && copied.value->flags == raw.value->flags,
        "unchanged member reuses its 8192-sector block");

    // 默认扇区大小（4096）的归档不接受按 8192 切分的块。
    if ((raw.value->flags & Mpq::kFileSingleUnit) == 0 && raw.value->fileSize != 0) {
        const std::vector<Mpq::MpqBuildFile> files{{"war3map.j", {}, true, *raw.value}};
        const auto rejected = Mpq::WriteMpqArchive(directory.Path() / "mismatch.w3x", files);
        Require(!rejected.Succeeded() && rejected.diagnostics.front().code == "RAW_BLOCK_NOT_PORTABLE",
            "writer rejects a block split with another sector size");
    }
}

// 保存后由内存构造的会话与从磁盘重新打开的会话一致；新文件被改动时拒绝。
void TestAfterSaveMatchesReopenedSession() {
    TempDirectory directory;
    const auto source = CreateSource(directory);
    auto opened = MapArchiveSession::Open(source);
    RequireSuccess(opened, "open after-save fixture");
    const std::vector<MemberReplacement> edits{{"war3map.j", Bytes("edited-script")}, {"war3map.w3t", Bytes("new-object")}};
    const std::vector<std::string> removals{"imports/empty.bin"};
    const auto candidatePath = directory.Path() / "after.w3x";
    const auto saved = opened.value->SaveCandidate(candidatePath, edits, removals);
    RequireSuccess(saved, "save for after-save");
    Require(saved.value->fileSize == std::filesystem::file_size(candidatePath), "receipt records file size");
    auto rebased = opened.value->AfterSave(*saved.value, edits, removals);
    auto reopened = MapArchiveSession::Open(candidatePath);
    RequireSuccess(rebased, "rebase in memory");
    RequireSuccess(reopened, "reopen from disk");
    Require(MemberMap(*rebased.value) == MemberMap(*reopened.value), "same members and bytes");
    Require(rebased.value->ReadMember("(listfile)").value == reopened.value->ReadMember("(listfile)").value, "same listfile");
    Require(std::ranges::equal(rebased.value->Prefix(), reopened.value->Prefix()), "same prefix");
    Require(rebased.value->SourceSha256() == reopened.value->SourceSha256(), "same hash");
    Require(rebased.value->SourcePath() == reopened.value->SourcePath(), "same path");
    Require(rebased.value->VerifySourceUnchanged().Succeeded(), "rebased session verifies against the file");
    Require(rebased.value->SaveCandidate(directory.Path() / "chained.w3x", {}).Succeeded(), "rebased session saves again");

    auto forgedReceipt = *saved.value;
    forgedReceipt.sha256[0] = forgedReceipt.sha256[0] == '0' ? '1' : '0';
    const auto forged = opened.value->AfterSave(forgedReceipt, edits, removals);
    Require(!forged.Succeeded() && !forged.value && forged.diagnostics.front().code == "SAVED_FILE_CHANGED",
        "receipt with incorrect SHA cannot rebase");

    // 前置数据可改而不影响归档成员和长度；此时旧收据仍必须拒绝重设基线。
    {
        std::fstream file(candidatePath, std::ios::binary | std::ios::in | std::ios::out);
        Require(file.good(), "open saved candidate for same-size tamper");
        file.seekp(128);
        file.put(static_cast<char>(0xC5));
        Require(file.good(), "write same-size prefix tamper");
    }
    Require(std::filesystem::file_size(candidatePath) == saved.value->fileSize, "tampered file keeps receipt size");
    auto structurallyValid = MapArchiveSession::Open(candidatePath);
    RequireSuccess(structurallyValid, "same-size tamper keeps archive readable");
    Require(MemberMap(*structurallyValid.value) == MemberMap(*reopened.value), "same-size tamper keeps member inventory");
    const auto oldBaseline = opened.value->SourceSha256();
    const auto tampered = opened.value->AfterSave(*saved.value, edits, removals);
    Require(!tampered.Succeeded() && !tampered.value && tampered.diagnostics.front().code == "SAVED_FILE_CHANGED",
        "same-size changed result cannot rebase");
    Require(opened.value->SourceSha256() == oldBaseline && opened.value->VerifySourceUnchanged().Succeeded(),
        "failed rebase leaves original session baseline intact");

    // 原地保存后同样可以直接切换。
    auto inPlace = opened.value->SaveInPlace(edits, removals, {directory.Path() / "backups", 0});
    RequireSuccess(inPlace, "save in place for after-save");
    auto rebasedInPlace = opened.value->AfterSave(inPlace.value->written, edits, removals);
    RequireSuccess(rebasedInPlace, "rebase after in-place save");
    Require(rebasedInPlace.value->VerifySourceUnchanged().Succeeded(), "in-place rebase verifies");

    { std::ofstream file(candidatePath, std::ios::binary | std::ios::app); file.put('x'); }
    const auto changed = opened.value->AfterSave(*saved.value, edits, removals);
    Require(!changed.Succeeded() && changed.diagnostics.front().code == "SAVED_FILE_CHANGED", "changed result is refused");
}

} // namespace

int main() {
    try {
        TestCompleteRoundTripAndIndependentBaseline();
        TestRejectedChangesLeaveSessionAndFilesUntouched();
        TestExternalSourceChangeAndReplacement();
        TestIncompleteListAndTrailingData();
        TestOpaqueMemberPreservedByAppendOnlyPatch();
        TestExactMemberCaptureDistinguishesOpaqueMissingAndEmpty();
        TestReservedPlatformByteAndMetadataMembers();
        TestProjectWriterAndInvalidPrefix();
        TestSaveInPlaceReplacesSourceAndKeepsBackup();
        TestSaveInPlaceRejectsExternallyChangedSource();
        TestSaveInPlacePrunesOnlyOwnBackups();
        TestSaveInPlaceSeparatesSameNamedSourcesAndSupportsUnlimitedRetention();
        TestSaveInPlacePreservesChangedBackupsAndInvalidOwnership();
        TestSaveInPlaceRejectsBackupFilePathBeforeSourceChange();
        TestSaveInPlaceCrossVolumeBackup();
        TestSaveInPlaceWithLongBackupOwnershipPaths();
        TestSaveInPlaceCleansRecoveryWhileOriginalReaderIsHeld();
        TestSaveInPlaceKeepsSourceWhenLocked();
        TestUnchangedMembersReuseSourceBlocks();
        TestSourceSectorSizeIsKept();
        TestAfterSaveMatchesReopenedSession();
        std::cout << "MapArchiveSession: all scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
