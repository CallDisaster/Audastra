#include "War3MapCore/Services/CampaignService.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <map>

namespace War3Map::Services {
Result<std::vector<ArchiveExactMember>> CampaignSession::CaptureExactMembers(CampaignRevision expected,
    std::span<const std::string> names, const ArchiveExactMemberLimits& limits) const {
    using Output = Result<std::vector<ArchiveExactMember>>;
    if (const auto checked = Check(expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    // 共享归档默认允许 8 MiB 路径；战役实际请求仍收紧到 8 KiB，并保留调用者更小的限制。
    const auto pathBudget = std::min<std::size_t>(limits.totalPathBytes, 8192);
    if (names.size() > limits.memberCount || limits.memberCount > 256 || limits.pathBytes > 1024
        || limits.totalPathBytes > ArchiveExactMemberLimits{}.totalPathBytes
        || limits.memberBytes > 64U * 1024U * 1024U
        || limits.totalBytes > 64U * 1024U * 1024U)
        return Output::Failure("战役", "CAMPAIGN_SOURCE_BUDGET", "战役固定成员读取超过预算。");
    std::map<std::string, std::size_t, std::less<>> indexes;
    std::vector<ArchiveExactMember> result;
    std::vector<std::string> baseNames;
    std::size_t retained = 0, pathBytes = 0;
    for (const auto& name : names) {
        auto key = Resources::CanonicalImportPath(name);
        if (!key.Succeeded()) return {{}, std::move(key.diagnostics)};
        if (name.size() > limits.pathBytes || pathBytes > pathBudget || name.size() > pathBudget - pathBytes
            || !indexes.emplace(*key.value, result.size()).second)
            return Output::Failure("战役", "CAMPAIGN_SOURCE_PATHS", "战役固定成员路径重复或超过预算。");
        pathBytes += name.size();
        ArchiveExactMember input{name, false, {}};
        if (const auto found = state_.replacements.find(*key.value); found != state_.replacements.end()) {
            if (state_.removals.contains(*key.value)) return Output::Failure("战役", "CAMPAIGN_SOURCE_CONFLICT", "战役成员同时覆盖和删除。");
            const auto& bytes = *found->second.bytes;
            if (bytes.size() > limits.memberBytes || bytes.size() > limits.totalBytes - retained)
                return Output::Failure("战役", "CAMPAIGN_SOURCE_BUDGET", "战役表覆盖超过读取预算。");
            retained += bytes.size(); input.present = true; input.bytes = bytes;
        } else if (!state_.removals.contains(*key.value) && archive_) baseNames.push_back(name);
        result.push_back(std::move(input));
    }
    if (archive_ && !baseNames.empty()) {
        auto remaining = limits; remaining.totalBytes -= retained;
        remaining.totalPathBytes = pathBudget;
        const auto captured = archive_->CaptureExactMembers(baseNames, remaining);
        if (!captured.Succeeded()) return {{}, captured.diagnostics};
        for (const auto& input : *captured.value) {
            const auto key = Resources::CanonicalImportPath(input.name);
            result[indexes.at(*key.value)] = input;
        }
    }
    if (const auto checked = Check(expected); !checked.Succeeded()) return {{}, checked.diagnostics};
    return Output::Success(std::move(result));
}
} // namespace War3Map::Services
