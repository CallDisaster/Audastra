#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include <array>
#include <string_view>

namespace War3Map::Services {

inline constexpr std::array<std::string_view, 7> kClassicLuaGeneratedMembers{
    "war3map.j", "audastra\\map.lua", "audastra\\compat.lua", "audastra\\bootstrap.lua",
    "audastra\\objects.lua", "audastra\\continue.lua", "audastra\\generated.manifest"};

enum class ClassicLuaSourcePolicy { RequireManifest, AllowAbsentForFirstSave };

// 调用者先锁定源文件并核对地图SHA，再按固定七名捕获实际身份和清单字节。
// 不重新解码整图；此纯校验共用于保存快照与测试的流式只读租约。
[[nodiscard]] Status VerifyClassicLuaGeneratedIdentities(
    std::span<const ArchiveExactMemberIdentity> identities, std::span<const std::byte> manifestBytes,
    ClassicLuaSourcePolicy policy = ClassicLuaSourcePolicy::RequireManifest);

// 保存与测试共用严格来源校验。首次保存仅允许没有任何同名模块的源图；
// 测试必须已有完整清单，前六成员的实际字节SHA逐项匹配，不依赖listfile列名。
[[nodiscard]] Status VerifyClassicLuaGeneratedSources(const MapArchiveSession& archive,
    std::string_view expectedSourceSha256,
    ClassicLuaSourcePolicy policy = ClassicLuaSourcePolicy::RequireManifest);

} // namespace War3Map::Services
