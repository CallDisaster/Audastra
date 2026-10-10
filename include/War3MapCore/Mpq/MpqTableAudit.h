#pragma once

#include "War3MapCore/Mpq/MpqArchive.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace War3Map::Mpq {

// 哈希表 / 块表审计：统计不满足严格清单规则的条目，并按游戏的查找规则说明它们会被如何处理。
// 只读，不修改归档；结果用于诊断与会话层的宽松清单。
struct MpqTableAudit {
    std::size_t hashEntries = 0;
    std::size_t hashEmpty = 0;
    std::size_t hashDeleted = 0;
    std::size_t hashActive = 0;
    std::map<std::uint16_t, std::size_t> activeByLocale;   // locale → 活跃项数（0 = 中性）
    std::map<std::uint8_t, std::size_t> activeByPlatform;  // platform 字节 → 活跃项数
    std::map<std::uint8_t, std::size_t> activeByReserved;  // reserved 字节 → 活跃项数（不影响查找）
    std::size_t activeBlockOutOfRange = 0;                 // 指向块表之外
    std::size_t activeBlockMissing = 0;                    // 指向的块没有 exists 标志
    std::size_t duplicateNamePairs = 0;                    // 同一名字哈希对有多个活跃项

    std::size_t blockEntries = 0;
    std::size_t blocksExisting = 0;
    std::size_t blocksUnreferenced = 0;  // 存在但没有任何活跃哈希项指向（匿名 / 孤立）
    std::size_t blocksAliased = 0;       // 被多个活跃哈希项指向
    std::size_t blocksPatch = 0;
    std::size_t blocksSectorCrc = 0;
    std::size_t blocksUnknownFlags = 0;

    std::size_t listedNames = 0;
    std::size_t listedResolved = 0;      // 按游戏查找规则能找到有效块
    std::vector<std::string> listedUnresolved;
    std::vector<std::string> specialMembers; // 存在的特殊成员：(listfile) / (attributes) / (signature) / (user data)
    std::size_t anonymousBlocks = 0;     // 存在的块中无法由 (listfile) 或特殊名字命名的数量

    // 可读的问题摘要（中文），每项一行。
    [[nodiscard]] std::vector<std::string> Findings() const;
};

[[nodiscard]] MpqTableAudit AuditTables(const MpqArchive& archive);

} // namespace War3Map::Mpq
