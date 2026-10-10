import { uid } from '../studio/model';
import type { Definition, Graph, GraphNode } from '../studio/model';
import type { ParameterBinding } from './triggerCode';
import type { WorkbenchProject } from './types';
import { bindingLabel, objectKindForType } from './valueSources';
import { enumChoices, resolveFieldType } from './parameterFields';
import { findObject } from './objectCatalog';

// 传统 ECA 视图与蓝图共用同一张图：这里只从执行连线推导事件/条件/动作块，并以改写执行连线实现重排。
type Field = Definition['fields'][number];
export type Operand = { value: unknown; binding?: ParameterBinding };
export type CompareOp = '==' | '!=' | '<' | '<=' | '>' | '>=';
export type TriggerCondition =
  | { id: string; kind: 'compare'; valueType: string; left: Operand; op: CompareOp; right: Operand; enabled?: boolean }
  | { id: string; kind: 'and' | 'or'; children: TriggerCondition[]; enabled?: boolean };

export const operatorLabels: Record<CompareOp, string> = { '==': '等于', '!=': '不等于', '<': '小于', '<=': '小于或等于', '>': '大于', '>=': '大于或等于' };
const orderedTypes = ['integer', 'real', 'number'];
export const operatorsFor = (valueType: string): CompareOp[] => orderedTypes.includes(valueType) ? ['==', '!=', '<', '<=', '>', '>='] : ['==', '!='];
export const conditionTemplates: { id: string; title: string; kind: 'compare' | 'and' | 'or'; valueType?: string; left?: unknown; right?: unknown }[] = [
  { id: 'integer', title: '整数比较', kind: 'compare', valueType: 'integer', left: 0, right: 0 },
  { id: 'real', title: '实数比较', kind: 'compare', valueType: 'real', left: 0, right: 0 },
  { id: 'boolean', title: '布尔值比较', kind: 'compare', valueType: 'boolean', left: true, right: true },
  { id: 'string', title: '字符串比较', kind: 'compare', valueType: 'string', left: '', right: '' },
  { id: 'unitcode', title: '单位类型比较', kind: 'compare', valueType: 'unitcode', left: '', right: '' },
  { id: 'player', title: '玩家比较', kind: 'compare', valueType: 'player', left: 'Player(0)', right: 'Player(0)' },
  { id: 'and', title: 'And - 多项条件', kind: 'and' },
  { id: 'or', title: 'Or - 多项条件', kind: 'or' },
];
export const conditionTypeTitle = (valueType: string) => conditionTemplates.find(item => item.valueType === valueType)?.title.replace('比较', '') ?? valueType;
export function makeCondition(templateId: string): TriggerCondition {
  const template = conditionTemplates.find(item => item.id === templateId) ?? conditionTemplates[0];
  if (template.kind !== 'compare') return { id: uid(), kind: template.kind, children: [] };
  return { id: uid(), kind: 'compare', valueType: template.valueType!, op: '==', left: { value: template.left }, right: { value: template.right } };
}
export const graphConditions = (graph: Graph): TriggerCondition[] => Array.isArray(graph.conditions) ? graph.conditions as TriggerCondition[] : [];

/** 条件树的定位：返回所在列表与下标，便于同级重排、删除和嵌套添加。 */
export function mapConditions(list: TriggerCondition[], id: string, change: (siblings: TriggerCondition[], index: number) => TriggerCondition[]): TriggerCondition[] {
  const index = list.findIndex(item => item.id === id);
  if (index >= 0) return change(list, index);
  return list.map(item => item.kind === 'compare' ? item : { ...item, children: mapConditions(item.children, id, change) });
}
export function findCondition(list: TriggerCondition[], id: string): TriggerCondition | undefined {
  for (const item of list) {
    if (item.id === id) return item;
    if (item.kind !== 'compare') { const found = findCondition(item.children, id); if (found) return found; }
  }
  return undefined;
}

// ---- 语句模板 ----
// 定义可自带 `category` 与 `sentence`（`{字段或端口 id}` 占位）；内置核心节点在此补齐，其他节点回退为“标题：参数…”。
const builtinSentences: Record<string, { category: string; text: string }> = {
  'core.interval': { category: '时间', text: '每隔 {interval} 秒触发（循环：{repeat}）' },
  'core.branch': { category: '通用', text: 'If（{expression}）成立' },
  'core.spawn': { category: '单位', text: '创建 {count} 个 {template} 在 {position}' },
  'core.order': { category: '单位组', text: '命令 {objects} 执行 {command} 到 {target}' },
  'core.log': { category: '游戏', text: '对所有玩家显示文本：{message}' },
  'core.number': { category: '数值', text: '{name} = {value}' },
};
export type SentencePart = { text: string } | { field: Field } | { port: string; label: string };
export function sentenceOf(definition: Definition | undefined): { category: string; parts: SentencePart[] } {
  if (!definition) return { category: '未知', parts: [{ text: '缺少定义' }] };
  const builtin = builtinSentences[definition.id];
  const template = typeof definition.sentence === 'string' ? definition.sentence : builtin?.text;
  const category = typeof definition.category === 'string' ? definition.category : builtin?.category ?? definition.namespace;
  if (!template) {
    const parts: SentencePart[] = [{ text: definition.title }];
    definition.fields.forEach((field, index) => parts.push({ text: index ? '，' + field.label + ' ' : '：' + field.label + ' ' }, { field }));
    return { category, parts };
  }
  const parts: SentencePart[] = [];
  for (const piece of template.split(/(\{[A-Za-z0-9_.-]+\})/)) {
    const match = piece.match(/^\{(.+)\}$/);
    if (!match) { if (piece) parts.push({ text: piece }); continue; }
    const field = definition.fields.find(item => item.id === match[1]);
    const port = definition.ports.find(item => item.id === match[1] && item.direction === 'in' && item.type !== 'exec');
    if (field) parts.push({ field });
    else if (port) parts.push({ port: port.id, label: port.label });
    else parts.push({ text: piece });
  }
  return { category, parts };
}

function enumLabel(field: Field, value: unknown): string | undefined {
  return enumChoices(field).find(choice => choice.value === String(value))?.label;
}
export type TokenState = 'set' | 'missing' | 'unresolved';
/** 参数在语句中的显示文字与状态；缺值/未解析在 UI 中以红色标出，对应原版红色下划线。 */
export function parameterDisplay(node: GraphNode, field: Field, project: WorkbenchProject): { text: string; state: TokenState } {
  const binding = (node.bindings as Record<string, ParameterBinding> | undefined)?.[field.id];
  if (binding && binding.mode !== 'constant') return { text: bindingLabel(binding, project), state: 'set' };
  const linked = node.definition === 'core.number' && field.id === 'value' && project.variables.find(v => v.name === node.values.name || v.id === node.values.name);
  if (linked) return { text: linked.initial, state: 'set' };
  const value = node.values[field.id] ?? field.default;
  const type = resolveFieldType(field, node, project.variables);
  if (type.startsWith('globalvar')) return { text: '（选择变量）', state: 'missing' };
  return operandDisplay(value, type, project, field);
}
export function operandDisplay(value: unknown, type: string, project: WorkbenchProject, field?: Field): { text: string; state: TokenState } {
  if (value === undefined || value === '') return { text: '（未设置）', state: 'missing' };
  if (typeof value === 'boolean') return { text: value ? '真' : '假', state: 'set' };
  const enumText = field && enumLabel(field, value);
  if (enumText) return { text: enumText, state: 'set' };
  if (objectKindForType(type)) {
    const object = findObject(project, value, objectKindForType(type));
    if (object) return { text: object.name, state: 'set' };
    return /^[\x20-\x7e]{4}$/.test(String(value)) ? { text: `'${String(value)}'`, state: 'unresolved' } : { text: String(value), state: 'unresolved' };
  }
  if (type === 'rect' || type === 'location') {
    const region = project.world.regions.find(item => item.id === value || item.name === value);
    if (region) return { text: region.name, state: 'set' };
    return /^\s*-?\d+(\.\d+)?\s*[,，]\s*-?\d+(\.\d+)?\s*$/.test(String(value)) ? { text: `坐标(${String(value)})`, state: 'set' } : { text: String(value), state: 'unresolved' };
  }
  if (type === 'player') { const match = String(value).match(/^Player\((\d+)\)$/); if (match) return { text: playerName(Number(match[1])), state: 'set' }; }
  if (type === 'unit' || type === 'item') { const instance = project.world.instances.find(item => item.id === value); if (instance) return { text: `${project.objects.find(o => o.id === instance.objectId)?.name ?? '实例'} ${instance.id.replace(/^instance\./, '#')}`, state: 'set' }; }
  if (type === 'trigger') { const target = project.graphs.find(item => item.id === value); return target ? { text: target.name, state: 'set' } : { text: String(value), state: 'unresolved' }; }
  if (type === 'sound') { const sound = project.sounds.find(item => item.id === value); if (sound) return { text: sound.name, state: 'set' }; }
  if (typeof value === 'number') return { text: Number.isInteger(value) ? String(value) : value.toFixed(2), state: 'set' };
  return { text: type === 'string' || type === 'text' ? `"${String(value)}"` : String(value), state: 'set' };
}
const playerColors = ['红色', '蓝色', '青色', '紫色', '黄色', '橙色', '绿色', '粉色', '灰色', '浅蓝色', '深绿色', '棕色'];
export const playerName = (index: number) => `玩家${index + 1}（${playerColors[index] ?? `#${index + 1}`}）`;

// ---- ECA 块推导 ----
export type EcaItem = { node: GraphNode; definition?: Definition; blocks: EcaBlock[]; shared: boolean };
export type EcaBlock = { owner: string; port: string; label: string; items: EcaItem[]; locked: boolean; detached?: boolean };
export type EcaDocument = { events: GraphNode[]; actions: EcaBlock[]; detached: EcaBlock | null; unlinkedValues: GraphNode[] };
export type BlockRef = { owner: string; port: string };

const execOut = (definition?: Definition) => definition?.ports.filter(port => port.direction === 'out' && port.type === 'exec') ?? [];
export const hasExecInput = (definition?: Definition) => !!definition?.ports.some(port => port.direction === 'in' && port.type === 'exec');
/** 顺序执行的后续端口：普通动作取第一个执行输出；流程节点只把 then/done/completed 视为后续，其余输出是子块。 */
export function continuationPort(definition?: Definition): string | undefined {
  const out = execOut(definition);
  if (definition?.kind === 'flow') return out.find(port => ['then', 'done', 'completed', 'next'].includes(port.id))?.id;
  return out[0]?.id;
}
const blockPorts = (definition?: Definition) => definition?.kind === 'flow' ? execOut(definition).filter(port => port.id !== continuationPort(definition)) : [];
const isExecEdge = (graph: Graph, definitions: Map<string, Definition>, edge: Graph['edges'][number]) => {
  const source = graph.nodes.find(node => node.id === edge.from);
  return !!definitions.get(source?.definition ?? '')?.ports.some(port => port.id === edge.output && port.direction === 'out' && port.type === 'exec');
};

export function buildEca(graph: Graph, list: Definition[]): EcaDocument {
  const definitions = new Map(list.map(item => [item.id, item]));
  const nodes = new Map(graph.nodes.map(node => [node.id, node]));
  const visited = new Set<string>();
  const targets = (owner: string, port: string) => graph.edges.filter(edge => edge.from === owner && edge.output === port).map(edge => nodes.get(edge.to)).filter((node): node is GraphNode => !!node && hasExecInput(definitions.get(node.definition)));
  const chain = (owner: string, port: string, label: string, starts: GraphNode[], detached = false): EcaBlock => {
    const block: EcaBlock = { owner, port, label, items: [], locked: starts.length > 1, detached };
    for (const start of starts) {
      let current: GraphNode | undefined = start;
      while (current) {
        const definition = definitions.get(current.definition);
        if (visited.has(current.id)) { block.items.push({ node: current, definition, blocks: [], shared: true }); block.locked = true; break; }
        visited.add(current.id);
        const item: EcaItem = { node: current, definition, blocks: [], shared: false };
        block.items.push(item);
        item.blocks = blockPorts(definition).map(port => chain(current!.id, port.id, `${port.label} - 动作`, targets(current!.id, port.id)));
        const next = continuationPort(definition);
        const following: GraphNode[] = next ? targets(current.id, next) : [];
        if (following.length > 1) block.locked = true;
        current = following[0];
        if (following.length > 1) following.slice(1).forEach(extra => { if (!visited.has(extra.id)) block.items.push({ node: extra, definition: definitions.get(extra.definition), blocks: [], shared: true }); });
      }
    }
    return block;
  };
  const events = graph.nodes.filter(node => definitions.get(node.definition)?.kind === 'event');
  const actions: EcaBlock[] = [];
  events.forEach((event, index) => {
    const port = continuationPort(definitions.get(event.definition)) ?? '';
    const block = chain(event.id, port, index === 0 ? '动作' : `动作 · 事件 ${index + 1}`, port ? targets(event.id, port) : []);
    if (index === 0 || block.items.length) actions.push(block);
  });
  const roots = graph.nodes.filter(node => {
    const definition = definitions.get(node.definition);
    return !visited.has(node.id) && hasExecInput(definition) && !graph.edges.some(edge => edge.to === node.id && isExecEdge(graph, definitions, edge));
  });
  // 与代码生成一致：没有事件时，无前驱的动作链在地图初始化时执行一次；有事件时它们不会执行。
  if (!events.length) actions.push(chain('', '', '动作', roots));
  const detached = events.length && roots.length ? chain('', '', '未连接的动作（不会执行）', roots, true) : null;
  if (detached) detached.locked = true;
  const unlinkedValues = graph.nodes.filter(node => definitions.get(node.definition)?.kind === 'value' && !graph.edges.some(edge => edge.from === node.id));
  return { events, actions, detached, unlinkedValues };
}
export function findBlock(document: EcaDocument, ref: BlockRef): EcaBlock | undefined {
  const search = (blocks: EcaBlock[]): EcaBlock | undefined => {
    for (const block of blocks) {
      if (block.owner === ref.owner && block.port === ref.port) return block;
      for (const item of block.items) { const found = search(item.blocks); if (found) return found; }
    }
    return undefined;
  };
  return search(document.detached ? [...document.actions, document.detached] : document.actions);
}
/** 行所在的块与下标。 */
export function locateItem(document: EcaDocument, nodeId: string): { block: EcaBlock; index: number } | undefined {
  const search = (blocks: EcaBlock[]): { block: EcaBlock; index: number } | undefined => {
    for (const block of blocks) {
      const index = block.items.findIndex(item => item.node.id === nodeId && !item.shared);
      if (index >= 0) return { block, index };
      for (const item of block.items) { const found = search(item.blocks); if (found) return found; }
    }
    return undefined;
  };
  return search(document.detached ? [...document.actions, document.detached] : document.actions);
}
export function subtreeIds(item: EcaItem): string[] {
  return [item.node.id, ...item.blocks.flatMap(block => block.items.filter(child => !child.shared).flatMap(subtreeIds))];
}

/** 以新的顺序重写一个块的执行连线；数据连线与块外连线不变。 */
export function setChain(graph: Graph, list: Definition[], block: BlockRef & { items: { node: GraphNode }[] }, ids: string[]): Graph {
  const definitions = new Map(list.map(item => [item.id, item]));
  const byId = new Map(graph.nodes.map(node => [node.id, node]));
  const affected = new Set([...block.items.map(item => item.node.id), ...ids]);
  const cont = (id: string) => continuationPort(definitions.get(byId.get(id)?.definition ?? ''));
  ids.slice(0, -1).forEach(id => { if (!cont(id)) throw new Error(`“${definitions.get(byId.get(id)?.definition ?? '')?.title ?? id}”没有后续执行端口，只能放在块末尾`); });
  const execInput = (id: string) => definitions.get(byId.get(id)?.definition ?? '')?.ports.find(port => port.direction === 'in' && port.type === 'exec')?.id;
  const edges = graph.edges.filter(edge => {
    if (edge.from === block.owner && edge.output === block.port) return false;
    if (affected.has(edge.from) && edge.output === cont(edge.from) && affected.has(edge.to)) return false;
    if (affected.has(edge.to) && edge.input === execInput(edge.to) && isExecEdge(graph, definitions, edge)) return false;
    return true;
  });
  const links: Graph['edges'] = [];
  ids.forEach((id, index) => {
    const input = execInput(id);
    if (!input) throw new Error('该节点没有执行输入');
    if (index === 0 && !block.owner) return;
    const from = index === 0 ? block.owner : ids[index - 1];
    const output = index === 0 ? block.port : cont(ids[index - 1])!;
    links.push({ id: uid(), from, output, to: id, input });
  });
  return { ...graph, edges: [...edges, ...links] };
}
export function insertNode(graph: Graph, list: Definition[], block: EcaBlock, index: number, node: GraphNode): Graph {
  const ids = block.items.filter(item => !item.shared).map(item => item.node.id);
  ids.splice(Math.max(0, Math.min(index, ids.length)), 0, node.id);
  return setChain({ ...graph, nodes: [...graph.nodes, node] }, list, block, ids);
}
export function removeItems(graph: Graph, list: Definition[], nodeIds: string[]): Graph {
  let next = graph;
  const removed = new Set<string>();
  for (const id of nodeIds) {
    const location = locateItem(buildEca(next, list), id);
    if (!location) continue;
    const { block, index } = location;
    const subtree = subtreeIds(block.items[index]);
    subtree.forEach(item => removed.add(item));
    if (!block.locked && !block.detached) next = setChain(next, list, block, block.items.filter((item, i) => i !== index && !item.shared).map(item => item.node.id));
    next = { ...next, nodes: next.nodes.filter(node => !subtree.includes(node.id)), edges: next.edges.filter(edge => !subtree.includes(edge.from) && !subtree.includes(edge.to)) };
  }
  return next;
}
/** 把一行（含子块）移动到目标块的指定位置，可跨分支；禁止移入自身子块。 */
export function moveItem(graph: Graph, list: Definition[], nodeId: string, target: BlockRef, index: number): Graph {
  const document = buildEca(graph, list);
  const source = locateItem(document, nodeId);
  const targetBlock = findBlock(document, target);
  if (!source || !targetBlock) throw new Error('找不到移动位置');
  if (source.block.locked || targetBlock.locked) throw new Error('该分支与其他分支共享后续动作，请在蓝图中调整');
  if (subtreeIds(source.block.items[source.index]).slice(1).includes(target.owner) || target.owner === nodeId) throw new Error('不能移动到自身的子动作中');
  const sameBlock = source.block.owner === target.owner && source.block.port === target.port;
  const remaining = source.block.items.map(item => item.node.id).filter(id => id !== nodeId);
  let next = setChain(graph, list, source.block, remaining);
  const targetIds = (sameBlock ? remaining : targetBlock.items.map(item => item.node.id)).slice();
  const at = sameBlock && index > source.index ? index - 1 : index;
  targetIds.splice(Math.max(0, Math.min(at, targetIds.length)), 0, nodeId);
  const refreshed = findBlock(buildEca(next, list), target);
  next = setChain(next, list, { ...target, items: [...(refreshed?.items ?? []), { node: source.block.items[source.index].node }] }, targetIds);
  return next;
}

const opSymbols: Record<CompareOp, string> = { '==': '=', '!=': '≠', '<': '<', '<=': '≤', '>': '>', '>=': '≥' };
/** 条件的单行摘要（蓝图节点与折叠显示用）。 */
export function conditionText(list: TriggerCondition[], project: WorkbenchProject, joiner = ' 且 '): string {
  const operand = (item: Operand, type: string) => item.binding && item.binding.mode !== 'constant' ? bindingLabel(item.binding, project) : operandDisplay(item.value, type, project).text;
  return list.filter(item => item.enabled !== false).map(item => item.kind === 'compare' ? `${operand(item.left, item.valueType)} ${opSymbols[item.op]} ${operand(item.right, item.valueType)}` : `(${conditionText(item.children, project, item.kind === 'and' ? ' 且 ' : ' 或 ') || '成立'})`).join(joiner);
}
export const nodeConditions = (node: GraphNode): TriggerCondition[] => Array.isArray(node.conditions) ? node.conditions as TriggerCondition[] : [];
/** 选择器与语句前缀共用的类别。 */
export const categoryOf = (definition: Definition) => sentenceOf(definition).category;
/** 语句的纯文本（引用列表、物体管理器用），格式同原版“类别 - 句子”。 */
export function sentenceText(node: GraphNode, definition: Definition | undefined, project: WorkbenchProject): string {
  const sentence = sentenceOf(definition);
  const body = sentence.parts.map(part => 'text' in part ? part.text : 'field' in part ? parameterDisplay(node, part.field, project).text : `（${part.label}）`).join('');
  return definition && sentence.category ? `${sentence.category} - ${body}` : body;
}
