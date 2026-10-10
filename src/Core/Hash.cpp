#include "War3MapCore/Hash.h"
#include "War3MapCore/ProjectSaveControl.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

namespace War3Map {
namespace {

class AlgorithmHandle {
public:
    ~AlgorithmHandle() { Reset(); }
    void Reset() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
            handle_ = nullptr;
        }
    }

    BCRYPT_ALG_HANDLE* Address() {
        return &handle_;
    }

    BCRYPT_ALG_HANDLE Get() const {
        return handle_;
    }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class HashHandle {
public:
    ~HashHandle() {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }

    BCRYPT_HASH_HANDLE* Address() {
        return &handle_;
    }

    BCRYPT_HASH_HANDLE Get() const {
        return handle_;
    }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

std::string StatusMessage(const char* operation, NTSTATUS status) {
    std::ostringstream stream;
    stream << operation << " failed with NTSTATUS 0x"
           << std::hex << std::uppercase
           << static_cast<unsigned long>(status);
    return stream.str();
}

class Sha256Provider {
public:
    Status EnsureReady() {
        if (ready_) return Status::Ok();
        // 仅成功的provider按线程保留。失败下一次可重试；每次摘要仍有独立hash对象。
        algorithm_.Reset();
        auto status = BCryptOpenAlgorithmProvider(algorithm_.Address(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status < 0) return Status::Failure(
            "Hash", "HASH_OPEN_FAILED", StatusMessage("BCryptOpenAlgorithmProvider", status));
        DWORD returned = 0;
        status = BCryptGetProperty(algorithm_.Get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength_), sizeof(objectLength_), &returned, 0);
        if (status < 0) return Status::Failure(
            "Hash", "HASH_PROPERTY_FAILED", StatusMessage("BCryptGetProperty", status));
        if (returned != sizeof(objectLength_) || objectLength_ == 0) return Status::Failure(
            "Hash", "HASH_PROPERTY_FAILED", "Invalid SHA-256 hash object length");
        status = BCryptGetProperty(algorithm_.Get(), BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestLength_), sizeof(digestLength_), &returned, 0);
        if (status < 0) return Status::Failure(
            "Hash", "HASH_PROPERTY_FAILED", StatusMessage("BCryptGetProperty", status));
        if (returned != sizeof(digestLength_) || digestLength_ != 32) return Status::Failure(
            "Hash", "HASH_PROPERTY_FAILED", "Invalid SHA-256 digest length");
        ready_ = true;
        return Status::Ok();
    }
    BCRYPT_ALG_HANDLE Get() const { return algorithm_.Get(); }
    DWORD ObjectLength() const { return objectLength_; }
    DWORD DigestLength() const { return digestLength_; }
private:
    AlgorithmHandle algorithm_;
    DWORD objectLength_ = 0, digestLength_ = 0;
    bool ready_ = false;
};

Result<std::string> HashChunks(
    const std::function<bool(std::vector<std::byte>&)>& nextChunk, const ProjectSaveControl* control = nullptr) {
    // BCryptOpenAlgorithmProvider会查找/加载provider，不应对每个实例身份重复执行。
    // 不缓存摘要或共享可变hash状态；线程退出时provider由RAII释放。
    thread_local Sha256Provider algorithm;
    const auto ready = algorithm.EnsureReady();
    if (!ready.Succeeded()) return {std::nullopt, ready.diagnostics};
    std::vector<UCHAR> hashObject(algorithm.ObjectLength());
    HashHandle hash;
    auto status = BCryptCreateHash(
        algorithm.Get(),
        hash.Address(),
        hashObject.data(),
        static_cast<ULONG>(hashObject.size()),
        nullptr,
        0,
        0);
    if (status < 0) {
        return Result<std::string>::Failure(
            "Hash", "HASH_CREATE_FAILED", StatusMessage("BCryptCreateHash", status));
    }

    std::vector<std::byte> chunk;
    std::uint64_t processed = 0;
    for (;;) {
        if (control && !control->Checkpoint(ProjectSaveStage::Hash, processed))
            return {{}, ProjectSaveCancelledStatus().diagnostics};
        if (!nextChunk(chunk)) break;
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            const auto remaining = chunk.size() - offset;
            const auto length = static_cast<ULONG>(
                (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
            status = BCryptHashData(
                hash.Get(),
                reinterpret_cast<PUCHAR>(chunk.data() + offset),
                length,
                0);
            if (status < 0) {
                return Result<std::string>::Failure(
                    "Hash", "HASH_UPDATE_FAILED", StatusMessage("BCryptHashData", status));
            }
            offset += length;
        }
        processed += chunk.size();
    }

    std::vector<UCHAR> digest(algorithm.DigestLength());
    status = BCryptFinishHash(
        hash.Get(),
        digest.data(),
        static_cast<ULONG>(digest.size()),
        0);
    if (status < 0) {
        return Result<std::string>::Failure(
            "Hash", "HASH_FINISH_FAILED", StatusMessage("BCryptFinishHash", status));
    }

    constexpr char hex[] = "0123456789abcdef";
    std::string encoded(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        encoded[i * 2] = hex[digest[i] >> 4];
        encoded[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return Result<std::string>::Success(std::move(encoded));
}

} // namespace

struct Sha256Stream::State {
    // 声明顺序保证 hash 先销毁、对象内存随后、provider 最后。
    Sha256Provider algorithm;
    std::vector<UCHAR> object;
    HashHandle hash;
    std::array<std::byte, Sha256Stream::kBufferBytes> pending{};
    std::size_t maximum = 0, accepted = 0, used = 0;
    bool ready = false, terminal = false;
    Status failure;

    Status Fail(Status status) {
        terminal = true;
        failure = std::move(status);
        return failure;
    }
    Status Ready() {
        if (ready) return Status::Ok();
        const auto status = algorithm.EnsureReady();
        if (!status.Succeeded()) return Fail(status);
        object.resize(algorithm.ObjectLength());
        const auto created = BCryptCreateHash(algorithm.Get(), hash.Address(), object.data(),
            static_cast<ULONG>(object.size()), nullptr, 0, 0);
        if (created < 0) return Fail(Status::Failure(
            "Hash", "HASH_CREATE_FAILED", StatusMessage("BCryptCreateHash", created)));
        ready = true;
        return Status::Ok();
    }
    Status Feed(std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const auto length = static_cast<ULONG>((std::min)(bytes.size(),
                static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
            const auto status = BCryptHashData(hash.Get(),
                const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data())), length, 0);
            if (status < 0) return Fail(Status::Failure(
                "Hash", "HASH_UPDATE_FAILED", StatusMessage("BCryptHashData", status)));
            bytes = bytes.subspan(length);
        }
        return Status::Ok();
    }
    Status Flush() {
        const auto status = Feed(std::span(pending.data(), used));
        if (status.Succeeded()) used = 0;
        return status;
    }
    Status Closed() const {
        return failure.Succeeded()
            ? Status::Failure("Hash", "HASH_STREAM_CLOSED", "SHA-256 流已终结，不能重用。")
            : failure;
    }
};

Sha256Stream::Sha256Stream(std::size_t maximumBytes) : state_(std::make_unique<State>()) {
    state_->maximum = maximumBytes;
}
Sha256Stream::~Sha256Stream() = default;
std::size_t Sha256Stream::BytesAccepted() const noexcept { return state_->accepted; }
std::size_t Sha256Stream::ScratchBytes() const noexcept { return sizeof(State) + state_->object.capacity(); }

Status Sha256Stream::Update(std::span<const std::byte> bytes) {
    auto& state = *state_;
    if (state.terminal) return state.Closed();
    try {
        if (bytes.size() > state.maximum - state.accepted)
            return state.Fail(Status::Failure("Hash", "HASH_STREAM_BUDGET", "SHA-256 流超过调用方的总字节预算。"));
        const auto ready = state.Ready();
        if (!ready.Succeeded()) return ready;
        const auto count = bytes.size();
        while (!bytes.empty()) {
            if (state.used == 0 && bytes.size() >= kBufferBytes) {
                const auto status = state.Feed(bytes);
                if (!status.Succeeded()) return status;
                bytes = {};
            } else {
                const auto length = (std::min)(bytes.size(), kBufferBytes - state.used);
                std::copy_n(bytes.data(), length, state.pending.data() + state.used);
                state.used += length;
                bytes = bytes.subspan(length);
                if (state.used == kBufferBytes) {
                    const auto status = state.Flush();
                    if (!status.Succeeded()) return status;
                }
            }
        }
        state.accepted += count;
        return Status::Ok();
    } catch (...) {
        // 即使诊断本身也无法分配，先无分配封闭，不能稍后终结为前缀摘要。
        state.terminal = true;
        return state.Fail(Status::Failure("Hash", "HASH_STREAM_EXCEPTION", "SHA-256 流内部准备或更新异常，已封闭。"));
    }
}

Result<std::string> Sha256Stream::Finish() {
    auto& state = *state_;
    if (state.terminal) return {std::nullopt, state.Closed().diagnostics};
    state.terminal = true;
    try {
        const auto ready = state.Ready();
        if (!ready.Succeeded()) return {std::nullopt, ready.diagnostics};
        const auto flushed = state.Flush();
        if (!flushed.Succeeded()) return {std::nullopt, flushed.diagnostics};
        std::array<UCHAR, 32> digest{};
        const auto status = BCryptFinishHash(state.hash.Get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
        if (status < 0) {
            const auto failed = state.Fail(Status::Failure(
                "Hash", "HASH_FINISH_FAILED", StatusMessage("BCryptFinishHash", status)));
            return {std::nullopt, failed.diagnostics};
        }
        constexpr char hex[] = "0123456789abcdef";
        std::string encoded(digest.size() * 2, '0');
        for (std::size_t index = 0; index < digest.size(); ++index) {
            encoded[index * 2] = hex[digest[index] >> 4];
            encoded[index * 2 + 1] = hex[digest[index] & 0x0f];
        }
        return Result<std::string>::Success(std::move(encoded));
    } catch (...) {
        state.terminal = true;
        const auto failed = state.Fail(Status::Failure("Hash", "HASH_STREAM_EXCEPTION", "SHA-256 流内部终结异常，已封闭。"));
        return {std::nullopt, failed.diagnostics};
    }
}

Result<std::string> Sha256(std::span<const std::byte> bytes, const ProjectSaveControl* control) {
    std::size_t offset = 0;
    return HashChunks([bytes, &offset, control](std::vector<std::byte>& chunk) {
        if (offset == bytes.size()) return false;
        const auto length = control ? (std::min)(bytes.size() - offset, std::size_t{1024 * 1024}) : bytes.size();
        chunk.assign(bytes.begin() + offset, bytes.begin() + offset + length);
        offset += length;
        return true;
    }, control);
}

Result<std::string> Sha256Text(std::string_view text) {
    return Sha256(std::as_bytes(std::span(text.data(), text.size())));
}

Result<std::string> Sha256File(const std::filesystem::path& path, const ProjectSaveControl* control) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<std::string>::Failure(
            "Hash",
            "FILE_OPEN_FAILED",
            "Unable to open file for SHA-256: " + path.string());
    }

    constexpr std::size_t kChunkSize = 1024 * 1024;
    auto result = HashChunks([&input](std::vector<std::byte>& chunk) {
        chunk.resize(kChunkSize);
        input.read(
            reinterpret_cast<char*>(chunk.data()),
            static_cast<std::streamsize>(chunk.size()));
        const auto read = input.gcount();
        if (read <= 0) {
            chunk.clear();
            return false;
        }
        chunk.resize(static_cast<std::size_t>(read));
        return true;
    }, control);
    if (input.bad()) {
        return Result<std::string>::Failure(
            "Hash",
            "FILE_READ_FAILED",
            "I/O error while hashing file: " + path.string());
    }
    return result;
}

Result<std::string> Sha256Handle(void* fileHandle, const ProjectSaveControl* control) {
    const HANDLE handle = static_cast<HANDLE>(fileHandle);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return Result<std::string>::Failure(
            "Hash", "HANDLE_INVALID", "Unable to hash an invalid file handle");
    }
    LARGE_INTEGER original{};
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle, zero, &original, FILE_CURRENT)) {
        return Result<std::string>::Failure(
            "Hash", "HANDLE_SEEK_FAILED", "Unable to seek file handle for SHA-256");
    }
    // 借用 HANDLE 的游标属于调用方；包括进度回调抛出在内的异常退出也必须恢复。
    struct CursorRestore {
        HANDLE handle;
        LARGE_INTEGER original;
        bool active = true;
        [[nodiscard]] bool Restore() noexcept {
            if (!active) return true;
            active = false;
            return SetFilePointerEx(handle, original, nullptr, FILE_BEGIN) != FALSE;
        }
        ~CursorRestore() { if (active) (void)Restore(); }
    } cursor{handle, original};
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) {
        return Result<std::string>::Failure(
            "Hash", "HANDLE_SEEK_FAILED", "Unable to seek file handle for SHA-256");
    }
    bool ioFailed = false;
    constexpr DWORD kHandleChunkSize = 1024 * 1024;
    auto result = HashChunks([handle, &ioFailed](std::vector<std::byte>& chunk) {
        chunk.resize(kHandleChunkSize);
        DWORD read = 0;
        if (!ReadFile(handle, chunk.data(), kHandleChunkSize, &read, nullptr)) {
            ioFailed = true;
            chunk.clear();
            return false;
        }
        chunk.resize(read);
        return read != 0;
    }, control);
    const bool restored = cursor.Restore();
    if (ioFailed) {
        return Result<std::string>::Failure(
            "Hash", "HANDLE_READ_FAILED", "I/O error while hashing file handle");
    }
    if (!restored) {
        return Result<std::string>::Failure(
            "Hash", "HANDLE_RESTORE_FAILED", "Unable to restore file pointer after SHA-256");
    }
    return result;
}

} // namespace War3Map
