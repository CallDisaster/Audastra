import type { ModuleId, WorkbenchProject } from './types';
import { instanceObject } from './worldModel';

// “本次修改”：比较已保存基线与当前工程，按模块列出新增/删除/修改的记录及字段前后值，并支持逐项撤销。
// 纯逻辑，原生端对应“按成员/记录/字段的保存预览”：保存前让作者看清会写进地图的每一处改动。

export type ChangeKind = 'added' | 'removed' | 'modified';
export interface FieldChange { path: string; label: string; before: string; after: string }
export interface ChangeItem { key: string; group: string; kind: ChangeKind; title: string; module: ModuleId; target?: string; fields: FieldChange[]; summary?: string }
export interface ChangeGroup { id: string; label: string; module: ModuleId; items: ChangeItem[] }

type Row = { id: string; [key: string]: unknown };
type Collection = { id: string; label: string; module: ModuleId; path: string[]; title: (row: Row, project: WorkbenchProject) => string; target?: (row: Row) => string | undefined };
const text = (value: unknown) => typeof value === 'string' ? value : String(value ?? '');
const collections: Collection[] = [
  { id: 'graphs', label: '触发器', module: 'triggers', path: ['graphs'], title: row => text(row.name) },
  { id: 'objects', label: '物体', module: 'objects', path: ['objects'], title: row => `${text(row.name)}（${text(row.rawcode)}）` },
  { id: 'variables', label: '变量', module: 'variables', path: ['variables'], title: row => `udg_${text(row.name)}` },
  { id: 'sounds', label: '声音', module: 'sounds', path: ['sounds'], title: row => text(row.name) },
  { id: 'assets', label: '导入文件', module: 'assets', path: ['assets'], title: row => text(row.path) },
  { id: 'instances', label: '放置实例', module: 'world', path: ['world', 'instances'], title: (row, project) => `${instanceObject(project, { objectId: text(row.objectId) })?.name ?? text(row.objectId)} · 玩家 ${text(row.owner)}` },
  { id: 'regions', label: '区域', module: 'world', path: ['world', 'regions'], title: row => text(row.name) },
  { id: 'cameras', label: '镜头', module: 'world', path: ['world', 'cameras'], title: row => text(row.name) },
  { id: 'chapters', label: '战役章节', module: 'campaign', path: ['campaign', 'chapters'], title: row => `${text(row.title)} ${text(row.subtitle)}` },
  { id: 'aiBuild', label: 'AI 建造', module: 'ai', path: ['ai', 'build'], title: row => text(row.objectId), target: () => undefined },
  { id: 'aiWaves', label: 'AI 进攻波次', module: 'ai', path: ['ai', 'waves'], title: row => text(row.name), target: () => undefined },
];
/** 标量区块：整块作为一项，撤销时整块恢复。 */
const sections: { id: string; title: string; module: ModuleId; path: string[]; keys?: string[] }[] = [
  { id: 'name', title: '工程名称', module: 'triggers', path: [], keys: ['name'] },
  { id: 'settings', title: '地图设置', module: 'world', path: ['settings'] },
  { id: 'campaign', title: '战役信息', module: 'campaign', path: ['campaign'], keys: ['name', 'author', 'description', 'difficulty'] },
  { id: 'ai', title: 'AI 设置', module: 'ai', path: ['ai'], keys: ['name', 'race', 'repair', 'retreat', 'workers', 'hero', 'skills'] },
  { id: 'bounds', title: '地图范围', module: 'world', path: ['world'], keys: ['bounds'] },
];
const labels: Record<string, string> = {
  name: '名称', folder: '文件夹', enabled: '允许', initiallyOn: '初始打开', x: 'X', y: 'Y', z: '高度', owner: '所属玩家', rotation: '朝向', scale: '缩放', life: '生命%', mana: '魔法%', level: '等级', objectId: '物体',
  width: '宽', height: '高', color: '颜色', distance: '距离', aoa: '俯仰', fov: '视野', health: '生命值', speed: '移动速度', rawcode: 'rawcode', type: '类型', array: '数组', size: '大小', initial: '初始值',
  volume: '音量', pitch: '音调', loop: '循环', spatial: '3D', fadeIn: '淡入', fadeOut: '淡出', path: '路径', target: '目标版本', language: '脚本语言', players: '玩家数', fog: '战争迷雾', author: '作者',
  'fields.armor': '护甲', 'fields.attack': '攻击', 'fields.cost': '价格', 'fields.range': '射程', 'fields.mana': '魔法', 'fields.tooltip': '提示', 'fields.abilities': '技能', 'fields.model': '模型', 'fields.icon': '图标',
};
export const fieldLabel = (path: string) => labels[path] ?? labels[path.split('.').at(-1)!] ?? path;

export function formatValue(value: unknown): string {
  if (value === undefined || value === null) return '—';
  if (typeof value === 'boolean') return value ? '是' : '否';
  if (typeof value === 'number') return Number.isInteger(value) ? String(value) : String(Math.round(value * 1000) / 1000);
  const out = typeof value === 'string' ? value : Array.isArray(value) && value.every(v => typeof v !== 'object') ? value.join('，') : JSON.stringify(value);
  return out.length > 60 ? `${out.slice(0, 57)}…` : out || '（空）';
}
const plain = (value: unknown): value is Record<string, unknown> => !!value && typeof value === 'object' && !Array.isArray(value);
const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
function get(root: unknown, path: string[]): unknown { return path.reduce<unknown>((node, key) => plain(node) ? node[key] : undefined, root); }
function set<T>(root: T, path: string[], value: unknown): T {
  if (!path.length) return value as T;
  const [head, ...rest] = path, node = root as Record<string, unknown>;
  return { ...node, [head]: set(node[head], rest, value) } as T;
}
/** 逐字段比较，嵌套对象展开一层（fields.armor），数组与更深对象整体比较。 */
export function fieldChanges(before: Record<string, unknown>, after: Record<string, unknown>, skip: string[] = ['id'], prefix = '', depth = 0): FieldChange[] {
  const out: FieldChange[] = [];
  for (const key of [...new Set([...Object.keys(before), ...Object.keys(after)])]) {
    if (skip.includes(key) || same(before[key], after[key])) continue;
    const path = prefix + key;
    if (depth < 1 && plain(before[key]) && plain(after[key])) out.push(...fieldChanges(before[key] as Record<string, unknown>, after[key] as Record<string, unknown>, [], `${path}.`, depth + 1));
    else out.push({ path, label: fieldLabel(path), before: formatValue(before[key]), after: formatValue(after[key]) });
  }
  return out;
}
function graphSummary(before: Row, after: Row): string | undefined {
  const parts: string[] = [];
  for (const [key, label] of [['nodes', '节点'], ['edges', '连线'], ['conditions', '条件']] as const) {
    const a = (before[key] as Row[] | undefined) ?? [], b = (after[key] as Row[] | undefined) ?? [];
    const ia = new Map(a.map(x => [x.id, x])), ib = new Map(b.map(x => [x.id, x]));
    const added = b.filter(x => !ia.has(x.id)).length, removed = a.filter(x => !ib.has(x.id)).length, changed = b.filter(x => ia.has(x.id) && !same(ia.get(x.id), x)).length;
    const bits = [added && `新增 ${added}`, removed && `删除 ${removed}`, changed && `修改 ${changed}`].filter(Boolean);
    if (bits.length) parts.push(`${label} ${bits.join(' · ')}`);
  }
  return parts.join('；') || undefined;
}

export function diffProjects(before: WorkbenchProject, after: WorkbenchProject): ChangeGroup[] {
  const groups: ChangeGroup[] = [];
  const project: ChangeItem[] = [];
  for (const section of sections) {
    const a = get(before, section.path) as Record<string, unknown> | undefined, b = get(after, section.path) as Record<string, unknown> | undefined;
    if (!a || !b) continue;
    const pick = (o: Record<string, unknown>) => section.keys ? Object.fromEntries(section.keys.map(k => [k, o[k]])) : o;
    const fields = fieldChanges(pick(a), pick(b), []);
    if (fields.length) project.push({ key: `section:${section.id}`, group: 'project', kind: 'modified', title: section.title, module: section.module, fields });
  }
  if (project.length) groups.push({ id: 'project', label: '工程与设置', module: 'triggers', items: project });
  for (const c of collections) {
    const a = (get(before, c.path) as Row[] | undefined) ?? [], b = (get(after, c.path) as Row[] | undefined) ?? [];
    const ia = new Map(a.map(x => [x.id, x])), ib = new Map(b.map(x => [x.id, x]));
    const items: ChangeItem[] = [];
    const target = (row: Row) => c.target ? c.target(row) : row.id;
    for (const row of b) {
      const old = ia.get(row.id);
      if (!old) { items.push({ key: `${c.id}:${row.id}`, group: c.id, kind: 'added', title: c.title(row, after), module: c.module, target: target(row), fields: [] }); continue; }
      if (same(old, row)) continue;
      const fields = c.id === 'graphs' ? fieldChanges(old, row, ['id', 'nodes', 'edges', 'conditions', 'x', 'y', 'zoom']) : fieldChanges(old, row);
      const summary = c.id === 'graphs' ? graphSummary(old, row) : undefined;
      if (fields.length || summary) items.push({ key: `${c.id}:${row.id}`, group: c.id, kind: 'modified', title: c.title(row, after), module: c.module, target: target(row), fields, summary });
    }
    for (const row of a) if (!ib.has(row.id)) items.push({ key: `${c.id}:${row.id}`, group: c.id, kind: 'removed', title: c.title(row, before), module: c.module, target: undefined, fields: [] });
    if (items.length) groups.push({ id: c.id, label: c.label, module: c.module, items });
  }
  return groups;
}
export const changeCount = (groups: ChangeGroup[]) => groups.reduce((sum, g) => sum + g.items.length, 0);

/** 把一项改动恢复为基线状态；返回新工程（作为一次普通编辑进入撤销历史）。 */
export function revertChange(current: WorkbenchProject, baseline: WorkbenchProject, item: ChangeItem): WorkbenchProject {
  if (item.group === 'project') {
    const section = sections.find(s => `section:${s.id}` === item.key)!;
    const base = get(baseline, section.path) as Record<string, unknown>, now = get(current, section.path) as Record<string, unknown>;
    const restored = section.keys ? { ...now, ...Object.fromEntries(section.keys.map(k => [k, base[k]])) } : base;
    return set(current, section.path, restored);
  }
  const c = collections.find(x => x.id === item.group)!, id = item.key.slice(c.id.length + 1);
  const now = [...((get(current, c.path) as Row[] | undefined) ?? [])], base = (get(baseline, c.path) as Row[] | undefined) ?? [];
  const index = now.findIndex(x => x.id === id), original = base.find(x => x.id === id);
  if (item.kind === 'added') { if (index >= 0) now.splice(index, 1); }
  else if (item.kind === 'modified' && original) { if (index >= 0) now[index] = original; }
  else if (item.kind === 'removed' && original && index < 0) {
    // 尽量放回原来的位置：插在基线中它前一项的后面。
    const before = base.slice(0, base.indexOf(original)).reverse().find(x => now.some(y => y.id === x.id));
    now.splice(before ? now.findIndex(y => y.id === before.id) + 1 : 0, 0, original);
  }
  return set(current, c.path, now);
}
