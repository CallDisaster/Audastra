#pragma once
#include <string_view>
#include <cstddef>
namespace Audastra::Runtime32 {
inline constexpr std::string_view kLuaObjectSnapshotMember = "audastra\\test\\object-data.lua";
inline constexpr std::size_t kLuaObjectSnapshotBytes = 16u * 1024u * 1024u;
}
