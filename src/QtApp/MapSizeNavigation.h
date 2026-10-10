#pragma once

#include "EditorProject.h"
#include "Modules/EditorModule.h"
#include "War3MapCore/Services/MapSizeAnalysis.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace War3Map::QtApp {

// 统计identity只属于MapSize报告；资源导航必须交付当前AssetEntries的真实来源identity。
// 宿主先按完整report/revision重新核对行，再调用此元数据适配器；不读取或散列正文。
[[nodiscard]] inline std::optional<Shell::NavigationTarget> MakeMapSizeAssetNavigationTarget(
    const Services::MapSizeRow& row, std::span<const QtProject::AssetEntry> entries) {
    using Origin = Services::MapSizeRowOrigin;
    using Trust = Services::MapSizeByteTrust;
    if (!row.navigationAllowed || row.managedMember || row.nonAsciiIdentity || !row.currentLogicalBytes
        || (row.trust != Trust::OwnedMetadata && row.trust != Trust::ReportedUnverified)
        || row.currentPath.empty() || row.currentPath.size() > 2048 || row.originalPath.size() > 2048
        || row.currentPath.find('\0') != std::string::npos || row.originalPath.find('\0') != std::string::npos)
        return std::nullopt;
    const bool pending = row.origin == Origin::PendingImport || row.origin == Origin::PendingReplacement;
    if (!pending && row.origin != Origin::Original && row.origin != Origin::Renamed) return std::nullopt;
    const auto path = QString::fromUtf8(row.currentPath.data(), static_cast<qsizetype>(row.currentPath.size()));
    const auto original = QString::fromUtf8(row.originalPath.data(), static_cast<qsizetype>(row.originalPath.size()));
    const auto key = QtProject::EditorProject::AssetKey(path);
    const QtProject::AssetEntry* selected = nullptr;
    for (const auto& entry : entries) {
        if (QtProject::EditorProject::AssetKey(entry.path) != key) continue;
        if (selected || entry.ambiguous || entry.path != path || entry.originalPath != original
            || entry.Pending() != pending || (pending && entry.Replacement() != (row.origin == Origin::PendingReplacement)))
            return std::nullopt;
        const auto size = entry.ownedPayload
            ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(entry.ownedPayload->size())}
            : entry.size >= 0 ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(entry.size)} : std::nullopt;
        const auto trust = pending && !entry.ownedPayload ? Trust::ReportedUnverified : Trust::OwnedMetadata;
        if (size != row.currentLogicalBytes || trust != row.trust) return std::nullopt;
        selected = &entry;
    }
    if (!selected) return std::nullopt;
    const auto identity = pending ? QStringLiteral("pending/") + selected->path : selected->originalPath;
    if (identity.isEmpty()) return std::nullopt;
    return Shell::NavigationTarget{"assets", "file", selected->path,
        {{"stableIdentity", true}, {"sourceIdentity", identity}}};
}

} // namespace War3Map::QtApp
