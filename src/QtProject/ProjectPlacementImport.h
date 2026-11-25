#pragma once

#include "War3MapCore/MapData/PlacementManifest.h"
#include "War3MapCore/Services/PlacementImport.h"

namespace War3Map::QtProject {
class EditorProject;

// 摆放清单导入（界面与 Agent 共用；仅在工程所属线程调用）。
// 规划只读当前工程，不改任何状态。提交按编辑器分开记录：导入文件进资源撤销栈，
// 新建的装饰物/单位类型进各自物编分类的撤销栈，全部摆放作为一步进世界页撤销栈。
// 任何一步失败时，已完成的步骤撤回并移出各自撤销栈，工程保持导入前的状态。
[[nodiscard]] Result<Services::PlacementImportPlan> PlanProjectPlacementImport(EditorProject& project,
    const MapData::PlacementManifest& manifest);
// 文件正文移入资源导入，调用方传入后不再持有。
[[nodiscard]] Result<Services::PlacementImportReport> CommitProjectPlacementImport(EditorProject& project,
    MapData::PlacementManifest manifest);
// 编辑器内导入与资源导入共用内存上限（单文件 64MiB、合计 256MiB）；更大的清单只能由命令行保存导入。
[[nodiscard]] Status CheckPlacementImportPayloadBudget(const MapData::PlacementManifest& manifest);

} // namespace War3Map::QtProject
