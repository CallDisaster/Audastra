#pragma once

#include "War3MapCore/Services/ProjectSearch.h"

#include <array>

namespace War3Map::QtProject {
class EditorProject;

// 查询适配器共用生效物编层：LNI优先，缺席时native；不复制整份目录或延长借用生命。
[[nodiscard]] std::array<Services::ProjectSearchObjectSource, 7> CaptureProjectObjectSources(const EditorProject& project);
struct ProjectMapQuerySnapshot {
    MapDocument document;
    Services::MapDataQuerySnapshot data;
};
// 来源归档SHA或已加载成员摘要；不重新读作者地图。返回自有当前overlay供有界查询。
[[nodiscard]] Result<ProjectMapQuerySnapshot> CaptureProjectMapQuery(const EditorProject& project,
    std::string documentId, std::uint64_t generation);
// owned只允许复用调用方在同一同步来源/会话/修订stamp下取得的自有文档。
// 未提供时正常捕获当前overlay；不减少成员hash或未知状态检查。
[[nodiscard]] Result<Services::MapDataQuerySnapshot> CaptureProjectMapDataQuery(const EditorProject& project,
    std::string documentId, std::uint64_t generation, std::shared_ptr<const MapDocument> owned = {});
}
