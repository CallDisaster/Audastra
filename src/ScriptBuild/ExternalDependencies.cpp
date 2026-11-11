#include "War3MapCore/Services/ExternalDependencies.h"

#include "RuntimePeInspection.h"
#include "../Core/Serialization/StrictJson.h"
#include "../WarcraftRuntime32/OwnLua/NativeSignature.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace Native = Audastra::Runtime32::OwnLua;
constexpr std::size_t kEntryLimit = 512;
constexpr std::size_t kManifestLimit = 128 * 1024;
constexpr std::size_t kDllLimit = 32 * 1024 * 1024;
constexpr std::size_t kTotalDllLimit = 256 * 1024 * 1024;

template<class T> Result<T> Fail(std::string code, std::string message) {
    return Result<T>::Failure("Map.Test.ExternalDependencies", std::move(code), std::move(message));
}
std::string Lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
bool SafeLocalPath(const fs::path& path) {
    const auto drive = path.root_name().native();
    if (!path.is_absolute() || drive.size() != 2 || drive[1] != L':' || path.native().size() >= 4096)
        return false;
    for (const auto& part : path.relative_path()) {
        const auto& value = part.native();
        if (value.empty() || value.back() == L'.' || value.back() == L' ' ||
            value.find_first_of(L":\"|?*<>") != std::wstring::npos ||
            std::any_of(value.begin(), value.end(), [](wchar_t ch) { return ch < 0x20; })) return false;
    }
    return true;
}
bool NoReparse(const fs::path& path) {
    for (auto current = path; !current.empty();) {
        const auto attrs = GetFileAttributesW(current.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        } else if (const auto error = GetLastError(); error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return true;
}
bool LeafDll(std::string_view name) {
    if (name.size() < 5 || name.size() > 80 || !Lower(std::string(name)).ends_with(".dll")) return false;
    const auto stem = name.substr(0, name.size() - 4);
    const auto lower = Lower(std::string(stem));
    if (lower == "con" || lower == "prn" || lower == "nul" || lower == "aux" ||
        (lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) &&
            lower.back() >= '0' && lower.back() <= '9')) return false;
    return std::all_of(stem.begin(), stem.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
bool SystemImport(std::string_view name) {
    if (!LeafDll(name)) return false;
    if (name.starts_with("api-ms-win-") || name.starts_with("ext-ms-win-")) return true;
    std::array<wchar_t, MAX_PATH> dir{};
    auto length = GetSystemWow64DirectoryW(dir.data(), static_cast<UINT>(dir.size()));
    if (!length) length = GetSystemDirectoryW(dir.data(), static_cast<UINT>(dir.size()));
    if (!length || length >= dir.size()) return false;
    const auto attrs = GetFileAttributesW((fs::path(dir.data()) / std::string(name)).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
void Problem(ExternalDependencyProvider& provider, std::string code, std::string message) {
    provider.diagnostics.push_back({DiagnosticSeverity::Error, "Map.Test.ExternalDependencies",
        std::move(code), std::move(message)});
}
bool ExactKeys(const Json& value, std::initializer_list<std::string_view> keys) {
    return value.is_object() && value.size() == keys.size() &&
        std::all_of(keys.begin(), keys.end(), [&](auto key) { return value.contains(key); });
}
Json ReadDescriptor(const fs::path& path) {
    if (!NoReparse(path) || !fs::is_regular_file(path)) throw std::runtime_error("manifest path");
    const auto bytes = IO::ReadFileBytes(path, kManifestLimit);
    if (!bytes.Succeeded()) throw std::runtime_error("manifest read");
    Serialization::StrictJsonLimits limits;
    limits.maxBytes = kManifestLimit;
    limits.maxDepth = 8;
    limits.maxNodes = 4096;
    limits.maxStringBytes = 1024;
    limits.rejectFloatingPoint = true;
    auto descriptor = Serialization::ParseStrictJson(*bytes.value, limits);
    if (!descriptor.Succeeded()) throw std::runtime_error("manifest JSON");
    return std::move(*descriptor.value);
}

ExternalDependencyProvider InspectProvider(const fs::path& manifest, std::size_t& remaining) {
    ExternalDependencyProvider provider;
    provider.manifest = manifest;
    try {
        const auto descriptor = ReadDescriptor(manifest);
        if (!ExactKeys(descriptor, {"schema", "id", "abi", "modules"}) || descriptor.at("schema") != 1
            || descriptor.at("abi") != "audastra-native-provider-v1")
            throw std::runtime_error("schema");
        provider.id = descriptor.at("id").get<std::string>();
        if (!Native::NativeIdentifier(provider.id)) throw std::runtime_error("id");
        const auto& modules = descriptor.at("modules");
        if (!modules.is_array() || modules.empty() || modules.size() > kExternalDependencyModuleLimit)
            throw std::runtime_error("module count");
        std::set<std::string> earlier, natives;
        for (const auto& module : modules) {
            if (!ExactKeys(module, {"file", "entry", "natives"})) throw std::runtime_error("module keys");
            const auto filename = module.at("file").get<std::string>();
            const auto key = Lower(filename);
            if (!LeafDll(filename) || earlier.contains(key) || SystemImport(key) || key == "game.dll")
                throw std::runtime_error("module name");
            auto& file = provider.files.emplace_back();
            file.path = manifest.parent_path() / filename;
            file.entry = module.at("entry").get<std::string>();
            const auto& declared = module.at("natives");
            if (!declared.is_object() || declared.empty() != file.entry.empty() ||
                (!file.entry.empty() && !Native::NativeIdentifier(file.entry))) throw std::runtime_error("entry or natives");
            for (const auto& [name, value] : declared.items()) {
                const auto signature = value.get<std::string>();
                if (!Native::NativeIdentifier(name) || !Native::ParseNativeSignature(signature) ||
                    !natives.insert(name).second || natives.size() > kExternalDependencyNativeLimit)
                    throw std::runtime_error("native signature or count");
                file.natives.emplace(name, signature);
            }
            std::error_code ec;
            const auto size = NoReparse(file.path) && fs::is_regular_file(file.path, ec) ? fs::file_size(file.path, ec) : 0;
            if (!size || ec || size > kDllLimit || size > remaining) {
                Problem(provider, "EXTERNAL_DEPENDENCY_FILE", "文件缺失、经重解析路径或超出预算：" + IO::PathUtf8(file.path));
                earlier.insert(key);
                continue;
            }
            const auto bytes = IO::ReadFileBytes(file.path, static_cast<std::size_t>(size));
            if (!bytes.Succeeded() || bytes.value->size() != size) {
                Problem(provider, "EXTERNAL_DEPENDENCY_FILE", "无法读取 DLL：" + IO::PathUtf8(file.path));
                earlier.insert(key);
                continue;
            }
            remaining -= bytes.value->size();
            const auto sha = Sha256(*bytes.value);
            if (sha.Succeeded()) file.sha256 = *sha.value;
            const auto pe = Script::InspectExternalDependencyPe(*bytes.value, file.entry);
            file.peValid = pe.Succeeded();
            if (!file.peValid) {
                for (const auto& item : pe.diagnostics)
                    Problem(provider, item.code, filename + "：" + item.message);
            } else {
                file.imports = *pe.value;
                for (const auto& name : file.imports) {
                    if (!earlier.contains(name) && !SystemImport(name))
                        Problem(provider, "EXTERNAL_DEPENDENCY_IMPORT_MISSING",
                            filename + " 需要先声明并加载 " + name + "。");
                }
            }
            if (file.sha256.empty()) Problem(provider, "EXTERNAL_DEPENDENCY_SHA", "无法计算文件身份：" + filename);
            earlier.insert(key);
        }
        if (natives.empty()) throw std::runtime_error("no natives");
        provider.usable = provider.diagnostics.empty();
    } catch (const std::exception&) {
        Problem(provider, "EXTERNAL_DEPENDENCY_MANIFEST", "提供者声明无效：" + IO::PathUtf8(manifest) +
            "。请核对 schema、abi、模块顺序、文件名、入口和 native 签名。");
    }
    return provider;
}
} // namespace

fs::path ExternalDependenciesDirectory(const fs::path& installation, const fs::path& configured) {
    return configured.empty() ? installation / kExternalDependenciesDirectory : configured;
}

Status EnsureExternalDependenciesDirectory(const fs::path& directory) {
    if (!SafeLocalPath(directory) || !NoReparse(directory))
        return Status::Failure("Map.Test.ExternalDependencies", "EXTERNAL_DEPENDENCY_DIRECTORY", "请选择普通的本机绝对目录。");
    std::error_code ec;
    if (fs::is_directory(directory, ec)) return Status::Ok();
    ec.clear();
    if (!fs::create_directories(directory, ec) || ec || !NoReparse(directory))
        return Status::Failure("Map.Test.ExternalDependencies", "EXTERNAL_DEPENDENCY_DIRECTORY", "无法创建外部依赖目录。");
    return Status::Ok();
}

Result<ExternalDependenciesStatus> QueryExternalDependencies(const fs::path& directory) {
    using R = ExternalDependenciesStatus;
    if (!SafeLocalPath(directory) || !NoReparse(directory))
        return Fail<R>("EXTERNAL_DEPENDENCY_DIRECTORY", "外部依赖目录不是普通本机绝对目录。");
    try {
        R status;
        status.directory = directory;
        status.exists = fs::exists(directory);
        if (!status.exists) return Result<R>::Success(std::move(status));
        if (!fs::is_directory(directory))
            return Fail<R>("EXTERNAL_DEPENDENCY_DIRECTORY", "外部依赖路径不是目录。");
        std::size_t remaining = kTotalDllLimit, entries = 0;
        std::vector<fs::path> directories{directory};
        std::set<std::string> described;
        for (std::size_t i = 0; i < directories.size(); ++i) {
            const auto current = directories[i];
            const auto manifest = current / L"provider.json";
            if (fs::exists(manifest)) {
                auto provider = InspectProvider(manifest, remaining);
                for (const auto& file : provider.files) described.insert(Lower(IO::PathUtf8(file.path)));
                status.providers.push_back(std::move(provider));
            }
            for (const auto& item : fs::directory_iterator(current)) {
                if (++entries > kEntryLimit) return Fail<R>("EXTERNAL_DEPENDENCY_BUDGET", "外部依赖目录条目超过 512 项。");
                if (!NoReparse(item.path())) continue;
                if (i == 0 && item.is_directory()) directories.push_back(item.path());
                if (item.is_regular_file() && Lower(IO::PathUtf8(item.path().extension())) == ".dll" &&
                    !described.contains(Lower(IO::PathUtf8(item.path())))) status.undescribedDlls.push_back(item.path());
            }
        }
        std::sort(status.providers.begin(), status.providers.end(), [](const auto& a, const auto& b) { return a.manifest < b.manifest; });
        std::sort(status.undescribedDlls.begin(), status.undescribedDlls.end());
        std::map<std::string, std::size_t, std::less<>> ids;
        for (const auto& provider : status.providers) if (!provider.id.empty()) ++ids[provider.id];
        for (auto& provider : status.providers) if (!provider.id.empty() && ids[provider.id] > 1) {
            provider.usable = false;
            Problem(provider, "EXTERNAL_DEPENDENCY_ID_CONFLICT", "提供者 ID 重复：" + provider.id);
        }
        return Result<R>::Success(std::move(status));
    } catch (const std::exception&) {
        return Fail<R>("EXTERNAL_DEPENDENCY_DIRECTORY", "无法完整枚举外部依赖目录。");
    }
}

Result<std::vector<ExternalDependencyFile>> SelectExternalDependencyFiles(
    const ExternalDependenciesStatus& status, const ExternalNativeRequirements& requirements) {
    using R = std::vector<ExternalDependencyFile>;
    std::set<std::size_t> chosen;
    std::vector<std::string> missing;
    for (const auto& [name, signature] : requirements) {
        const auto expected = signature.empty() ? std::nullopt : Native::ParseNativeSignature(signature);
        std::optional<std::size_t> match;
        for (std::size_t index = 0; index < status.providers.size(); ++index) {
            const auto& provider = status.providers[index];
            if (!provider.usable) continue;
            for (const auto& file : provider.files) {
                const auto found = file.natives.find(name);
                if (found == file.natives.end()) continue;
                if (match) return Fail<R>("EXTERNAL_DEPENDENCY_NATIVE_CONFLICT", "多个提供者声明了 " + name + "。");
                if (!signature.empty() && (!expected || Native::ParseNativeSignature(found->second) != expected))
                    return Fail<R>("EXTERNAL_DEPENDENCY_NATIVE_SIGNATURE",
                        name + " 的地图签名 " + signature + " 与 " + provider.id + " 声明的 " + found->second + " 不符。");
                match = index;
            }
        }
        if (match) chosen.insert(*match);
        else missing.push_back(name);
    }
    if (!missing.empty()) {
        std::string names;
        for (const auto& name : missing) { if (!names.empty()) names += "、"; names += name; }
        std::string message = "外部依赖目录 " + IO::PathUtf8(status.directory) + " 缺少可用声明：" + names + "。";
        for (const auto& provider : status.providers)
            for (const auto& issue : provider.diagnostics)
                if (message.size() < 8192) message += " " + issue.message;
        return Fail<R>("EXTERNAL_DEPENDENCY_NATIVE_MISSING", std::move(message));
    }
    R selected;
    std::set<std::string> modules, natives;
    for (const auto index : chosen) for (const auto& file : status.providers[index].files) {
        if (!modules.insert(Lower(IO::PathUtf8(file.path.filename()))).second)
            return Fail<R>("EXTERNAL_DEPENDENCY_MODULE_CONFLICT", "选中的提供者含同名 DLL。");
        for (const auto& [name, signature] : file.natives)
            if (!natives.insert(name).second)
                return Fail<R>("EXTERNAL_DEPENDENCY_NATIVE_CONFLICT", "选中的提供者重复声明 " + name + "。");
        selected.push_back(file);
    }
    if (selected.size() > kExternalDependencyModuleLimit || natives.size() > kExternalDependencyNativeLimit)
        return Fail<R>("EXTERNAL_DEPENDENCY_BUDGET", "选中的模块或 native 数量超出单次测试预算。");
    return Result<R>::Success(std::move(selected));
}

std::string ExternalDependenciesJson(const ExternalDependenciesStatus& status) {
    Json providers = Json::array(), ignored = Json::array();
    for (const auto& path : status.undescribedDlls) ignored.push_back(IO::PathUtf8(path));
    for (const auto& provider : status.providers) {
        Json files = Json::array(), issues = Json::array();
        for (const auto& file : provider.files)
            files.push_back({{"path", IO::PathUtf8(file.path)}, {"sha256", file.sha256}, {"entry", file.entry},
                {"pe_valid", file.peValid}, {"imports", file.imports}, {"natives", file.natives}});
        for (const auto& issue : provider.diagnostics)
            issues.push_back({{"code", issue.code}, {"message", issue.message}});
        providers.push_back({{"id", provider.id}, {"manifest", IO::PathUtf8(provider.manifest)},
            {"usable", provider.usable}, {"modules", files}, {"issues", issues}});
    }
    return Json{{"directory", IO::PathUtf8(status.directory)}, {"exists", status.exists},
        {"providers", providers}, {"undescribed_dlls", ignored}}.dump();
}
} // namespace War3Map::Services
