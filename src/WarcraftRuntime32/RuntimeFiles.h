#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Audastra::Runtime32 {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
inline bool PlainPath(const std::filesystem::path& path) {
    if (!path.is_absolute() || path.wstring().size() >= 4096 || path.wstring().starts_with(L"\\\\")) return false;
    for (auto current = path; !current.empty();) {
        const auto attr = GetFileAttributesW(current.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return true;
}
inline HANDLE OpenLockedFile(const std::filesystem::path& path) {
    if (!PlainPath(path)) return INVALID_HANDLE_VALUE;
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1) {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}
inline std::string Sha256(HANDLE file, std::uint64_t maxBytes = 64ull * 1024 * 1024) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD size = 0, returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &returned, 0) < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); return {}; }
    std::vector<UCHAR> object(size);
    std::array<UCHAR, 32> digest{};
    bool ok = BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0;
    LARGE_INTEGER zero{};
    ok = ok && SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE;
    std::array<UCHAR, 65536> bytes{};
    std::uint64_t total = 0;
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) { ok = false; break; }
        if (read == 0) break;
        total += read;
        if (total > maxBytes || BCryptHashData(hash, bytes.data(), read, 0) < 0) { ok = false; break; }
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    for (const auto value : digest) { result += alphabet[value >> 4]; result += alphabet[value & 15]; }
    return result;
}
inline bool IsX86Image(HANDLE file) {
    IMAGE_DOS_HEADER dos{};
    DWORD count = 0;
    LARGE_INTEGER offset{};
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) || !ReadFile(file, &dos, sizeof(dos), &count, nullptr)
        || count != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 || dos.e_lfanew > 1024 * 1024) return false;
    offset.QuadPart = dos.e_lfanew;
    struct Header { DWORD signature; IMAGE_FILE_HEADER file; } header{};
    return SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) && ReadFile(file, &header, sizeof(header), &count, nullptr)
        && count == sizeof(header) && header.signature == IMAGE_NT_SIGNATURE && header.file.Machine == IMAGE_FILE_MACHINE_I386;
}
inline bool EqualPath(std::wstring left, std::wstring right) {
    const auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; };
    std::transform(left.begin(), left.end(), left.begin(), lower);
    std::transform(right.begin(), right.end(), right.begin(), lower);
    return left == right;
}
} // namespace Audastra::Runtime32
