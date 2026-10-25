#pragma once

#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/MapData/TerrainStrokeJournal.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>

namespace War3Map::Services::TerrainPathingFinalDetail {

// 只借用本次冻结的最终成员。缺失与删除分开：删除直接返回map missing，
// 后续game fallback仍由Core的实际资源捕获器完成，绝不读回被删除的旧成员。
class FinalMemberOverlay {
public:
    static Result<FinalMemberOverlay> Prepare(std::span<const MemberReplacement> members,
        std::span<const std::string> removals, std::size_t maximumBytes) {
        using Output = Result<FinalMemberOverlay>;
        const auto fail = [](const char* code, const char* message) { return Output::Failure("Terrain.PathingSave", code, message); };
        if (!maximumBytes || maximumBytes > MapData::TerrainSavePlanLimits{}.maximumBytes
            || members.size() > 4096 || removals.size() > 4096 - members.size())
            return fail("TERRAIN_FINAL_INPUT_BUDGET", "最终通行输入数量或捕获预算超过固定上限。");
        FinalMemberOverlay result;
        const auto count = members.size() + removals.size();
        if (count > maximumBytes / sizeof(Entry)) return fail("TERRAIN_FINAL_INPUT_BUDGET", "最终成员索引超过剩余预算。");
        result.entries_.reserve(count);
        result.metadataBytes_ = sizeof(result) + result.entries_.capacity() * sizeof(Entry) + 256;
        if (result.metadataBytes_ >= maximumBytes) return fail("TERRAIN_FINAL_INPUT_BUDGET", "最终成员索引没有剩余资源捕获预算。");
        const auto append = [&](std::string_view name, const std::vector<std::byte>* bytes) -> Status {
            if (name.size() > ArchiveExactMemberLimits{}.pathBytes)
                return Status::Failure("Terrain.PathingSave", "TERRAIN_FINAL_INPUT_PATH", "最终通行输入路径超过1024字节。");
            if (std::max(name.size(), std::string{}.capacity()) + 1 > maximumBytes - result.metadataBytes_)
                return Status::Failure("Terrain.PathingSave", "TERRAIN_FINAL_INPUT_BUDGET", "最终成员名称超过剩余索引预算。");
            auto key = Resources::CanonicalImportPath(name);
            if (!key.Succeeded() || !key.value) return {std::move(key.diagnostics)};
            if (key.value->capacity() + 1 > maximumBytes - result.metadataBytes_)
                return Status::Failure("Terrain.PathingSave", "TERRAIN_FINAL_INPUT_BUDGET", "最终成员实际名称容量超过剩余索引预算。");
            result.metadataBytes_ += key.value->capacity() + 1;
            result.entries_.push_back({std::move(*key.value), bytes});
            return Status::Ok();
        };
        for (const auto& member : members) {
            auto status = append(member.name, &member.data);
            if (!status.Succeeded()) return {{}, std::move(status.diagnostics)};
        }
        for (const auto& removed : removals) {
            auto status = append(removed, nullptr);
            if (!status.Succeeded()) return {{}, std::move(status.diagnostics)};
        }
        std::sort(result.entries_.begin(), result.entries_.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        for (std::size_t i = 1; i < result.entries_.size(); ++i) if (result.entries_[i - 1].name == result.entries_[i].name)
            return fail("TERRAIN_FINAL_INPUT_CONFLICT", "最终通行输入有重复路径或覆盖/删除冲突。");
        if (result.metadataBytes_ >= maximumBytes) return fail("TERRAIN_FINAL_INPUT_BUDGET", "最终通行输入没有剩余资源捕获预算。");
        return Output::Success(std::move(result));
    }

    [[nodiscard]] std::size_t MetadataBytes() const noexcept { return metadataBytes_; }

    // 历史结构只覆盖固定十项；借用冻结正文，非结构输入仍从当前实际来源读取。
    // 这是内部读取视图，不给raw字节授予任何保存或special谱系。
    static Result<FinalMemberOverlay> PrepareBaseline(std::span<const ArchiveExactMember> members,
        std::size_t maximumBytes) {
        using Output = Result<FinalMemberOverlay>;
        if (members.size() != MapData::TerrainJournalMemberNames.size() || !maximumBytes
            || maximumBytes > MapData::TerrainSavePlanLimits{}.maximumBytes)
            return Output::Failure("Terrain.PathingSave", "TERRAIN_HISTORY_SOURCE_MISMATCH", "历史结构视图必须是固定十成员。");
        FinalMemberOverlay result;
        if (members.size() > maximumBytes / sizeof(Entry))
            return Output::Failure("Terrain.PathingSave", "TERRAIN_REPLAY_BUDGET", "历史结构索引超过剩余预算。");
        result.entries_.reserve(members.size());
        result.metadataBytes_ = sizeof(result) + result.entries_.capacity() * sizeof(Entry) + 256;
        for (std::size_t i = 0; i < members.size(); ++i) {
            const auto& member = members[i];
            if (member.name != MapData::TerrainJournalMemberNames[i] || (!member.present && !member.bytes.empty()))
                return Output::Failure("Terrain.PathingSave", "TERRAIN_HISTORY_SOURCE_MISMATCH", "历史结构名称、顺序或缺席状态不符。");
            if (member.name.capacity() + 1 > maximumBytes - std::min(maximumBytes, result.metadataBytes_))
                return Output::Failure("Terrain.PathingSave", "TERRAIN_REPLAY_BUDGET", "历史结构索引超过重放预算。");
            auto key = Resources::CanonicalImportPath(member.name);
            if (!key.Succeeded() || !key.value) return {{}, std::move(key.diagnostics)};
            if (key.value->capacity() + 1 > maximumBytes - std::min(maximumBytes, result.metadataBytes_))
                return Output::Failure("Terrain.PathingSave", "TERRAIN_REPLAY_BUDGET", "历史规范路径索引超过重放预算。");
            result.entries_.push_back({std::move(*key.value), member.present ? &member.bytes : nullptr});
            result.metadataBytes_ += result.entries_.back().name.capacity() + 1;
        }
        if (result.metadataBytes_ >= maximumBytes)
            return Output::Failure("Terrain.PathingSave", "TERRAIN_REPLAY_BUDGET", "历史结构索引未给资源读取留下预算。");
        std::sort(result.entries_.begin(), result.entries_.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        return Output::Success(std::move(result));
    }

    Result<std::vector<ArchiveExactMember>> Read(const MapData::TerrainPathingResources::HeightSourceReader& source,
        std::span<const std::string> names, const ArchiveExactMemberLimits& limits,
        const std::vector<std::byte>* currentDoodads) const {
        using Output = Result<std::vector<ArchiveExactMember>>;
        const auto fail = [](const char* code, const char* message) { return Output::Failure("Terrain.PathingSave", code, message); };
        const ArchiveExactMemberLimits hard;
        if (names.size() > limits.memberCount || limits.memberCount > hard.memberCount
            || limits.pathBytes > hard.pathBytes || limits.totalPathBytes > hard.totalPathBytes
            || limits.memberBytes > hard.memberBytes || limits.totalBytes > hard.totalBytes)
            return fail("TERRAIN_FINAL_INPUT_BUDGET", "最终通行读取不能放宽成员、路径或累计读取上限。");
        std::vector<ArchiveExactMember> result;
        result.reserve(names.size());
        std::vector<std::string> keys, fallbackNames;
        std::vector<std::size_t> fallbackIndices;
        keys.reserve(names.size()); fallbackNames.reserve(names.size()); fallbackIndices.reserve(names.size());
        std::size_t total = 0, pathBytes = 0, overlayPathBytes = 0;
        for (const auto& name : names) {
            if (name.size() > limits.pathBytes || name.size() > limits.totalPathBytes - pathBytes)
                return fail("TERRAIN_FINAL_INPUT_PATH", "最终通行读取路径超过本次预算。");
            pathBytes += name.size();
            auto key = Resources::CanonicalImportPath(name);
            if (!key.Succeeded() || !key.value) return {{}, std::move(key.diagnostics)};
            if (std::find(keys.begin(), keys.end(), *key.value) != keys.end())
                return fail("TERRAIN_FINAL_INPUT_READ", "最终通行请求包含重复的规范成员路径。");
            const auto found = std::lower_bound(entries_.begin(), entries_.end(), *key.value,
                [](const auto& entry, const auto& wanted) { return entry.name < wanted; });
            ArchiveExactMember member{name, false, {}};
            if (found != entries_.end() && found->name == *key.value) {
                overlayPathBytes += name.size();
                if (found->bytes) {
                    if (found->bytes->size() > limits.memberBytes || found->bytes->size() > limits.totalBytes - total)
                        return fail("TERRAIN_FINAL_INPUT_BUDGET", "最终覆盖成员超过本次逐项或累计读取上限。");
                    member.present = true; member.bytes = *found->bytes;
                    total += member.bytes.size();
                }
            } else {
                fallbackIndices.push_back(result.size()); fallbackNames.push_back(name);
            }
            keys.push_back(std::move(*key.value));
            result.push_back(std::move(member));
        }
        if (!fallbackNames.empty()) {
            // 同一批真实来源只建立一次读取租约；大图不能每个未覆盖资源都重新散列整张归档。
            auto fallbackLimits = limits; fallbackLimits.memberCount = fallbackNames.size();
            fallbackLimits.totalBytes -= total; fallbackLimits.totalPathBytes -= overlayPathBytes;
            auto read = source(fallbackNames, fallbackLimits);
            if (!read.Succeeded() || !read.value) return {{}, std::move(read.diagnostics)};
            if (read.value->size() != fallbackNames.size())
                return fail("TERRAIN_FINAL_INPUT_READ", "真实来源返回的成员数与本批唯一请求不一致。");
            for (std::size_t i = 0; i < fallbackIndices.size(); ++i) {
                const auto index = fallbackIndices[i]; auto& member = (*read.value)[i];
                auto actualKey = Resources::CanonicalImportPath(member.name);
                if (!actualKey.Succeeded() || !actualKey.value || *actualKey.value != keys[index])
                    return fail("TERRAIN_FINAL_INPUT_READ", "真实来源成员重复、顺序或名称与本批请求不一致。");
                if ((!member.present && !member.bytes.empty()) || member.bytes.size() > limits.memberBytes
                    || member.bytes.size() > limits.totalBytes - total)
                    return fail("TERRAIN_FINAL_INPUT_READ", "真实来源成员的存在性或累计读取量不一致。");
                total += member.bytes.size();
                result[index].present = member.present; result[index].bytes = std::move(member.bytes);
            }
        }
        for (std::size_t i = 0; i < result.size(); ++i)
            if (keys[i] == "WAR3MAP.DOO" && currentDoodads
                && (!result[i].present || result[i].bytes != *currentDoodads))
                return fail("TERRAIN_FINAL_INPUT_SESSION_MISMATCH", "最终DOO遗漏或改变了本次冻结会话的摆放修改。");
        return Output::Success(std::move(result));
    }

public:
    struct Entry { std::string name; const std::vector<std::byte>* bytes = nullptr; };
    [[nodiscard]] std::span<const Entry> Entries() const noexcept { return entries_; }
    [[nodiscard]] const Entry* Find(std::string_view canonicalName) const noexcept {
        const auto found = std::lower_bound(entries_.begin(), entries_.end(), canonicalName,
            [](const auto& entry, auto name) { return entry.name < name; });
        return found != entries_.end() && found->name == canonicalName ? &*found : nullptr;
    }
private:
    std::vector<Entry> entries_;
    std::size_t metadataBytes_ = 0;
};

} // namespace War3Map::Services::TerrainPathingFinalDetail
