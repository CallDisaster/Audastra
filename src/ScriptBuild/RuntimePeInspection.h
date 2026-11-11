#pragma once

#include "War3MapCore/Types.h"
#include <span>
#include <string_view>
#include <vector>

namespace War3Map::Script {
// 从调用方已核验的同一份文件字节读取，避免散列与 PE 检查分开取样。
[[nodiscard]] Result<std::vector<std::string>> InspectExternalDependencyPe(
    std::span<const std::byte> bytes, std::string_view entry);
}
