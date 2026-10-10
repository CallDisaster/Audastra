#include "War3MapCore/Update/UpdateManifest.h"
#include "War3MapCore/Update/SemanticVersion.h"

#include "../Serialization/StrictJson.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Update {
namespace {

using Json = nlohmann::json;

template<typename T>
Result<T> Invalid(std::string code, std::string message) {
    return Result<T>::Failure("Update", std::move(code), std::move(message));
}
bool LowerHex(std::string_view text, std::size_t size) {
    return text.size() == size && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Keys(const Json& object, std::initializer_list<std::string_view> names) {
    if (!object.is_object() || object.size() != names.size()) return false;
    return std::all_of(names.begin(), names.end(), [&](std::string_view key) { return object.contains(key); });
}
bool String(const Json& object, std::string_view key, std::size_t limit, std::string& result) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return false;
    const auto& value = found->get_ref<const std::string&>();
    if (value.size() > limit) return false;
    result = value;
    return true;
}
bool Integer(const Json& object, std::string_view key, std::uint64_t limit, std::uint64_t& result) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_unsigned()) return false;
    result = found->get<std::uint64_t>();
    return result <= limit;
}
std::string CaseKey(std::string_view path) {
    std::string result(path);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

} // namespace

Result<std::string> ValidateUpdateRelativePath(std::string_view path) {
    const auto fail = [] { return Invalid<std::string>("UPDATE_PATH_INVALID",
        "程序文件必须使用安全的ASCII相对路径，不接受路径越界、UNC、设备、ADS或Windows别名。"); };
    if (path.empty() || path.size() > 1024 || path.front() == '/' || path.back() == '/') return fail();
    for (char c : path) {
        if (c < 32 || c > 126 || c == '\\' || c == ':' || c == '<' || c == '>' || c == '"'
            || c == '|' || c == '?' || c == '*') return fail();
    }
    std::size_t offset = 0, components = 0;
    while (offset < path.size()) {
        const auto end = path.find('/', offset);
        const auto part = path.substr(offset, end == std::string_view::npos ? path.size() - offset : end - offset);
        if (part.empty() || part.size() > 255 || part == "." || part == ".." || part.back() == '.'
            || part.back() == ' ' || ++components > 64) return fail();
        auto stem = CaseKey(part.substr(0, part.find('.')));
        while (!stem.empty() && stem.back() == ' ') stem.pop_back();
        static const std::set<std::string> reserved{"con", "prn", "aux", "nul", "clock$", "conin$", "conout$"};
        if (reserved.contains(stem) || (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt"))
            && stem[3] >= '1' && stem[3] <= '9')) return fail();
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return Result<std::string>::Success(std::string(path));
}

Result<UpdateManifest> ParseUpdateManifest(std::span<const std::byte> rawBytes) {
    Serialization::StrictJsonLimits limits;
    limits.maxBytes = kMaxUpdateManifestBytes;
    limits.maxDepth = 8;
    limits.maxNodes = 40000;
    limits.maxStringBytes = 8192;
    limits.rejectFloatingPoint = true;
    auto parsed = Serialization::ParseStrictJson(rawBytes, limits);
    if (!parsed.Succeeded()) return Invalid<UpdateManifest>("UPDATE_MANIFEST_JSON_INVALID",
        "更新清单不是完整的有界UTF-8 JSON，或包含重复字段、非整数与超限结构。");
    try {
        const auto& root = *parsed.value;
        if (!Keys(root, {"format", "schema", "product", "platform", "version", "minimum_direct_upgrade",
                "source_commit", "zip", "release_notes", "files"})) return Invalid<UpdateManifest>(
                    "UPDATE_MANIFEST_SCHEMA_INVALID", "更新清单字段缺失、未知或格式不是schema 1。");
        std::string format, product, platform;
        std::uint64_t schema = 0;
        if (!String(root, "format", 64, format) || format != "audastra-update"
            || !String(root, "product", 64, product) || product != "Audastra"
            || !String(root, "platform", 64, platform) || platform != "windows-x64"
            || !Integer(root, "schema", 1, schema) || schema != 1) return Invalid<UpdateManifest>(
                "UPDATE_MANIFEST_IDENTITY_INVALID", "更新清单的产品、平台或schema身份不匹配。");
        UpdateManifest manifest;
        if (!String(root, "version", 128, manifest.version)
            || !String(root, "minimum_direct_upgrade", 128, manifest.minimumDirectUpgrade)
            || !String(root, "source_commit", 40, manifest.sourceCommit) || !LowerHex(manifest.sourceCommit, 40)
            || !String(root, "release_notes", 8192, manifest.releaseNotes)) return Invalid<UpdateManifest>(
                "UPDATE_MANIFEST_FIELD_INVALID", "更新清单的版本、来源提交或发布摘要无效。");
        const auto version = ParseSemanticVersion(manifest.version);
        const auto minimum = ParseSemanticVersion(manifest.minimumDirectUpgrade);
        if (!version.Succeeded() || !minimum.Succeeded()
            || CompareSemanticVersions(*minimum.value, *version.value) > 0) return Invalid<UpdateManifest>(
                "UPDATE_MANIFEST_VERSION_INVALID", "更新版本或最低直接升级版本不是有效且有序的SemVer。");
        const auto& package = root.at("zip");
        if (!Keys(package, {"filename", "bytes", "sha256"})
            || !String(package, "filename", 255, manifest.packageFilename)
            || manifest.packageFilename.find('/') != std::string::npos
            || !manifest.packageFilename.ends_with(".zip")
            || !ValidateUpdateRelativePath(manifest.packageFilename).Succeeded()
            || !Integer(package, "bytes", kMaxUpdatePackageBytes, manifest.packageBytes) || manifest.packageBytes == 0
            || !String(package, "sha256", 64, manifest.packageSha256) || !LowerHex(manifest.packageSha256, 64)) {
            return Invalid<UpdateManifest>("UPDATE_MANIFEST_PACKAGE_INVALID", "更新ZIP的文件名、容量或SHA-256无效。");
        }
        const auto& files = root.at("files");
        if (!files.is_array() || files.empty() || files.size() > kMaxUpdateFiles) return Invalid<UpdateManifest>(
            "UPDATE_MANIFEST_FILES_INVALID", "程序文件清单为空、类型错误或超过4096项。");
        std::unordered_set<std::string> names;
        std::unordered_map<std::string, std::string> prefixSpellings;
        std::uint64_t total = 0;
        manifest.files.reserve(files.size());
        for (const auto& file : files) {
            UpdateFileEntry entry;
            if (!Keys(file, {"path", "bytes", "sha256"})
                || !String(file, "path", 1024, entry.path) || !ValidateUpdateRelativePath(entry.path).Succeeded()
                || !Integer(file, "bytes", kMaxUpdateFileBytes, entry.bytes)
                || !String(file, "sha256", 64, entry.sha256) || !LowerHex(entry.sha256, 64)) {
                return Invalid<UpdateManifest>("UPDATE_MANIFEST_FILE_INVALID", "程序文件路径、容量或SHA-256无效。");
            }
            if (!names.insert(CaseKey(entry.path)).second) return Invalid<UpdateManifest>(
                "UPDATE_MANIFEST_PATH_COLLISION", "程序文件清单包含重复路径或Windows大小写碰撞。");
            for (auto end = entry.path.find('/'); end != std::string::npos; end = entry.path.find('/', end + 1)) {
                const auto prefix = entry.path.substr(0, end);
                const auto [existing, added] = prefixSpellings.emplace(CaseKey(prefix), prefix);
                if (!added && existing->second != prefix) return Invalid<UpdateManifest>(
                    "UPDATE_MANIFEST_PATH_COLLISION", "程序文件父目录包含不同大小写的Windows别名。");
            }
            if (entry.bytes > kMaxUpdateExpandedBytes - total) return Invalid<UpdateManifest>(
                "UPDATE_MANIFEST_EXPANDED_BUDGET", "程序文件解压后的累计容量超过8GiB。");
            total += entry.bytes;
            manifest.files.push_back(std::move(entry));
        }
        for (const auto& name : names) {
            for (auto end = name.find('/'); end != std::string::npos; end = name.find('/', end + 1)) {
                if (names.contains(name.substr(0, end))) return Invalid<UpdateManifest>(
                    "UPDATE_MANIFEST_PATH_COLLISION", "程序文件与另一成员的父目录发生冲突。");
            }
        }
        return Result<UpdateManifest>::Success(std::move(manifest));
    } catch (const std::exception&) {
        return Invalid<UpdateManifest>("UPDATE_MANIFEST_SCHEMA_INVALID", "更新清单字段无法完整检查，未返回部分清单。");
    }
}

Result<UpdateCheckResult> CheckUpdate(const VerifiedUpdateManifest& verified, std::string_view currentVersion,
    UpdateChannel channel) {
    const auto current = ParseSemanticVersion(currentVersion);
    if (!current.Succeeded()) return Invalid<UpdateCheckResult>("UPDATE_CURRENT_VERSION_INVALID", "当前程序版本不是有效SemVer。");
    const auto& manifest = verified.Manifest();
    const auto target = ParseSemanticVersion(manifest.version);
    const auto minimum = ParseSemanticVersion(manifest.minimumDirectUpgrade);
    if (!target.Succeeded() || !minimum.Succeeded()) return Invalid<UpdateCheckResult>(
        "UPDATE_MANIFEST_VERSION_INVALID", "已验证清单的版本身份无效。");
    UpdateCheckResult result{std::string(currentVersion), manifest.version};
    if (channel == UpdateChannel::Stable && target.value->HasPrerelease()) {
        result.reason = "稳定渠道忽略预发布版本。";
    } else if (CompareSemanticVersions(*target.value, *current.value) <= 0) {
        result.reason = "当前没有优先级更高的版本。";
    } else {
        result.updateAvailable = true;
        result.directlyUpgradeable = CompareSemanticVersions(*current.value, *minimum.value) >= 0;
        result.reason = result.directlyUpgradeable ? "有可直接升级的新版本。" : "有新版本，但当前版本低于最低直接升级版本。";
    }
    return Result<UpdateCheckResult>::Success(std::move(result));
}

} // namespace War3Map::Update
