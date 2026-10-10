#include "MainWindow.h"
#include "AgentSettingsStore.h"
#include "CredentialStore.h"
#include "FeedbackClient.h"
#include "McpProductTools.h"
#include "UpdateClient.h"
#include "AudastraVersion.generated.h"
#include "Serialization/StrictJson.h"
#include "Update/UpdateInstallerInternal.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <ShlObj.h>
#include <KnownFolders.h>

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

// root 的测试 OBJECT 直接编译生产 Main，仅此 OBJECT 局部重命名。
int AudastraProductMainForHealthTest(int argc, char* argv[]);

namespace {
namespace Native = War3Map::Update::Detail;
using Json = nlohmann::json;
using Path = std::filesystem::path;
using KnownFolderFunction = HRESULT (WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);

constexpr std::uint64_t kAckDeadlineMs = 45000;
constexpr std::size_t kJsonLimit = 64 * 1024;
constexpr std::array<const char*, 6> kPortableSkills{{
    "forge-editor-mcp/SKILL.md", "forge-editor-mcp/agents/openai.yaml", "forge-editor-mcp/references/current-contract.md",
    "forge-t-event-optimization/SKILL.md", "forge-t-event-optimization/agents/openai.yaml",
    "forge-t-event-optimization/references/dispatch-contract.md"}};

void Require(bool value, const char* code) {
    if (!value) throw std::runtime_error(code);
}
QString QtPath(const Path& path) { return QString::fromStdWString(path.wstring()); }
std::string Utf8(const Path& path) { return QtPath(path).toUtf8().toStdString(); }
std::string RelativeUtf8(const Path& path) {
    auto value = Utf8(path); std::replace(value.begin(), value.end(), '\\', '/'); return value;
}
std::uint64_t OwnCreationTime() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    Require(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE, "OWN_IDENTITY_FAILED");
    return (std::uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime;
}
std::wstring Environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    Require(size > 1 && size < 32768, "FIXTURE_ENVIRONMENT_MISSING");
    std::wstring value(size, L'\0');
    const auto count = GetEnvironmentVariableW(name, value.data(), size);
    Require(count > 0 && count < size, "FIXTURE_ENVIRONMENT_CHANGED");
    value.resize(count);
    return value;
}
Path ModulePath(HMODULE module) {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    Require(count > 0 && count < path.size(), "MODULE_PATH_FAILED");
    return Path(std::wstring(path.data(), count));
}
Json ReadJson(const Path& path, std::size_t maximum = kJsonLimit) {
    auto lease = Native::OpenSafeReadFile(path, maximum);
    Require(lease.Succeeded(), "FIXTURE_JSON_LEASE_FAILED");
    const auto bytes = Native::ReadLeaseBytes(*lease.value);
    Require(bytes.Succeeded(), "FIXTURE_JSON_READ_FAILED");
    War3Map::Serialization::StrictJsonLimits limits;
    limits.maxBytes = maximum; limits.maxDepth = 8; limits.maxNodes = 1024;
    limits.maxStringBytes = 32768; limits.rejectFloatingPoint = true;
    const auto parsed = War3Map::Serialization::ParseStrictJson(*bytes.value, limits);
    Require(parsed.Succeeded() && parsed.value->is_object(), "FIXTURE_JSON_INVALID");
    return *parsed.value;
}
std::string HashFile(const Path& path) {
    auto lease = Native::OpenSafeReadFile(path, 128 * 1024 * 1024);
    Require(lease.Succeeded(), "FIXTURE_HASH_LEASE_FAILED");
    const auto hash = War3Map::Sha256Handle(lease.value->file.Get());
    Require(hash.Succeeded(), "FIXTURE_HASH_FAILED");
    return *hash.value;
}
bool ExactUnsigned(const Json& object, const char* key, std::uint64_t value) {
    return object.contains(key) && object.at(key).is_number_unsigned()
        && object.at(key).get<std::uint64_t>() == value;
}

struct Mapping {
    std::wstring local, low, roaming, programData, home;
    alignas(TOKEN_USER) std::array<std::byte, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> ownUser{};
    KnownFolderFunction original = nullptr;
    std::atomic<std::uint64_t> mapped{0}, denied{0}, fonts{0};
    std::atomic<std::uint64_t> profileMapped{0}, profileDenied{0}, profileSizing{0};
};
Mapping* activeMapping = nullptr;

// callback 不调用 Qt、不查询其他目录、不抛异常；只有显式 Fonts 允许系统只读回退。
HRESULT WINAPI FixtureKnownFolder(REFKNOWNFOLDERID folder, DWORD flags, HANDLE token, PWSTR* output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    auto* mapping = activeMapping;
    if (!mapping || token != nullptr || flags != KF_FLAG_DONT_VERIFY) return E_ACCESSDENIED;
    const std::wstring* path = nullptr;
    if (IsEqualGUID(folder, FOLDERID_LocalAppData)) path = &mapping->local;
    else if (IsEqualGUID(folder, FOLDERID_LocalAppDataLow)) path = &mapping->low;
    else if (IsEqualGUID(folder, FOLDERID_RoamingAppData)) path = &mapping->roaming;
    else if (IsEqualGUID(folder, FOLDERID_ProgramData)) path = &mapping->programData;
    else if (IsEqualGUID(folder, FOLDERID_Fonts)) {
        mapping->fonts.fetch_add(1, std::memory_order_relaxed);
        return mapping->original(folder, flags, token, output);
    } else {
        mapping->denied.fetch_add(1, std::memory_order_relaxed);
        return E_ACCESSDENIED;
    }
    const auto bytes = (path->size() + 1) * sizeof(wchar_t);
    auto* copy = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    if (!copy) return E_OUTOFMEMORY;
    std::memcpy(copy, path->c_str(), bytes);
    *output = copy;
    mapping->mapped.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

// Qt homePath 优先调用此 API。只比较 token SID，不调用原 profile API 或读取作者目录。
BOOL WINAPI FixtureProfileDirectory(HANDLE token, LPWSTR output, LPDWORD size) noexcept {
    auto* mapping = activeMapping;
    if (!mapping || !token || !size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    alignas(TOKEN_USER) std::array<std::byte, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> user{};
    DWORD actual = 0;
    if (!GetTokenInformation(token, TokenUser, user.data(), static_cast<DWORD>(user.size()), &actual)
        || actual < sizeof(TOKEN_USER) || !IsValidSid(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid)
        || !EqualSid(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,
            reinterpret_cast<TOKEN_USER*>(mapping->ownUser.data())->User.Sid)) {
        mapping->profileDenied.fetch_add(1, std::memory_order_relaxed);
        SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    const auto required = static_cast<DWORD>(mapping->home.size() + 1);
    if (!output || *size < required) {
        *size = required; mapping->profileSizing.fetch_add(1, std::memory_order_relaxed);
        SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE;
    }
    std::memcpy(output, mapping->home.c_str(), std::size_t{required} * sizeof(wchar_t));
    *size = required; mapping->profileMapped.fetch_add(1, std::memory_order_relaxed);
    return TRUE;
}

bool ReadableMemory(const void* pointer, std::size_t bytes, const void* allocation) {
    auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (bytes > (std::numeric_limits<std::uintptr_t>::max)() - address) return false;
    const auto end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)
            || info.AllocationBase != allocation || info.Type != MEM_IMAGE || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto protection = info.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY
            && protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - region) return false;
        const auto next = region + info.RegionSize;
        if (next <= address) return false;
        address = (std::min)(next, end);
    }
    return true;
}

class ImageReader final {
public:
    explicit ImageReader(HMODULE module) : base_(reinterpret_cast<std::byte*>(module)) {
        Require(ReadableMemory(base_, sizeof(IMAGE_DOS_HEADER), base_), "IAT_DOS_UNREADABLE");
        IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, base_, sizeof(dos));
        Require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 1024 * 1024, "IAT_DOS_INVALID");
        Require(ReadableMemory(base_ + dos.e_lfanew, sizeof(IMAGE_NT_HEADERS64), base_), "IAT_PE_UNREADABLE");
        std::memcpy(&headers_, base_ + dos.e_lfanew, sizeof(headers_));
        Require(headers_.Signature == IMAGE_NT_SIGNATURE && headers_.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
            && headers_.FileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)
            && headers_.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
            && headers_.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT
            && headers_.OptionalHeader.SizeOfImage > sizeof(headers_)
            && headers_.OptionalHeader.SizeOfImage <= 256 * 1024 * 1024, "IAT_PE_INVALID");
        size_ = headers_.OptionalHeader.SizeOfImage;
        Require(std::uint64_t{static_cast<DWORD>(dos.e_lfanew)} + sizeof(headers_) <= size_
            && headers_.OptionalHeader.SizeOfHeaders >= std::uint64_t{static_cast<DWORD>(dos.e_lfanew)} + sizeof(headers_),
            "IAT_PE_HEADER_BOUNDS_INVALID");
    }
    template<typename T> T Read(std::uint64_t rva) const {
        Require(Contains(rva, sizeof(T)), "IAT_RVA_INVALID");
        T value{}; std::memcpy(&value, base_ + rva, sizeof(T)); return value;
    }
    bool Contains(std::uint64_t rva, std::size_t bytes) const {
        return rva <= size_ && bytes <= size_ - rva && ReadableMemory(base_ + rva, bytes, base_);
    }
    std::string Name(std::uint64_t rva) const {
        std::string value;
        for (std::size_t index = 0; index < 256; ++index) {
            const auto c = Read<char>(rva + index);
            if (!c) return value;
            Require(c > 0 && static_cast<unsigned char>(c) < 128, "IAT_IMPORT_NAME_INVALID");
            value.push_back(c);
        }
        throw std::runtime_error("IAT_IMPORT_NAME_UNTERMINATED");
    }
    void** ImportSlot(const char* dll, const char* import, std::uint64_t& slotRva) const {
        const auto directory = headers_.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        Require(directory.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR)
            && directory.Size <= 1024 * 1024 && Contains(directory.VirtualAddress, directory.Size), "IAT_DIRECTORY_INVALID");
        void** result = nullptr; std::size_t hits = 0; bool terminated = false;
        for (std::size_t index = 0; index < directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR); ++index) {
            const auto descriptor = Read<IMAGE_IMPORT_DESCRIPTOR>(std::uint64_t{directory.VirtualAddress} + index * sizeof(IMAGE_IMPORT_DESCRIPTOR));
            if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk) { terminated = true; break; }
            const auto name = Name(descriptor.Name);
            if (_stricmp(name.c_str(), dll) != 0) continue;
            Require(descriptor.OriginalFirstThunk && descriptor.FirstThunk, "IAT_ORIGINAL_THUNK_MISSING");
            bool thunkEnd = false;
            for (std::size_t thunk = 0; thunk < 16384; ++thunk) {
                const auto value = Read<IMAGE_THUNK_DATA64>(std::uint64_t{descriptor.OriginalFirstThunk} + thunk * sizeof(IMAGE_THUNK_DATA64));
                if (!value.u1.AddressOfData) { thunkEnd = true; break; }
                if (IMAGE_SNAP_BY_ORDINAL64(value.u1.Ordinal)) continue;
                Require(value.u1.AddressOfData <= (std::numeric_limits<DWORD>::max)(), "IAT_IMPORT_RVA_OVERFLOW");
                const auto function = Name(value.u1.AddressOfData + sizeof(WORD));
                if (function != import) continue;
                const auto rva = std::uint64_t{descriptor.FirstThunk} + thunk * sizeof(IMAGE_THUNK_DATA64);
                Require(Contains(rva, sizeof(void*)) && rva % alignof(void*) == 0, "IAT_SLOT_INVALID");
                result = reinterpret_cast<void**>(base_ + rva); slotRva = rva; ++hits;
            }
            Require(thunkEnd, "IAT_THUNK_UNTERMINATED");
        }
        Require(terminated && hits == 1 && result != nullptr, "IAT_SINGLE_IMPORT_REQUIRED");
        return result;
    }
private:
    std::byte* base_ = nullptr;
    std::uint64_t size_ = 0;
    IMAGE_NT_HEADERS64 headers_{};
};

class ImportOverride final {
public:
    ~ImportOverride() { if (installed_ || protectionPending_) (void)Restore(); }
    void* Install(HMODULE module, const wchar_t* library, const char* dll, const char* import, void* callback, Json& receipt) {
        Require(!installed_, "IAT_ALREADY_INSTALLED");
        ImageReader image(module);
        std::uint64_t rva = 0; slot_ = image.ImportSlot(dll, import, rva);
        const auto importedModule = GetModuleHandleW(library);
        Require(importedModule != nullptr, "IMPORTED_MODULE_MISSING");
        original_ = reinterpret_cast<void*>(GetProcAddress(importedModule, import));
        Require(original_ && *slot_ == original_, "IAT_ORIGINAL_POINTER_MISMATCH");
        callback_ = callback;
        DWORD protection = 0;
        Require(VirtualProtect(slot_, sizeof(void*), PAGE_READWRITE, &protection) != FALSE, "IAT_PROTECTION_FAILED");
        protection_ = protection;
        protectionPending_ = true;
        const auto previous = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot_),
            callback_, original_);
        installed_ = previous == original_;
        DWORD unused = 0;
        const bool protectedAgain = VirtualProtect(slot_, sizeof(void*), protection_, &unused) != FALSE;
        protectionPending_ = !protectedAgain;
        receipt = {{"dll", dll}, {"import", import}, {"single_import", true}, {"slot_rva", rva}, {"installed", installed_},
            {"protection_restored_after_install", protectedAgain}};
        Require(installed_ && protectedAgain, "IAT_INSTALL_FAILED");
        return original_;
    }
    bool Restore() noexcept {
        if (!installed_ && !protectionPending_) return true;
        bool pointerRestored = true;
        if (installed_) {
            DWORD previousProtection = 0;
            if (!VirtualProtect(slot_, sizeof(void*), PAGE_READWRITE, &previousProtection)) return false;
            protectionPending_ = true;
            const auto previous = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot_), original_, callback_);
            pointerRestored = previous == callback_;
            if (pointerRestored) installed_ = false;
        }
        DWORD unused = 0;
        const bool protectionRestored = VirtualProtect(slot_, sizeof(void*), protection_, &unused) != FALSE;
        protectionPending_ = !protectionRestored;
        return pointerRestored && protectionRestored;
    }
private:
    void** slot_ = nullptr;
    void* original_ = nullptr;
    void* callback_ = nullptr;
    DWORD protection_ = 0;
    bool installed_ = false;
    bool protectionPending_ = false;
};

class KnownFolderIsolation final {
public:
    ~KnownFolderIsolation() {
        const bool profile = profile_.Restore(), known = known_.Restore();
        if (profile && known) activeMapping = nullptr;
    }
    void Install(Mapping& mapping, Json& receipt) {
        Require(activeMapping == nullptr, "IAT_MAPPING_ALREADY_ACTIVE");
        const auto module = GetModuleHandleW(L"Qt6Core.dll");
        Require(module != nullptr, "QT_CORE_MODULE_MISSING");
        activeMapping = &mapping;
        // 初始化原函数后再调用任何会查询 Qt 路径的业务代码。
        mapping.original = reinterpret_cast<KnownFolderFunction>(known_.Install(module, L"shell32.dll", "SHELL32.dll",
            "SHGetKnownFolderPath", reinterpret_cast<void*>(&FixtureKnownFolder), receipt["known_folder"]));
        (void)profile_.Install(module, L"userenv.dll", "USERENV.dll", "GetUserProfileDirectoryW",
            reinterpret_cast<void*>(&FixtureProfileDirectory), receipt["profile_directory"]);
        receipt["override_count"] = 2;
    }
    bool Restore(Json& receipt) noexcept {
        const bool profile = profile_.Restore();
        const bool known = known_.Restore();
        if (profile && known) activeMapping = nullptr;
        try { receipt["profile_restored"] = profile; receipt["known_folder_restored"] = known; } catch (...) {}
        return profile && known;
    }
private:
    ImportOverride known_, profile_;
};

struct Context {
    Path root, receiptPath;
    Native::DirectoryReadLease rootLease;
    Native::FileReadLease markerLease;
    std::vector<Native::FileReadLease> skillLeases;
    Mapping mapping;
    KnownFolderIsolation isolation;
    Json marker, receipt;
    QSettings::Format previousFormat = QSettings::NativeFormat;
    bool formatChanged = false, bootstrapPassed = false, originalMainEntered = false;
    bool originalMainReturned = false, observed = false, ackAccepted = false, closeAccepted = false, aboutToQuit = false;
    std::uint64_t creation = 0, observedAt = 0;
    QPointer<MainWindow> window;
    std::string error;
};
Context* activeContext = nullptr;

Path CheckedDirectory(Context& context, const Path& path, bool create) {
    Require(Native::Within(path, context.root), "FIXTURE_PATH_ESCAPED");
    if (create) std::filesystem::create_directories(path);
    const auto lease = Native::LockSafeDirectory(path);
    Require(lease.Succeeded(), "FIXTURE_DIRECTORY_UNSAFE");
    Require(QDir::toNativeSeparators(QFileInfo(QtPath(path)).canonicalFilePath()).compare(QtPath(path), Qt::CaseInsensitive) == 0,
        "FIXTURE_DIRECTORY_ALIAS");
    return path;
}
Json VerifyPortableSkills(Context& context, bool retainLeases) {
    const auto root = CheckedDirectory(context, context.root / L"install" / L"skills", false);
    const auto& expected = context.marker.at("skills_files");
    Require(expected.is_object() && expected.size() == kPortableSkills.size(), "PORTABLE_SKILLS_MANIFEST_INVALID");
    std::set<std::string> files, directories;
    Json inventory = Json::object();
    std::uint64_t total = 0;
    for (const auto* relative : kPortableSkills) {
        Require(expected.contains(relative), "PORTABLE_SKILL_NOT_APPROVED");
        const auto path = root / Path(std::wstring(relative, relative + std::strlen(relative)));
        const auto& identity = expected.at(relative);
        Require(identity.is_object() && identity.size() == 2 && identity.at("bytes").is_number_unsigned()
            && identity.at("sha256").is_string(), "PORTABLE_SKILL_IDENTITY_INVALID");
        auto lease = Native::OpenSafeReadFile(path, 65536);
        Require(lease.Succeeded() && lease.value->bytes > 0 && lease.value->bytes == identity.at("bytes").get<std::uint64_t>(),
            "PORTABLE_SKILL_LEASE_INVALID");
        const auto hash = War3Map::Sha256Handle(lease.value->file.Get());
        Require(hash.Succeeded() && *hash.value == identity.at("sha256").get<std::string>(), "PORTABLE_SKILL_SHA_MISMATCH");
        total += lease.value->bytes; Require(total <= 256 * 1024, "PORTABLE_SKILLS_BYTES_BUDGET");
        inventory[relative] = {{"bytes", lease.value->bytes}, {"sha256", *hash.value}};
        files.insert(relative);
        auto parent = Path(std::wstring(relative, relative + std::strlen(relative))).parent_path();
        while (!parent.empty()) { directories.insert(RelativeUtf8(parent)); parent = parent.parent_path(); }
        if (retainLeases) context.skillLeases.push_back(std::move(*lease.value));
    }
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        Require(++count <= 32 && Native::Within(entry.path(), root), "PORTABLE_SKILLS_INVENTORY_BUDGET");
        const auto flags = GetFileAttributesW(entry.path().c_str());
        Require(flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_REPARSE_POINT), "PORTABLE_SKILLS_REPARSE_POINT");
        const auto relative = RelativeUtf8(entry.path().lexically_relative(root));
        if (flags & FILE_ATTRIBUTE_DIRECTORY) Require(directories.erase(relative) == 1, "PORTABLE_SKILLS_UNKNOWN_DIRECTORY");
        else Require(files.erase(relative) == 1, "PORTABLE_SKILLS_UNKNOWN_FILE");
    }
    Require(files.empty() && directories.empty(), "PORTABLE_SKILLS_INVENTORY_INCOMPLETE");
    // 这两个标记使生产 SkillsDirectory 的首个 portable 分支成立，不能回到编译期源码根。
    Require(QFileInfo(QtPath(root / L"forge-editor-mcp" / L"SKILL.md")).isFile()
        && QFileInfo(QtPath(root / L"forge-t-event-optimization" / L"SKILL.md")).isFile(), "PORTABLE_SKILLS_SELECTION_MARKER_MISSING");
    return {{"root", Utf8(root)}, {"portable_branch_markers_present", true}, {"files", inventory}, {"total_bytes", total}};
}
void RequirePath(Context& context, Json& paths, const char* label, const QString& location) {
    Require(!location.isEmpty(), "QT_STANDARD_PATH_EMPTY");
    const auto normalized = Native::NormalizeLocalPath(Path(location.toStdWString()), true);
    Require(normalized.Succeeded() && Native::Within(*normalized.value, context.root), "QT_STANDARD_PATH_ESCAPED");
    CheckedDirectory(context, *normalized.value, true);
    paths[label] = Utf8(*normalized.value);
}
Json StandardPathPreflight(Context& context) {
    Json paths = Json::object();
    const std::array<std::pair<const char*, QStandardPaths::StandardLocation>, 12> locations{{
        {"generic_data", QStandardPaths::GenericDataLocation}, {"app_config", QStandardPaths::AppConfigLocation},
        {"app_local_data", QStandardPaths::AppLocalDataLocation}, {"app_data", QStandardPaths::AppDataLocation},
        {"config", QStandardPaths::ConfigLocation}, {"generic_config", QStandardPaths::GenericConfigLocation},
        {"cache", QStandardPaths::CacheLocation}, {"generic_cache", QStandardPaths::GenericCacheLocation},
        {"state", QStandardPaths::StateLocation}, {"generic_state", QStandardPaths::GenericStateLocation},
        {"home", QStandardPaths::HomeLocation}, {"temp", QStandardPaths::TempLocation}}};
    for (const auto& [name, location] : locations) RequirePath(context, paths, name, QStandardPaths::writableLocation(location));
    // QStandardPaths 的只读列表还含 exe/data；本进程 exe 本身就在 fixture install 内。
    for (const auto location : {QStandardPaths::AppConfigLocation, QStandardPaths::AppLocalDataLocation,
             QStandardPaths::AppDataLocation, QStandardPaths::GenericDataLocation}) {
        for (const auto& path : QStandardPaths::standardLocations(location))
            RequirePath(context, paths, ("fallback_" + std::to_string(paths.size())).c_str(), path);
    }
    return paths;
}
void CheckSettings(Context& context, QSettings& settings, Json& facts, const char* label, QSettings::Scope scope = QSettings::UserScope) {
    Require(settings.format() == QSettings::IniFormat && settings.scope() == scope
        && settings.organizationName() == QStringLiteral("CallDisaster") && settings.applicationName() == QStringLiteral("Audastra"),
        "SETTINGS_IDENTITY_INVALID");
    settings.sync();
    Require(settings.status() == QSettings::NoError, "SETTINGS_SYNC_FAILED");
    const auto normalized = Native::NormalizeLocalPath(Path(settings.fileName().toStdWString()), true);
    Require(normalized.Succeeded() && Native::Within(*normalized.value, context.root), "SETTINGS_PATH_ESCAPED");
    auto lease = Native::OpenSafeReadFile(*normalized.value, kJsonLimit);
    Require(lease.Succeeded(), "SETTINGS_FILE_UNSAFE");
    facts[label] = {{"format", "IniFormat"}, {"scope", scope == QSettings::UserScope ? "UserScope" : "SystemScope"}, {"file", Utf8(*normalized.value)}};
    Require(!War3Map::QtFeedback::FeedbackClient::ReadPreferences(settings).consent
        && settings.value(QStringLiteral("update/automaticEnabled"), true).toString() == QStringLiteral("false")
        && settings.value(QStringLiteral("ui/dataRoot")).toString().isEmpty()
        && settings.value(QStringLiteral("war3/gameRoot")).toString().isEmpty(), "SETTINGS_BUSINESS_FLAGS_INVALID");
}

void ValidateInvocation(const Context& context, int argc, char* argv[]) {
    const bool invalid = context.marker.at("scenario") == "duplicate-parameter";
    Require(argc == (invalid ? 9 : 7), "LAUNCHER_ARGUMENT_COUNT_INVALID");
    Require(std::string_view(argv[1]) == "--update-health-transaction"
        && std::string_view(argv[2]) == context.marker.at("transaction").get<std::string>()
        && std::string_view(argv[3]) == "--update-health-challenge"
        && std::string_view(argv[4]) == context.marker.at("challenge").get<std::string>()
        && std::string_view(argv[5]) == "--update-health-pipe", "LAUNCHER_ARGUMENTS_INVALID");
    std::uintptr_t pipe = 0;
    const std::string_view value(argv[6]);
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), pipe);
    DWORD flags = 0;
    Require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && pipe
        && GetFileType(reinterpret_cast<HANDLE>(pipe)) == FILE_TYPE_PIPE
        && GetHandleInformation(reinterpret_cast<HANDLE>(pipe), &flags) && (flags & HANDLE_FLAG_INHERIT), "LAUNCHER_PIPE_INVALID");
    if (invalid) Require(std::string_view(argv[7]) == "--update-health-transaction" && std::string_view(argv[8]) == argv[2],
        "LAUNCHER_NEGATIVE_ARGUMENTS_INVALID");
}
void Bootstrap(Context& context, int argc, char* argv[]) {
    const auto normalized = Native::NormalizeLocalPath(Path(Environment(L"AUDASTRA_MAIN_HEALTH_ROOT")), true);
    Require(normalized.Succeeded(), "FIXTURE_ROOT_INVALID");
    context.root = *normalized.value;
    auto lease = Native::LockSafeDirectory(context.root);
    Require(lease.Succeeded(), "FIXTURE_ROOT_UNSAFE"); context.rootLease = std::move(*lease.value);
    auto marker = Native::OpenSafeReadFile(context.root / L"ownership.json", kJsonLimit);
    Require(marker.Succeeded(), "FIXTURE_MARKER_UNSAFE"); context.markerLease = std::move(*marker.value);
    context.marker = ReadJson(context.root / L"ownership.json");
    const auto nonce = QString::fromStdWString(Environment(L"AUDASTRA_MAIN_HEALTH_NONCE")).toUtf8().toStdString();
    const auto scenario = QString::fromStdWString(Environment(L"AUDASTRA_MAIN_HEALTH_SCENARIO")).toUtf8().toStdString();
    Require(context.marker.at("format") == "audastra-product-main-health-fixture"
        && context.marker.at("nonce") == nonce && context.marker.at("scenario") == scenario
        && (scenario == "valid" || scenario == "duplicate-parameter"), "FIXTURE_MARKER_IDENTITY_INVALID");
    const auto identity = Native::ReadRootIdentity(context.root);
    Require(identity.Succeeded() && ExactUnsigned(context.marker, "root_volume", identity.value->volume)
        && ExactUnsigned(context.marker, "root_index", identity.value->index), "FIXTURE_ROOT_IDENTITY_MISMATCH");
    CheckedDirectory(context, context.root / L"receipts", false);
    context.receiptPath = context.root / L"receipts" / L"child.json";
    context.creation = OwnCreationTime();
    context.receipt = {{"format", "audastra-product-main-health-child"}, {"scenario", scenario}, {"nonce", nonce},
        {"transaction", context.marker.at("transaction")}, {"challenge", context.marker.at("challenge")},
        {"pid", static_cast<std::uint64_t>(GetCurrentProcessId())}, {"creation_time_100ns", context.creation},
        {"version", std::string(War3Map::BuildInfo::Version)}, {"commit", std::string(War3Map::BuildInfo::Commit)},
        {"fixture_root", Utf8(context.root)}, {"platform", "offscreen"}};
    Require(Environment(L"QT_QPA_PLATFORM") == L"offscreen", "OFFSCREEN_REQUIRED");
    ValidateInvocation(context, argc, argv);
    const auto self = ModulePath(nullptr);
    Require(Native::Within(self, context.root / L"install") && context.marker.at("launcher_sha256").is_string()
        && HashFile(self) == context.marker.at("launcher_sha256").get<std::string>(),
        "LAUNCHER_IDENTITY_MISMATCH");
    context.receipt["launcher_path"] = Utf8(self); context.receipt["launcher_sha256"] = context.marker.at("launcher_sha256");
    context.receipt["skills_before_main"] = VerifyPortableSkills(context, true);
    const auto core = GetModuleHandleW(L"Qt6Core.dll");
    Require(core != nullptr && std::string_view(qVersion()) == QT_VERSION_STR, "LOADED_QT_VERSION_MISMATCH");
    const auto corePath = ModulePath(core);
    Require(Native::Within(corePath, context.root / L"install")
        && corePath.filename().native() == L"Qt6Core.dll" && context.marker.at("qt_core_sha256").is_string()
        && HashFile(corePath) == context.marker.at("qt_core_sha256").get<std::string>(),
        "LOADED_QT_CORE_IDENTITY_MISMATCH");
    context.receipt["qt_core"] = {{"path", Utf8(corePath)}, {"sha256", context.marker.at("qt_core_sha256")}, {"version", qVersion()}};
    context.mapping.local = CheckedDirectory(context, context.root / L"local", false).wstring();
    context.mapping.low = CheckedDirectory(context, context.root / L"local-low", false).wstring();
    context.mapping.roaming = CheckedDirectory(context, context.root / L"roaming", false).wstring();
    context.mapping.programData = CheckedDirectory(context, context.root / L"programdata", false).wstring();
    context.mapping.home = CheckedDirectory(context, context.root / L"home", false).wstring();
    HANDLE token = nullptr;
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "OWN_USER_TOKEN_FAILED");
    Native::NativeHandle ownToken(token);
    DWORD tokenBytes = 0;
    Require(GetTokenInformation(ownToken.Get(), TokenUser, context.mapping.ownUser.data(),
        static_cast<DWORD>(context.mapping.ownUser.size()), &tokenBytes) != FALSE && tokenBytes >= sizeof(TOKEN_USER)
        && IsValidSid(reinterpret_cast<TOKEN_USER*>(context.mapping.ownUser.data())->User.Sid), "OWN_USER_TOKEN_INFORMATION_FAILED");
    for (const auto& [name, expected] : std::array<std::pair<const wchar_t*, Path>, 6>{{
             {L"APPDATA", context.root / L"roaming"}, {L"LOCALAPPDATA", context.root / L"local"},
             {L"PROGRAMDATA", context.root / L"programdata"}, {L"USERPROFILE", context.root / L"home"},
             {L"TEMP", context.root / L"tmp"}, {L"TMP", context.root / L"tmp"}}})
        Require(Path(Environment(name)) == expected, "FIXTURE_ENVIRONMENT_PATH_INVALID");
    context.isolation.Install(context.mapping, context.receipt["iat"]);
    context.previousFormat = QSettings::defaultFormat(); context.formatChanged = true;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QtPath(context.root / L"settings-user"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, QtPath(context.root / L"settings-system"));
    // 同生产身份；原 Main 随后重复配置，不另造 QApplication 或变更 app 名绕开路径。
    QCoreApplication::setOrganizationName(QStringLiteral("CallDisaster"));
    QCoreApplication::setApplicationName(QStringLiteral("Audastra"));
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("CallDisaster"), QStringLiteral("Audastra"));
    settings.setValue(QStringLiteral("update/automaticEnabled"), false);
    settings.setValue(QStringLiteral("feedback/consent"), false);
    CheckSettings(context, settings, context.receipt["settings"], "bootstrap_explicit");
    QSettings system(QSettings::IniFormat, QSettings::SystemScope, QStringLiteral("CallDisaster"), QStringLiteral("Audastra"));
    system.setValue(QStringLiteral("update/automaticEnabled"), false);
    system.setValue(QStringLiteral("feedback/consent"), false);
    CheckSettings(context, system, context.receipt["settings"], "bootstrap_system", QSettings::SystemScope);
    context.receipt["preflight_paths"] = StandardPathPreflight(context);
    Require(context.mapping.mapped.load() > 0 && context.mapping.profileMapped.load() > 0
        && context.mapping.profileSizing.load() > 0, "IAT_PATH_MAPPING_NOT_OBSERVED");
    War3Map::Agent::AgentSettingsStore store;
    QString error;
    RequirePath(context, context.receipt["preflight_paths"], "agent_directory", QFileInfo(store.FilePath()).absolutePath());
    War3Map::Agent::AgentSettings disabled;
    Require(store.Save(disabled, &error), "AGENT_FIXTURE_SAVE_FAILED");
    const auto actual = store.Load(&error);
    Require(error.isEmpty() && !actual.aiEnabled && !actual.mcpEnabled && actual.providers.isEmpty(), "AGENT_FIXTURE_NOT_DISABLED");
    const auto credentials = War3Map::Agent::CredentialStore().Directory();
    RequirePath(context, context.receipt["preflight_paths"], "credentials_directory", credentials);
    Require(QDir(credentials).entryList(QDir::Files | QDir::Hidden | QDir::System).isEmpty(), "CREDENTIAL_FIXTURE_NOT_EMPTY");
    context.bootstrapPassed = true;
}

void RecordActiveFailure(Context& context, const char* error) noexcept {
    try {
        context.error = error;
        const auto text = Json{{"format", "audastra-product-main-health-still-active"},
            {"pid", static_cast<std::uint64_t>(GetCurrentProcessId())}, {"creation_time_100ns", context.creation},
            {"transaction", context.marker.at("transaction")}, {"event_loop_observed", context.observed},
            {"parent_ack_accepted", context.ackAccepted}, {"close_accepted", context.closeAccepted}, {"error", context.error}}.dump() + "\n";
        (void)Native::WriteNewFile(context.root / L"receipts" / L"still-active.json", std::as_bytes(std::span(text.data(), text.size())));
    } catch (...) {}
}

void ObserveMainWindow() {
    auto* context = activeContext;
    if (!context) return;
    try {
        Require(context->originalMainEntered && !context->observed && QApplication::instance()
            && QThread::currentThread() == QApplication::instance()->thread(), "MAIN_EVENT_LOOP_IDENTITY_INVALID");
        QList<MainWindow*> windows;
        for (auto* widget : QApplication::topLevelWidgets()) if (auto* window = dynamic_cast<MainWindow*>(widget)) windows.push_back(window);
        Require(windows.size() == 1 && windows.front()->isVisible() && windows.front()->thread() == QThread::currentThread(),
            "UNIQUE_VISIBLE_MAIN_WINDOW_REQUIRED");
        context->window = windows.front(); context->observed = true; context->observedAt = GetTickCount64();
        context->receipt["main_window"] = {{"unique", true}, {"visible", true}, {"type", typeid(*windows.front()).name()},
            {"ui_thread", true}, {"event_loop_observed", true}, {"main_arguments_mode", "normal-health"}};
        context->receipt["observed_paths"] = StandardPathPreflight(*context);
        context->receipt["skills_in_event_loop"] = VerifyPortableSkills(*context, false);
        Require(QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("skills")))
            .compare(QtPath(context->root / L"install" / L"skills"), Qt::CaseInsensitive) == 0, "PRODUCTION_PORTABLE_SKILLS_ROOT_MISMATCH");
        QSettings defaults;
        QSettings explicitSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("CallDisaster"), QStringLiteral("Audastra"));
        CheckSettings(*context, defaults, context->receipt["settings"], "observed_default");
        CheckSettings(*context, explicitSettings, context->receipt["settings"], "observed_explicit");
        const auto updates = context->window->findChildren<War3Map::QtUpdate::UpdateClient*>();
        const auto feedback = context->window->findChildren<War3Map::QtFeedback::FeedbackClient*>();
        Require(updates.size() == 1 && !updates.front()->AutomaticCheckEnabled()
            && feedback.size() == 1 && !feedback.front()->Preferences().consent, "ACTUAL_PRODUCT_SERVICE_FLAGS_INVALID");
        const auto queue = War3Map::Mcp::ApplicationFeedbackQueue();
        Require(queue.Succeeded() && !(*queue.value)->Consent(), "ACTUAL_FEEDBACK_QUEUE_INVALID");
        const auto entries = (*queue.value)->List();
        Require(entries.Succeeded() && entries.value->empty(), "ACTUAL_FEEDBACK_QUEUE_NOT_EMPTY");
        context->receipt["business"] = {{"automatic_update", false}, {"feedback_consent", false}, {"feedback_queue_empty", true},
            {"ai_config_enabled", false}, {"mcp_config_enabled", false}, {"network_observation", "business-gates-only"}};
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, context->window.data(), [context] { context->aboutToQuit = true; });
        auto* timer = new QTimer(context->window.data());
        timer->setInterval(20);
        QObject::connect(timer, &QTimer::timeout, context->window.data(), [context, timer] {
            try {
                const auto ackPath = context->root / L"receipts" / L"health-ack.json";
                if (!std::filesystem::exists(ackPath)) {
                    if (GetTickCount64() - context->observedAt > kAckDeadlineMs) {
                        timer->stop(); RecordActiveFailure(*context, "PARENT_ACK_TIMEOUT_CHILD_RETAINED");
                    }
                    return;
                }
                const auto ack = ReadJson(ackPath, 2048);
                Require(ack.size() == 7 && ack.at("format") == "audastra-product-main-health-ack"
                    && ack.at("transaction") == context->marker.at("transaction") && ack.at("challenge") == context->marker.at("challenge")
                    && ExactUnsigned(ack, "pid", GetCurrentProcessId()) && ExactUnsigned(ack, "creation_time_100ns", context->creation)
                    && ack.at("version") == std::string(War3Map::BuildInfo::Version) && ack.at("commit") == std::string(War3Map::BuildInfo::Commit),
                    "PARENT_ACK_IDENTITY_INVALID");
                timer->stop(); context->ackAccepted = true;
                Require(context->window && context->window->isVisible(), "MAIN_WINDOW_DISAPPEARED");
                context->closeAccepted = context->window->close();
                Require(context->closeAccepted, "MAIN_CLOSE_GUARD_REJECTED_CHILD_RETAINED");
            } catch (const std::exception& error) {
                timer->stop(); RecordActiveFailure(*context, error.what());
            }
        });
        timer->start();
    } catch (const std::exception& error) {
        RecordActiveFailure(*context, error.what()); // 失败保留活动主窗，父驱动不会结束它。
    }
}
void QueueMainObservation() {
    if (activeContext) QTimer::singleShot(0, QCoreApplication::instance(), [] { ObserveMainWindow(); });
}
Q_COREAPP_STARTUP_FUNCTION(QueueMainObservation)

} // namespace

int main(int argc, char* argv[]) {
    Context context;
    int mainResult = -1;
    try {
        Bootstrap(context, argc, argv);
        activeContext = &context; context.originalMainEntered = true;
        mainResult = AudastraProductMainForHealthTest(argc, argv);
        context.originalMainReturned = true;
        context.receipt["skills_after_main"] = VerifyPortableSkills(context, false);
    } catch (const std::exception& error) {
        context.error = error.what();
    } catch (...) {
        context.error = "MAIN_HEALTH_UNEXPECTED_EXCEPTION";
    }
    // 原 Main 的所有栈对象（包括 QApplication/业务线程）已经析构，再恢复进程内状态。
    activeContext = nullptr;
    const bool restored = context.isolation.Restore(context.receipt["iat"]);
    if (context.formatChanged) QSettings::setDefaultFormat(context.previousFormat);
    const bool formatRestored = !context.formatChanged || QSettings::defaultFormat() == context.previousFormat;
    if (context.receiptPath.empty()) { std::fputs("MAIN_HEALTH_BOOTSTRAP_REJECTED\n", stderr); return 70; }
    const bool invalid = context.marker.at("scenario") == "duplicate-parameter";
    const bool scenarioPassed = context.bootstrapPassed && context.originalMainReturned && restored && formatRestored
        && context.error.empty() && (invalid ? mainResult == 1 && !context.observed && !context.ackAccepted
            : mainResult == 0 && context.observed && context.ackAccepted && context.closeAccepted && context.aboutToQuit);
    context.receipt["bootstrap_passed"] = context.bootstrapPassed;
    context.receipt["original_main_entered"] = context.originalMainEntered;
    context.receipt["original_main_returned"] = context.originalMainReturned;
    context.receipt["original_main_return_code"] = mainResult;
    context.receipt["event_loop_observed"] = context.observed;
    context.receipt["parent_ack_accepted"] = context.ackAccepted;
    context.receipt["close_accepted"] = context.closeAccepted;
    context.receipt["about_to_quit"] = context.aboutToQuit;
    context.receipt["iat_restored"] = restored;
    context.receipt["default_format_restored"] = formatRestored;
    context.receipt["known_folder_calls"] = {{"mapped", context.mapping.mapped.load()},
        {"denied", context.mapping.denied.load()}, {"fonts_read_only", context.mapping.fonts.load()}};
    context.receipt["profile_directory_calls"] = {{"mapped", context.mapping.profileMapped.load()},
        {"denied", context.mapping.profileDenied.load()}, {"sizing", context.mapping.profileSizing.load()}};
    context.receipt["error"] = context.error; context.receipt["scenario_passed"] = scenarioPassed;
    try {
        const auto text = context.receipt.dump(2) + "\n";
        const auto written = Native::WriteNewFile(context.receiptPath, std::as_bytes(std::span(text.data(), text.size())));
        Require(written.Succeeded(), "CHILD_RECEIPT_CREATE_NEW_FAILED");
    } catch (...) { std::fputs("MAIN_HEALTH_RECEIPT_FAILED\n", stderr); return 71; }
    return scenarioPassed ? mainResult : 72;
}
