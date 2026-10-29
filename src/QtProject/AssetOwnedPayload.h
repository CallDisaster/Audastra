#pragma once

#include "EditorProject.h"
#include "War3MapCore/Services/ImportPayloadPolicy.h"

namespace War3Map::QtProject {

inline constexpr std::size_t kOwnedLuaAssetBytes = Services::ImportPayloadPolicy::luaDraftBytes;

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
};

// 仅读一个明确有效资源，不提取整图，不改作者磁盘来源。载荷与来源身份由返回值自持有。
[[nodiscard]] Result<OwnedAssetSource> ReadOwnedAsset(const EditorProject& project, const QString& path,
    std::size_t maxBytes = kOwnedLuaAssetBytes);
[[nodiscard]] Status VerifyOwnedAsset(const EditorProject& project, const OwnedAssetSource& source);
// 在 private AssetState 中规划，与普通资源导入同一份队列；发布由 RecordAssetEdit 共同负责。
[[nodiscard]] QString PutOwnedAsset(const EditorProject& project, AssetState& candidate, const QString& path,
    std::shared_ptr<const std::vector<std::byte>> bytes, bool runtimeLuaDraft = true);

} // namespace War3Map::QtProject
