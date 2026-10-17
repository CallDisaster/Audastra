#pragma once

#include <string_view>

namespace War3Map::Cli {

// 只新增导入成员和对应 war3map.imp 记录，不重建脚本、不改触发器/物编/地形。
// 参数：<source-map> <expected-source-sha256> <new-candidate.w3x> <member> <file> [...]
[[nodiscard]] int RunArchiveMemberAdd(int argc, wchar_t** argv);

} // namespace War3Map::Cli
