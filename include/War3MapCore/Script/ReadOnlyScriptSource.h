#pragma once

#include "War3MapCore/Script/ScriptToolchainLayout.h"

namespace War3Map::Script {

struct ReadOnlyScriptSource {
    ScriptToolchainFileBinding binding;
    std::vector<std::byte> bytes;
    std::uint64_t lastWriteTicks = 0;
};

// 复用保存链的只读源租约、reparse拒绝、SHA与读取后身份复核；不存在也返回绑定。
// 代码检查不得把此来源捕获结果作为脚本执行或地图保存资格。
[[nodiscard]] Result<ReadOnlyScriptSource> CaptureReadOnlyScriptSource(
    const std::filesystem::path& path, bool required = false, std::uint64_t maxBytes = 4 * 1024 * 1024);

} // namespace War3Map::Script
