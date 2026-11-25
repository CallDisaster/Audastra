#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::IO {

// 普通流式 I/O；不提供 VerifiedFile 的文件身份/SHA 保证，也不承诺原子写入。
[[nodiscard]] Result<std::vector<std::byte>> ReadFileBytes(
    const std::filesystem::path& path, std::uint64_t maxBytes = 1024ULL * 1024 * 1024);
[[nodiscard]] Status WriteFileBytes(const std::filesystem::path& path, std::span<const std::byte> bytes);
[[nodiscard]] std::string PathUtf8(const std::filesystem::path& path);
// 使用 generic 分隔符（'/'）；与原先 Workspace/EditorSession 的路径身份一致。
[[nodiscard]] std::string GenericPathUtf8(const std::filesystem::path& path);
// UTF-8 文本转路径。std::filesystem::path(std::string) 在 Windows 上按系统 ANSI 代码页解读，含中文会变成别的路径。
[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view utf8);
// 清单里引用其所在目录内文件的相对路径：非空、最长260、无盘符或冒号、不以分隔符开头、
// 不含空段及“.”“..”段；'/' 与 '\\' 都视为分隔符。
[[nodiscard]] bool IsSafeManifestRelativePath(std::string_view text) noexcept;

} // namespace War3Map::IO
