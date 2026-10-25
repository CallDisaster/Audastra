#include "aj/WindowsLoader.h"
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace audastra::japi {
namespace {
constexpr size_t MaxDllBytes = 64u * 1024u * 1024u;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Library {
    HMODULE handle = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path path, directory;
    ~Library() {
        if (handle) FreeLibrary(handle);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        // Only the two exact paths created by this instance. Never recursive cleanup.
        if (!path.empty()) DeleteFileW(path.c_str());
        if (!directory.empty()) RemoveDirectoryW(directory.c_str());
    }
};
std::string Hex(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (uint8_t b : bytes) { text += digits[b >> 4]; text += digits[b & 15]; }
    return text;
}
bool Read(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    Handle input;
    input.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(input.value, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        || !GetFileSizeEx(input.value, &size) || size.QuadPart <= 0 || size.QuadPart > MaxDllBytes) return false;
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    return ReadFile(input.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
}
bool PrivateDirectory(const std::filesystem::path& parent, std::filesystem::path& directory, std::string& diagnostic) {
    auto fail = [&](const char* stage) { diagnostic = std::string(stage) + ": " + std::to_string(GetLastError()); return false; };
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return fail("OpenProcessToken");
    DWORD count = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &count);
    if (!count) return fail("TokenUser size");
    std::vector<uint8_t> buffer(count);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), count, &count)) return fail("TokenUser");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) return fail("SID conversion");
    const std::wstring sddl = std::wstring(L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;") + sid + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return fail("private ACL conversion");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    bool created = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint8_t random[16];
        if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) { diagnostic = "random generation failed"; break; }
        const auto candidate = parent / ("audastra-japi-" + Hex(random));
        if (CreateDirectoryW(candidate.c_str(), &security)) { directory = candidate; created = true; break; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) { fail("CreateDirectory"); break; }
    }
    LocalFree(descriptor);
    return created;
}
bool WriteLocked(Library& library, const std::vector<uint8_t>& bytes) {
    {
        Handle output;
        output.value = CreateFileW(library.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output.value == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        if (!WriteFile(output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
            || written != bytes.size() || !FlushFileBuffers(output.value)) return false;
    }
    library.file = CreateFileW(library.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    return library.file != INVALID_HANDLE_VALUE;
}
LoadResult Failure(int32_t status, const char* text) { LoadResult r; r.status = status; r.diagnostic = text; return r; }
}
std::string Sha256(std::span<const uint8_t> bytes) {
    if (bytes.size() > UINT32_MAX) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD size = 0, copied = 0;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &copied, 0) >= 0;
    std::vector<uint8_t> object(size);
    uint8_t digest[32]{};
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0;
    if (ok) ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0;
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? Hex(digest) : std::string{};
}
bool InspectPe32(std::span<const uint8_t> b) {
    auto has = [&](size_t p, size_t n) { return p <= b.size() && n <= b.size() - p; };
    auto u16 = [&](size_t p) -> uint16_t { uint16_t n = 0; if (has(p, 2)) std::memcpy(&n, b.data() + p, 2); return n; };
    auto u32 = [&](size_t p) -> uint32_t { uint32_t n = 0; if (has(p, 4)) std::memcpy(&n, b.data() + p, 4); return n; };
    if (!has(0, 64) || u16(0) != 0x5a4d) return false;
    const size_t pe = u32(0x3c);
    if (!has(pe, 24) || u32(pe) != 0x00004550 || u16(pe + 4) != 0x014c
        || !(u16(pe + 22) & 0x2000)) return false;
    const size_t opt = pe + 24, optSize = u16(pe + 20), sections = u16(pe + 6);
    if (optSize < 224 || !has(opt, optSize) || u16(opt) != 0x10b || !sections || sections > 96
        || u32(opt + 92) < 1 || !has(opt + optSize, sections * 40)) return false;
    auto rva = [&](uint32_t address, size_t size) -> size_t {
        if (address < u32(opt + 60) && size <= u32(opt + 60) - address && has(address, size)) return address;
        for (size_t i = 0; i < sections; ++i) {
            const size_t section = opt + optSize + i * 40;
            const uint32_t start = u32(section + 12), raw = u32(section + 16);
            if (address < start) continue;
            const size_t delta = address - start;
            const uint64_t wideFile = static_cast<uint64_t>(u32(section + 20)) + delta;
            if (wideFile > b.size()) continue;
            const size_t file = static_cast<size_t>(wideFile);
            if (delta <= raw && size <= raw - delta && has(file, size)) return file;
        }
        return b.size();
    };
    const uint32_t exportRva = u32(opt + 96), exportSize = u32(opt + 100);
    const size_t exports = rva(exportRva, 40);
    if (!exportRva || exportSize < 40 || exports == b.size()) return false;
    const uint32_t functions = u32(exports + 20), names = u32(exports + 24);
    if (!functions || !names || functions > 65536 || names > 65536) return false;
    const size_t functionTable = rva(u32(exports + 28), static_cast<size_t>(functions) * 4);
    const size_t nameTable = rva(u32(exports + 32), static_cast<size_t>(names) * 4);
    const size_t ordinalTable = rva(u32(exports + 36), static_cast<size_t>(names) * 2);
    if (functionTable == b.size() || nameTable == b.size() || ordinalTable == b.size()) return false;
    for (uint32_t i = 0; i < names; ++i) {
        constexpr std::string_view wanted = AJ_INIT_EXPORT;
        const size_t text = rva(u32(nameTable + i * 4), wanted.size() + 1);
        if (text == b.size() || std::memcmp(b.data() + text, wanted.data(), wanted.size()) || b[text + wanted.size()]) continue;
        const auto ordinal = u16(ordinalTable + i * 2);
        if (ordinal >= functions) return false;
        const auto function = u32(functionTable + ordinal * 4);
        if (!function || (function >= exportRva && static_cast<uint64_t>(function) < static_cast<uint64_t>(exportRva) + exportSize)
            || rva(function, 1) == b.size()) return false; // reject forwarded exports
        for (size_t j = 0; j < sections; ++j) {
            const size_t s = opt + optSize + j * 40;
            const auto start = u32(s + 12), size = std::max(u32(s + 8), u32(s + 16));
            if (function >= start && function - start < size && (u32(s + 36) & 0x20000000)) return true;
        }
        return false;
    }
    return false;
}
PinnedDllLoader::PinnedDllLoader(std::filesystem::path directory, std::vector<ModulePin> pins,
    std::filesystem::path staging) : pins_(std::move(pins)) {
    if (!directory.is_absolute() || (!staging.empty() && !staging.is_absolute()))
        throw std::invalid_argument("JAPI loader roots must be absolute trusted paths");
    directory_ = std::filesystem::canonical(directory);
    staging_ = std::filesystem::canonical(staging.empty() ? std::filesystem::temp_directory_path() : staging);
    std::vector<std::string> names;
    for (const auto& pin : pins_) {
        if (!ValidName(pin.contract.name) || pin.contract.name == "host" || pin.contract.name == "module"
            || pin.sha256.size() != 64 || std::find(names.begin(), names.end(), pin.contract.name) != names.end())
            throw std::invalid_argument("invalid/duplicate trusted JAPI pin");
        for (char c : pin.sha256) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            throw std::invalid_argument("SHA-256 pin must use 64 lowercase hex digits");
        names.push_back(pin.contract.name);
    }
}
LoadResult PinnedDllLoader::Open(std::string_view name) {
    if (!ValidName(name)) return Failure(AJ_ARGUMENT, "invalid module name");
    const auto pin = std::find_if(pins_.begin(), pins_.end(), [&](const auto& p) { return p.contract.name == name; });
    if (pin == pins_.end()) return Failure(AJ_POLICY, "no trusted installation pin");
    if (sizeof(void*) != 4) return Failure(AJ_UNSUPPORTED, "game modules require a Windows x86 host");
    std::vector<uint8_t> bytes;
    if (!Read(directory_ / (std::string(name) + ".dll"), bytes)) return Failure(AJ_POLICY, "module file unreadable/unsafe/oversized");
    if (Sha256(bytes) != pin->sha256) return Failure(AJ_POLICY, "module SHA-256 differs from trusted pin");
    if (!InspectPe32(bytes)) return Failure(AJ_VERSION, "module must be PE32/i386 DLL with a direct executable V2 export");
    auto library = std::make_shared<Library>();
    std::string stagingError;
    if (!PrivateDirectory(staging_, library->directory, stagingError)) {
        auto failure = Failure(AJ_POLICY, "cannot create private staging directory: ");
        failure.diagnostic += stagingError;
        return failure;
    }
    library->path = library->directory / L"module.dll";
    if (!WriteLocked(*library, bytes)) return Failure(AJ_POLICY, "cannot stage and lock module bytes");
    // No PATH/current-directory/DLL-directory search, and no process-global search-path mutation.
    library->handle = LoadLibraryExW(library->path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library->handle) {
        auto failure = Failure(AJ_BACKEND, "system-only LoadLibraryEx failed: ");
        failure.diagnostic += std::to_string(GetLastError());
        return failure;
    }
    const auto init = reinterpret_cast<AjInit>(GetProcAddress(library->handle, AJ_INIT_EXPORT));
    if (!init) return Failure(AJ_VERSION, "V2 export unavailable");
    LoadResult result;
    result.status = AJ_OK;
    result.module = {init, library, pin->contract};
    return result;
}
}
