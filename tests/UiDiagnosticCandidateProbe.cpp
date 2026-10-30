#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

// 手动诊断工具；不是产品保存入口，不安装、不读取不透明成员正文。
namespace {
using namespace War3Map;
using namespace War3Map::Mpq;
using Json = nlohmann::json;
void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void Check(Status value) {
    for (const auto& d : value.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    Check(value.Succeeded(), "core operation rejected");
}
template<class T> T Take(Result<T> value) {
    for (const auto& d : value.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    Check(value.Succeeded() && value.value.has_value(), "core result rejected");
    return std::move(*value.value);
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    bool discard = false;
    ~Handle() {
        if (value == INVALID_HANDLE_VALUE) return;
        if (discard) { FILE_DISPOSITION_INFO info{TRUE}; SetFileInformationByHandle(value, FileDispositionInfo, &info, sizeof(info)); }
        CloseHandle(value);
    }
};
std::string Key(std::string name) {
    for (auto& c : name) { if (c == '/') c = '\\'; else if (c >= 'a' && c <= 'z') c -= 'a' - 'A'; }
    return name;
}
std::vector<std::byte> Bytes(std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return {bytes.begin(), bytes.end()};
}
std::vector<std::byte> ReadAt(HANDLE handle, std::uint64_t at, std::size_t count) {
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(at);
    Check(SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != FALSE, "seek failed");
    std::vector<std::byte> bytes(count); DWORD read = 0;
    Check(count <= MAXDWORD && ReadFile(handle, bytes.data(), static_cast<DWORD>(count), &read, nullptr)
        && read == count, "raw comparison read failed");
    return bytes;
}
void VerifyAppend(const MpqArchive& before, const MpqArchive& after, HANDLE source, HANDLE target,
    const std::vector<MpqStrictFileInfo>& inventory, const std::vector<MpqBuildFile>& edits) {
    Check(before.ArchivePrefixSize() == after.ArchivePrefixSize(), "archive prefix moved");
    const auto bh = before.HashTableEntries(), ah = after.HashTableEntries();
    Check(bh.size() == ah.size(), "hash table size changed");
    for (std::size_t i = 0; i < bh.size(); ++i) Check(bh[i].name1 == ah[i].name1 && bh[i].name2 == ah[i].name2
        && bh[i].locale == ah[i].locale && bh[i].platform == ah[i].platform && bh[i].blockIndex == ah[i].blockIndex,
        "hash table identity changed");
    std::set<std::uint32_t> changed;
    for (const auto& edit : edits) {
        auto member = std::find_if(inventory.begin(), inventory.end(), [&](const auto& row) { return Key(row.name) == Key(edit.name); });
        Check(member != inventory.end() && changed.insert(member->blockIndex).second, "edit block missing or duplicate");
        Check(Take(after.ReadFileBounded(edit.name, 16 * 1024 * 1024)) == edit.data, "edited content mismatch");
    }
    const auto bb = before.BlockTableEntries(), ab = after.BlockTableEntries();
    Check(bb.size() == ab.size(), "block table size changed");
    for (std::size_t i = 0; i < bb.size(); ++i) if (!changed.contains(static_cast<std::uint32_t>(i)))
        Check(bb[i].filePos == ab[i].filePos && bb[i].fileSize == ab[i].fileSize
            && bb[i].compressedSize == ab[i].compressedSize && bb[i].flags == ab[i].flags, "unchanged block metadata mismatch");
    const auto originalSize = before.ArchivePrefixSize() + before.DeclaredArchiveSize();
    Check(after.ArchivePrefixSize() + after.DeclaredArchiveSize() >= originalSize, "candidate truncated");
    for (std::uint64_t offset = 0; offset < originalSize;) {
        const auto count = static_cast<std::size_t>((std::min)(std::uint64_t{1 << 20}, originalSize - offset));
        const auto a = ReadAt(source, offset, count), b = ReadAt(target, offset, count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto at = offset + i, base = before.ArchivePrefixSize();
            if ((at >= base + 8 && at < base + 12) || (at >= base + 0x14 && at < base + 0x18)) continue;
            Check(a[i] == b[i], "non-header original archive bytes changed");
        }
        offset += count;
    }
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 7 || argc == 9 || argc == 10, "usage: diagnostic-candidate source.w3x expected-source-sha new-script.j expected-script-sha new-candidate.w3x marker [payload.dll expected-payload-sha [generated-trigger-directory]]");
        const std::filesystem::path sourcePath(argv[1]), scriptPath(argv[3]), targetPath(argv[5]);
        Check(sourcePath.is_absolute() && scriptPath.is_absolute() && targetPath.is_absolute()
            && targetPath.extension() == L".w3x" && !std::filesystem::exists(targetPath), "absolute source/script and absent w3x target required");
        Check(scriptPath.extension() == L".j", "only an explicit JASS replacement input is accepted");
        const auto utf8 = [](const wchar_t* s) { auto u = std::filesystem::path(s).u8string(); return std::string(u.begin(), u.end()); };
        const auto expected = utf8(argv[2]), scriptSha = utf8(argv[4]);
        Handle source{CreateFileW(sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        Check(source.value != INVALID_HANDLE_VALUE, "source lease failed");
        BY_HANDLE_FILE_INFORMATION info{};
        Check(GetFileInformationByHandle(source.value, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            && info.nNumberOfLinks == 1, "source must be a plain singly linked file");
        Check(Take(Sha256Handle(source.value)) == expected, "source identity mismatch");
        const auto script = Take(ReadVerifiedFile(scriptPath, scriptSha, 16 * 1024 * 1024));
        MpqOpenLimits limits; limits.maxArchivePrefixBytes = 16 * 1024 * 1024;
        limits.maxDeclaredArchiveBytes = 1024ull * 1024 * 1024; limits.maxStrictMemberCount = 131072;
        auto archive = Take(MpqArchive::OpenReadOnlyHandle(source.value, sourcePath, limits));
        LARGE_INTEGER sourceSize{};
        Check(GetFileSizeEx(source.value, &sourceSize) && sourceSize.QuadPart >= 0
            && static_cast<std::uint64_t>(sourceSize.QuadPart) == archive.ArchivePrefixSize() + archive.DeclaredArchiveSize(), "trailing bytes rejected");
        // 只有这两个固定成员解压；清单和表由Core解析，其他正文不读取。
        const auto oldScript = Take(archive.ReadFileBounded("war3map.j", 16 * 1024 * 1024));
        const auto oldPackage = Take(archive.ReadFileBounded("audastra/runtime/package.json", 64 * 1024));
        auto metadata = Json::parse(std::string_view(reinterpret_cast<const char*>(oldPackage.data()), oldPackage.size()));
        Check(metadata.at("scriptSha256") == Take(Sha256(oldScript)), "installed script receipt is stale");
        metadata["scriptSha256"] = scriptSha;
        std::vector<std::byte> payload;
        std::string payloadSha;
        std::string payloadMember = "warvk_runtime.blp";
        if (argc >= 9) {
            const std::filesystem::path payloadPath(argv[7]);
            Check(payloadPath.is_absolute() && payloadPath.extension() == L".dll", "only explicit DLL payload input accepted");
            if (payloadPath.filename() == L"warvk_runtime_module.dll") payloadMember = "warvk_runtime_module.dll";
            payloadSha = utf8(argv[8]);
            payload = Take(ReadVerifiedFile(payloadPath, payloadSha, 16 * 1024 * 1024));
            std::size_t matches = 0;
            for (auto& file : metadata.at("files")) if (Key(file.at("name").get<std::string>()) == Key(payloadMember)) {
                file["sha256"] = payloadSha; file["size"] = payload.size(); ++matches;
            }
            Check(matches == 1, "source installation has ambiguous payload role");
            // 一次性诊断输出故意不能冒充可管理/分发的运行库包。
            metadata["diagnosticOnly"] = true;
            metadata["diagnosticParentPackageSha256"] = metadata.at("packageSha256");
            metadata["packageSha256"] = Take(Sha256Text(Json{{"sourceMap",expected},{"payload",payloadSha},{"script",scriptSha}}.dump()));
        }
        const auto encoded = metadata.dump();
        std::vector<MpqBuildFile> edits{{"war3map.j", script, true, {}}, {"audastra/runtime/package.json", Bytes(encoded), true, {}}};
        if (argc >= 9) edits.push_back({payloadMember, std::move(payload), true, {}});
        if (argc == 10) {
            const std::filesystem::path directory(argv[9]);
            Check(directory.is_absolute() && std::filesystem::is_directory(directory), "explicit generated trigger directory required");
            for (const auto* name : {"war3map.wtg", "war3map.wct"}) {
                const auto path = directory / name;
                const auto sha = Take(Sha256File(path));
                edits.push_back({name, Take(ReadVerifiedFile(path, sha, 16 * 1024 * 1024)), true, {}});
            }
        }
        const auto inventory = Take(archive.ListFilesPreservingOpaque());
        const bool opaque = inventory.size() < archive.FileCount();
        if (!opaque) Check(std::none_of(inventory.begin(), inventory.end(), [](const auto& member) {
            return IsMpqMetadataMemberName(member.name);
        }), "strict source contains dependent archive metadata; two-member experiment refuses to drop or stale it");
        Handle output{CreateFileW(targetPath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr), true};
        Check(output.value != INVALID_HANDLE_VALUE, "exclusive target create failed");
        // 整归档原字节保留，不按成员读取任何未改导入，即使它有公开名字也不读取原块。
        Check(archive.WriteOpaquePreservingPatchToHandle(output.value, edits, true));
        Check(FlushFileBuffers(output.value) != FALSE, "candidate flush failed");
        auto candidate = Take(MpqArchive::OpenReadOnlyHandle(output.value, targetPath, limits));
        VerifyAppend(archive, candidate, source.value, output.value, inventory, edits);
        Check(Take(Sha256Handle(source.value)) == expected, "source changed during experiment");
        const auto resultSha = Take(Sha256Handle(output.value));
        Json result{{"source_sha256", expected}, {"script_sha256", scriptSha}, {"candidate_sha256", resultSha},
            {"mode", "whole_archive_preserving_append"}, {"experiment_only", true}, {"ai_content_decoded", false},
            {"replacement_members", {"war3map.j", "audastra/runtime/package.json"}}, {"game_qualified", false}};
        // argv[6]是上层要求的明确实验标记，不被解释为文件或执行代码。
        result["marker"] = utf8(argv[6]);
        if (argc >= 9) { result["payload_sha256"] = payloadSha; result["managed_package_qualified"] = false;
            result["replacement_members"].push_back(payloadMember); }
        if (argc == 10) for (const auto* name : {"war3map.wtg", "war3map.wct"}) result["replacement_members"].push_back(name);
        output.discard = false;
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "EXPERIMENT_REJECTED: " << error.what() << '\n'; return 1; }
}
