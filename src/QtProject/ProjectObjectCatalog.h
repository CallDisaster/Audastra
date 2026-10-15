#pragma once

#include "War3MapCore/Services/ObjectCatalogSources.h"

namespace War3Map::QtProject {
class EditorProject;
class ProjectObjectCatalogState;

// 值对象拥有最终覆盖/删除；Sources()借用的span只在此对象存活期间有效。
struct ProjectObjectCatalogSources {
    Services::ObjectCatalogSources base;
    std::vector<MemberReplacement> replacements;
    std::vector<std::string> removals;
    [[nodiscard]] Services::ObjectCatalogSources Sources() const;
};

[[nodiscard]] Result<ProjectObjectCatalogSources> CaptureProjectObjectCatalogSources(
    const EditorProject& project, std::span<const std::string> names = {});
[[nodiscard]] Status EnsureProjectObjectCatalog(EditorProject& project, bool force = false);
// 来源验证是读取资格，不能据此声称已支持地图内SLK/profile写回。
[[nodiscard]] Status VerifyProjectObjectCatalog(const EditorProject& project);
[[nodiscard]] std::shared_ptr<const ObjectData::EffectiveObjectCatalog> ProjectEffectiveObjectCatalog(const EditorProject& project);
void InstallProjectObjectCatalogInvalidation(EditorProject& project);
} // namespace War3Map::QtProject
