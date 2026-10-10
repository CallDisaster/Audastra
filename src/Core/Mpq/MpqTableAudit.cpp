#include "War3MapCore/Mpq/MpqTableAudit.h"

#include "MpqCrypt.h"

#include <set>
#include <utility>

namespace War3Map::Mpq {
namespace {

constexpr std::uint32_t kKnownFlags =
    kFileImplode | kFileCompress | kFileEncrypted | kFileFixKey | kFilePatch |
    kFileSingleUnit | kFileSectorCrc | kFileExists;

// 与读取器一致的查找：从 TableOffset 桶开始线性探测，遇空项终止，跳过已删除项，
// 取第一个名字哈希匹配且指向有效块的项（游戏对中性 locale 的查找行为）。
const MpqHashEntry* Resolve(const std::vector<MpqHashEntry>& hash, const std::vector<MpqBlockEntry>& blocks,
    const std::string& name) {
    if (hash.empty()) return nullptr;
    const auto& crypt = MpqCrypt::Instance();
    const std::uint32_t mask = static_cast<std::uint32_t>(hash.size() - 1);
    const std::uint32_t start = crypt.HashString(name, MpqCrypt::HashType::TableOffset) & mask;
    const std::uint32_t n1 = crypt.HashString(name, MpqCrypt::HashType::NameA);
    const std::uint32_t n2 = crypt.HashString(name, MpqCrypt::HashType::NameB);
    std::uint32_t index = start;
    do {
        const auto& entry = hash[index];
        if (entry.blockIndex == kMpqHashEmpty) return nullptr;
        if (entry.blockIndex != kMpqHashDeleted && entry.name1 == n1 && entry.name2 == n2 &&
            entry.blockIndex < blocks.size() && (blocks[entry.blockIndex].flags & kFileExists) != 0) {
            return &entry;
        }
        index = (index + 1) & mask;
    } while (index != start);
    return nullptr;
}

} // namespace

MpqTableAudit AuditTables(const MpqArchive& archive) {
    MpqTableAudit audit;
    const auto hash = archive.HashTableEntries();
    const auto blocks = archive.BlockTableEntries();
    audit.hashEntries = hash.size();
    audit.blockEntries = blocks.size();

    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> pairs;
    std::map<std::uint32_t, std::size_t> blockRefs;
    for (const auto& entry : hash) {
        if (entry.blockIndex == kMpqHashEmpty) { ++audit.hashEmpty; continue; }
        if (entry.blockIndex == kMpqHashDeleted) { ++audit.hashDeleted; continue; }
        ++audit.hashActive;
        ++audit.activeByLocale[entry.locale];
        ++audit.activeByPlatform[MpqPlatformByte(entry.platform)];
        ++audit.activeByReserved[MpqReservedByte(entry.platform)];
        ++pairs[{entry.name1, entry.name2}];
        if (entry.blockIndex >= blocks.size()) { ++audit.activeBlockOutOfRange; continue; }
        if ((blocks[entry.blockIndex].flags & kFileExists) == 0) { ++audit.activeBlockMissing; continue; }
        ++blockRefs[entry.blockIndex];
    }
    for (const auto& [pair, count] : pairs) {
        if (count > 1) ++audit.duplicateNamePairs;
    }
    for (std::uint32_t index = 0; index < blocks.size(); ++index) {
        const auto flags = blocks[index].flags;
        if ((flags & kFileExists) == 0) continue;
        ++audit.blocksExisting;
        const auto refs = blockRefs.find(index);
        if (refs == blockRefs.end()) ++audit.blocksUnreferenced;
        else if (refs->second > 1) ++audit.blocksAliased;
        if ((flags & kFilePatch) != 0) ++audit.blocksPatch;
        if ((flags & kFileSectorCrc) != 0) ++audit.blocksSectorCrc;
        if ((flags & ~kKnownFlags) != 0) ++audit.blocksUnknownFlags;
    }

    std::set<std::uint32_t> named;
    for (const auto& name : archive.RawListfileNames()) {
        ++audit.listedNames;
        if (const auto* entry = Resolve(hash, blocks, name)) {
            ++audit.listedResolved;
            named.insert(entry->blockIndex);
        } else {
            audit.listedUnresolved.push_back(name);
        }
    }
    // MPQ 约定的特殊成员通常不写进 (listfile)。
    for (const char* special : {"(listfile)", "(attributes)", "(signature)", "(user data)"}) {
        if (const auto* entry = Resolve(hash, blocks, special)) {
            named.insert(entry->blockIndex);
            audit.specialMembers.push_back(special);
        }
    }
    audit.anonymousBlocks = audit.blocksExisting - std::min(audit.blocksExisting, named.size());
    return audit;
}

std::vector<std::string> MpqTableAudit::Findings() const {
    std::vector<std::string> findings;
    const auto add = [&findings](std::size_t count, const std::string& text) {
        if (count > 0) findings.push_back(std::to_string(count) + " " + text);
    };
    std::size_t nonNeutral = 0;
    for (const auto& [locale, count] : activeByLocale) if (locale != 0) nonNeutral += count;
    std::size_t nonDefaultPlatform = 0;
    for (const auto& [platform, count] : activeByPlatform) if (platform != 0) nonDefaultPlatform += count;
    add(nonNeutral, "个活跃哈希项使用非中性 locale");
    add(nonDefaultPlatform, "个活跃哈希项使用非零 platform 字节");
    add(activeBlockOutOfRange, "个活跃哈希项指向块表之外（游戏查找时跳过）");
    add(activeBlockMissing, "个活跃哈希项指向不存在的块（游戏查找时跳过）");
    add(duplicateNamePairs, "组名字哈希对重复");
    add(blocksUnreferenced, "个存在的块没有哈希项指向（孤立块）");
    add(blocksAliased, "个块被多个哈希项指向");
    add(anonymousBlocks, "个存在的块无法由 (listfile) 命名");
    add(listedUnresolved.size(), "个 (listfile) 名字找不到有效块");
    add(blocksPatch, "个补丁块");
    add(blocksSectorCrc, "个带扇区 CRC 的块");
    add(blocksUnknownFlags, "个块含未知标志");
    return findings;
}

} // namespace War3Map::Mpq
