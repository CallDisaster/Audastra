import type { EditorObject, ModuleId, WorkbenchProject } from './types';
import { findObject, pickableObjects } from './objectCatalog';
import { referencesTo } from './references';
import { gridInRegion, instanceObject, patchEntities, placeInstance } from './worldModel';

// 助手面板的原型解析器：只识别几类固定句式，把自然语言翻成与界面相同的编辑命令。
// 它不是语言模型；原生端由 Agent（MCP）调用同一批命令。无论来源如何，改动都先作为“提议”进入审阅，确认后才写入。

export interface AssistantResult {
  reply: string;
  /** 查询结果：点击跳到对应模块。 */
  results?: { title: string; detail: string; module: ModuleId; target?: string }[];
  /** 编辑提议：确认后作为一次撤销步骤应用。 */
  proposal?: { label: string; project: WorkbenchProject };
  /** 选择请求：直接改世界选择集（不改数据）。 */
  select?: string[];
  /** 对应的命令调用，展示给作者，也是原生 Agent 接口的形状。 */
  calls: string[];
}
export interface AssistantContext { project: WorkbenchProject; selection: string[]; revision: string; owner: number }

export const assistantExamples = [
  '查找未使用的变量', '守卫步兵 的引用', '选中所有 巡林射手', '把选中的单位改为玩家 2',
  '把 守卫步兵 的生命值增加 50', '在 防守区域 放置 4 个 守卫步兵',
];
const clean = (value: string) => value.trim().replace(/^[“"「]|[”"」]$/g, '').replace(/^(所有的?|全部的?)/, '').trim();

/** 按名称或 rawcode 找物体：地图物体优先，其次原版；先精确后包含。 */
export function objectByName(project: WorkbenchProject, name: string): EditorObject | undefined {
  const q = clean(name); if (!q) return undefined;
  const direct = findObject(project, q); if (direct) return direct;
  const pools = [project.objects, ...(['unit', 'item', 'destructable', 'doodad', 'ability'] as const).map(kind => pickableObjects(project, kind).filter(o => !o.custom))];
  for (const pool of pools) { const exact = pool.find(o => o.name === q); if (exact) return exact; }
  for (const pool of pools) { const partial = pool.find(o => o.name.includes(q)); if (partial) return partial; }
  return undefined;
}
const instancesOf = (project: WorkbenchProject, object: EditorObject) => project.world.instances.filter(i => { const o = instanceObject(project, i); return o && o.kind === object.kind && o.rawcode === object.rawcode && o.custom === object.custom; });
const selectedInstances = (ctx: AssistantContext) => ctx.project.world.instances.filter(i => ctx.selection.includes(i.id));

const fieldMap: Record<string, { key: 'health' | 'speed' | 'armor' | 'attack' | 'cost'; label: string }> = {
  生命值: { key: 'health', label: '生命值' }, 生命: { key: 'health', label: '生命值' }, 移动速度: { key: 'speed', label: '移动速度' }, 速度: { key: 'speed', label: '移动速度' },
  护甲: { key: 'armor', label: '护甲' }, 攻击力: { key: 'attack', label: '攻击' }, 攻击: { key: 'attack', label: '攻击' }, 价格: { key: 'cost', label: '价格' }, 金币: { key: 'cost', label: '价格' },
};

export function interpret(input: string, ctx: AssistantContext): AssistantResult {
  const text = input.trim(), { project } = ctx;
  if (!text) return { reply: '请输入指令。', calls: [] };

  if (/(未使用|没用到|没有使用|没被使用)/.test(text) && /变量/.test(text)) {
    const unused = project.variables.filter(v => !referencesTo(project, v.id).length);
    return { reply: unused.length ? `找到 ${unused.length} 个静态未检出引用的变量。自定义脚本中的动态引用不在统计内，删除前请确认。` : '所有变量都有静态引用。', results: unused.map(v => ({ title: `udg_${v.name}`, detail: `${v.type}${v.array ? ' 数组' : ''} · ${v.folder}`, module: 'variables', target: v.id })), calls: ['references.unused(kind="variable")'] };
  }

  const refMatch = text.match(/^(?:列出|查找|找出|查看)?\s*(.+?)\s*(?:的引用|在哪(?:里)?被?(?:引用|用到|使用)|被哪些.*引用)/);
  if (refMatch) {
    const name = clean(refMatch[1]);
    const object = objectByName(project, name), variable = project.variables.find(v => v.name === name || `udg_${v.name}` === name), region = project.world.regions.find(r => r.name === name), sound = project.sounds.find(s => s.name === name);
    const id = variable?.id ?? region?.id ?? sound?.id ?? object?.id;
    if (!id) return { reply: `没有找到名为“${name}”的物体、变量、区域或声音。`, calls: [`catalog.find(name="${name}")`] };
    const refs = referencesTo(project, id, object && !object.custom ? [object.rawcode] : []);
    return { reply: refs.length ? `“${name}”有 ${refs.length} 处静态引用。` : `“${name}”没有静态引用（动态脚本引用不在统计内）。`, results: refs.map(r => ({ title: r.label, detail: r.detail, module: r.module, target: r.target })), calls: [`references.of(id="${id}")`] };
  }

  const selectMatch = text.match(/^选中\s*(?:所有|全部)?\s*的?\s*(.+)$/);
  if (selectMatch) {
    const object = objectByName(project, selectMatch[1]);
    if (!object) return { reply: `没有找到物体“${clean(selectMatch[1])}”。`, calls: [] };
    const ids = instancesOf(project, object).map(i => i.id);
    return { reply: ids.length ? `已选中 ${ids.length} 个“${object.name}”。` : `地图上没有放置“${object.name}”。`, select: ids, calls: [`world.query(object="${object.custom ? object.id : object.rawcode}")`, 'world.select(ids)'] };
  }

  const ownerMatch = text.match(/^把\s*(.+?)\s*(?:全部|都)?\s*(?:改为|改成|设为|换成|交给)\s*玩家\s*(\d+)/);
  if (ownerMatch) {
    const owner = Number(ownerMatch[2]);
    if (!(owner >= 1 && owner <= 12) && owner !== 13 && owner !== 16) return { reply: '玩家编号应为 1–12，或 13（中立敌对）、16（中立被动）。', calls: [] };
    const bySelection = /选中|所选|选择的/.test(ownerMatch[1]);
    const object = bySelection ? undefined : objectByName(project, ownerMatch[1]);
    // 说“单位”只改单位；说“对象/实例”或未指明类别时改全部所选实例。
    const unitsOnly = bySelection && /单位/.test(ownerMatch[1]);
    const targets = bySelection ? selectedInstances(ctx).filter(i => !unitsOnly || (instanceObject(project, i)?.kind ?? 'unit') === 'unit') : object ? instancesOf(project, object) : [];
    if (!targets.length) return { reply: bySelection ? (unitsOnly ? '当前选择里没有单位。' : '当前没有选中放置实例。先在世界页选择对象。') : `地图上没有找到“${clean(ownerMatch[1])}”的放置实例。`, calls: [] };
    const ids = targets.map(i => i.id);
    return { reply: `将 ${ids.length} 个实例的所属玩家改为 ${owner}。请在下方确认。`, proposal: { label: `助手：${ids.length} 个实例改为玩家 ${owner}`, project: patchEntities(project, ids, { instance: { owner } }) }, calls: [`world.setOwner(ids=[${ids.length} 项], owner=${owner}, baseRevision="${ctx.revision}")`] };
  }

  const fieldMatch = text.match(/^把\s*(.+?)\s*的\s*(生命值|生命|移动速度|速度|护甲|攻击力|攻击|价格|金币)\s*(改为|改成|设为|增加|提高|减少|降低)\s*(-?\d+(?:\.\d+)?)/);
  if (fieldMatch) {
    const object = objectByName(project, fieldMatch[1]), field = fieldMap[fieldMatch[2]], amount = Number(fieldMatch[4]);
    if (!object) return { reply: `没有找到物体“${clean(fieldMatch[1])}”。`, calls: [] };
    if (!object.custom || !project.objects.some(o => o.id === object.id)) return { reply: `“${object.name}”是原版物体。原型只修改地图物体；原生端会先在物编中为它创建修改记录，再改字段。`, calls: [`objects.override(kind="${object.kind}", rawcode="${object.rawcode}")`] };
    const current = field.key === 'health' || field.key === 'speed' ? object[field.key] : Number(object.fields[field.key] ?? 0);
    const mode = fieldMatch[3], next = /增加|提高/.test(mode) ? current + amount : /减少|降低/.test(mode) ? current - amount : amount;
    if (next < 0) return { reply: `${field.label}不能小于 0（当前 ${current}）。`, calls: [] };
    const updated = project.objects.map(o => o.id !== object.id ? o : field.key === 'health' || field.key === 'speed' ? { ...o, [field.key]: next } : { ...o, fields: { ...o.fields, [field.key]: next } });
    return { reply: `${object.name} 的${field.label}：${current} → ${next}。请在下方确认。`, proposal: { label: `助手：${object.name} ${field.label} ${current}→${next}`, project: { ...project, objects: updated } }, calls: [`objects.setField(id="${object.id}", field="${field.key}", value=${next}, expected=${current}, baseRevision="${ctx.revision}")`] };
  }

  const placeMatch = text.match(/^在\s*(.+?)\s*(?:放置|摆放|创建|放)\s*(\d+)\s*个\s*(.+?)(?:[，,\s]+(?:给|属于)?\s*玩家\s*(\d+))?$/);
  if (placeMatch) {
    const region = project.world.regions.find(r => r.name === clean(placeMatch[1])), count = Number(placeMatch[2]), object = objectByName(project, placeMatch[3]);
    if (!region) return { reply: `没有找到区域“${clean(placeMatch[1])}”。`, calls: [] };
    if (!object || !['unit', 'item', 'destructable', 'doodad'].includes(object.kind)) return { reply: `没有找到可摆放的物体“${clean(placeMatch[3])}”。`, calls: [] };
    if (count < 1 || count > 64) return { reply: '一次最多放置 64 个。', calls: [] };
    const owner = placeMatch[4] ? Number(placeMatch[4]) : ctx.owner;
    let next = project; for (const p of gridInRegion(region, count)) next = placeInstance(next, object.custom ? object.id : object.rawcode, p.x, p.y, owner).project;
    return { reply: `在“${region.name}”按网格放置 ${count} 个${object.name}（玩家 ${owner}）。请在下方确认。`, proposal: { label: `助手：在${region.name}放置 ${count} 个${object.name}`, project: next }, calls: [`world.place(object="${object.custom ? object.id : object.rawcode}", region="${region.id}", count=${count}, owner=${owner}, layout="grid", baseRevision="${ctx.revision}")`] };
  }

  return { reply: '原型只识别下面几类指令（原生端接入 Agent 后不受此限制）：', results: assistantExamples.map(example => ({ title: example, detail: '点击填入', module: 'world' as ModuleId, target: `example:${example}` })), calls: [] };
}
