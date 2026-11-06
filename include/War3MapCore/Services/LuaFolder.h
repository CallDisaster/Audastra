#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include <memory>

namespace War3Map::Services {

inline constexpr std::string_view kLuaFolderMember = "audastra/editor/lua-folder.json";
inline constexpr std::size_t kLuaFolderFiles = 4096;
inline constexpr std::size_t kLuaFolderTotalBytes = 32u * 1024u * 1024u;

struct LuaFolderFile {
    std::string path;
    std::string sha256;
    std::shared_ptr<const std::vector<std::byte>> bytes;
};
class LuaFolderReadLease;
struct LuaFolderSnapshot {
    std::filesystem::path root;
    std::vector<LuaFolderFile> files;
    std::vector<std::filesystem::path> directories;
    std::string sha256;
    bool missing = false;
    std::size_t skippedLinks = 0;
    // 只有保存快照持有租约；视图快照不阻止外部编辑器保存。
    std::shared_ptr<const LuaFolderReadLease> lease;
};
enum class LuaFolderChoice { Folder, Map, Cancel };
struct LuaFolderDifference {
    std::vector<std::string> mapOnly, folderOnly, different;
};

[[nodiscard]] bool IsAuthorLuaMember(std::string_view path);
[[nodiscard]] Status ValidateLuaFolderFile(std::string_view path, std::span<const std::byte> bytes, bool syntax);
[[nodiscard]] Result<LuaFolderSnapshot> CaptureLuaFolder(const std::filesystem::path& root,
    bool syntax = false, bool holdReadLease = false);
[[nodiscard]] Status VerifyLuaFolder(const LuaFolderSnapshot& snapshot);
[[nodiscard]] LuaFolderDifference CompareLuaFolder(std::span<const LuaFolderFile> map,
    const LuaFolderSnapshot& folder);
[[nodiscard]] Result<std::vector<LuaFolderFile>> CaptureMapLuaFiles(const MapArchiveSession& archive,
    std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {});
// 写盘不进入地图撤销栈。已有文件在同一排他写句柄中核对SHA并写入，不留核对/替换竞态窗口。
[[nodiscard]] Status WriteLuaFolderFile(const std::filesystem::path& root, std::string_view path,
    std::span<const std::byte> bytes, std::string_view expectedSha256);
[[nodiscard]] Status RemoveLuaFolderFile(const std::filesystem::path& root, const LuaFolderFile& expected);
[[nodiscard]] Status RenameLuaFolderFile(const std::filesystem::path& root,
    const LuaFolderFile& expected, std::string_view newPath);
[[nodiscard]] Status ReconcileLuaFolder(const LuaFolderSnapshot& reviewed,
    std::span<const LuaFolderFile> map, LuaFolderChoice choice);

[[nodiscard]] Result<std::optional<std::filesystem::path>> DecodeLuaFolderLink(
    std::span<const std::byte> metadata, const std::filesystem::path& map);
[[nodiscard]] Result<std::vector<std::byte>> EncodeLuaFolderLink(
    const std::filesystem::path& folder, const std::filesystem::path& map);
[[nodiscard]] Result<std::optional<std::filesystem::path>> ReadLuaFolderLink(const MapArchiveSession& archive,
    std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {});
// 只打开固定元数据或显式Lua成员；只读CLI/测试入口不为查询解码其它导入载荷。
[[nodiscard]] Result<std::optional<std::filesystem::path>> ReadLuaFolderLinkFile(const std::filesystem::path& map);
[[nodiscard]] Result<std::vector<LuaFolderFile>> CaptureMapLuaFiles(const std::filesystem::path& map);

} // namespace War3Map::Services
