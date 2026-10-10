#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace War3Map::IO {

// 普通流式 I/O；不提供 VerifiedFile 的文件身份/SHA 保证，也不承诺原子写入。
[[nodiscard]] Result<std::vector<std::byte>> ReadFileBytes(
    const std::filesystem::path& path, std::uint64_t maxBytes = 1024ULL * 1024 * 1024);
[[nodiscard]] Status WriteFileBytes(const std::filesystem::path& path, std::span<const std::byte> bytes);
[[nodiscard]] std::string PathUtf8(const std::filesystem::path& path);
// 使用 generic 分隔符（'/'）；与原先 Workspace/EditorSession 的路径身份一致。
[[nodiscard]] std::string GenericPathUtf8(const std::filesystem::path& path);

} // namespace War3Map::IO
