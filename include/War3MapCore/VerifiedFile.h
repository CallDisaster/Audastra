#pragma once

#include "War3MapCore/Types.h"

#include <string_view>

namespace War3Map {
class ProjectSaveControl;

// 从只读独占写入/删除租约下的磁盘文件读取完整内容，并核对已确认的 SHA-256。
// expectedSha256 必须是 64 位十六进制摘要；支持正常相对路径和 Unicode 路径。
// 返回的是独立字节快照。返回后磁盘文件再改变，不会改变此快照，也不代表磁盘仍未变化。
// 不写入源文件，不接受目录、设备、管道或超过读取预算的内容。
[[nodiscard]] Result<std::vector<std::byte>> ReadVerifiedFile(
    const std::filesystem::path& path,
    std::string_view expectedSha256,
    std::uint64_t maxBytes = 256ull * 1024 * 1024, const ProjectSaveControl* control = nullptr);

} // namespace War3Map
