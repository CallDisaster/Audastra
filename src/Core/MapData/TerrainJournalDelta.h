#pragma once

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainPlacementJournalDelta.h"

#include <algorithm>
#include <span>

namespace War3Map::MapData::Detail {

inline constexpr std::size_t JournalDeltaMaximumBytes = 64u * 1024u * 1024u;

inline Status ValidatePlacementDelta(const TerrainPlacementJournalDelta& edit,
    std::size_t minimumBytes, const char* code) {
    const auto sha = [](std::string_view value) {
        return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    if (!sha(edit.beforeSha256) || !sha(edit.afterSha256) || edit.beforeSha256 == edit.afterSha256
        || edit.beforeBytes < minimumBytes || edit.afterBytes < minimumBytes
        || edit.beforeBytes > JournalDeltaMaximumBytes || edit.afterBytes > JournalDeltaMaximumBytes
        || edit.prefixBytes > edit.beforeBytes || edit.removedBytes > edit.beforeBytes - edit.prefixBytes
        || edit.inserted.size() > JournalDeltaMaximumBytes
        || edit.beforeBytes - edit.removedBytes > JournalDeltaMaximumBytes - edit.inserted.size()
        || edit.afterBytes != edit.beforeBytes - edit.removedBytes + edit.inserted.size()
        || (!edit.removedBytes && edit.inserted.empty()))
        return Status::Failure("Terrain.Journal", code, "摆放记录的完整身份、长度或有界替换范围无效。");
    return Status::Ok();
}

template<class Delta>
Result<Delta> BuildPlacementDelta(std::span<const std::byte> before, std::span<const std::byte> after,
    std::size_t maximumInsertedBytes, std::size_t minimumBytes, const char* deltaCode) {
    using Output = Result<Delta>;
    if (before.size() > JournalDeltaMaximumBytes || after.size() > JournalDeltaMaximumBytes
        || maximumInsertedBytes > JournalDeltaMaximumBytes)
        return Output::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET", "摆放记录超过单成员或中段预算。");
    const auto beforeSha = Sha256(before), afterSha = Sha256(after);
    if (!beforeSha.Succeeded() || !beforeSha.value) return {{}, beforeSha.diagnostics};
    if (!afterSha.Succeeded() || !afterSha.value) return {{}, afterSha.diagnostics};
    std::size_t prefix = 0, suffix = 0;
    while (prefix < std::min(before.size(), after.size()) && before[prefix] == after[prefix]) ++prefix;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix
        && before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) ++suffix;
    if (after.size() - prefix - suffix > maximumInsertedBytes)
        return Output::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET", "摆放中段超过工程剩余预算，未分配或截断载荷。");
    Delta result;
    result.beforeSha256 = *beforeSha.value; result.afterSha256 = *afterSha.value;
    result.beforeBytes = static_cast<std::uint32_t>(before.size()); result.afterBytes = static_cast<std::uint32_t>(after.size());
    result.prefixBytes = static_cast<std::uint32_t>(prefix);
    result.removedBytes = static_cast<std::uint32_t>(before.size() - prefix - suffix);
    result.inserted.assign(after.begin() + prefix, after.end() - suffix);
    const auto valid = ValidatePlacementDelta(result, minimumBytes, deltaCode);
    return valid.Succeeded() ? Output::Success(std::move(result)) : Output{{}, valid.diagnostics};
}

inline Result<std::vector<std::byte>> ApplyPlacementDelta(std::span<const std::byte> current,
    const TerrainPlacementJournalDelta& edit, std::size_t minimumBytes,
    const char* deltaCode, const char* staleCode, const char* outputCode) {
    using Output = Result<std::vector<std::byte>>;
    const auto valid = ValidatePlacementDelta(edit, minimumBytes, deltaCode);
    if (!valid.Succeeded()) return {{}, valid.diagnostics};
    if (current.size() != edit.beforeBytes)
        return Output::Failure("Terrain.Journal", staleCode, "摆放记录不属于当前完整成员长度。");
    const auto beforeSha = Sha256(current);
    if (!beforeSha.Succeeded() || !beforeSha.value) return {{}, beforeSha.diagnostics};
    if (*beforeSha.value != edit.beforeSha256)
        return Output::Failure("Terrain.Journal", staleCode, "摆放记录的完整旧成员SHA与当前会话不同。");
    std::vector<std::byte> after; after.reserve(edit.afterBytes);
    after.insert(after.end(), current.begin(), current.begin() + edit.prefixBytes);
    after.insert(after.end(), edit.inserted.begin(), edit.inserted.end());
    after.insert(after.end(), current.begin() + edit.prefixBytes + edit.removedBytes, current.end());
    const auto afterSha = Sha256(after);
    if (!afterSha.Succeeded() || !afterSha.value) return {{}, afterSha.diagnostics};
    if (after.size() != edit.afterBytes || *afterSha.value != edit.afterSha256)
        return Output::Failure("Terrain.Journal", outputCode, "摆放替换结果不符合完整新成员身份。");
    std::size_t prefix = 0, suffix = 0;
    while (prefix < std::min(current.size(), after.size()) && current[prefix] == after[prefix]) ++prefix;
    while (suffix < current.size() - prefix && suffix < after.size() - prefix
        && current[current.size() - 1 - suffix] == after[after.size() - 1 - suffix]) ++suffix;
    if (prefix != edit.prefixBytes || current.size() - prefix - suffix != edit.removedBytes
        || after.size() - prefix - suffix != edit.inserted.size())
        return Output::Failure("Terrain.Journal", deltaCode, "摆放记录必须使用完整公共前后缀的唯一最小中段。");
    return Output::Success(std::move(after));
}

} // namespace War3Map::MapData::Detail
