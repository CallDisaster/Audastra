#pragma once

#include "War3MapCore/Update/UpdateManifest.h"

namespace War3Map::Update {

struct UpdatePackageVerification {
    std::filesystem::path packagePath;
    std::uint64_t bytes = 0;
    std::uint64_t expandedBytes = 0;
    std::string sha256;
    std::string manifestSha256;
    std::vector<UpdateFileEntry> files;
};

// 不写文件。在同一拒绝写入/删除的普通文件句柄上检查大小、SHA与完整ZIP内容。
// 仅支持单卷、未加密、stored/Deflate；成员必须与签名allowlist一一对应。
// 回执是已验证快照，不保证函数返回后的磁盘仍未改变，安装入口必须重新验证。
[[nodiscard]] Result<UpdatePackageVerification> VerifyUpdatePackage(
    const VerifiedUpdateManifest& manifest, const std::filesystem::path& packagePath);

} // namespace War3Map::Update
