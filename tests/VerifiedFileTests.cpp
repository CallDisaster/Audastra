#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

using namespace War3Map;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
void RequireSuccess(const Result<T>& result, const std::string& context) {
    if (result.Succeeded()) return;
    std::string message = context;
    for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
    throw std::runtime_error(message);
}

template <typename T>
void RequireCode(const Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "failure must not return partial bytes");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [code](const auto& diagnostic) {
        return diagnostic.code == code;
    }), "missing diagnostic " + std::string(code));
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path() / ("Forge-verified-file-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
            + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directories(path_), "create temporary fixture directory");
    }
    ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    ~FileHandle() { if (Valid()) CloseHandle(handle_); }
    bool Valid() const { return handle_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE handle_;
};

void Write(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(stream), "create authored fixture");
    if (!bytes.empty()) stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    Require(static_cast<bool>(stream), "write authored fixture");
}

std::string Hash(const std::vector<std::byte>& bytes) {
    const auto hash = Sha256(bytes);
    RequireSuccess(hash, "hash original fixture bytes");
    return *hash.value;
}

void TestVerifiedBytesRemainIndependent() {
    TempDirectory temp;
    const auto path = temp.Path() / L"贴图 快照.bin";
    // 跨越多个读取块，含零字节及全部字节值；不使用任何游戏资源。
    std::vector<std::byte> original(2 * 1024 * 1024 + 17);
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = static_cast<std::byte>((i * 73) & 0xFF);
    Write(path, original);
    auto expected = Hash(original);
    std::transform(expected.begin(), expected.end(), expected.begin(), [](char c) {
        return c >= 'a' && c <= 'f' ? static_cast<char>(c - ('a' - 'A')) : c;
    });
    const auto read = ReadVerifiedFile(path, expected);
    RequireSuccess(read, "read complete unicode fixture");
    Require(*read.value == original, "verified payload matches original bytes");
    const auto stillOriginal = Sha256File(path);
    RequireSuccess(stillOriginal, "source still readable");
    Require(*stillOriginal.value == Hash(original), "snapshot read never modifies source");
    Write(path, {std::byte{0x37}});
    Require(*read.value == original, "later source modifications cannot change returned snapshot");
}

void TestMismatchMissingBudgetAndEmpty() {
    TempDirectory temp;
    const auto path = temp.Path() / "payload.bin";
    const std::vector<std::byte> original{std::byte{0}, std::byte{1}, std::byte{0xFF}};
    Write(path, original);
    const auto hash = Hash(original);
    RequireCode(ReadVerifiedFile(path, std::string(64, '0')), "VERIFIED_FILE_SHA_MISMATCH");
    Write(path, {std::byte{0}, std::byte{2}, std::byte{0xFF}});
    RequireCode(ReadVerifiedFile(path, hash), "VERIFIED_FILE_SHA_MISMATCH");
    Write(path, original);
    RequireCode(ReadVerifiedFile(path, hash, original.size() - 1), "VERIFIED_FILE_BUDGET_EXCEEDED");
    RequireCode(ReadVerifiedFile(path, hash, 0), "VERIFIED_FILE_BUDGET_EXCEEDED");
    RequireSuccess(ReadVerifiedFile(path, hash, original.size()), "exact byte budget accepted");
    RequireCode(ReadVerifiedFile(temp.Path() / "missing.bin", hash), "VERIFIED_FILE_OPEN_FAILED");
    RequireCode(ReadVerifiedFile({}, hash), "VERIFIED_FILE_PATH_INVALID");
    for (const auto& invalid : {std::string{}, std::string("123"), std::string(64, 'g'), std::string(65, 'a')})
        RequireCode(ReadVerifiedFile(path, invalid), "VERIFIED_FILE_EXPECTED_SHA_INVALID");
    Require(Sha256File(path).value == std::optional(hash), "all failed reads preserve source bytes");
    const auto emptyPath = temp.Path() / "empty.bin";
    Write(emptyPath, {});
    const auto empty = ReadVerifiedFile(emptyPath, Hash({}), 0);
    RequireSuccess(empty, "zero-byte file with zero budget");
    Require(empty.value->empty(), "empty file remains empty");
}

void TestExistingWriterAndDeleteLeaseAreRejected() {
    TempDirectory temp;
    const auto path = temp.Path() / "leased.bin";
    const std::vector<std::byte> original{std::byte{0x45}, std::byte{0xAB}};
    Write(path, original);
    const auto hash = Hash(original);
    {
        FileHandle writer(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(writer.Valid(), "hold permissively shared writer");
        RequireCode(ReadVerifiedFile(path, hash), "VERIFIED_FILE_OPEN_FAILED");
    }
    {
        FileHandle deleter(CreateFileW(path.c_str(), DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(deleter.Valid(), "hold permissively shared delete handle");
        RequireCode(ReadVerifiedFile(path, hash), "VERIFIED_FILE_OPEN_FAILED");
    }
    {
        FileHandle reader(CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(reader.Valid(), "hold compatible reader");
        RequireSuccess(ReadVerifiedFile(path, hash), "concurrent readonly handles are compatible");
    }
    RequireSuccess(ReadVerifiedFile(path, hash), "locks released after rejection");
    Require(Sha256File(path).value == std::optional(hash), "lease tests leave source intact");
}

void TestRelativePathsAndNonFileTargets() {
    TempDirectory temp;
    const auto path = temp.Path() / "relative.bin";
    const std::vector<std::byte> bytes{std::byte{0x51}};
    Write(path, bytes);
    const auto previous = std::filesystem::current_path();
    struct RestoreDirectory {
        std::filesystem::path path;
        ~RestoreDirectory() { std::error_code ec; std::filesystem::current_path(path, ec); }
    } restore{previous};
    std::filesystem::current_path(temp.Path());
    const auto relative = ReadVerifiedFile("relative.bin", Hash(bytes));
    RequireSuccess(relative, "relative filename retains normal filesystem semantics");
    Require(*relative.value == bytes, "relative bytes");
    RequireCode(ReadVerifiedFile(temp.Path(), Hash({})), "VERIFIED_FILE_NOT_REGULAR");
    RequireCode(ReadVerifiedFile(L"NUL", Hash({})), "VERIFIED_FILE_NOT_REGULAR");
}

} // namespace

int main() {
    try {
        TestVerifiedBytesRemainIndependent();
        TestMismatchMissingBudgetAndEmpty();
        TestExistingWriterAndDeleteLeaseAreRejected();
        TestRelativePathsAndNonFileTargets();
        std::cout << "VerifiedFile: 4 scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
