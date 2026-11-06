#include "War3MapCore/Services/LuaFolder.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqArchive.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace War3Map::Services {
namespace {
using Json = nlohmann::json;
std::string Key(std::string name) {
    for (auto& c : name) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return name;
}
std::string Utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
Result<std::optional<std::vector<std::byte>>> Member(const MapArchiveSession& archive, std::string_view path,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) {
    const auto key = Key(std::string(path));
    if (std::any_of(removals.begin(), removals.end(), [&](const auto& item) { return Key(item) == key; }))
        return Result<std::optional<std::vector<std::byte>>>::Success(std::nullopt);
    const MemberReplacement* replacement = nullptr;
    for (const auto& item : replacements) if (Key(item.name) == key) {
        if (replacement) return Result<std::optional<std::vector<std::byte>>>::Failure("LuaFolder", "LUA_FOLDER_DUPLICATE", "Lua来源包含重复替换成员：" + std::string(path));
        replacement = &item;
    }
    if (replacement) return Result<std::optional<std::vector<std::byte>>>::Success(replacement->data);
    if (!archive.HasMember(path)) return Result<std::optional<std::vector<std::byte>>>::Success(std::nullopt);
    const auto read = archive.ReadMember(path);
    if (!read.Succeeded()) return {std::nullopt, read.diagnostics};
    return Result<std::optional<std::vector<std::byte>>>::Success(*read.value);
}
}

Result<std::optional<std::filesystem::path>> DecodeLuaFolderLink(std::span<const std::byte> metadata,
    const std::filesystem::path& map) {
    using Output = Result<std::optional<std::filesystem::path>>;
    try {
        if (metadata.empty() || metadata.size() > 8192) throw std::runtime_error("metadata budget");
        std::vector<std::set<std::string>> keys;
        const auto* text = reinterpret_cast<const char*>(metadata.data());
        const auto parsed = Json::parse(text, text + metadata.size(), [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 2) throw std::runtime_error("metadata depth");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key && (!keys.size() || !keys.back().insert(value.get<std::string>()).second))
                throw std::runtime_error("duplicate metadata key");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
        if (!parsed.is_object() || parsed.size() != 2 || !parsed.contains("schema") || !parsed["schema"].is_number_integer()
            || parsed["schema"] != 1 || !parsed.contains("path") || !parsed["path"].is_string()) throw std::runtime_error("metadata schema");
        const auto value = parsed["path"].get<std::string>();
        if (value.empty() || value.size() > 4096 || value.find('\0') != value.npos) throw std::runtime_error("metadata path");
        auto path = std::filesystem::u8path(value);
        if (path.has_root_name() && !path.is_absolute()) throw std::runtime_error("drive relative path");
        if (path.is_relative() && map.empty()) throw std::runtime_error("relative path without map anchor");
        if (path.is_relative()) path = std::filesystem::absolute(map).parent_path() / path;
        path = path.lexically_normal();
        if (path.root_name().wstring().size() != 2 || path.root_name().wstring()[1] != L':') throw std::runtime_error("local path required");
        return Output::Success(std::move(path));
    } catch (...) {
        return Output::Failure("LuaFolder", "LUA_FOLDER_METADATA_INVALID", "Lua外部文件夹关联信息无效，未访问所指路径。");
    }
}

Result<std::vector<std::byte>> EncodeLuaFolderLink(const std::filesystem::path& folder, const std::filesystem::path& map) {
    try {
        if (map.empty() || folder.empty()) throw std::runtime_error("missing path anchor");
        const auto root = std::filesystem::absolute(folder).lexically_normal();
        const auto parent = std::filesystem::absolute(map).parent_path().lexically_normal();
        auto value = root;
        if (Key(Utf8(root.root_name())) == Key(Utf8(parent.root_name()))) {
            // 盘符大小写不能使lexically_relative失败后被误写成地图所在目录。
            const auto sameDriveRoot = parent.root_name() / root.root_directory() / root.relative_path();
            value = sameDriveRoot.lexically_relative(parent);
            if (value.empty()) throw std::runtime_error("cannot form relative path");
        }
        const auto text = Json{{"schema", 1}, {"path", Utf8(value)}}.dump();
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        auto decoded = DecodeLuaFolderLink(std::span(bytes, text.size()), map);
        if (!decoded.Succeeded()) return {std::nullopt, decoded.diagnostics};
        if (!*decoded.value || Key(Utf8(**decoded.value)) != Key(Utf8(root))) throw std::runtime_error("path round trip changed target");
        return Result<std::vector<std::byte>>::Success({bytes, bytes + text.size()});
    } catch (...) { return Result<std::vector<std::byte>>::Failure("LuaFolder", "LUA_FOLDER_METADATA_INVALID", "无法编码Lua文件夹关联路径。"); }
}

Result<std::optional<std::filesystem::path>> ReadLuaFolderLink(const MapArchiveSession& archive,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) {
    auto member = Member(archive, kLuaFolderMember, replacements, removals);
    if (!member.Succeeded()) return {std::nullopt, member.diagnostics};
    if (!*member.value) return Result<std::optional<std::filesystem::path>>::Success(std::nullopt);
    return DecodeLuaFolderLink(**member.value, archive.SourcePath());
}

Result<std::vector<LuaFolderFile>> CaptureMapLuaFiles(const MapArchiveSession& archive,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) {
    using Output = Result<std::vector<LuaFolderFile>>;
    std::map<std::string, std::string> names;
    for (const auto& file : archive.Members()) if (IsAuthorLuaMember(file.name)) {
        if (!names.emplace(Key(file.name), file.name).second) return Output::Failure("LuaFolder", "LUA_FOLDER_DUPLICATE", "地图Lua成员存在大小写歧义：" + file.name);
    }
    for (const auto& file : replacements) if (IsAuthorLuaMember(file.name)) names[Key(file.name)] = file.name;
    std::vector<LuaFolderFile> result;
    std::size_t total = 0;
    for (const auto& [key, name] : names) {
        auto read = Member(archive, name, replacements, removals);
        if (!read.Succeeded()) return {std::nullopt, read.diagnostics};
        if (!*read.value) continue;
        auto bytes = std::move(**read.value);
        const auto valid = ValidateLuaFolderFile(name, bytes, false);
        if (!valid.Succeeded()) return {std::nullopt, valid.diagnostics};
        total += bytes.size();
        if (result.size() == kLuaFolderFiles || total > kLuaFolderTotalBytes)
            return Output::Failure("LuaFolder", "LUA_FOLDER_TOTAL_BUDGET", name + "：地图Lua超过4096文件或32 MiB。");
        const auto sha = Sha256(bytes);
        if (!sha.Succeeded()) return {std::nullopt, sha.diagnostics};
        result.push_back({name, *sha.value, std::make_shared<const std::vector<std::byte>>(std::move(bytes))});
    }
    return Output::Success(std::move(result));
}

Result<std::optional<std::filesystem::path>> ReadLuaFolderLinkFile(const std::filesystem::path& path) {
    const auto map = Mpq::MpqArchive::OpenReadOnly(path);
    if (!map.Succeeded()) return {std::nullopt, map.diagnostics};
    if (!map.value->HasFile(kLuaFolderMember)) return Result<std::optional<std::filesystem::path>>::Success(std::nullopt);
    const auto bytes = map.value->ReadFileBounded(kLuaFolderMember, 8192);
    return bytes.Succeeded() ? DecodeLuaFolderLink(*bytes.value, path)
        : Result<std::optional<std::filesystem::path>>{std::nullopt, bytes.diagnostics};
}

Result<std::vector<LuaFolderFile>> CaptureMapLuaFiles(const std::filesystem::path& path) {
    using Output = Result<std::vector<LuaFolderFile>>;
    const auto map = Mpq::MpqArchive::OpenReadOnly(path);
    if (!map.Succeeded()) return {std::nullopt, map.diagnostics};
    const auto names = map.value->ListFilesStrict();
    if (!names.Succeeded()) return {std::nullopt, names.diagnostics};
    std::vector<LuaFolderFile> result;
    std::size_t total = 0;
    for (const auto& file : *names.value) {
        if (!IsAuthorLuaMember(file.name)) continue;
        const auto bytes = map.value->ReadFileBounded(file.name, 2u * 1024u * 1024u);
        if (!bytes.Succeeded()) return {std::nullopt, bytes.diagnostics};
        const auto valid = ValidateLuaFolderFile(file.name, *bytes.value, false);
        if (!valid.Succeeded()) return {std::nullopt, valid.diagnostics};
        total += bytes.value->size();
        if (result.size() == kLuaFolderFiles || total > kLuaFolderTotalBytes)
            return Output::Failure("LuaFolder", "LUA_FOLDER_TOTAL_BUDGET", file.name + "：地图Lua超过4096文件或32 MiB。");
        const auto hash = Sha256(*bytes.value);
        if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
        result.push_back({file.name, *hash.value, std::make_shared<const std::vector<std::byte>>(*bytes.value)});
    }
    return Output::Success(std::move(result));
}
} // namespace War3Map::Services
