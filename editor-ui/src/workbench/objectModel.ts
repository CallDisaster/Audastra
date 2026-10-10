import type { EditorObject, ObjectKind, WorkbenchProject } from './types';
import type { KindValues, War3Field, War3Index, War3Object, War3Value } from './war3Data';

// 物编的数据语义：原版目录只读；地图只保存“对原版的覆盖”和“基于原版的自定义物体”。
// 覆盖写在 EditorObject.values（字段 ID → 值；等级字段为数组），base 指向原版 rawcode。改回原值即撤销覆盖。
export type ObjectRow = {
  key: string; kind: ObjectKind; rawcode: string; base: string; name: string; suffix: string; group: string[]; icon: string;
  custom: boolean; modified: number; hidden: boolean; standard?: War3Object; edit?: EditorObject;
};
const customRoot = '自定义';
export const sameValue = (a: unknown, b: unknown) => JSON.stringify(a ?? '') === JSON.stringify(b ?? '');
export const editOf = (project: WorkbenchProject, kind: ObjectKind, rawcode: string) => project.objects.find(object => object.kind === kind && object.rawcode === rawcode);
const findField = (fields: War3Field[], pattern: RegExp) => fields.find(field => field.category === 'text' && pattern.test(field.field));
export const nameField = (index: War3Index, kind: ObjectKind) => findField(index.kinds[kind].fields, /^(name|editorname)$/i);
export const suffixField = (index: War3Index, kind: ObjectKind) => findField(index.kinds[kind].fields, /^editorsuffix$/i);

/** 生效值：覆盖 → （旧工程自定义物体的名称）→ 基础物体。 */
export function effectiveValues(index: War3Index, kind: ObjectKind, values: KindValues, row: Pick<ObjectRow, 'base' | 'edit' | 'custom'>): Record<string, War3Value> {
  const merged = { ...(values[row.base] ?? {}), ...(row.edit?.values ?? {}) };
  const name = nameField(index, kind);
  if (name && row.custom && row.edit && !(row.edit.values && name.id in row.edit.values) && row.edit.name) merged[name.id] = Array.isArray(merged[name.id]) ? (merged[name.id] as (string | number)[]).map((item, i) => i === 0 ? row.edit!.name : item) : row.edit.name;
  return merged;
}
const text = (value: War3Value | undefined) => Array.isArray(value) ? String(value[0] ?? '') : String(value ?? '');

export function listObjects(project: WorkbenchProject, index: War3Index, kind: ObjectKind): ObjectRow[] {
  const edits = project.objects.filter(object => object.kind === kind);
  const byRawcode = new Map(edits.map(edit => [edit.rawcode, edit]));
  const name = nameField(index, kind), suffix = suffixField(index, kind);
  const rows: ObjectRow[] = index.kinds[kind].objects.map(object => {
    const edit = byRawcode.get(object.id);
    return { key: `${kind}:${object.id}`, kind, rawcode: object.id, base: object.id, name: edit?.values && name && name.id in edit.values ? text(edit.values[name.id]) : object.name, suffix: edit?.values && suffix && suffix.id in edit.values ? text(edit.values[suffix.id]) : object.suffix, group: object.group, icon: object.icon, custom: false, modified: Object.keys(edit?.values ?? {}).length, hidden: object.hidden, standard: object, edit };
  });
  const standard = new Set(index.kinds[kind].objects.map(object => object.id));
  for (const edit of edits) {
    if (standard.has(edit.rawcode) && !edit.custom) continue;
    const base = edit.base && index.kinds[kind].objects.find(object => object.id === edit.base);
    const values = edit.values ?? {};
    rows.push({ key: `${kind}:${edit.rawcode}`, kind, rawcode: edit.rawcode, base: base ? base.id : '', name: name && name.id in values ? text(values[name.id]) : edit.name, suffix: suffix && suffix.id in values ? text(values[suffix.id]) : '', group: [customRoot, ...(base ? base.group : ['未关联原版'])], icon: base ? base.icon : '', custom: true, modified: Object.keys(values).length, hidden: false, standard: base || undefined, edit });
  }
  return rows;
}

/** 字段对该物体是否适用：以基础物体实际拥有的字段为准（导出时已按单位/英雄/建筑、技能 useSpecific 过滤）。 */
export function applicableFields(index: War3Index, kind: ObjectKind, values: KindValues, row: Pick<ObjectRow, 'base'>): War3Field[] {
  const own = values[row.base];
  return own ? index.kinds[kind].fields.filter(field => field.id in own) : [];
}
export function levelCount(kind: ObjectKind, effective: Record<string, War3Value>, row: Pick<ObjectRow, 'standard'>) {
  const raw = kind === 'ability' ? effective.alev : kind === 'upgrade' ? effective.glvl : undefined;
  return Math.max(1, Math.min(100, Number(raw) || row.standard?.levels || 1));
}

function ensureEdit(project: WorkbenchProject, row: ObjectRow): { objects: EditorObject[]; edit: EditorObject } {
  if (row.edit) return { objects: project.objects, edit: row.edit };
  const edit: EditorObject = { id: `std.${row.kind}.${row.rawcode}`, name: row.name, category: row.group.join('/'), health: 0, speed: 0, enabled: true, kind: row.kind, rawcode: row.rawcode, parent: row.rawcode, base: row.rawcode, race: row.standard?.race ?? '', custom: false, fields: {}, defaults: {}, levels: {}, values: {} };
  return { objects: [...project.objects, edit], edit };
}
/** 写入字段；与基础值相同则删除覆盖，未修改的原版物体不留空记录。 */
export function setFieldValue(project: WorkbenchProject, index: War3Index, values: KindValues, row: ObjectRow, fieldId: string, value: War3Value): WorkbenchProject {
  const { objects, edit } = ensureEdit(project, row);
  const next = { ...(edit.values ?? {}) };
  const base = values[row.base]?.[fieldId];
  if (sameValue(base, value)) delete next[fieldId]; else next[fieldId] = value;
  const name = nameField(index, row.kind);
  const updated: EditorObject = { ...edit, values: next, ...(name?.id === fieldId ? { name: text(value) || edit.name } : {}) };
  if (!updated.custom && !Object.keys(next).length) return { ...project, objects: objects.filter(object => object !== edit) };
  return { ...project, objects: objects.map(object => object === edit ? updated : object) };
}
export function resetFields(project: WorkbenchProject, row: ObjectRow, fieldIds?: string[]): WorkbenchProject {
  if (!row.edit) return project;
  const next = { ...(row.edit.values ?? {}) };
  for (const id of fieldIds ?? Object.keys(next)) delete next[id];
  if (!row.custom && !Object.keys(next).length) return { ...project, objects: project.objects.filter(object => object !== row.edit) };
  return { ...project, objects: project.objects.map(object => object === row.edit ? { ...row.edit!, values: next } : object) };
}

// 自定义 ID：单位沿用基础物体首字母（英雄保持大写），其余类别用固定前缀，后三位 0-9A-Z 递增，与原版及地图已有 ID 不冲突。
const prefixes: Record<ObjectKind, string> = { unit: '', item: 'I', ability: 'A', buff: 'B', upgrade: 'R', destructable: 'B', doodad: 'D' };
const digits = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ';
export function nextRawcode(project: WorkbenchProject, index: War3Index, kind: ObjectKind, base: string): string {
  const prefix = prefixes[kind] || base[0] || 'h';
  const taken = new Set([...index.kinds[kind].objects.map(object => object.id), ...project.objects.filter(object => object.kind === kind).map(object => object.rawcode)]);
  for (let n = 0; n < digits.length ** 3; n++) {
    const code = prefix + digits[Math.floor(n / 1296) % 36] + digits[Math.floor(n / 36) % 36] + digits[n % 36];
    if (!taken.has(code)) return code;
  }
  throw new Error('自定义 ID 已用尽');
}
export function createCustom(project: WorkbenchProject, index: War3Index, kind: ObjectKind, from: ObjectRow): { project: WorkbenchProject; rawcode: string } {
  if (!from.base) throw new Error('该物体没有原版基础，无法复制');
  const rawcode = nextRawcode(project, index, kind, from.base);
  const edit: EditorObject = { id: `custom.${kind}.${rawcode}`, name: from.name, category: [customRoot, ...(from.standard?.group ?? [])].join('/'), health: 0, speed: 0, enabled: true, kind, rawcode, parent: from.base, base: from.base, race: from.standard?.race ?? '', custom: true, fields: {}, defaults: {}, levels: {}, values: { ...(from.edit?.values ?? {}) } };
  return { project: { ...project, objects: [...project.objects, edit] }, rawcode };
}
export function deleteCustom(project: WorkbenchProject, row: ObjectRow): WorkbenchProject {
  if (!row.custom || !row.edit) throw new Error('只能删除自定义物体');
  return { ...project, objects: project.objects.filter(object => object !== row.edit) };
}

// ---- 显示 ----
const referenceKinds: Record<string, ObjectKind[]> = {
  abilityList: ['ability'], heroAbilityList: ['ability'], abilCode: ['ability'], unitList: ['unit'], unitCode: ['unit'],
  itemList: ['item'], upgradeList: ['upgrade'], buffList: ['buff'], effectList: ['buff'], techList: ['unit', 'upgrade'],
  itemCode: ['item'], buffCode: ['buff'], upgradeCode: ['upgrade'], destructableCode: ['destructable'], doodadCode: ['doodad'],
};
export const referenceKindsOf = (type: string) => referenceKinds[type];
export const isListType = (type: string) => /List$/.test(type) && type !== 'stringList' && type !== 'intList';
export const isFlagType = (index: War3Index, type: string) => !!index.enums[type] && (/List$|Flags$/.test(type) || type === 'attackBits');
export const isNumberType = (type: string) => ['int', 'real', 'unreal'].includes(type);
export const splitValues = (value: War3Value | undefined) => String(Array.isArray(value) ? value.join(',') : value ?? '').split(',').map(item => item.trim()).filter(Boolean);
/** 物体引用的名称：地图物体优先，其次原版目录。 */
export function objectLabel(project: WorkbenchProject, index: War3Index, kinds: ObjectKind[], rawcode: string): { name: string; icon: string; known: boolean } {
  for (const kind of kinds) {
    const edit = editOf(project, kind, rawcode);
    const standard = index.kinds[kind].objects.find(object => object.id === rawcode);
    if (edit || standard) {
      const baseIcon = standard?.icon ?? (edit?.base ? index.kinds[kind].objects.find(object => object.id === edit.base)?.icon : '') ?? '';
      return { name: edit?.name ?? [standard!.name.trim(), standard!.suffix.trim()].filter(Boolean).join(' '), icon: baseIcon, known: true };
    }
  }
  return { name: rawcode, icon: '', known: false };
}
export function displayValue(project: WorkbenchProject, index: War3Index, field: War3Field, value: War3Value | undefined): string {
  if (value === undefined || value === '') return '';
  if (field.type === 'bool') return Number(value) === 1 ? '是' : '否';
  const kinds = referenceKindsOf(field.type);
  if (kinds) return splitValues(value).map(code => objectLabel(project, index, kinds, code).name).join('，');
  const choices = index.enums[field.type];
  if (choices) return splitValues(value).map(item => choices.find(choice => choice.value.toLowerCase() === item.toLowerCase())?.label ?? item).join('，');
  if (typeof value === 'number') return Number.isInteger(value) ? String(value) : String(Number(value.toFixed(3)));
  return String(value);
}
/** 原版文本颜色代码：|cffRRGGBB…|r 着色，|n 换行；返回片段供预览渲染。 */
export function wc3Segments(value: string): { text: string; color?: string; newline?: boolean }[] {
  const segments: { text: string; color?: string; newline?: boolean }[] = [];
  let color: string | undefined;
  for (const part of value.split(/(\|c[0-9a-fA-F]{8}|\|r|\|n)/)) {
    if (!part) continue;
    if (/^\|c/i.test(part)) color = `#${part.slice(4)}`;
    else if (/^\|r$/i.test(part)) color = undefined;
    else if (/^\|n$/i.test(part)) segments.push({ text: '', newline: true });
    else segments.push({ text: part, color });
  }
  return segments;
}
export const fieldCategoryOrder = ['text', 'stats', 'data', 'combat', 'abil', 'tech', 'move', 'art', 'sound', 'path', 'editor'];
/** 需要抽屉编辑的字段：物体引用、标志集合、地形集合与长文本。 */
export function needsDrawer(index: War3Index, field: War3Field, value: War3Value | undefined) {
  if (referenceKindsOf(field.type) || isFlagType(index, field.type) || field.type === 'tilesetList') return true;
  if (field.type !== 'string') return false;
  const sample = Array.isArray(value) ? value.join('') : String(value ?? '');
  return field.category === 'text' && (sample.length > 36 || /\|[cnr]/i.test(sample) || /tip$/i.test(field.field));
}
/** 给未关联原版的自定义物体指定基础物体（旧工程迁移）；已有的字段覆盖保留。 */
export function setBase(project: WorkbenchProject, row: ObjectRow, base: string): WorkbenchProject {
  if (!row.custom || !row.edit) throw new Error('只能为自定义物体指定基础物体');
  return { ...project, objects: project.objects.map(object => object === row.edit ? { ...row.edit, base, parent: base } : object) };
}
export const baseFieldType: Record<ObjectKind, string> = { unit: 'unitCode', item: 'itemCode', ability: 'abilCode', buff: 'buffCode', upgrade: 'upgradeCode', destructable: 'destructableCode', doodad: 'doodadCode' };
/** 解析提示文字中的 <ID,列名[,%]> 占位符（如 <AHbz,DataA1>、<AHbz,Cool2>）；取不到值时保留原文。 */
export function resolvePlaceholders(text: string, lookup: (code: string, column: string) => War3Value | undefined): string {
  return text.replace(/<([A-Za-z0-9]{4}),([A-Za-z]+\d*)(,%)?>/g, (whole, code: string, column: string, percent?: string) => {
    const value = lookup(code, column);
    if (value === undefined || value === '' || Array.isArray(value)) return whole;
    const number = Number(value);
    if (!Number.isFinite(number)) return String(value);
    return percent ? String(Math.round(number * 100)) : String(Number(number.toFixed(2)));
  });
}
/** 技能数据列查找：DataA1 → 字段 Data（数据 A）等级 1；Cool2 → 字段 Cool 等级 2。地图覆盖优先。 */
export function abilityColumnLookup(project: WorkbenchProject, index: War3Index, values: KindValues) {
  return (code: string, column: string): War3Value | undefined => {
    const match = column.match(/^(Data)([A-I])(\d+)$/i) ?? column.match(/^([A-Za-z]+?)()(\d+)$/);
    if (!match) return undefined;
    const [, name, letter, levelText] = match;
    const edit = editOf(project, 'ability', code);
    const base = edit?.base ?? code;
    const own = { ...(values[base] ?? {}), ...(edit?.values ?? {}) };
    const data = letter ? 'ABCDEFGHI'.indexOf(letter.toUpperCase()) + 1 : 0;
    const field = index.kinds.ability.fields.find(item => item.field.toLowerCase() === name.toLowerCase() && item.data === data && item.id in own);
    if (!field) return undefined;
    const value = own[field.id], level = Number(levelText) - 1;
    return Array.isArray(value) ? value[Math.min(level, value.length - 1)] : value;
  };
}
