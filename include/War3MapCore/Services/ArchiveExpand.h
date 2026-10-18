#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Services {

// 把 .w3x/.w3m 的具名成员写到一个新的展开目录，供 war3-map-mcp --workspace 打开。
// CLI archive-expand 与独立 MCP archive_expand 共用。只读源图；目录只是编辑工作区，
// 写回地图仍由 save_map_edit_candidate 绑定原归档与 SHA 完成，未展开的成员从原归档保留。
struct ArchiveExpandOptions {
    bool dataOnly = false; // 只写编辑器管理的地图数据文件（war3map*、Scripts\war3map.j 等），不写导入资源
};

struct ArchiveExpandSkipped {
    std::string member;
    std::string reason; // archive-metadata | unsafe-path | case-collision | import（dataOnly 时）
};

struct ArchiveExpandReceipt {
    std::filesystem::path directory;
    std::string sourceSha256;
    std::size_t writtenMembers = 0;
    std::uint64_t writtenBytes = 0;
    std::vector<ArchiveExpandSkipped> skipped;
    std::size_t opaqueMembers = 0; // 归档里没有名字、无法展开的成员
    bool triggerPairPresent = false; // war3map.wtg 与 war3map.wct 都已写出
};

// target 必须尚不存在、父目录必须存在；先写到同级临时目录，全部成功后再改名。
[[nodiscard]] Result<ArchiveExpandReceipt> ExpandArchive(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, const std::filesystem::path& target,
    const ArchiveExpandOptions& options = {});

} // namespace War3Map::Services
