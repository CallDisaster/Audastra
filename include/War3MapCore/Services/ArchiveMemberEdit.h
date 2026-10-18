#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <filesystem>
#include <set>
#include <span>
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

// 某个成员的路径在另一个成员里以字符串出现。kind：script / object-data / map-data / model / ui-text。
struct ArchivePathMention {
    std::string member;
    std::string kind;
    std::size_t count = 0;
};

struct ArchiveMemberRemoveOptions {
    bool allowReferenced = false; // 找到路径出现处时仍然删除
};

struct ArchiveMemberEditReceipt {
    std::filesystem::path candidate;
    std::string candidateSha256;
    std::string sourceSha256;
    std::size_t memberCount = 0;
    std::vector<std::string> added;
    std::vector<std::pair<std::string, std::size_t>> removed; // 成员与删掉的导入记录数
    std::vector<std::vector<ArchivePathMention>> mentions;    // 与 removed 对齐：删除前找到的路径出现处
    bool referencesChecked = false; // 路径查找不覆盖动态拼接的路径，不能证明没有引用
};

// 在脚本、七类物编（含 Skin）、w3i/wts/misc、导入模型和 UI 文本里按字符串查找每个目标路径。
// 大小写和斜杠方向不限，.mdl/.mdx 互通，JASS 字符串里的双反斜杠按一个算；.ai 等不透明成员不读。
// 目标自身之间的出现不计。结果与 targets 对齐；找不到不等于没有引用（动态拼接查不出）。
[[nodiscard]] std::vector<std::vector<ArchivePathMention>> FindArchivePathMentions(
    std::span<const MapArchiveMember> members, std::span<const std::string> targets);

// 校验一个成员路径：安全的归档相对路径、不是编辑器管理的地图数据文件、本批内不重复。
// 返回作者写法（统一为反斜杠）；requested 记录本批已用的规范路径。
[[nodiscard]] Result<std::string> NormalizeArchiveMemberPath(std::string_view member, std::set<std::string>& requested);

// 新增成员：已存在的成员（含未列入文件清单的）一律拒绝，不覆盖。
[[nodiscard]] Result<ArchiveMemberEditReceipt> AddArchiveMembers(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, const std::filesystem::path& target,
    const std::vector<ArchiveMemberAddition>& additions);

// 删除已有成员及解析到它的全部导入记录；不在导入清单里的成员也可删，清单原字节保留。
// 先查路径出现处：找到时拒绝（MEMBER_REFERENCED），除非 allowReferenced；回执列出找到的位置。
[[nodiscard]] Result<ArchiveMemberEditReceipt> RemoveArchiveMembers(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, const std::filesystem::path& target,
    const std::vector<std::string>& members, const ArchiveMemberRemoveOptions& options = {});

} // namespace War3Map::Services
