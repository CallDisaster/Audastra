import type { Definition } from '../studio/model';
import type { ObjectKind, WorkbenchProject } from './types';
import type { ParameterBinding } from './triggerCode';

type Field = Definition['fields'][number];
const objectTypes: Record<string, ObjectKind> = {
  unitcode: 'unit', itemcode: 'item', abilcode: 'ability', buffcode: 'buff',
  destructablecode: 'destructable', doodadcode: 'doodad', upgradecode: 'upgrade',
};
export function objectKindForType(type: string): ObjectKind | undefined { return objectTypes[type]; }
export function expectedFieldType(field: Field, definitionId?: string): string {
  if (typeof field.valueType === 'string') return field.valueType;
  if (typeof field.objectKind === 'string') return Object.entries(objectTypes).find(([, kind]) => kind === field.objectKind)?.[0] ?? field.type;
  const legacy: Record<string, Record<string, string>> = { 'core.spawn': { template: 'unitcode', position: 'rect' }, 'core.order': { target: 'rect' }, 'core.branch': { expression: 'boolean' } };
  return legacy[definitionId ?? '']?.[field.id] ?? (field.type === 'text' ? 'string' : field.type);
}
export function compatibleValueType(actual: string, expected: string): boolean {
  const normalize = (type: string) => type === 'text' ? 'string' : type;
  actual = normalize(actual); expected = normalize(expected);
  if (expected.startsWith('globalvar')) { const inner = expected.split(':')[1]; return !inner || compatibleValueType(actual, inner); }
  return actual === expected || expected === 'any' || (expected === 'number' && ['integer', 'real', 'number'].includes(actual)) || (expected === 'real' && actual === 'integer');
}
export function returnTypeOf(definition: Definition): string | undefined {
  if (typeof definition.returnType === 'string') return definition.returnType;
  if (definition.ports.some(port => port.type === 'exec')) return undefined;
  return definition.ports.find(port => port.direction === 'out')?.type;
}
const argument = (id: string, label: string, valueType: string, initial: string | number | boolean): Field => ({ id, label, valueType, type: typeof initial === 'number' ? 'number' : typeof initial === 'boolean' ? 'boolean' : 'text', default: initial });
const native = (symbol: string, title: string, returnType: string, fields: Field[] = []): Definition => ({
  id: `native.${symbol}`, title, namespace: 'Warcraft III', version: '1', kind: 'value', returnType,
  ports: [{ id: 'result', label: '返回值', direction: 'out', type: returnType }], fields, codegen: { jass: symbol, lua: symbol },
});
const arithmetic = [{ label: '+', value: '+' }, { label: '-', value: '-' }, { label: '×', value: '*' }, { label: '÷', value: '/' }];
/** 非普通调用形式的取值（全局变量、运算表达式），codegen.template 以 {参数} 占位。 */
const templated = (id: string, title: string, returnType: string, fields: Field[], template: string): Definition => ({
  id: `native.${id}`, title, namespace: 'Warcraft III', version: '1', kind: 'value', returnType,
  ports: [{ id: 'result', label: '返回值', direction: 'out', type: returnType }], fields, codegen: { template: { jass: template, lua: template } },
});
// 原生函数也走公开定义契约；项目导入的同 ID 定义可替换此目录。
const nativeFunctions: Definition[] = [
  native('GetUnitTypeId', '单位的单位类型', 'unitcode', [argument('unit', '单位', 'unit', 'null')]),
  native('GetTriggerUnit', '触发单位', 'unit'),
  native('GetEnteringUnit', '进入区域的单位', 'unit'),
  native('GetDyingUnit', '死亡单位', 'unit'),
  native('GetUnitName', '单位名称', 'string', [argument('unit', '单位', 'unit', 'null')]),
  native('GetItemTypeId', '物品的物品类型', 'itemcode', [argument('item', '物品', 'item', 'null')]),
  native('GetManipulatedItem', '被操作的物品', 'item'),
  native('GetRandomInt', '随机整数', 'integer', [argument('min', '最小值', 'integer', 1), argument('max', '最大值', 'integer', 10)]),
  native('GetRandomReal', '随机实数', 'real', [argument('min', '最小值', 'real', 0), argument('max', '最大值', 'real', 1)]),
  native('Player', '玩家', 'player', [argument('index', '玩家编号', 'integer', 0)]),
  native('GetOwningPlayer', '单位所属玩家', 'player', [argument('unit', '单位', 'unit', 'null')]),
  native('IsPlayerAlly', '玩家是否为盟友', 'boolean', [argument('player', '玩家', 'player', 'null'), argument('other', '另一玩家', 'player', 'null')]),
  native('GetWorldBounds', '地图边界', 'rect'),
  native('GetRectCenter', '区域中心', 'location', [argument('rect', '区域', 'rect', '')]),
  native('GetUnitsInRectAll', '区域内的单位组', 'group', [argument('rect', '区域', 'rect', '')]),
  native('GetEnumUnit', '选取单位', 'unit'),
  native('GetKillingUnitBJ', '凶手单位', 'unit'),
  native('GetLastCreatedUnit', '最后创建的单位', 'unit'),
  native('GetLastCreatedGroup', '最后创建的单位组', 'group'),
  native('GetTriggerPlayer', '触发玩家', 'player'),
  native('GetEventPlayerChatString', '输入的聊天字符串', 'string'),
  native('GetUnitLoc', '单位位置', 'location', [argument('unit', '单位', 'unit', 'null')]),
  native('GetPlayableMapRect', '可用地图区域', 'rect'),
  native('I2S', '转换整数为字符串', 'string', [argument('value', '整数', 'integer', 0)]),
  templated('forLoopA', '整数 A', 'integer', [], 'bj_forLoopAIndex'),
  templated('arith.integer', '算术（整数）', 'integer', [argument('a', '左值', 'integer', 0), { ...argument('op', '运算', 'arithop', '+'), options: arithmetic }, argument('b', '右值', 'integer', 0)], '({a} {op} {b})'),
  templated('arith.real', '算术（实数）', 'real', [argument('a', '左值', 'real', 0), { ...argument('op', '运算', 'arithop', '+'), options: arithmetic }, argument('b', '右值', 'real', 0)], '({a} {op} {b})'),
];
export function valueFunctionDefinitions(project: WorkbenchProject): Definition[] {
  const definitions = new Map(nativeFunctions.map(definition => [definition.id, definition]));
  for (const definition of project.definitions) if (definition.kind === 'value' && returnTypeOf(definition)) definitions.set(definition.id, definition);
  return [...definitions.values()];
}
export function bindingLabel(binding: ParameterBinding | undefined, project: WorkbenchProject): string {
  if (!binding) return '';
  if (binding.mode === 'variable') {
    const variable = project.variables.find(item => item.id === binding.value || item.name === binding.value);
    return `${variable?.name ?? binding.value}${variable?.array ? `[${binding.index ?? '0'}]` : ''}`;
  }
  if (binding.mode === 'function') return `${valueFunctionDefinitions(project).find(item => item.id === binding.value)?.title ?? binding.value}(…)`;
  return binding.value;
}

// 固定值预设：对应原版取值窗口中的“预设”，按类型给出常用常量；代码生成直接使用 value。
const playerColors = ['红色', '蓝色', '青色', '紫色', '黄色', '橙色', '绿色', '粉色', '灰色', '浅蓝色', '深绿色', '棕色'];
export function presetValues(type: string): { label: string; value: string | number }[] {
  if (type === 'player') return [...playerColors.map((color, index) => ({ label: `玩家${index + 1}（${color}）`, value: `Player(${index})` })), { label: '中立敌对', value: 'Player(PLAYER_NEUTRAL_AGGRESSIVE)' }, { label: '中立被动', value: 'Player(PLAYER_NEUTRAL_PASSIVE)' }];
  if (type === 'real') return [{ label: '默认建筑朝向', value: 270 }];
  return [];
}
