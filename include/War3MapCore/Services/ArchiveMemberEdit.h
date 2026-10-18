#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace War3Map::Services {

// 导入成员的增删：CLI archive-member-add/remove 与独立 MCP archive_members_add/remove 共用。
// 只改动请求的成员和 war3map.imp，不重建脚本、不改触发器/物编/地形；结果写到新的候选路径。
struct ArchiveMemberEditLimits {
    static constexpr std::size_t maxMembers = 256;
    static constexpr std::uint64_t maxMemberBytes = 64ull * 1024 * 1024;
    static constexpr std::uint64_t maxTotalBytes = 256ull * 1024 * 1024;
};

struct ArchiveMemberAddition {
    std::string member;         // 地图内路径，UTF-8；斜杠方向不限
    std::filesystem::path file; // 本机文件，调用时读入
};

struct ArchiveMemberEditReceipt {
    std::filesystem::path candidate;
    std::string candidateSha256;
    std::string sourceSha256;
    std::size_t memberCount = 0;
    std::vector<std::string> added;
    std::vector<std::pair<std::string, std::size_t>> removed; // 成员与删掉的导入记录数
    bool referencesChecked = false; // 删除不检查模型、脚本或物编引用
};

// 校验一个成员路径：安全的归档相对路径、不是编辑器管理的地图数据文件、本批内不重复。
// 返回作者写法（统一为反斜杠）；requested 记录本批已用的规范路径。
[[nodiscard]] Result<std::string> NormalizeArchiveMemberPath(std::string_view member, std::set<std::string>& requested);

// 新增成员：已存在的成员（含未列入文件清单的）一律拒绝，不覆盖。
[[nodiscard]] Result<ArchiveMemberEditReceipt> AddArchiveMembers(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, const std::filesystem::path& target,
    const std::vector<ArchiveMemberAddition>& additions);

// 删除已有成员及解析到它的全部导入记录；不在导入清单里的成员也可删，清单原字节保留。
[[nodiscard]] Result<ArchiveMemberEditReceipt> RemoveArchiveMembers(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, const std::filesystem::path& target,
    const std::vector<std::string>& members);

} // namespace War3Map::Services
