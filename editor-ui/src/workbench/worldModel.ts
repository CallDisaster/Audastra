import type { EditorObject, ObjectKind, WorkbenchProject, WorldCamera, WorldInstance, WorldRegion } from './types';
import { catalogIcon, findObject } from './objectCatalog';

// 世界页纯逻辑：坐标、吸附、玩家颜色、实体查找与批量编辑。不依赖 React，供界面、命令面板、助手和检查脚本共用。
// 坐标一律为魔兽世界坐标（x 向东、y 向北，单位 = 游戏单位）；一个地形格 = 128。

export const CELL = 128;
export const defaultBounds = { minX: -4096, minY: -4096, maxX: 4096, maxY: 4096 };
export type Bounds = typeof defaultBounds;

export type EntityKind = 'unit' | 'item' | 'destructable' | 'doodad' | 'region' | 'camera';
export type WorldEntity =
  | { kind: 'unit' | 'item' | 'destructable' | 'doodad'; id: string; instance: WorldInstance; object?: EditorObject }
  | { kind: 'region'; id: string; region: WorldRegion }
  | { kind: 'camera'; id: string; camera: WorldCamera };

export const entityKindLabels: Record<EntityKind, string> = { unit: '单位', item: '物品', destructable: '可破坏物', doodad: '装饰物', region: '区域', camera: '镜头' };

/** 玩家编号沿用原版 WE：1–12 为玩家，13 中立敌对，16 中立被动。 */
export const players: { id: number; name: string; color: string }[] = [
  { id: 1, name: '玩家 1（红）', color: '#ff0303' }, { id: 2, name: '玩家 2（蓝）', color: '#0042ff' },
  { id: 3, name: '玩家 3（青）', color: '#1ce6b9' }, { id: 4, name: '玩家 4（紫）', color: '#540081' },
  { id: 5, name: '玩家 5（黄）', color: '#fffc00' }, { id: 6, name: '玩家 6（橙）', color: '#fe8a0e' },
  { id: 7, name: '玩家 7（绿）', color: '#20c000' }, { id: 8, name: '玩家 8（粉）', color: '#e55bb0' },
  { id: 9, name: '玩家 9（灰）', color: '#959697' }, { id: 10, name: '玩家 10（浅蓝）', color: '#7ebff1' },
  { id: 11, name: '玩家 11（深绿）', color: '#106246' }, { id: 12, name: '玩家 12（棕）', color: '#4e2a04' },
  { id: 13, name: '中立敌对', color: '#282828' }, { id: 16, name: '中立被动', color: '#c8c8c8' },
];
export const playerOf = (owner: number) => players.find(p => p.id === owner) ?? { id: owner, name: `玩家 ${owner}`, color: '#888888' };

export function worldBounds(project: WorkbenchProject): Bounds { return project.world.bounds ?? defaultBounds; }
export const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
export const snap = (value: number, step: number) => step > 0 ? Math.round(value / step) * step : value;
/** 朝向按原版：0° 朝东，逆时针为正，保存为 0–360。 */
export const normalizeAngle = (degrees: number) => ((degrees % 360) + 360) % 360;
export function angleTo(from: { x: number; y: number }, to: { x: number; y: number }, step = 0) {
  const raw = normalizeAngle(Math.atan2(to.y - from.y, to.x - from.x) * 180 / Math.PI);
  return normalizeAngle(step > 0 ? Math.round(raw / step) * step : Math.round(raw));
}

/** 放置实例对应的物体：地图物体按 ID，原版物体按 rawcode（与物编、选择器共用 findObject）。 */
export function instanceObject(project: WorkbenchProject, instance: Pick<WorldInstance, 'objectId'>): EditorObject | undefined {
  return findObject(project, instance.objectId);
}
export function instanceKind(project: WorkbenchProject, instance: WorldInstance): 'unit' | 'item' | 'destructable' | 'doodad' {
  const kind = instanceObject(project, instance)?.kind;
  return kind === 'item' || kind === 'destructable' || kind === 'doodad' ? kind : 'unit';
}
export function entityName(entity: WorldEntity): string {
  if (entity.kind === 'region') return entity.region.name;
  if (entity.kind === 'camera') return entity.camera.name;
  return entity.object?.name ?? `${entity.instance.objectId}（目录中没有）`;
}
export function entityPosition(entity: WorldEntity): { x: number; y: number } {
  if (entity.kind === 'region') return { x: entity.region.x + entity.region.width / 2, y: entity.region.y + entity.region.height / 2 };
  if (entity.kind === 'camera') return { x: entity.camera.x, y: entity.camera.y };
  return { x: entity.instance.x, y: entity.instance.y };
}
export function worldEntities(project: WorkbenchProject): WorldEntity[] {
  return [
    ...project.world.instances.map(instance => { const object = instanceObject(project, instance); return { kind: instanceKind(project, instance), id: instance.id, instance, object } as WorldEntity; }),
    ...project.world.regions.map(region => ({ kind: 'region', id: region.id, region }) as WorldEntity),
    ...(project.world.cameras ?? []).map(camera => ({ kind: 'camera', id: camera.id, camera }) as WorldEntity),
  ];
}
export function findEntity(project: WorkbenchProject, id: string): WorldEntity | undefined { return worldEntities(project).find(entity => entity.id === id); }

/** 选择集的包围盒（世界坐标），用于“框选到视图”。 */
export function selectionBounds(project: WorkbenchProject, ids: readonly string[]): Bounds | undefined {
  const boxes = worldEntities(project).filter(entity => ids.includes(entity.id)).map(entity => entity.kind === 'region'
    ? { minX: entity.region.x, minY: entity.region.y, maxX: entity.region.x + entity.region.width, maxY: entity.region.y + entity.region.height }
    : (() => { const p = entityPosition(entity); return { minX: p.x - 64, minY: p.y - 64, maxX: p.x + 64, maxY: p.y + 64 }; })());
  if (!boxes.length) return undefined;
  return { minX: Math.min(...boxes.map(b => b.minX)), minY: Math.min(...boxes.map(b => b.minY)), maxX: Math.max(...boxes.map(b => b.maxX)), maxY: Math.max(...boxes.map(b => b.maxY)) };
}

let counter = 0;
export function nextWorldId(project: WorkbenchProject, prefix: 'instance' | 'region' | 'camera'): string {
  const used = new Set([...project.world.instances, ...project.world.regions, ...(project.world.cameras ?? [])].map(item => item.id));
  for (;;) { const id = `${prefix}.${Date.now().toString(36)}${(counter++).toString(36)}`; if (!used.has(id)) return id; }
}

/** 批量修改：只改选择集中的实例/区域/镜头，返回新工程；一次调用即一次撤销步骤。 */
export function patchEntities(project: WorkbenchProject, ids: readonly string[], patch: { instance?: Partial<WorldInstance>; region?: Partial<WorldRegion>; camera?: Partial<WorldCamera> }): WorkbenchProject {
  const set = new Set(ids);
  return { ...project, world: {
    ...project.world,
    instances: patch.instance ? project.world.instances.map(item => set.has(item.id) ? { ...item, ...patch.instance } : item) : project.world.instances,
    regions: patch.region ? project.world.regions.map(item => set.has(item.id) ? { ...item, ...patch.region } : item) : project.world.regions,
    cameras: patch.camera && project.world.cameras ? project.world.cameras.map(item => set.has(item.id) ? { ...item, ...patch.camera } : item) : project.world.cameras,
  } };
}
/** 平移选择集（区域平移左下角），坐标限制在地图范围内。 */
export function moveEntities(project: WorkbenchProject, ids: readonly string[], dx: number, dy: number): WorkbenchProject {
  const set = new Set(ids), b = worldBounds(project);
  const px = (x: number) => clamp(x, b.minX, b.maxX), py = (y: number) => clamp(y, b.minY, b.maxY);
  return { ...project, world: {
    ...project.world,
    instances: project.world.instances.map(item => set.has(item.id) ? { ...item, x: px(item.x + dx), y: py(item.y + dy) } : item),
    regions: project.world.regions.map(item => set.has(item.id) ? { ...item, x: clamp(item.x + dx, b.minX, b.maxX - item.width), y: clamp(item.y + dy, b.minY, b.maxY - item.height) } : item),
    cameras: project.world.cameras?.map(item => set.has(item.id) ? { ...item, x: px(item.x + dx), y: py(item.y + dy) } : item),
  } };
}
export function removeEntities(project: WorkbenchProject, ids: readonly string[]): WorkbenchProject {
  const set = new Set(ids);
  return { ...project, world: { ...project.world, instances: project.world.instances.filter(item => !set.has(item.id)), regions: project.world.regions.filter(item => !set.has(item.id)), cameras: project.world.cameras?.filter(item => !set.has(item.id)) } };
}
/** 复制选择集并整体平移到 (dx, dy)；返回新工程与新 ID（新 ID 成为新的选择集）。 */
export function duplicateEntities(project: WorkbenchProject, ids: readonly string[], dx: number, dy: number): { project: WorkbenchProject; ids: string[] } {
  let next = project; const created: string[] = [];
  for (const instance of project.world.instances.filter(item => ids.includes(item.id))) { const id = nextWorldId(next, 'instance'); created.push(id); next = { ...next, world: { ...next.world, instances: [...next.world.instances, { ...instance, id }] } }; }
  for (const region of project.world.regions.filter(item => ids.includes(item.id))) { const id = nextWorldId(next, 'region'); created.push(id); next = { ...next, world: { ...next.world, regions: [...next.world.regions, { ...region, id, name: uniqueName(next.world.regions.map(r => r.name), region.name) }] } }; }
  for (const camera of (project.world.cameras ?? []).filter(item => ids.includes(item.id))) { const id = nextWorldId(next, 'camera'); created.push(id); next = { ...next, world: { ...next.world, cameras: [...(next.world.cameras ?? []), { ...camera, id, name: uniqueName((next.world.cameras ?? []).map(c => c.name), camera.name) }] } }; }
  return { project: moveEntities(next, created, dx, dy), ids: created };
}
export function uniqueName(used: string[], base: string): string {
  const stem = base.replace(/\s+\d+$/, '');
  for (let n = 2; ; n++) { const name = `${stem} ${n}`; if (!used.includes(name)) return name; }
}
export function placeInstance(project: WorkbenchProject, objectId: string, x: number, y: number, owner: number, rotation = 270): { project: WorkbenchProject; id: string } {
  const b = worldBounds(project), id = nextWorldId(project, 'instance');
  const instance: WorldInstance = { id, objectId, x: clamp(Math.round(x), b.minX, b.maxX), y: clamp(Math.round(y), b.minY, b.maxY), owner, rotation };
  return { project: { ...project, world: { ...project.world, instances: [...project.world.instances, instance] } }, id };
}
/** 在区域内按网格排开 count 个实例（助手与批量放置共用）。 */
export function gridInRegion(region: WorldRegion, count: number, spacing = CELL): { x: number; y: number }[] {
  const columns = Math.max(1, Math.min(count, Math.floor(region.width / spacing) || 1));
  const cx = region.x + region.width / 2, cy = region.y + region.height / 2, rows = Math.ceil(count / columns);
  return Array.from({ length: count }, (_, i) => ({ x: Math.round(cx + (i % columns - (columns - 1) / 2) * spacing), y: Math.round(cy - (Math.floor(i / columns) - (rows - 1) / 2) * spacing) }));
}

/** 旧版原型（R07 及以前）把实例存为 0–100 的俯视比例；读取时换算为世界坐标，y 轴翻转为向北。 */
export function migrateLegacyWorld(world: WorkbenchProject['world']): WorkbenchProject['world'] {
  if (world.space === 'world') return world;
  const b = defaultBounds, w = b.maxX - b.minX, h = b.maxY - b.minY;
  const x = (v: number) => Math.round(b.minX + v / 100 * w), y = (v: number) => Math.round(b.maxY - v / 100 * h);
  return {
    ...world, space: 'world', bounds: b,
    instances: world.instances.map(i => ({ ...i, x: x(i.x), y: y(i.y) })),
    regions: world.regions.map(r => ({ ...r, x: x(r.x), y: y(r.y + r.height), width: Math.round(r.width / 100 * w), height: Math.round(r.height / 100 * h) })),
  };
}

export const placeableKinds: ObjectKind[] = ['unit', 'item', 'destructable', 'doodad'];

/** 物体图标：地图物体自带的本机图标优先，否则用其原版基础物体图标。 */
export function objectIcon(object: EditorObject | undefined): string | undefined {
  if (!object) return undefined;
  const own = typeof object.fields.icon === 'string' ? object.fields.icon : '';
  if (/^(?:\.\/)?local-data\/|^\/portraits\//.test(own)) return own;
  return catalogIcon(object);
}
export const isBuilding = (object?: EditorObject) => !!object && /建筑/.test(object.category);
