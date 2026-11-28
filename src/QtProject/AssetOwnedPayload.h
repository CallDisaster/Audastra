#pragma once

#include "EditorProject.h"
#include "War3MapCore/Services/ImportPayloadPolicy.h"

namespace War3Map::QtProject {

inline constexpr std::size_t kOwnedLuaAssetBytes = Services::ImportPayloadPolicy::luaDraftBytes;
// 只读取（不编辑）一个资源正文时的上限，供材质图片等较大的文件；编辑 Lua 仍按 kOwnedLuaAssetBytes。
inline constexpr std::size_t kOwnedAssetReadMaxBytes = 64 * 1024 * 1024;

struct OwnedAssetSource {
    QString path;
    QString originalPath, diskPath; // 仅用于无草稿时跟随资源页面的唯一来源改名。
    std::shared_ptr<const std::vector<std::byte>> bytes;
    std::string sha256;
    const MapArchiveSession* archive = nullptr;
    const DirectoryWorkspace* directory = nullptr;
    std::filesystem::path sourcePath, projectPath;
    std::string sourceSha256;
    std::uint64_t generation = 0;
    bool runtimeLuaDraft = false;
    std::filesystem::path externalLuaRoot;
};

// 仅读一个明确有效资源，不提取整图，不改作者磁盘来源。载荷与来源身份由返回值自持有。
[[nodiscard]] Result<OwnedAssetSource> ReadOwnedAsset(const EditorProject& project, const QString& path,
    std::size_t maxBytes = kOwnedLuaAssetBytes);
[[nodiscard]] Result<OwnedAssetSource> ReadOwnedMapAsset(const EditorProject& project, const QString& path,
    std::size_t maxBytes = kOwnedLuaAssetBytes);
[[nodiscard]] Status VerifyOwnedAsset(const EditorProject& project, const OwnedAssetSource& source);
// 在 private AssetState 中规划，与普通资源导入同一份队列；发布由 RecordAssetEdit 共同负责。
[[nodiscard]] QString PutOwnedAsset(const EditorProject& project, AssetState& candidate, const QString& path,
    std::shared_ptr<const std::vector<std::byte>> bytes, bool runtimeLuaDraft = true);
// 把一个本机文件读成导入载荷，单个文件按 ImportPayloadPolicy::ownedFileBytes 限制；只读文件，不动工程。
// Agent 导入命令与材质面板共用。
[[nodiscard]] Result<EditorProject::ImportPayload> ReadDiskImportPayload(const QString& diskPath, const QString& archivePath);

} // namespace War3Map::QtProject
