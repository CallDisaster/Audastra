import type { Definition } from '../studio/model';
import type { ModuleId, WorkbenchProject } from './types';
import type { ParameterBinding } from './triggerCode';
import type { TriggerCondition } from './ecaModel';
import { categoryOf, conditionTemplates, conditionText, graphConditions, nodeConditions, sentenceText } from './ecaModel';
import { valueFunctionDefinitions } from './valueSources';
import { findObject } from './objectCatalog';

// 工程级静态引用索引：一次遍历触发器、条件、物体字段、放置实例、AI 与声音，
// 物体管理器、物编“静态引用”和变量页共用。只收录工程数据中的直接引用，脚本文本与动态拼接不在其中。

export type Usage = {
  module: ModuleId;
  /** 跳转目标（触发器语句为节点 ID，触发条件为触发器 ID）。 */
  target: string;
  /** 所在的触发器 / 物体 / 模块名称，列表按它分组。 */
  owner: string;
  ownerId: string;
  /** 事件、条件、动作、物体字段名、放置实例… */
  place: string;
  /** 语句全文或字段值。 */
  text: string;
  /** 所在触发器或语句已禁用：引用仍存在，但不会生成代码。 */
  disabled?: boolean;
};
export type FunctionKind = '事件' | '条件' | '动作' | '流程' | '取值';
export type FunctionEntry = { id: string; title: string; kind: FunctionKind; category: string };
export interface ReferenceIndex {
  tokens: Map<string, Usage[]>;
  functions: Map<string, Usage[]>;
  /** 每个触发器 / 物体使用到的记号，用于“使用了”列表。 */
  outgoing: Map<string, Set<string>>;
}
export type FieldLabel = (kind: string, fieldId: string) => string | undefined;

const kindPlace: Record<string, string> = { event: '事件', action: '动作', flow: '动作', value: '数据' };
const usageKey = (usage: Usage) => `${usage.module}|${usage.target}|${usage.place}|${usage.text}`;

/** 字符串值拆成可能的引用记号：整串、去掉 `$` 前缀、逗号分隔的列表项（物编列表字段）。 */
function tokensOf(value: unknown, into: Set<string>) {
  if (typeof value === 'string') {
    const text = value.trim();
    if (!text || text.length > 256) return;
    into.add(text);
    if (text.startsWith('$')) into.add(text.slice(1));
    if (text.includes(',')) for (const part of text.split(',')) { const token = part.trim(); if (token) into.add(token); }
    return;
  }
  if (Array.isArray(value)) { value.forEach(item => tokensOf(item, into)); return; }
  if (value && typeof value === 'object') Object.values(value).forEach(item => tokensOf(item, into));
}
/** 参数绑定中调用的取值函数（含嵌套参数）。 */
function functionsOf(binding: ParameterBinding | undefined, into: Set<string>) {
  if (!binding) return;
  if (binding.mode === 'function') into.add(binding.value);
  for (const arg of Object.values(binding.args ?? {})) functionsOf(arg.binding, into);
}

export function buildReferenceIndex(project: WorkbenchProject, fieldLabel?: FieldLabel): ReferenceIndex {
  const tokens = new Map<string, Usage[]>(), functions = new Map<string, Usage[]>(), outgoing = new Map<string, Set<string>>();
  const seen = new Set<string>();
  const add = (map: Map<string, Usage[]>, key: string, usage: Usage) => {
    const id = `${key}#${usageKey(usage)}`;
    if (seen.has(id)) return;
    seen.add(id);
    const list = map.get(key);
    if (list) list.push(usage); else map.set(key, [usage]);
  };
  const recordTokens = (found: Set<string>, usage: Usage) => {
    let out = outgoing.get(usage.ownerId);
    if (!out) outgoing.set(usage.ownerId, out = new Set());
    for (const token of found) { add(tokens, token, usage); out.add(token); }
  };
  const definitions = new Map(project.definitions.map(definition => [definition.id, definition]));

  for (const graph of project.graphs) {
    const graphOff = graph.enabled === false;
    const conditionWalk = (list: TriggerCondition[], target: string, disabled: boolean) => {
      for (const condition of list) {
        const off = disabled || condition.enabled === false;
        const usage: Usage = { module: 'triggers', target, owner: graph.name, ownerId: graph.id, place: '条件', text: condition.kind === 'compare' ? conditionText([condition], project) : condition.kind === 'and' ? 'And - 多项条件' : 'Or - 多项条件', disabled: off };
        add(functions, `condition:${condition.kind === 'compare' ? condition.valueType : condition.kind}`, usage);
        if (condition.kind === 'compare') {
          const found = new Set<string>(), called = new Set<string>();
          for (const operand of [condition.left, condition.right]) { tokensOf(operand.value, found); tokensOf(operand.binding, found); functionsOf(operand.binding, called); }
          recordTokens(found, usage);
          for (const id of called) add(functions, id, usage);
        } else conditionWalk(condition.children, target, off);
      }
    };
    conditionWalk(graphConditions(graph), graph.id, graphOff);
    for (const node of graph.nodes) {
      const definition = definitions.get(node.definition);
      const usage: Usage = { module: 'triggers', target: node.id, owner: graph.name, ownerId: graph.id, place: kindPlace[definition?.kind ?? 'action'] ?? '动作', text: sentenceText(node, definition, project), disabled: graphOff || node.enabled === false };
      add(functions, node.definition, usage);
      const found = new Set<string>(), called = new Set<string>();
      tokensOf(node.values, found); tokensOf(node.bindings, found);
      // 表达式与自定义脚本字段：按标识符拆分，`udg_` 前缀对应全局变量名。
      for (const field of definition?.fields ?? []) {
        const value = node.values[field.id];
        if (typeof value === 'string' && /expression|script|code|jass/i.test(`${field.id} ${String(field.valueType ?? '')}`)) for (const word of value.match(/[A-Za-z_][A-Za-z0-9_]*/g) ?? []) { found.add(word); if (word.startsWith('udg_')) found.add(word.slice(4)); }
      }
      for (const binding of Object.values((node.bindings as Record<string, ParameterBinding> | undefined) ?? {})) functionsOf(binding, called);
      recordTokens(found, usage);
      for (const id of called) add(functions, id, usage);
      conditionWalk(nodeConditions(node), node.id, usage.disabled ?? false);
    }
  }
  for (const object of project.objects) {
    const entries = [...Object.entries(object.values ?? {}), ...Object.entries(object.fields ?? {}).filter(([id]) => id !== 'icon' || typeof object.fields.icon !== 'string' || !object.fields.icon.startsWith('data:'))];
    for (const [fieldId, value] of entries) {
      const found = new Set<string>();
      tokensOf(value, found);
      found.delete(object.id); found.delete(object.rawcode);
      if (!found.size) continue;
      const text = Array.isArray(value) ? value.join(', ') : String(value);
      recordTokens(found, { module: 'objects', target: object.id, owner: object.name, ownerId: object.id, place: fieldLabel?.(object.kind, fieldId) ?? fieldId, text: text.length > 80 ? `${text.slice(0, 80)}…` : text });
    }
  }
  for (const variable of project.variables) {
    const found = new Set<string>();
    tokensOf(variable.initial, found);
    found.delete(variable.id); found.delete(variable.name);
    if (found.size) recordTokens(found, { module: 'variables', target: variable.id, owner: variable.name, ownerId: variable.id, place: '初始值', text: variable.initial });
  }
  const objectName = (id: string) => findObject(project, id)?.name ?? id;
  for (const instance of project.world.instances) recordTokens(new Set([instance.objectId]), { module: 'world', target: instance.id, owner: '地图放置', ownerId: 'world', place: `玩家 ${instance.owner}`, text: `${objectName(instance.objectId)} · (${instance.x}, ${instance.y})` });
  const ai = project.ai;
  for (const row of ai.build) recordTokens(new Set([row.objectId]), { module: 'ai', target: row.id, owner: ai.name, ownerId: 'ai', place: '建造优先级', text: `${objectName(row.objectId)} × ${row.count}` });
  for (const row of ai.waves) recordTokens(new Set([row.unitId]), { module: 'ai', target: row.id, owner: ai.name, ownerId: 'ai', place: '进攻波次', text: `${row.name} · ${objectName(row.unitId)} × ${row.count}` });
  if (ai.hero) recordTokens(new Set([ai.hero]), { module: 'ai', target: 'heroes', owner: ai.name, ownerId: 'ai', place: '英雄培养', text: '训练英雄' });
  ai.skills.forEach((skill, index) => recordTokens(new Set([skill]), { module: 'ai', target: 'heroes', owner: ai.name, ownerId: 'ai', place: '英雄培养', text: `学习顺序 ${index + 1}` }));
  for (const sound of project.sounds) recordTokens(new Set([sound.assetId]), { module: 'sounds', target: sound.id, owner: sound.name, ownerId: sound.id, place: '声音文件', text: sound.name });
  return { tokens, functions, outgoing };
}

/** 一组记号（ID、rawcode、名称）的全部引用，去重；exclude 为引用者自身（物体自己的字段不算引用）。 */
export function usagesOf(index: ReferenceIndex, keys: (string | undefined)[], exclude?: string): Usage[] {
  const result: Usage[] = [], seen = new Set<string>();
  for (const key of new Set(keys.filter((item): item is string => !!item))) for (const usage of index.tokens.get(key) ?? []) {
    const id = usageKey(usage);
    if (usage.ownerId === exclude || seen.has(id)) continue;
    seen.add(id); result.push(usage);
  }
  return result;
}
/** 按所在触发器 / 物体分组，组内保持遍历顺序（即语句顺序）。 */
export function groupUsages(list: Usage[]): { ownerId: string; owner: string; module: ModuleId; items: Usage[] }[] {
  const groups = new Map<string, { ownerId: string; owner: string; module: ModuleId; items: Usage[] }>();
  for (const usage of list) {
    const key = `${usage.module}:${usage.ownerId}`;
    const group = groups.get(key);
    if (group) group.items.push(usage); else groups.set(key, { ownerId: usage.ownerId, owner: usage.owner, module: usage.module, items: [usage] });
  }
  return [...groups.values()];
}

const functionKind: Record<Definition['kind'], FunctionKind> = { event: '事件', action: '动作', flow: '流程', value: '取值' };
/** 物体管理器“函数”页的完整目录：事件、条件、动作、流程与取值函数（工程定义 + 内置目录）。 */
export function functionCatalog(project: WorkbenchProject): FunctionEntry[] {
  const list: FunctionEntry[] = [];
  const ids = new Set<string>();
  for (const definition of [...project.definitions, ...valueFunctionDefinitions(project)]) {
    if (ids.has(definition.id)) continue;
    ids.add(definition.id);
    list.push({ id: definition.id, title: definition.title, kind: functionKind[definition.kind], category: definition.kind === 'value' ? (typeof definition.category === 'string' ? definition.category : '取值函数') : categoryOf(definition) });
  }
  for (const template of conditionTemplates) list.push({ id: `condition:${template.valueType ?? template.kind}`, title: template.title, kind: '条件', category: '比较' });
  return list;
}
