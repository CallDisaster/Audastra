#pragma once

#include "War3MapCore/EditorSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapDocument.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <span>

namespace War3Map::MapData::detail {

// 仅 codec 完成领域重放证明之后使用；共同绑定原目录成员，不接受协议 bytes。
inline Result<std::vector<BinaryFilePatch>> BindMapDataPatchSources(const MapDocument& original,
    const MapDocument& current, const WorkspaceSnapshot& source, std::span<const MapDataKind> kinds,
    std::string summary) {
    const auto lower = [](const std::filesystem::path& input) {
        auto value = input.generic_string();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };
    std::set<MapDataKind> seen;
    std::vector<BinaryFilePatch> files;
    files.reserve(kinds.size());
    for (const auto kind : kinds) {
        const auto& before = original.Member(kind);
        const auto& after = current.Member(kind);
        if (!seen.insert(kind).second || before.state != MapDataState::Ready || after.state != MapDataState::Ready
            || before.sources.size() != 1 || after.sources.size() != 1)
            return Result<std::vector<BinaryFilePatch>>::Failure("MapData", "MAP_FIELD_PATCH_SOURCE",
                "地图数据密封需要唯一且完整的原生来源。");
        const auto memberName = lower(before.memberName);
        const WorkspaceFile* bound = nullptr;
        for (const auto& file : source.files) {
            const auto path = lower(file.relativePath);
            if (path != memberName && path != "map/" + memberName) continue;
            if (bound)
                return Result<std::vector<BinaryFilePatch>>::Failure("MapData", "MAP_FIELD_PATCH_SOURCE",
                    "原生成员在目录快照中有多个来源。");
            bound = &file;
        }
        const auto sha = Sha256(before.sources.front().data);
        if (!sha.Succeeded()) return {{}, sha.diagnostics};
        if (!bound || bound->sha256 != *sha.value || bound->size != before.sources.front().data.size())
            return Result<std::vector<BinaryFilePatch>>::Failure("MapData", "MAP_FIELD_PATCH_SOURCE",
                "原生文档不属于已打开目录快照的精确成员。");
        files.push_back({bound->relativePath, after.sources.front().data, summary, *sha.value});
    }
    return Result<std::vector<BinaryFilePatch>>::Success(std::move(files));
}

} // namespace War3Map::MapData::detail
