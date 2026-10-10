import type { KeyboardEvent } from 'react';
import type { Definition } from '../studio/model';
import type { ObjectKind } from './types';
import { expectedFieldType, objectKindForType } from './valueSources';

// 蓝图节点、属性区与传统语句共用的参数元数据判断。
type Field = Definition['fields'][number];
/** 输入框内按键不冒泡到画布/树，但保留 Ctrl+S 工程保存。 */
export const keepEditorSave = (event: KeyboardEvent) => { if (!((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's')) event.stopPropagation(); };
export type ReferenceKind = 'object' | 'region' | 'instance' | 'sound' | 'asset' | undefined;
const knownKinds = new Set<ObjectKind>(['unit', 'item', 'destructable', 'doodad', 'ability', 'buff', 'upgrade']);
export function fieldReference(field: Field, definitionId: string): { kind: ReferenceKind; objectKind?: ObjectKind } {
  const type = expectedFieldType(field, definitionId);
  const inferred = objectKindForType(type);
  if (inferred) return { kind: 'object', objectKind: inferred };
  const declared = typeof field.objectKind === 'string' && knownKinds.has(field.objectKind as ObjectKind) ? field.objectKind as ObjectKind : undefined;
  if (declared) return { kind: 'object', objectKind: declared };
  if (field.referenceType === 'instance' || ['unit', 'item', 'destructable'].includes(type)) return { kind: 'instance' };
  if (field.referenceType === 'object') return { kind: 'object' };
  if (field.referenceType === 'region' || ['rect', 'location'].includes(type)) return { kind: 'region' };
  if (field.referenceType === 'sound' || type === 'sound') return { kind: 'sound' };
  if (field.referenceType === 'asset' || ['model', 'texture', 'image', 'asset'].includes(type)) return { kind: 'asset' };
  return { kind: undefined };
}

export function enumChoices(field: Field): { label: string; value: string }[] {
  const source = field.options ?? field.enum;
  if (Array.isArray(source)) return source.flatMap(item => typeof item === 'string' ? [{ label: item, value: item }] : item && typeof item === 'object' && 'value' in item ? [{ label: String('label' in item ? item.label : item.value), value: String(item.value) }] : []);
  return field.id === 'command' ? [{ label: '移动', value: 'move' }, { label: '攻击', value: 'attack' }, { label: '巡逻', value: 'patrol' }, { label: '智能指令', value: 'smart' }, { label: '停止', value: 'stop' }, { label: '保持位置', value: 'holdposition' }] : [];
}
/** 参数的实际类型：`$type:字段` 表示跟随同节点中所选变量的类型（如“设置变量”的值）。 */
export function resolveFieldType(field: Field, node: { definition: string; values: Record<string, unknown>; bindings?: unknown }, variables: { id: string; name: string; type: string }[]): string {
  const type = expectedFieldType(field, node.definition);
  const match = type.match(/^\$type:(.+)$/);
  if (!match) return type;
  const binding = (node.bindings as Record<string, { mode: string; value: string }> | undefined)?.[match[1]];
  const id = binding?.mode === 'variable' ? binding.value : node.values[match[1]];
  return variables.find(variable => variable.id === id || variable.name === id)?.type ?? 'any';
}
