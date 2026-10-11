#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include <array>
#include <string_view>

namespace War3Map::Services {

inline constexpr std::array<std::string_view, 7> kClassicLuaGeneratedMembers{
    "war3map.j", "audastra\\map.lua", "audastra\\compat.lua", "audastra\\bootstrap.lua",
    "audastra\\objects.lua", "audastra\\continue.lua", "audastra\\generated.manifest"};

enum class ClassicLuaSourcePolicy { RequireManifest, AllowAbsentForFirstSave };

// 保存与测试共用严格来源校验。首次保存仅允许没有任何同名模块的源图；
// 测试必须已有完整清单，前六成员的实际字节SHA逐项匹配，不依赖listfile列名。
[[nodiscard]] Status VerifyClassicLuaGeneratedSources(const MapArchiveSession& archive,
    std::string_view expectedSourceSha256,
    ClassicLuaSourcePolicy policy = ClassicLuaSourcePolicy::RequireManifest);

} // namespace War3Map::Services
