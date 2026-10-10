import type { ModuleId, WorkbenchProject } from './types';
import { buildReferenceIndex, usagesOf } from './referenceIndex';
export type Reference = { label: string; detail: string; module: ModuleId; target: string };
/** 静态引用：只统计工程数据中的直接引用；脚本文本与动态拼接不在此列，结果不能作为安全删除的依据。
 *  单次查询的简便接口，内部使用与物体管理器相同的引用索引；需要多次查询时直接用 buildReferenceIndex。 */
export function referencesTo(project: WorkbenchProject, id: string, extraKeys: string[] = []): Reference[] {
  const object = project.objects.find(o => o.id === id);
  const variable = project.variables.find(v => v.id === id);
  const asset = project.assets.find(a => a.id === id);
  const keys = [id, object?.rawcode, variable?.name, asset?.path, ...extraKeys];
  return usagesOf(buildReferenceIndex(project), keys, object?.id ?? (extraKeys.length ? project.objects.find(o => extraKeys.includes(o.rawcode))?.id : undefined))
    .map(usage => ({ label: usage.owner, detail: usage.place === '条件' && usage.target === usage.ownerId ? '触发条件' : usage.place, module: usage.module, target: usage.target }));
}
