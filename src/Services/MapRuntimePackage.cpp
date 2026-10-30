#include "War3MapCore/Services/MapRuntimePackage.h"
#include "War3MapCore/Services/MapRuntimeCatalog.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/VerifiedFile.h"
#include "MapRuntimeJson.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <Windows.h>

namespace War3Map::Services {
namespace {
using Json = nlohmann::json;
constexpr std::uint64_t kMaximumFile = 16 * 1024 * 1024, kMaximumPackage = 32 * 1024 * 1024;
bool Digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Identifier(std::string_view value) {
    return !value.empty() && value.size() <= 96 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    });
}
bool Keys(const Json& value, std::initializer_list<std::string_view> keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(), [&](auto key) { return value.contains(key); });
}
std::vector<std::byte> Bytes(std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}
Status Fail(std::string code, std::string message) {
    return Status::Failure("MapRuntime", std::move(code), std::move(message));
}
bool SafePath(std::string_view name) {
    if (name.empty() || name.size() > 180 || name.front() == '/' || name.back() == '/') return false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i < name.size()) {
            const auto c = name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/')) return false;
            if (c != '/') continue;
        }
        const auto part = name.substr(start, i - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.') return false;
        const auto stem = part.substr(0, part.find('.'));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul"
            || (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt"))
                && stem[3] >= '1' && stem[3] <= '9')) return false;
        start = i + 1;
    }
    return true;
}
bool PlainPath(const std::filesystem::path& path) {
    auto current = path;
    while (!current.empty()) {
        const auto attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    return true;
}
bool LocalRoot(const std::filesystem::path& root) {
    const auto drive = root.root_name().wstring();
    return root.is_absolute() && root == root.lexically_normal()
        && drive.size() == 2 && drive[1] == L':'
        && ((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z'))
        && PlainPath(root) && (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_DIRECTORY);
}
}

Result<std::vector<MapRuntimePackageSource>> LoadInstalledMapRuntimeSources(
    const std::filesystem::path& installationRoot) {
    using Output = Result<std::vector<MapRuntimePackageSource>>;
    const auto fail = [] { return Output::Failure("MapRuntime", "RUNTIME_CATALOG_INVALID",
        "安装侧运行库目录无效或不可读取；未采用地图提供的路径或自动选择其他包。"); };
    if (!LocalRoot(installationRoot)) return fail();
    const auto directory = installationRoot / "runtime";
    const auto directoryAttributes = GetFileAttributesW(directory.c_str());
    if (directoryAttributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Output::Success({}) : fail();
    }
    if (!(directoryAttributes & FILE_ATTRIBUTE_DIRECTORY) || !PlainPath(directory)) return fail();
    const auto catalog = directory / "catalog.json";
    const auto attributes = GetFileAttributesW(catalog.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? Output::Success({}) : fail();
    }
    if (!PlainPath(catalog)) return fail();
    const auto first = IO::ReadFileBytes(catalog, 32 * 1024);
    if (!first.Succeeded()) return fail();
    const auto identity = Sha256(*first.value);
    if (!identity.Succeeded()) return fail();
    const auto frozen = ReadVerifiedFile(catalog, *identity.value, 32 * 1024);
    if (!frozen.Succeeded() || !PlainPath(catalog)) return fail();
    try {
        const auto json = detail::ParseRuntimeJson(*frozen.value, 32 * 1024);
        if (!Keys(json, {"schema", "packages"}) || !json.at("schema").is_number_unsigned()
            || json.at("schema") != 1 || !json.at("packages").is_array()
            || json.at("packages").size() > 16) return fail();
        std::set<std::string> directories, identities;
        std::vector<MapRuntimePackageSource> sources;
        for (const auto& item : json.at("packages")) {
            if (!Keys(item, {"directory", "manifestSha256", "publicDistributionApproved"})) return fail();
            const auto name = item.at("directory").get<std::string>();
            const auto digest = item.at("manifestSha256").get<std::string>();
            if (!Identifier(name) || !SafePath(name) || !Digest(digest)
                || !item.at("publicDistributionApproved").is_boolean()
                || !directories.insert(name).second || !identities.insert(digest).second) return fail();
            sources.push_back({directory / "packages" / name, digest, item.at("publicDistributionApproved").get<bool>()});
        }
        return Output::Success(std::move(sources));
    } catch (const std::exception&) { return fail(); }
}

Result<std::vector<MapRuntimePackageInfo>> QueryMapRuntimePackages(std::span<const MapRuntimePackageSource> sources) {
    using Output = Result<std::vector<MapRuntimePackageInfo>>;
    if (sources.size() > 16) return Output::Failure("MapRuntime", "RUNTIME_CATALOG_INVALID", "运行库包目录超过16项预算。");
    std::set<std::string> identities;
    for (const auto& source : sources) if (!Digest(source.manifestSha256) || !identities.insert(source.manifestSha256).second)
        return Output::Failure("MapRuntime", "RUNTIME_PACKAGE_CONFLICT", "可信目录存在无效或重复包身份。");
    std::vector<MapRuntimePackageInfo> result;
    for (const auto& source : sources) {
        MapRuntimePackageInfo info; info.packageSha256 = source.manifestSha256;
        const auto package = MapRuntimePackage::Capture(source, MapRuntimeSavePurpose::InternalValidation);
        if (package.Succeeded()) {
            const auto& value = **package.value;
            info.id = value.Id(); info.version = value.Version(); info.payloadSha256 = value.PayloadSha256();
            info.internalOnly = value.InternalOnly(); info.available = true;
            info.modulePayloadSha256 = value.ModulePayloadSha256();
        } else info.diagnostics = package.diagnostics;
        result.push_back(std::move(info));
    }
    return Output::Success(std::move(result));
}

Result<MapRuntimeSettings> ParseMapRuntimeSettings(std::span<const std::byte> bytes) {
    using Output = Result<MapRuntimeSettings>;
    if (bytes.size() > 4096 || bytes.empty()) return Output::Failure("MapRuntime", "RUNTIME_SETTINGS_INVALID", "运行库地图设置为空或超过4KiB。");
    try {
        const auto json = detail::ParseRuntimeJson(bytes, 4096);
        if (!Keys(json, {"schema", "mode", "packageSha256"}) || !json.at("schema").is_number_unsigned()
            || json.at("schema") != 1) throw std::runtime_error("schema");
        MapRuntimeSettings result;
        const auto mode = json.at("mode").get<std::string>();
        if (mode == "automatic") result.mode = MapRuntimeMode::Automatic;
        else if (mode == "enabled") result.mode = MapRuntimeMode::Enabled;
        else if (mode == "disabled") result.mode = MapRuntimeMode::Disabled;
        else throw std::runtime_error("mode");
        result.packageSha256 = json.at("packageSha256").get<std::string>();
        if (!result.packageSha256.empty() && !Digest(result.packageSha256)) throw std::runtime_error("digest");
        return Output::Success(std::move(result));
    } catch (const std::exception&) {
        return Output::Failure("MapRuntime", "RUNTIME_SETTINGS_INVALID", "运行库设置须为schema=1、明确mode及小写packageSha256；不接受路径、未知字段或重复键。");
    }
}

Result<std::vector<std::byte>> SerializeMapRuntimeSettings(const MapRuntimeSettings& settings) {
    std::string mode;
    switch (settings.mode) {
    case MapRuntimeMode::Automatic: mode = "automatic"; break;
    case MapRuntimeMode::Enabled: mode = "enabled"; break;
    case MapRuntimeMode::Disabled: mode = "disabled"; break;
    default: return Result<std::vector<std::byte>>::Failure("MapRuntime", "RUNTIME_SETTINGS_INVALID", "未知运行库模式。");
    }
    if (!settings.packageSha256.empty() && !Digest(settings.packageSha256))
        return Result<std::vector<std::byte>>::Failure("MapRuntime", "RUNTIME_SETTINGS_INVALID", "包身份须为小写SHA-256。");
    return Result<std::vector<std::byte>>::Success(Bytes(Json{{"schema", 1}, {"mode", mode}, {"packageSha256", settings.packageSha256}}.dump()));
}

Result<std::shared_ptr<const MapRuntimePackage>> MapRuntimePackage::Capture(
    const MapRuntimePackageSource& source, MapRuntimeSavePurpose purpose) {
    using Output = Result<std::shared_ptr<const MapRuntimePackage>>;
    const auto fail = [](std::string code, std::string message) { return Output::Failure("MapRuntime", std::move(code), std::move(message)); };
    if (purpose != MapRuntimeSavePurpose::Normal && purpose != MapRuntimeSavePurpose::InternalValidation
        && purpose != MapRuntimeSavePurpose::TestRun)
        return fail("RUNTIME_SAVE_PURPOSE_INVALID", "未知保存用途不能授予内部包使用资格。");
    if (!LocalRoot(source.root) || !Digest(source.manifestSha256))
        return fail("RUNTIME_PACKAGE_SOURCE_INVALID", "可信包须使用本机盘符普通目录和已批准的清单SHA-256，不接受设备、网络或重解析路径。");
    const auto manifest = source.root / "manifest.json";
    if (!PlainPath(manifest)) return fail("RUNTIME_PACKAGE_MISSING", "运行库包缺少普通manifest.json。");
    const auto captured = ReadVerifiedFile(manifest, source.manifestSha256, 64 * 1024);
    if (!captured.Succeeded() || !captured.value) return fail("RUNTIME_HASH_MISMATCH", "运行库包清单缺失、不可读取或摘要不符。");
    try {
        auto result = std::shared_ptr<MapRuntimePackage>(new MapRuntimePackage);
        result->source_ = source;
        const auto json = detail::ParseRuntimeJson(*captured.value, 64 * 1024);
        if (!Keys(json, {"schema", "id", "version", "abi", "gameSha256", "scope", "files"})
            || !json.at("schema").is_number_unsigned() || (json.at("schema") != 1 && json.at("schema") != 2)
            || !json.at("abi").is_number_unsigned() || json.at("abi") != 0x20000
            || json.at("gameSha256") != "e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a")
            throw std::runtime_error("schema");
        result->id_ = json.at("id").get<std::string>(); result->version_ = json.at("version").get<std::string>();
        if (!Identifier(result->id_) || !Identifier(result->version_)) throw std::runtime_error("id");
        const auto scope = json.at("scope").get<std::string>();
        if (scope != "internal" && scope != "public") throw std::runtime_error("scope");
        result->internalOnly_ = scope == "internal";
        if ((result->internalOnly_ && purpose == MapRuntimeSavePurpose::Normal)
            || (!result->internalOnly_ && !source.publicDistributionApproved))
            return fail("RUNTIME_DISTRIBUTION_NOT_APPROVED", "此包仅批准内部专用验收，不能用于普通地图交付或公开分发。");
        const auto& files = json.at("files");
        if (!files.is_array() || files.size() < 4 || files.size() > 16) throw std::runtime_error("files");
        std::set<std::string> paths, members, roles;
        std::uint64_t total = 0;
        for (const auto& file : files) {
            if (!Keys(file, {"path", "member", "size", "sha256", "role"})) throw std::runtime_error("file keys");
            const auto path = file.at("path").get<std::string>(), member = file.at("member").get<std::string>();
            const auto hash = file.at("sha256").get<std::string>(), role = file.at("role").get<std::string>();
            if (!file.at("size").is_number_unsigned()) throw std::runtime_error("size");
            const auto size = file.at("size").get<std::uint64_t>();
            const bool client = role == "client" && member.empty() && path.ends_with(".j");
            if (!SafePath(path) || (!client && !SafePath(member)) || !Digest(hash) || !size || size > kMaximumFile
                || size > kMaximumPackage - total || !paths.insert(path).second || (!client && !members.insert(member).second))
                throw std::runtime_error("path or budget");
            const bool bootstrap = role == "bootstrap" && member == "warvk_loader_raw.ai";
            const bool payload = role == "payload" && member == "warvk_runtime.blp";
            const bool module = json.at("schema") == 2 && role == "module" && member == "warvk_runtime_module.dll";
            const bool notice = role == "notice" && member.starts_with("audastra/runtime/licenses/")
                && member.ends_with(".txt");
            if ((!bootstrap && !payload && !module && !notice && !client) || (!notice && !roles.insert(role).second)) throw std::runtime_error("role");
            const auto disk = source.root / std::filesystem::path(path);
            if (!PlainPath(disk)) return fail("RUNTIME_PACKAGE_MISSING", "运行库文件缺失或含重解析路径：" + path);
            auto bytes = ReadVerifiedFile(disk, hash, size);
            if (!bytes.Succeeded() || !bytes.value || bytes.value->size() != size)
                return fail("RUNTIME_HASH_MISMATCH", "运行库文件大小或摘要不符：" + path);
            result->files_.push_back({disk, size, hash});
            if (!client) result->members_.push_back({member, std::move(*bytes.value)});
            if (client) result->clientSha256_ = hash;
            if (payload) result->payloadSha256_ = hash;
            if (module) result->modulePayloadSha256_ = hash;
            total += size;
        }
        if (!roles.contains("bootstrap") || !roles.contains("payload") || !roles.contains("client")) throw std::runtime_error("roles");
        if (json.at("schema") == 2 && !roles.contains("module")) throw std::runtime_error("module missing");
        if (std::none_of(result->members_.begin(), result->members_.end(), [](const auto& m) { return m.name.starts_with("audastra/runtime/licenses/"); }))
            throw std::runtime_error("notice missing");
        const auto current = result->VerifySource();
        if (!current.Succeeded()) { Output failed; failed.diagnostics = current.diagnostics; return failed; }
        return Output::Success(std::move(result));
    } catch (const std::exception&) {
        return fail("RUNTIME_PACKAGE_INVALID", "运行库清单的结构、角色、固定目标、路径或预算无效；未捕获部分包。");
    }
}

Status MapRuntimePackage::VerifySource() const {
    if (!PlainPath(source_.root / "manifest.json")
        || !ReadVerifiedFile(source_.root / "manifest.json", source_.manifestSha256, 64 * 1024).Succeeded())
        return Fail("RUNTIME_SOURCE_CHANGED", "运行库包清单在保存期间变化或不可读取。");
    for (const auto& file : files_) {
        if (!PlainPath(file.path)) return Fail("RUNTIME_SOURCE_CHANGED", "运行库文件来源在保存期间变化。");
        const auto current = ReadVerifiedFile(file.path, file.sha256, file.size);
        if (!current.Succeeded() || !current.value || current.value->size() != file.size)
            return Fail("RUNTIME_SOURCE_CHANGED", "运行库文件身份在保存期间变化。");
    }
    return Status::Ok();
}
Status MapRuntimePackage::VerifyScriptInputs(std::span<const std::string> injectedSourceSha256) const {
    // client 是实际发布的合成注入文件，已经包含JAPI与界面客户端。
    // 身份来自注入器消费的那份原字节，不以之后重读同路径代替。
    if (std::count(injectedSourceSha256.begin(), injectedSourceSha256.end(), clientSha256_) != 1)
        return Fail("RUNTIME_SCRIPT_INPUT_MISMATCH", "注入的 AudastraUI 函数库与当前运行库包不是同一版本（或注入了多份）。请在触发器的“函数库…”里更新自带的 AudastraUI 后再保存。");
    return Status::Ok();
}
} // namespace War3Map::Services
