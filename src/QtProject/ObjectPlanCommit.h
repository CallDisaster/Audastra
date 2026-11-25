#pragma once

#include "War3MapCore/Services/ObjectCommands.h"

namespace War3Map::QtProject {
class EditorProject;

// 把一份物编命令计划作为一步推入该物编分类自己的撤销栈（修改面板、Agent 与摆放导入共用）。
// 计划的 before 必须就是当前文档（尚无文档时为空文档），否则拒绝且不改动工程。仅在工程所属线程调用。
[[nodiscard]] Status CommitObjectCommandPlan(EditorProject& project, const Services::ObjectCommandPlan& plan);

} // namespace War3Map::QtProject
