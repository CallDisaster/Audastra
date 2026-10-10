#include "AudastraVersion.generated.h"
#include "Serialization/StrictJson.h"
#include "Update/UpdateInstallerInternal.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>

#include <QRandomGenerator>
#include <QString>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace Native = War3Map::Update::Detail;
using Json = nlohmann::json;
using Path = std::filesystem::path;

constexpr std::uint64_t kFileLimit = 128 * 1024 * 1024;
constexpr std::uint64_t kHealthTimeoutMs = 20000, kExitTimeoutMs = 20000;
constexpr std::size_t kReceiptLimit = 64 * 1024;
constexpr std::string_view kSentinel = "Original product-main-health sentinel; preserve these bytes.\n";
constexpr std::array<const char*, 6> kPortableSkills{{
    "forge-editor-mcp/SKILL.md", "forge-editor-mcp/agents/openai.yaml", "forge-editor-mcp/references/current-contract.md",
    "forge-t-event-optimization/SKILL.md", "forge-t-event-optimization/agents/openai.yaml",
    "forge-t-event-optimization/references/dispatch-contract.md"}};

void Require(bool value, const char* code) { if (!value) throw std::runtime_error(code); }
std::string Utf8(const Path& path) { return QString::fromStdWString(path.wstring()).toUtf8().toStdString(); }
std::string RelativeUtf8(const Path& path) {
    auto value = Utf8(path); std::replace(value.begin(), value.end(), '\\', '/'); return value;
}
std::span<const std::byte> Bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }
std::string Lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string RandomHex(std::size_t length) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    while (result.size() < length) {
        const auto value = QRandomGenerator::system()->generate64();
        for (unsigned shift = 0; shift < 64 && result.size() < length; shift += 4)
            result.push_back(digits[(value >> shift) & 15]);
    }
    return result;
}
Path CheckedAbsolute(const Path& input) {
    const auto normalized = Native::NormalizeLocalPath(input, true);
    Require(normalized.Succeeded(), "INPUT_PATH_INVALID");
    return *normalized.value;
}
Path SystemDirectory() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    Require(count > 0 && count < path.size(), "SYSTEM_DIRECTORY_FAILED");
    return CheckedAbsolute(Path(std::wstring(path.data(), count)));
}
Path WindowsDirectory() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetWindowsDirectoryW(path.data(), static_cast<UINT>(path.size()));
    Require(count > 0 && count < path.size(), "WINDOWS_DIRECTORY_FAILED");
    return CheckedAbsolute(Path(std::wstring(path.data(), count)));
}
Path ActualTempDirectory() {
    const auto input = std::filesystem::temp_directory_path();
    Native::NativeHandle handle(CreateFileW(input.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    Require(handle.Valid(), "TEMP_DIRECTORY_OPEN_FAILED");
    std::array<wchar_t, 32768> path{};
    const auto count = GetFinalPathNameByHandleW(handle.Get(), path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    Require(count > 4 && count < path.size(), "TEMP_DIRECTORY_RESOLUTION_FAILED");
    const std::wstring value(path.data(), count);
    Require(value.starts_with(L"\\\\?\\") && !value.starts_with(L"\\\\?\\UNC\\"), "TEMP_DIRECTORY_NOT_LOCAL");
    return CheckedAbsolute(Path(value.substr(4)));
}
std::vector<std::byte> ReadFile(const Path& path, std::uint64_t maximum = kFileLimit) {
    auto lease = Native::OpenSafeReadFile(path, maximum);
    Require(lease.Succeeded(), "SOURCE_FILE_LEASE_FAILED");
    // Core ReadLeaseBytes 专用于有更小预算的签名清单，不用于 PE 运行库。
    // 保留上方普通文件/身份/容量租约，按有界分块完整读取同一原句柄。
    std::vector<std::byte> bytes(static_cast<std::size_t>(lease.value->bytes));
    LARGE_INTEGER zero{};
    Require(::SetFilePointerEx(lease.value->file.Get(), zero, nullptr, FILE_BEGIN) != FALSE,
        "SOURCE_FILE_SEEK_FAILED");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto amount = static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t{1024 * 1024}));
        DWORD received = 0;
        Require(::ReadFile(lease.value->file.Get(), bytes.data() + offset, amount, &received, nullptr) != FALSE
            && received > 0 && received <= amount, "SOURCE_FILE_READ_FAILED");
        offset += received;
    }
    return bytes;
}
std::string Hash(std::span<const std::byte> bytes) {
    const auto hash = War3Map::Sha256(bytes);
    Require(hash.Succeeded(), "SHA256_FAILED"); return *hash.value;
}
std::string HashFile(const Path& path) {
    auto lease = Native::OpenSafeReadFile(path, kFileLimit);
    Require(lease.Succeeded(), "INVENTORY_FILE_LEASE_FAILED");
    const auto hash = War3Map::Sha256Handle(lease.value->file.Get());
    Require(hash.Succeeded(), "INVENTORY_SHA256_FAILED"); return *hash.value;
}
void WriteNew(const Path& path, std::string_view text) {
    const auto result = Native::WriteNewFile(path, Bytes(text));
    Require(result.Succeeded(), "FIXTURE_CREATE_NEW_FAILED");
}
Json StrictObject(std::span<const std::byte> bytes, std::size_t maximum, std::size_t nodes = 2048) {
    War3Map::Serialization::StrictJsonLimits limits;
    limits.maxBytes = maximum; limits.maxDepth = 12; limits.maxNodes = nodes;
    limits.maxStringBytes = 32768; limits.rejectFloatingPoint = true;
    const auto result = War3Map::Serialization::ParseStrictJson(bytes, limits);
    Require(result.Succeeded() && result.value->is_object(), "STRICT_PROTOCOL_OBJECT_INVALID");
    return *result.value;
}
bool ExactUnsigned(const Json& object, const char* key, std::uint64_t value) {
    return object.contains(key) && object.at(key).is_number_unsigned() && object.at(key).get<std::uint64_t>() == value;
}
bool True(const Json& object, const char* key) { return object.contains(key) && object.at(key).is_boolean() && object.at(key).get<bool>(); }

// 只读取复制对象的 PE import/delay-import，不执行 DLL。按实际依赖图复制，不扫整个 build。
class DiskImage final {
public:
    explicit DiskImage(std::span<const std::byte> bytes) : bytes_(bytes) {
        const auto dos = Read<IMAGE_DOS_HEADER>(0);
        Require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0, "RUNTIME_DOS_INVALID");
        headers_ = Read<IMAGE_NT_HEADERS64>(static_cast<std::size_t>(dos.e_lfanew));
        Require(headers_.Signature == IMAGE_NT_SIGNATURE && headers_.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
            && headers_.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
            && headers_.FileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)
            && headers_.FileHeader.NumberOfSections > 0 && headers_.FileHeader.NumberOfSections <= 96
            && headers_.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, "RUNTIME_PE64_INVALID");
        const auto sectionOffset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)
            + headers_.FileHeader.SizeOfOptionalHeader;
        for (std::size_t index = 0; index < headers_.FileHeader.NumberOfSections; ++index)
            sections_.push_back(Read<IMAGE_SECTION_HEADER>(sectionOffset + index * sizeof(IMAGE_SECTION_HEADER)));
    }
    std::vector<std::string> Imports() const {
        std::vector<std::string> names;
        const auto directory = headers_.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress || directory.Size) {
            Require(directory.VirtualAddress && directory.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR) && directory.Size <= 1024 * 1024,
                "RUNTIME_IMPORT_DIRECTORY_INVALID");
            (void)Offset(directory.VirtualAddress, directory.Size);
            bool ended = false;
            for (std::size_t index = 0; index < directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR); ++index) {
                const auto entry = ReadRva<IMAGE_IMPORT_DESCRIPTOR>(std::uint64_t{directory.VirtualAddress} + index * sizeof(IMAGE_IMPORT_DESCRIPTOR));
                if (!entry.Name && !entry.FirstThunk && !entry.OriginalFirstThunk) { ended = true; break; }
                names.push_back(Name(entry.Name));
            }
            Require(ended, "RUNTIME_IMPORT_UNTERMINATED");
        }
        struct DelayImport { DWORD attributes, name, module, iat, names, bound, unload, timestamp; };
        const auto delay = headers_.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (delay.VirtualAddress || delay.Size) {
            Require(delay.VirtualAddress && delay.Size >= sizeof(DelayImport) && delay.Size <= 1024 * 1024,
                "RUNTIME_DELAY_DIRECTORY_INVALID");
            (void)Offset(delay.VirtualAddress, delay.Size);
            bool ended = false;
            for (std::size_t index = 0; index < delay.Size / sizeof(DelayImport); ++index) {
                const auto entry = ReadRva<DelayImport>(std::uint64_t{delay.VirtualAddress} + index * sizeof(DelayImport));
                if (!entry.name && !entry.iat && !entry.attributes) { ended = true; break; }
                Require(entry.attributes == 1, "RUNTIME_DELAY_RVA_REQUIRED");
                names.push_back(Name(entry.name));
            }
            Require(ended, "RUNTIME_DELAY_UNTERMINATED");
        }
        return names;
    }
private:
    template<typename T> T Read(std::size_t offset) const {
        Require(offset <= bytes_.size() && sizeof(T) <= bytes_.size() - offset, "RUNTIME_PE_FILE_BOUNDS");
        T result{}; std::memcpy(&result, bytes_.data() + offset, sizeof(T)); return result;
    }
    std::size_t Offset(std::uint64_t rva, std::size_t length = 1) const {
        Require(rva <= (std::numeric_limits<DWORD>::max)(), "RUNTIME_RVA_OVERFLOW");
        if (rva < headers_.OptionalHeader.SizeOfHeaders) {
            Require(rva <= bytes_.size() && length <= bytes_.size() - rva
                && length <= headers_.OptionalHeader.SizeOfHeaders - rva, "RUNTIME_HEADER_RVA_INVALID");
            return static_cast<std::size_t>(rva);
        }
        for (const auto& section : sections_) {
            if (rva < section.VirtualAddress) continue;
            const auto delta = rva - section.VirtualAddress;
            if (delta >= section.SizeOfRawData) continue;
            const auto offset = std::uint64_t{section.PointerToRawData} + delta;
            Require(offset <= bytes_.size() && length <= bytes_.size() - offset
                && length <= section.SizeOfRawData - delta, "RUNTIME_SECTION_RVA_INVALID"); return static_cast<std::size_t>(offset);
        }
        throw std::runtime_error("RUNTIME_RVA_UNMAPPED");
    }
    template<typename T> T ReadRva(std::uint64_t rva) const { return Read<T>(Offset(rva, sizeof(T))); }
    std::string Name(std::uint64_t rva) const {
        std::string result;
        for (std::size_t index = 0; index < 256; ++index) {
            const auto value = ReadRva<char>(rva + index);
            if (!value) {
                Require(!result.empty() && Native::SafeWindowsComponent(std::wstring(result.begin(), result.end()))
                    && Lower(result).ends_with(".dll"), "RUNTIME_DLL_NAME_INVALID");
                return result;
            }
            Require(value > 0 && static_cast<unsigned char>(value) < 128, "RUNTIME_DLL_NAME_NONASCII"); result.push_back(value);
        }
        throw std::runtime_error("RUNTIME_DLL_NAME_UNTERMINATED");
    }
    std::span<const std::byte> bytes_;
    IMAGE_NT_HEADERS64 headers_{};
    std::vector<IMAGE_SECTION_HEADER> sections_;
};

struct Scenario {
    Path root, install, executable;
    Native::DirectoryReadLease rootLease;
    Native::FileReadLease markerLease;
    Native::RootIdentity identity;
    std::string name, nonce, transaction, challenge;
    std::map<std::string, std::string> files;
    std::vector<Native::DirectoryReadLease> directoryLeases;
    Json marker;
    Json skills = Json::object();
    Path skillsSource;
};
void OwnDirectory(Scenario& scene, const Path& directory) {
    Require(Native::Within(directory, scene.root), "FIXTURE_DIRECTORY_ESCAPED");
    std::filesystem::create_directories(directory);
    auto lease = Native::LockSafeDirectory(directory);
    Require(lease.Succeeded(), "FIXTURE_DIRECTORY_UNSAFE"); scene.directoryLeases.push_back(std::move(*lease.value));
}
void CopyRuntimeGraph(Scenario& scene, const Path& launcher, const Path& runtimeDirectory) {
    std::set<std::string> visited;
    const auto system = SystemDirectory();
    const auto copy = [&](const Path& source, const std::string& relative) {
        Require(scene.files.size() < 128, "RUNTIME_COPY_BUDGET");
        const auto bytes = ReadFile(source);
        const auto target = scene.install / Path(std::wstring(relative.begin(), relative.end()));
        Require(Native::Within(target, scene.install), "RUNTIME_TARGET_ESCAPED");
        OwnDirectory(scene, target.parent_path());
        const auto written = Native::WriteNewFile(target, bytes);
        Require(written.Succeeded(), "RUNTIME_COPY_CREATE_NEW_FAILED");
        scene.files.emplace(relative, Hash(bytes));
        return DiskImage(bytes).Imports();
    };
    std::vector<std::string> pending = copy(launcher, "product-main-health-launcher.exe");
    Path offscreen = runtimeDirectory / L"platforms" / L"qoffscreen.dll";
    if (!std::filesystem::exists(offscreen)) offscreen = runtimeDirectory / L"plugins" / L"platforms" / L"qoffscreen.dll";
    Require(std::filesystem::exists(offscreen), "OFFSCREEN_RUNTIME_MISSING");
    const auto pluginImports = copy(offscreen, "platforms/qoffscreen.dll");
    pending.insert(pending.end(), pluginImports.begin(), pluginImports.end());
    for (std::size_t index = 0; index < pending.size(); ++index) {
        Require(pending.size() <= 4096, "RUNTIME_IMPORT_BUDGET");
        const auto name = pending[index]; const auto lower = Lower(name);
        if (!visited.insert(lower).second) continue;
        if (lower.starts_with("api-ms-win-") || lower.starts_with("ext-ms-")) continue;
        const Path filename(std::wstring(name.begin(), name.end()));
        const auto source = runtimeDirectory / filename;
        if (!std::filesystem::exists(source)) {
            const auto attributes = GetFileAttributesW((system / filename).c_str());
            Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)),
                "RUNTIME_DEPENDENCY_MISSING");
            continue; // Windows 系统库不复制，也不修改系统搜索路径。
        }
        const auto dependencies = copy(source, name);
        pending.insert(pending.end(), dependencies.begin(), dependencies.end());
    }
    Require(scene.files.contains("Qt6Core.dll"), "QT_CORE_COPY_REQUIRED");
    WriteNew(scene.install / L"qt.conf", "[Paths]\nPrefix=.\nPlugins=.\nTranslations=translations\n");
    scene.files.emplace("qt.conf", Hash(Bytes("[Paths]\nPrefix=.\nPlugins=.\nTranslations=translations\n")));
}
void CopyPortableSkills(Scenario& scene, const Path& source) {
    auto lease = Native::LockSafeDirectory(source);
    Require(lease.Succeeded(), "SKILLS_SOURCE_DIRECTORY_UNSAFE");
    std::set<std::string> approved, directories;
    std::uint64_t total = 0;
    scene.skillsSource = source;
    for (const auto* name : kPortableSkills) {
        const Path relative(std::wstring(name, name + std::strlen(name)));
        const auto bytes = ReadFile(source / relative, 65536);
        Require(!bytes.empty(), "SKILL_SOURCE_EMPTY");
        total += bytes.size(); Require(total <= 256 * 1024, "SKILLS_SOURCE_BYTES_BUDGET");
        const auto target = scene.install / L"skills" / relative;
        OwnDirectory(scene, target.parent_path());
        Require(Native::WriteNewFile(target, bytes).Succeeded(), "SKILL_COPY_CREATE_NEW_FAILED");
        scene.skills[name] = {{"bytes", static_cast<std::uint64_t>(bytes.size())}, {"sha256", Hash(bytes)}};
        approved.insert(name);
        auto parent = relative.parent_path();
        while (!parent.empty()) { directories.insert(RelativeUtf8(parent)); parent = parent.parent_path(); }
    }
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        Require(++count <= 32 && Native::Within(entry.path(), source), "SKILLS_SOURCE_INVENTORY_BUDGET");
        const auto flags = GetFileAttributesW(entry.path().c_str());
        Require(flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_REPARSE_POINT), "SKILLS_SOURCE_REPARSE_POINT");
        const auto relative = RelativeUtf8(entry.path().lexically_relative(source));
        if (flags & FILE_ATTRIBUTE_DIRECTORY) Require(directories.erase(relative) == 1, "SKILLS_SOURCE_UNKNOWN_DIRECTORY");
        else Require(approved.erase(relative) == 1, "SKILLS_SOURCE_UNKNOWN_FILE");
    }
    Require(approved.empty() && directories.empty(), "SKILLS_SOURCE_INVENTORY_INCOMPLETE");
}
Scenario Prepare(const Path& launcher, const Path& runtime, const Path& skillsSource, std::string name) {
    Scenario scene;
    scene.name = std::move(name); scene.nonce = RandomHex(32);
    scene.transaction = RandomHex(32); scene.challenge = RandomHex(64);
    const auto temp = ActualTempDirectory();
    auto parentLease = Native::LockSafeDirectory(temp);
    Require(parentLease.Succeeded(), "TEMP_DIRECTORY_UNSAFE");
    scene.root = temp / (L"AudastraProductMainHealth-" + std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::wstring(scene.nonce.begin(), scene.nonce.end()));
    Require(scene.root.parent_path() == temp && std::filesystem::create_directory(scene.root), "FIXTURE_EXCLUSIVE_ROOT_FAILED");
    auto lease = Native::LockSafeDirectory(scene.root);
    Require(lease.Succeeded(), "FIXTURE_ROOT_LEASE_FAILED"); scene.rootLease = std::move(*lease.value);
    const auto identity = Native::ReadRootIdentity(scene.root);
    Require(identity.Succeeded(), "FIXTURE_ROOT_IDENTITY_FAILED"); scene.identity = *identity.value;
    scene.install = scene.root / L"install"; scene.executable = scene.install / L"product-main-health-launcher.exe";
    for (const auto* name : {L"install", L"settings-user", L"settings-system", L"local", L"local-low", L"roaming",
             L"programdata", L"home", L"tmp", L"receipts"}) OwnDirectory(scene, scene.root / name);
    WriteNew(scene.root / L"sentinel.txt", kSentinel);
    std::cout << Json{{"fixture_created", Utf8(scene.root)}, {"scenario", scene.name}}.dump() << std::endl;
    try {
        CopyRuntimeGraph(scene, launcher, runtime);
        CopyPortableSkills(scene, skillsSource);
        scene.marker = {{"format", "audastra-product-main-health-fixture"}, {"scenario", scene.name}, {"nonce", scene.nonce},
            {"transaction", scene.transaction}, {"challenge", scene.challenge}, {"root_volume", scene.identity.volume},
            {"root_index", scene.identity.index}, {"launcher_sha256", scene.files.at("product-main-health-launcher.exe")},
            {"qt_core_sha256", scene.files.at("Qt6Core.dll")}, {"runtime_files", scene.files}, {"skills_files", scene.skills}};
        WriteNew(scene.root / L"ownership.json", scene.marker.dump(2) + "\n");
        auto marker = Native::OpenSafeReadFile(scene.root / L"ownership.json", kReceiptLimit);
        Require(marker.Succeeded(), "FIXTURE_MARKER_LEASE_FAILED"); scene.markerLease = std::move(*marker.value);
    } catch (const std::exception& error) {
        WriteNew(scene.root / L"receipts" / L"preparation-failed.json", Json{{"format", "audastra-product-main-health-parent"},
            {"scenario", scene.name}, {"fixture_root", Utf8(scene.root)}, {"child_started", false}, {"passed", false},
            {"error", error.what()}, {"copied_runtime_files", scene.files}, {"copied_skills_files", scene.skills}, {"retained", true}}.dump(2) + "\n");
        throw;
    }
    return scene;
}

std::wstring Quote(std::wstring_view text) {
    std::wstring result = L"\""; std::size_t slashes = 0;
    for (const wchar_t c : text) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') result.push_back(L'\\');
        result.push_back(c); slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result.push_back(L'"'); return result;
}
std::vector<wchar_t> ChildEnvironment(const Scenario& scene) {
    const auto windows = WindowsDirectory(), system = SystemDirectory();
    // 从白名单创建子环境，不继承作者 PATH、API key、Qt 插件或外部 HOME。
    std::map<std::wstring, std::wstring, Native::WindowsPathLess> values{{L"SystemRoot", windows.wstring()}, {L"WINDIR", windows.wstring()},
        {L"PATH", scene.install.wstring() + L";" + system.wstring() + L";" + windows.wstring()},
        {L"APPDATA", (scene.root / L"roaming").wstring()}, {L"LOCALAPPDATA", (scene.root / L"local").wstring()},
        {L"PROGRAMDATA", (scene.root / L"programdata").wstring()}, {L"USERPROFILE", (scene.root / L"home").wstring()},
        {L"HOME", (scene.root / L"home").wstring()}, {L"TEMP", (scene.root / L"tmp").wstring()}, {L"TMP", (scene.root / L"tmp").wstring()},
        {L"QT_QPA_PLATFORM", L"offscreen"}, {L"QT_PLUGIN_PATH", scene.install.wstring()},
        {L"QT_QPA_PLATFORM_PLUGIN_PATH", (scene.install / L"platforms").wstring()}, {L"QT_SCALE_FACTOR", L"1"},
        {L"AUDASTRA_MAIN_HEALTH_ROOT", scene.root.wstring()},
        {L"AUDASTRA_MAIN_HEALTH_NONCE", std::wstring(scene.nonce.begin(), scene.nonce.end())},
        {L"AUDASTRA_MAIN_HEALTH_SCENARIO", std::wstring(scene.name.begin(), scene.name.end())}};
    std::vector<wchar_t> result;
    for (const auto& [name, value] : values) {
        Require(name.find(L'=') == std::wstring::npos && value.find(L'\0') == std::wstring::npos, "CHILD_ENVIRONMENT_INVALID");
        const auto entry = name + L"=" + value;
        result.insert(result.end(), entry.begin(), entry.end()); result.push_back(L'\0');
    }
    result.push_back(L'\0'); return result;
}
struct Child {
    Native::NativeHandle process, pipe;
    std::uint64_t pid = 0, creation = 0;
    bool Finished() const { return process.Valid() && WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0; }
    DWORD ExitCode() const {
        DWORD code = STILL_ACTIVE;
        Require(process.Valid() && GetExitCodeProcess(process.Get(), &code), "CHILD_EXIT_CODE_FAILED"); return code;
    }
};
void Start(Scenario& scene, Child& child) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rawRead = nullptr, rawWrite = nullptr;
    Require(CreatePipe(&rawRead, &rawWrite, &security, 4096) != FALSE, "HEALTH_PIPE_CREATE_FAILED");
    Native::NativeHandle read(rawRead), write(rawWrite);
    Require(SetHandleInformation(read.Get(), HANDLE_FLAG_INHERIT, 0) != FALSE, "HEALTH_READ_INHERIT_FAILED");
    SIZE_T bytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    Require(bytes > 0 && bytes <= 65536, "HANDLE_LIST_SIZE_INVALID");
    std::vector<std::byte> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    Require(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE, "HANDLE_LIST_INITIALIZE_FAILED");
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } attributeGuard{attributes};
    const auto inherit = write.Get();
    const bool updated = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE*>(&inherit), sizeof(inherit), nullptr, nullptr) != FALSE;
    auto command = Quote(scene.executable.wstring());
    const auto add = [&](std::wstring_view value) { command += L" " + Quote(value); };
    add(L"--update-health-transaction"); add(std::wstring(scene.transaction.begin(), scene.transaction.end()));
    add(L"--update-health-challenge"); add(std::wstring(scene.challenge.begin(), scene.challenge.end()));
    add(L"--update-health-pipe"); add(std::to_wstring(reinterpret_cast<std::uintptr_t>(write.Get())));
    if (scene.name == "duplicate-parameter") { add(L"--update-health-transaction"); add(std::wstring(scene.transaction.begin(), scene.transaction.end())); }
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    auto environment = ChildEnvironment(scene);
    PROCESS_INFORMATION process{};
    const bool started = updated && CreateProcessW(scene.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), scene.root.c_str(),
        &startup.StartupInfo, &process) != FALSE;
    Require(started, "NATURAL_CHILD_START_FAILED");
    child.process = Native::NativeHandle(process.hProcess);
    Native::NativeHandle thread(process.hThread);
    child.pipe = std::move(read); child.pid = process.dwProcessId;
    FILETIME creation{}, exit{}, kernel{}, user{};
    Require(GetProcessTimes(child.process.Get(), &creation, &exit, &kernel, &user) != FALSE, "CHILD_CREATION_TIME_FAILED");
    child.creation = (std::uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime;
    // write 在函数返回时释放父拥有的副本，子只继承这一个健康写端。
}
std::string Receive(Child& child) {
    const auto deadline = GetTickCount64() + kHealthTimeoutMs;
    std::string result;
    std::array<char, 1024> buffer{};
    while (GetTickCount64() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(child.pipe.Get(), nullptr, 0, nullptr, &available, nullptr)) {
            Require(GetLastError() == ERROR_BROKEN_PIPE, "HEALTH_PIPE_PEEK_FAILED"); return result;
        }
        if (available) {
            const auto size = static_cast<DWORD>((std::min)(buffer.size(), std::size_t{available}));
            DWORD received = 0;
            Require(::ReadFile(child.pipe.Get(), buffer.data(), size, &received, nullptr) != FALSE && received > 0, "HEALTH_PIPE_READ_FAILED");
            result.append(buffer.data(), received);
            Require(result.size() <= 2048, "HEALTH_MESSAGE_TOO_LARGE");
        }
        Sleep(10);
    }
    throw std::runtime_error("HEALTH_MESSAGE_TIMEOUT_CHILD_RETAINED");
}
Json ValidateHealth(const Scenario& scene, const Child& child, std::string_view text) {
    Require(!text.empty() && text.size() <= 2048 && text.back() == '\n' && text.find('\n') == text.size() - 1,
        "HEALTH_SINGLE_NEWLINE_REQUIRED");
    const auto message = text.substr(0, text.size() - 1);
    const auto json = StrictObject(Bytes(message), 2048, 32);
    Require(json.size() == 7 && json.at("format") == "audastra-update-health"
        && json.at("transaction") == scene.transaction && json.at("challenge") == scene.challenge
        && ExactUnsigned(json, "pid", child.pid) && ExactUnsigned(json, "creation_time_100ns", child.creation)
        && json.at("version") == std::string(War3Map::BuildInfo::Version) && json.at("commit") == std::string(War3Map::BuildInfo::Commit),
        "HEALTH_EXACT_IDENTITY_MISMATCH");
    Require(!child.Finished() && child.ExitCode() == STILL_ACTIVE, "HEALTH_CHILD_NOT_ACTIVE");
    return json;
}
bool WaitNatural(const Child& child) {
    const auto deadline = GetTickCount64() + kExitTimeoutMs;
    while (GetTickCount64() < deadline) {
        const auto state = WaitForSingleObject(child.process.Get(), 20);
        if (state == WAIT_OBJECT_0) return true;
        Require(state == WAIT_TIMEOUT, "NATURAL_CHILD_WAIT_FAILED");
    }
    return child.Finished();
}
void ValidatePathFacts(const Scenario& scene, const Json& paths) {
    Require(paths.is_object() && paths.size() >= 12 && paths.size() <= 64, "CHILD_PATH_FACTS_INVALID");
    for (const auto& [label, value] : paths.items()) {
        (void)label;
        Require(value.is_string(), "CHILD_PATH_FACT_TYPE_INVALID");
        const auto path = CheckedAbsolute(Path(QString::fromUtf8(value.get<std::string>().c_str()).toStdWString()));
        Require(Native::Within(path, scene.root) && Native::LockSafeDirectory(path).Succeeded(), "CHILD_PATH_FACT_ESCAPED");
    }
}
void ValidateChildReceipt(const Scenario& scene, const Child& child, const Json& receipt) {
    const bool invalid = scene.name == "duplicate-parameter";
    Require(receipt.at("format") == "audastra-product-main-health-child" && receipt.at("scenario") == scene.name
        && receipt.at("nonce") == scene.nonce && receipt.at("transaction") == scene.transaction && receipt.at("challenge") == scene.challenge
        && ExactUnsigned(receipt, "pid", child.pid) && ExactUnsigned(receipt, "creation_time_100ns", child.creation)
        && receipt.at("version") == std::string(War3Map::BuildInfo::Version) && receipt.at("commit") == std::string(War3Map::BuildInfo::Commit)
        && receipt.at("fixture_root") == Utf8(scene.root) && receipt.at("launcher_sha256") == scene.marker.at("launcher_sha256")
        && receipt.at("qt_core").at("sha256") == scene.marker.at("qt_core_sha256"), "CHILD_RECEIPT_IDENTITY_MISMATCH");
    Require(True(receipt, "bootstrap_passed") && True(receipt, "original_main_entered") && True(receipt, "original_main_returned")
        && True(receipt, "iat_restored") && True(receipt, "default_format_restored") && True(receipt, "scenario_passed")
        && receipt.at("error") == "" && receipt.at("original_main_return_code") == (invalid ? 1 : 0)
        && child.ExitCode() == (invalid ? 1 : 0), "CHILD_NATURAL_MAIN_RESULT_INVALID");
    const auto& iat = receipt.at("iat");
    Require(iat.at("override_count") == 2 && True(iat, "profile_restored") && True(iat, "known_folder_restored"),
        "CHILD_IAT_RESTORE_FACTS_INVALID");
    for (const auto* label : {"known_folder", "profile_directory"})
        Require(True(iat.at(label), "single_import") && True(iat.at(label), "installed")
            && True(iat.at(label), "protection_restored_after_install"), "CHILD_IAT_FACTS_INVALID");
    ValidatePathFacts(scene, receipt.at("preflight_paths"));
    for (const auto* phase : invalid ? std::vector<const char*>{"skills_before_main", "skills_after_main"}
             : std::vector<const char*>{"skills_before_main", "skills_in_event_loop", "skills_after_main"}) {
        const auto& facts = receipt.at(phase);
        const auto path = CheckedAbsolute(Path(QString::fromUtf8(facts.at("root").get<std::string>().c_str()).toStdWString()));
        Require(path == scene.install / L"skills" && Native::Within(path, scene.root)
            && Native::LockSafeDirectory(path).Succeeded() && True(facts, "portable_branch_markers_present")
            && facts.at("files") == scene.skills, "CHILD_PORTABLE_SKILLS_IDENTITY_INVALID");
    }
    if (invalid) {
        Require(receipt.at("event_loop_observed") == false && receipt.at("parent_ack_accepted") == false
            && receipt.at("close_accepted") == false && receipt.at("about_to_quit") == false, "NEGATIVE_MAIN_EVENT_LOOP_UNEXPECTED");
    } else {
        Require(True(receipt, "event_loop_observed") && True(receipt, "parent_ack_accepted") && True(receipt, "close_accepted")
            && True(receipt, "about_to_quit") && True(receipt.at("main_window"), "unique") && True(receipt.at("main_window"), "visible")
            && True(receipt.at("main_window"), "ui_thread") && True(receipt.at("main_window"), "event_loop_observed"), "MAIN_WINDOW_LIFECYCLE_INVALID");
        ValidatePathFacts(scene, receipt.at("observed_paths"));
        const auto& business = receipt.at("business");
        Require(business.at("automatic_update") == false && business.at("feedback_consent") == false
            && True(business, "feedback_queue_empty") && business.at("ai_config_enabled") == false
            && business.at("mcp_config_enabled") == false && business.at("network_observation") == "business-gates-only",
            "MAIN_BUSINESS_GATES_INVALID");
    }
    const auto& settings = receipt.at("settings");
    for (const auto* label : invalid ? std::vector<const char*>{"bootstrap_explicit", "bootstrap_system"}
             : std::vector<const char*>{"bootstrap_explicit", "bootstrap_system", "observed_default", "observed_explicit"}) {
        const auto& facts = settings.at(label);
        Require(facts.at("format") == "IniFormat" && facts.at("scope") == (std::string_view(label) == "bootstrap_system" ? "SystemScope" : "UserScope"),
            "CHILD_SETTINGS_FORMAT_INVALID");
        const auto path = CheckedAbsolute(Path(QString::fromUtf8(facts.at("file").get<std::string>().c_str()).toStdWString()));
        Require(Native::Within(path, scene.root) && Native::OpenSafeReadFile(path, kReceiptLimit).Succeeded(), "CHILD_SETTINGS_PATH_ESCAPED");
    }
}
Json Inventory(const Scenario& scene) {
    const auto identity = Native::ReadRootIdentity(scene.root);
    Require(identity.Succeeded() && *identity.value == scene.identity, "FINAL_ROOT_IDENTITY_CHANGED");
    Require(HashFile(scene.root / L"sentinel.txt") == Hash(Bytes(kSentinel)), "FINAL_SENTINEL_CHANGED");
    Json inventory{{"directories", Json::array()}, {"files", Json::array()}};
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(scene.root)) {
        Require(++count <= 4096 && Native::Within(entry.path(), scene.root), "FINAL_INVENTORY_BUDGET");
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "FINAL_INVENTORY_REPARSE_POINT");
        const auto relative = entry.path().lexically_relative(scene.root);
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) inventory["directories"].push_back(Utf8(relative));
        else inventory["files"].push_back({{"path", Utf8(relative)}, {"bytes", std::filesystem::file_size(entry.path())}, {"sha256", HashFile(entry.path())}});
    }
    for (const auto& [file, hash] : scene.files)
        Require(HashFile(scene.install / Path(std::wstring(file.begin(), file.end()))) == hash, "INSTALLED_RUNTIME_CHANGED");
    for (const auto& [file, identity] : scene.skills.items()) {
        const auto path = scene.install / L"skills" / Path(std::wstring(file.begin(), file.end()));
        Require(HashFile(path) == identity.at("sha256").get<std::string>()
            && std::filesystem::file_size(path) == identity.at("bytes").get<std::uint64_t>(), "INSTALLED_PORTABLE_SKILL_CHANGED");
    }
    const auto marker = StrictObject(ReadFile(scene.root / L"ownership.json", kReceiptLimit), kReceiptLimit);
    Require(marker == scene.marker, "FINAL_OWNERSHIP_MARKER_CHANGED");
    inventory["scope"] = "before-parent-receipt-create-new";
    inventory["sentinel_unchanged"] = true; inventory["runtime_unchanged"] = true; inventory["skills_unchanged"] = true;
    return inventory;
}

Json Run(const Path& launcher, const Path& runtime, const Path& skillsSource, const std::string& name) {
    auto scene = Prepare(launcher, runtime, skillsSource, name);
    Child child;
    Json report{{"format", "audastra-product-main-health-parent"}, {"scenario", name}, {"fixture_root", Utf8(scene.root)},
        {"transaction", scene.transaction}, {"challenge", scene.challenge}, {"runtime_files", scene.files},
        {"approved_skills_source", Utf8(skillsSource)}, {"skills_files", scene.skills},
        {"version", std::string(War3Map::BuildInfo::Version)}, {"commit", std::string(War3Map::BuildInfo::Commit)},
        {"child_started", false}, {"health_validated", false}, {"natural_exit", false}, {"passed", false}};
    bool passed = false;
    try {
        Start(scene, child);
        report["child_started"] = true; report["child_pid"] = child.pid; report["child_creation_time_100ns"] = child.creation;
        const auto text = Receive(child);
        report["health_pipe_text"] = text;
        if (name == "valid") {
            report["health"] = ValidateHealth(scene, child, text); report["health_validated"] = true;
            const Json ack{{"format", "audastra-product-main-health-ack"}, {"transaction", scene.transaction}, {"challenge", scene.challenge},
                {"pid", child.pid}, {"creation_time_100ns", child.creation}, {"version", std::string(War3Map::BuildInfo::Version)},
                {"commit", std::string(War3Map::BuildInfo::Commit)}};
            const auto pending = scene.root / L"receipts" / L"health-ack.pending.json";
            const auto published = scene.root / L"receipts" / L"health-ack.json";
            WriteNew(pending, ack.dump() + "\n");
            // 只在完整 flush/关闭后公布名字，避免子计时器碰到正在 CREATE_NEW 写入的半份 ack。
            Require(MoveFileW(pending.c_str(), published.c_str()) != FALSE, "HEALTH_ACK_EXCLUSIVE_PUBLISH_FAILED");
        } else Require(text.empty(), "NEGATIVE_UNEXPECTED_HEALTH_BYTES");
        Require(WaitNatural(child), "NATURAL_EXIT_TIMEOUT_CHILD_RETAINED");
        report["natural_exit"] = true; report["child_exit_code"] = child.ExitCode();
        const auto receipt = StrictObject(ReadFile(scene.root / L"receipts" / L"child.json", kReceiptLimit), kReceiptLimit);
        ValidateChildReceipt(scene, child, receipt); report["child_receipt"] = receipt;
        report["inventory"] = Inventory(scene); passed = true;
    } catch (const std::exception& error) {
        report["error"] = error.what();
        if (child.process.Valid()) {
            report["child_started"] = true; report["child_pid"] = child.pid; report["child_creation_time_100ns"] = child.creation;
            report["child_still_active"] = !child.Finished();
            if (child.Finished()) { report["natural_exit"] = true; report["child_exit_code"] = child.ExitCode(); }
            const auto activeFailure = scene.root / L"receipts" / L"still-active.json";
            if (std::filesystem::exists(activeFailure)) {
                try { report["active_child_failure"] = StrictObject(ReadFile(activeFailure, kReceiptLimit), kReceiptLimit); }
                catch (...) { report["active_child_failure_unreadable"] = true; }
            }
        }
        try { report["inventory"] = Inventory(scene); }
        catch (const std::exception& inventoryError) { report["inventory_error"] = inventoryError.what(); }
    }
    report["passed"] = passed;
    report["retained"] = true; // 所有原创现场保留给 root，无自动目录删除或进程结束。
    const auto path = scene.root / L"receipts" / L"parent.json";
    WriteNew(path, report.dump(2) + "\n");
    std::cout << Json{{"scenario", name}, {"passed", passed}, {"parent_receipt", Utf8(path)},
        {"child_pid", child.pid}, {"child_creation_time_100ns", child.creation},
        {"child_still_active", child.process.Valid() && !child.Finished()}}.dump() << '\n';
    return report;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        Require((argc == 5 || argc == 7) && std::wstring_view(argv[1]) == L"--launcher" && std::wstring_view(argv[3]) == L"--runtime-dir"
            && (argc == 5 || std::wstring_view(argv[5]) == L"--skills-dir"),
            "USAGE_PRODUCT_MAIN_HEALTH_LAUNCHER_RUNTIME_DIR");
        const auto launcher = CheckedAbsolute(Path(argv[2])), runtime = CheckedAbsolute(Path(argv[4]));
        Require(Native::OpenSafeReadFile(launcher, kFileLimit).Succeeded() && Native::LockSafeDirectory(runtime).Succeeded(),
            "PRODUCT_MAIN_HEALTH_INPUT_UNSAFE");
        const auto packagedSkills = runtime / L"skills";
        Path skillsSource;
        const auto packagedFlags = GetFileAttributesW(packagedSkills.c_str());
        if (packagedFlags != INVALID_FILE_ATTRIBUTES) {
            Require((packagedFlags & FILE_ATTRIBUTE_DIRECTORY) && !(packagedFlags & FILE_ATTRIBUTE_REPARSE_POINT),
                "PACKAGED_PORTABLE_SKILLS_UNSAFE");
            skillsSource = CheckedAbsolute(packagedSkills);
        } else {
            const auto error = GetLastError();
            Require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "PACKAGED_PORTABLE_SKILLS_QUERY_FAILED");
            Require(argc == 7, "EXPLICIT_PORTABLE_SKILLS_SOURCE_REQUIRED");
            skillsSource = CheckedAbsolute(Path(argv[6]));
        }
        Require(Native::LockSafeDirectory(skillsSource).Succeeded(), "PORTABLE_SKILLS_INPUT_UNSAFE");
        bool passed = true; unsigned scenarios = 0;
        for (const std::string name : {"valid", "duplicate-parameter"}) {
            const auto report = Run(launcher, runtime, skillsSource, name); ++scenarios;
            passed = passed && True(report, "passed");
            if (report.contains("child_still_active") && True(report, "child_still_active")) break;
        }
        Require(passed && scenarios == 2, "PRODUCT_MAIN_HEALTH_SCENARIOS_NOT_ACCEPTED");
        std::cout << "Product Main health: 2 natural-process scenarios accepted.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Product Main health failed: " << error.what() << '\n';
        return 1;
    }
}
