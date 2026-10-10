#pragma once

#include "War3MapCore/Script/ScriptToolchainLayout.h"

namespace War3Map::Script::Detail {

// 内容与末项属性来自同一只读捕获；租约不延续到调用方执行。
struct SourceBindingSnapshot {
    ScriptToolchainFileBinding binding;
    std::vector<std::byte> bytes;
    std::uint64_t lastWriteTicks = 0;
};

[[nodiscard]] Result<SourceBindingSnapshot> CaptureSourceBinding(const std::filesystem::path& path,
    ScriptToolchainBindingKind expectedKind, bool required, std::uint64_t maxBytes);
[[nodiscard]] bool NormalizeSourcePath(const std::filesystem::path& input, std::filesystem::path& output);

} // namespace War3Map::Script::Detail
