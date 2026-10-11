#include "War3MapCore/IO/FileBytes.h"

#include <exception>
#include <fstream>
#include <limits>
#include <system_error>

namespace War3Map::IO {
namespace {

Result<std::vector<std::byte>> ReadFailure(const std::filesystem::path& path,
    std::string code, std::string message) {
    auto result = Result<std::vector<std::byte>>::Failure("IO.FileBytes", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{path};
    return result;
}

Status WriteFailure(const std::filesystem::path& path, std::string code, std::string message) {
    auto result = Status::Failure("IO.FileBytes", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{path};
    return result;
}

} // namespace

std::string PathUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::string GenericPathUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::filesystem::path PathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

Result<std::vector<std::byte>> ReadFileBytes(const std::filesystem::path& path, std::uint64_t maxBytes) {
    try {
        if (path.empty()) return ReadFailure(path, "FILE_PATH_EMPTY", "文件路径不能为空。");
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            return ReadFailure(path, "FILE_NOT_REGULAR", "文件不存在、不可访问或不是普通文件。");
        }
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return ReadFailure(path, "FILE_OPEN_FAILED", "无法打开文件读取。");
        const auto end = input.tellg();
        if (end < 0) return ReadFailure(path, "FILE_SIZE_FAILED", "无法读取文件长度。");
        const auto size = static_cast<std::uint64_t>(end);
        std::vector<std::byte> bytes;
        if (size > maxBytes || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())
            || size > bytes.max_size()) {
            return ReadFailure(path, "FILE_SIZE_LIMIT", "文件超过允许的读取容量。");
        }
        bytes.resize(static_cast<std::size_t>(size));
        input.seekg(0, std::ios::beg);
        if (!input) return ReadFailure(path, "FILE_SEEK_FAILED", "无法定位文件起点。");
        if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) return ReadFailure(path, "FILE_READ_FAILED", "文件读取不完整。");
        char extra = 0;
        if (input.read(&extra, 1)) return ReadFailure(path, "FILE_CHANGED", "文件在读取期间增长。");
        if (!input.eof()) return ReadFailure(path, "FILE_READ_FAILED", "无法确认文件末尾。");
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    } catch (const std::exception&) {
        return ReadFailure(path, "FILE_READ_EXCEPTION", "文件读取失败。");
    }
}

Status WriteFileBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    try {
        if (path.empty()) return WriteFailure(path, "FILE_PATH_EMPTY", "文件路径不能为空。");
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            return WriteFailure(path, "FILE_SIZE_LIMIT", "文件超过允许的写入容量。");
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return WriteFailure(path, "FILE_OPEN_FAILED", "无法打开文件写入。");
        if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) return WriteFailure(path, "FILE_WRITE_FAILED", "文件未能完整写入。");
        return Status::Ok();
    } catch (const std::exception&) {
        return WriteFailure(path, "FILE_WRITE_EXCEPTION", "文件写入失败。");
    }
}

} // namespace War3Map::IO
