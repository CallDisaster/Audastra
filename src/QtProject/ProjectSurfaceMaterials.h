#pragma once

#include "EditorProject.h"
#include "War3MapCore/Services/SurfaceMaterialTableDocument.h"
#include "War3MapCore/Services/SurfaceMaterialValidation.h"

#include <QString>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::QtProject {
// 地图材质表（WarVK\materials.ini）在当前工程里的读写：材质面板、Agent 命令与保存前检查共用。
// 表和材质图片都是普通导入成员：读的是当前有效内容（待导入优先，其次已打开的地图或目录），
// 写入走资源撤销栈，一次修改（新表正文 + 新导入的图片）是一步撤销。
inline constexpr std::size_t kSurfaceMaterialTableMaxBytes = 1024 * 1024;

struct SurfaceMaterialTableState {
    Services::SurfaceMaterialTableDocument document;
    bool exists = false;  // 当前资源里有这张表
    std::string sha256;   // 表正文的 SHA-256；没有表时为空串的哈希
};
[[nodiscard]] Result<SurfaceMaterialTableState> ReadSurfaceMaterialTable(const EditorProject& project);

// 材质图片的当前有效内容；不在当前资源里时为 Missing，读不了时为 Failed。
[[nodiscard]] Services::SurfaceMaterialImageSource ReadSurfaceMaterialImage(const EditorProject& project, std::string_view path);

// 可挂材质的类型：war3map.doo（含未保存的摆放修改）里每个类型的摆放数，以及类别与显示名。
class SurfaceMaterialTypeIndex {
public:
    explicit SurfaceMaterialTypeIndex(const EditorProject& project);
    [[nodiscard]] Services::SurfaceMaterialTypeInfo Info(std::string_view rawcode) const;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>& Placements() const noexcept { return placements_; }

private:
    const EditorProject& project_;
    std::map<std::string, std::size_t, std::less<>> placements_;
};

// 校验来源：图片按当前资源读，类型按 index 查；cache 可为空。index 须活得比返回值长。
[[nodiscard]] Services::SurfaceMaterialValidationSources SurfaceMaterialSources(const EditorProject& project,
    const SurfaceMaterialTypeIndex& index, Services::SurfaceMaterialImageCache* cache);

// 把新表正文和要导入的图片作为一步资源撤销写入。expectedSha256 不为空时先核对当前表，对不上就拒绝。
// 图片路径已有文件时须 replaceImages。返回空字符串表示成功，否则为原因。
[[nodiscard]] QString CommitSurfaceMaterialTable(EditorProject& project, std::string_view expectedSha256,
    const Services::SurfaceMaterialTableDocument& document, std::vector<EditorProject::ImportPayload> images,
    bool replaceImages, const QString& label);
} // namespace War3Map::QtProject
