#include "War3MapCore/VerifiedFile.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/ProjectSaveControl.h"

#include <Windows.h>

#include <algorithm>
#include <exception>
#include <limits>

namespace War3Map {
namespace {

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { if (Valid()) CloseHandle(handle_); }
    [[nodiscard]] bool Valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    [[nodiscard]] HANDLE Get() const { return handle_; }
private:
    HANDLE handle_;
};

using ReadResult = Result<std::vector<std::byte>>;

ReadResult Failure(const std::filesystem::path& path, std::string code, std::string message) {
    auto result = ReadResult::Failure("VerifiedFile", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{path};
    return result;
}

bool HexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::uint64_t Size(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

bool SameIdentityAndWrite(const BY_HANDLE_FILE_INFORMATION& before, const BY_HANDLE_FILE_INFORMATION& after) {
    return before.dwVolumeSerialNumber == after.dwVolumeSerialNumber
        && before.nFileIndexHigh == after.nFileIndexHigh && before.nFileIndexLow == after.nFileIndexLow
        && Size(before) == Size(after)
        && CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0;
}

} // namespace

Result<std::vector<std::byte>> ReadVerifiedFile(const std::filesystem::path& path,
    std::string_view expectedSha256, std::uint64_t maxBytes, const ProjectSaveControl* control) {
    try {
        if (control && !control->Checkpoint(ProjectSaveStage::Read)) return {{}, ProjectSaveCancelledStatus().diagnostics};
        if (expectedSha256.size() != 64 || !std::all_of(expectedSha256.begin(), expectedSha256.end(), HexDigit)) {
            return Failure(path, "VERIFIED_FILE_EXPECTED_SHA_INVALID", "读取快照必须提供完整的 SHA-256 摘要。");
        }
        if (path.empty()) return Failure(path, "VERIFIED_FILE_PATH_INVALID", "读取快照的文件路径不能为空。");
        // share 仅允许读取：既有写入/删除句柄会使打开失败，新写入也被阻止，直到快照完成。
        FileHandle input(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!input.Valid()) return Failure(path, "VERIFIED_FILE_OPEN_FAILED",
            "无法只读锁定文件；文件可能丢失、不可访问或正在被写入/删除。");
        if (GetFileType(input.Get()) != FILE_TYPE_DISK) return Failure(path,
            "VERIFIED_FILE_NOT_REGULAR", "读取快照只接受普通磁盘文件，不接受设备或管道。");
        BY_HANDLE_FILE_INFORMATION before{};
        if (!GetFileInformationByHandle(input.Get(), &before)) return Failure(path,
            "VERIFIED_FILE_STAT_FAILED", "无法确认文件身份和内容长度。");
        if (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) return Failure(path,
            "VERIFIED_FILE_NOT_REGULAR", "读取快照不能使用目录或设备。");
        const auto length = Size(before);
        std::vector<std::byte> bytes;
        if (length > maxBytes || length > std::numeric_limits<std::size_t>::max() || length > bytes.max_size()) {
            return Failure(path, "VERIFIED_FILE_BUDGET_EXCEEDED", "文件超过快照读取容量限制。");
        }
        bytes.resize(static_cast<std::size_t>(length));
        std::size_t offset = 0;
        constexpr std::size_t chunkSize = 1024 * 1024;
        while (offset < bytes.size()) {
            if (control && !control->Checkpoint(ProjectSaveStage::Read, offset))
                return {{}, ProjectSaveCancelledStatus().diagnostics};
            const auto request = static_cast<DWORD>((std::min)(chunkSize, bytes.size() - offset));
            DWORD read = 0;
            if (!ReadFile(input.Get(), bytes.data() + offset, request, &read, nullptr)) {
                return Failure(path, "VERIFIED_FILE_READ_FAILED", "文件快照读取失败，未返回部分内容。");
            }
            if (read == 0 || read > request) return Failure(path,
                "VERIFIED_FILE_SHORT_READ", "文件未能完整读取，未返回部分内容。");
            offset += read;
        }
        if (control && !control->Checkpoint(ProjectSaveStage::Read, offset))
            return {{}, ProjectSaveCancelledStatus().diagnostics};
        std::byte extra{};
        DWORD extraRead = 0;
        if (!ReadFile(input.Get(), &extra, 1, &extraRead, nullptr)) return Failure(path,
            "VERIFIED_FILE_READ_FAILED", "无法确认文件末尾，未返回快照。");
        BY_HANDLE_FILE_INFORMATION after{};
        if (extraRead != 0 || !GetFileInformationByHandle(input.Get(), &after) || !SameIdentityAndWrite(before, after)) {
            return Failure(path, "VERIFIED_FILE_CHANGED", "文件在读取期间发生变化，未返回不确定的快照。");
        }
        const auto hash = Sha256(bytes, control);
        if (!hash.Succeeded()) {
            ReadResult result;
            result.diagnostics = hash.diagnostics;
            for (auto& diagnostic : result.diagnostics) diagnostic.origin = OriginSpan{path};
            return result;
        }
        std::string expected(expectedSha256);
        std::transform(expected.begin(), expected.end(), expected.begin(), [](char c) {
            return c >= 'A' && c <= 'F' ? static_cast<char>(c + ('a' - 'A')) : c;
        });
        if (*hash.value != expected) return Failure(path, "VERIFIED_FILE_SHA_MISMATCH",
            "文件内容与保存开始时确认的快照不一致，请重新检查后保存。");
        return ReadResult::Success(std::move(bytes));
    } catch (const std::exception&) {
        return Failure(path, "VERIFIED_FILE_READ_FAILED", "无法完成文件快照，未返回部分内容。");
    }
}

} // namespace War3Map
