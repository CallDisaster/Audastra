import type { EditorObject, ObjectKind, WorkbenchProject } from './types';

// 选择器/显示/代码生成共用的物体查找：地图物体优先，其次本机原版目录（war3Data 加载后注册）。
// 纯逻辑模块，不依赖 React，可在数据检查脚本中使用；未注册时只有地图物体。
type StandardEntry = { id: string; name: string; suffix: string; group: string[]; icon: string; hidden: boolean };
type Provider = { objects: (kind: ObjectKind) => StandardEntry[]; iconUrl: (icon: string) => string | undefined };
const kinds: ObjectKind[] = ['unit', 'item', 'destructable', 'doodad', 'ability', 'buff', 'upgrade'];
let provider: Provider | undefined;
const cache = new Map<ObjectKind, EditorObject[]>();
export function registerStandardCatalog(next: Provider | undefined) { provider = next; cache.clear(); }
export const hasStandardCatalog = () => !!provider;

function standardObjects(kind: ObjectKind): EditorObject[] {
  if (!provider) return [];
  let list = cache.get(kind);
  if (!list) {
    list = provider.objects(kind).filter(object => !object.hidden).map(object => ({
      id: object.id, name: [object.name.trim(), object.suffix.trim()].filter(Boolean).join(' '), category: object.group.join(' / '), health: 0, speed: 0, enabled: true,
      kind, rawcode: object.id, parent: object.id, base: object.id, race: object.group[0] ?? '', custom: false, fields: { icon: provider!.iconUrl(object.icon) ?? '' }, defaults: {}, levels: {},
    }));
    cache.set(kind, list);
  }
  return list;
}
/** 某类可选物体：地图物体（自定义与修改过的原版）在前，其余原版物体在后，同 rawcode 以地图为准。 */
export function pickableObjects(project: WorkbenchProject, kind: ObjectKind): EditorObject[] {
  const own = project.objects.filter(object => object.kind === kind);
  const codes = new Set(own.map(object => object.rawcode));
  return [...own, ...standardObjects(kind).filter(object => !codes.has(object.rawcode))];
}
/** 按 ID 或 rawcode 查找物体；指定类别时只在该类中找原版物体。 */
export function findObject(project: WorkbenchProject, value: unknown, kind?: ObjectKind): EditorObject | undefined {
  if (value === undefined || value === null || value === '') return undefined;
  const own = project.objects.find(object => (object.id === value || object.rawcode === value) && (!kind || object.kind === kind));
  if (own) return own;
  for (const item of kind ? [kind] : kinds) { const found = standardObjects(item).find(object => object.rawcode === value); if (found) return found; }
  return undefined;
}
/** 地图物体没有自己的图标时，使用其原版基础物体的图标。 */
export function catalogIcon(object: Pick<EditorObject, 'kind' | 'rawcode' | 'base'>): string | undefined {
  const code = object.base ?? object.rawcode;
  const entry = provider?.objects(object.kind).find(item => item.id === code);
  return entry?.icon ? provider!.iconUrl(entry.icon) : undefined;
}
