#include "WorldTerrainAssets.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <QTemporaryDir>
#include <Windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Modules;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
WorldTerrainSources Fixture(const std::filesystem::path& path) {
    const std::vector<Mpq::MpqBuildFile> files{{"Fixture.bin", {std::byte{1}, std::byte{2}, std::byte{3}}}};
    Require(Mpq::WriteMpqArchive(path, files).Succeeded(), "write original fixture archive");
    const auto digest = Sha256File(path); Require(digest.Succeeded(), "hash fixture");
    WorldTerrainSources sources; sources.archivePath = path; sources.archiveSha = *digest.value;
    sources.entries.push_back({"Fixture.bin", "Fixture.bin", {}});
    return sources;
}
void VerifiedLeaseSkipsHashWithoutSharingTheReader() {
    QTemporaryDir directory; Require(directory.isValid(), "temporary directory");
    const auto sources = Fixture(std::filesystem::path(directory.path().toStdWString()) / L"source.w3x");
    WorldResourceReader::OpenTimings cold, warm;
    auto first = WorldResourceReader::Open(sources, nullptr, &cold);
    Require(first.Succeeded() && !cold.verifiedArchiveReused && cold.hashedBytes == std::filesystem::file_size(sources.archivePath),
        "first lease performs complete SHA");
    auto second = WorldResourceReader::Open(sources, first.value->get(), &warm);
    Require(second.Succeeded() && warm.verifiedArchiveReused && warm.hashedBytes == 0 && warm.hashNs == 0,
        "same locked file reuses only verified SHA capability");
    const auto writer = CreateFileW(sources.archivePath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto error = GetLastError();
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    Require(writer == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION, "live source lease denies in-place writes");
    first.value->reset();
    const auto read = (*second.value)->ReadAnimationResource("Fixture.bin", 1024);
    Require(read.Succeeded() && read.value->bytes == std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}}
        && (*second.value)->VerifyCurrentSource().Succeeded(), "new reader owns independent lease and MPQ");
    auto wrongHash = sources; wrongHash.archiveSha.assign(64, '0');
    const auto refused = WorldResourceReader::Open(wrongHash, second.value->get());
    Require(!refused.Succeeded() && refused.diagnostics.front().code == "WORLD_ASSET_SOURCE_CHANGED",
        "caller supplied expected SHA remains mandatory");
}
void ReplacementWithIdenticalBytesDoesNotReuseOldIdentity() {
    QTemporaryDir directory; Require(directory.isValid(), "temporary directory");
    const auto sources = Fixture(std::filesystem::path(directory.path().toStdWString()) / L"source.w3x");
    auto original = WorldResourceReader::Open(sources); Require(original.Succeeded(), "open first source");
    auto replacement = sources; replacement.archivePath = sources.archivePath.parent_path() / L"replacement.w3x";
    std::filesystem::copy_file(sources.archivePath, replacement.archivePath);
    const auto identicalHash = Sha256File(replacement.archivePath);
    Require(identicalHash.Succeeded() && *identicalHash.value == sources.archiveSha, "replacement is byte identical");
    const auto foreign = WorldResourceReader::Open(replacement, original.value->get());
    Require(!foreign.Succeeded() && foreign.diagnostics.front().code == "WORLD_ASSET_SOURCE_CHANGED",
        "same digest and bytes on another file are not the held lease");
    if (MoveFileExW(replacement.archivePath.c_str(), sources.archivePath.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
        const auto replaceError = GetLastError();
        std::cerr << "MoveFileEx(REPLACE_EXISTING) under deny-write lease failed: " << replaceError
            << "; retaining original in a sibling test path before replacing source path\n";
        // 某些Windows文件系统不允许覆盖一个仍被拒写租约占用的目标，但允许共享DELETE
        // 的原文件改名。保留原文件与其句柄，再安装相同字节的新文件，仍必须证明确实拒旧身份。
        const auto retained = sources.archivePath.parent_path() / L"retained-original.w3x";
        const auto movedOriginal = MoveFileExW(sources.archivePath.c_str(), retained.c_str(), 0);
        if (!movedOriginal) std::cerr << "MoveFileEx(retain original) failed: " << GetLastError() << '\n';
        Require(movedOriginal != FALSE, "test-only original rename under delete-sharing lease");
        const auto movedReplacement = MoveFileExW(replacement.archivePath.c_str(), sources.archivePath.c_str(), 0);
        if (!movedReplacement) std::cerr << "MoveFileEx(install replacement) failed: " << GetLastError() << '\n';
        Require(movedReplacement != FALSE, "test-only new source installation after retaining original");
    }
    const auto current = (*original.value)->VerifyCurrentSource();
    Require(!current.Succeeded() && current.diagnostics.front().code == "WORLD_ASSET_SOURCE_CHANGED",
        "current path replacement is detected");
    const auto stale = WorldResourceReader::Open(sources, original.value->get());
    Require(!stale.Succeeded() && stale.diagnostics.front().code == "WORLD_ASSET_SOURCE_CHANGED",
        "replaced path cannot inherit old verification");
    WorldResourceReader::OpenTimings freshTiming;
    const auto fresh = WorldResourceReader::Open(sources, nullptr, &freshTiming);
    Require(fresh.Succeeded() && freshTiming.hashedBytes != 0 && !freshTiming.verifiedArchiveReused,
        "explicit fresh open validates replacement independently");
}
}
int main() {
    try {
        VerifiedLeaseSkipsHashWithoutSharingTheReader();
        ReplacementWithIdenticalBytesDoesNotReuseOldIdentity();
        std::cout << "world-resource-reader: verified lease and replacement regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "world-resource-reader: " << error.what() << '\n';
        return 1;
    }
}
