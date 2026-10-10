import type { EditorObject, FieldValue, ObjectKind } from './types';

export interface ObjectField {
    id: string;
    name: string;
    group: string;
    type: 'number' | 'string' | 'boolean' | 'list';
    value: FieldValue;
    kinds?: ObjectKind[];
    common?: boolean;
    unit?: string;
    levels?: boolean;
    objectKind?: ObjectKind;
    multiple?: boolean;
}

export const objectFields: ObjectField[] = [
    { id: 'name', name: '名称', group: '文本', type: 'string', value: '', common: true },
    { id: 'health', name: '最大生命值', group: '属性', type: 'number', value: 100, kinds: ['unit', 'destructable', 'item'], common: true },
    { id: 'mana', name: '最大魔法值', group: '属性', type: 'number', value: 0, kinds: ['unit'], common: true },
    { id: 'mana', name: '魔法消耗', group: '数据', type: 'number', value: 0, kinds: ['ability'], common: true, levels: true },
    { id: 'speed', name: '移动速度', group: '移动', type: 'number', value: 270, kinds: ['unit'], common: true },
    { id: 'armor', name: '基础护甲', group: '战斗', type: 'number', value: 0, kinds: ['unit', 'destructable'], common: true },
    { id: 'armor', name: '护甲增益', group: '效果', type: 'number', value: 0, kinds: ['upgrade'], common: true, levels: true },
    { id: 'attack', name: '基础伤害', group: '战斗', type: 'number', value: 12, kinds: ['unit'], common: true },
    { id: 'attack', name: '攻击增益', group: '效果', type: 'number', value: 0, kinds: ['upgrade'], common: true, levels: true },
    { id: 'damage', name: '技能伤害', group: '数据', type: 'number', value: 0, kinds: ['ability'], common: true, levels: true },
    { id: 'range', name: '攻击距离', group: '战斗', type: 'number', value: 100, kinds: ['unit'], common: true },
    { id: 'cost', name: '金币消耗', group: '科技', type: 'number', value: 0, kinds: ['unit', 'item', 'upgrade'], common: true, levels: true },
    { id: 'abilities', name: '技能列表', group: '技能', type: 'list', value: [], kinds: ['unit', 'item'], common: true, objectKind: 'ability', multiple: true },
    { id: 'defaultAbility', name: '默认主动技能', group: '技能', type: 'string', value: '', kinds: ['unit'], objectKind: 'ability' },
    { id: 'buffs', name: '魔法效果', group: '数据', type: 'list', value: [], kinds: ['ability'], objectKind: 'buff', multiple: true },
    { id: 'summonedUnit', name: '召唤单位类型', group: '数据', type: 'string', value: '', kinds: ['ability'], objectKind: 'unit' },
    { id: 'upgrades', name: '使用科技', group: '科技', type: 'list', value: [], kinds: ['unit'], objectKind: 'upgrade', multiple: true },
    { id: 'trains', name: '可训练单位', group: '科技', type: 'list', value: [], kinds: ['unit'], objectKind: 'unit', multiple: true },
    { id: 'builds', name: '可建造建筑', group: '科技', type: 'list', value: [], kinds: ['unit'], objectKind: 'unit', multiple: true },
    { id: 'sellsItems', name: '可出售物品', group: '科技', type: 'list', value: [], kinds: ['unit'], objectKind: 'item', multiple: true },
    { id: 'sellsUnits', name: '可出售单位', group: '科技', type: 'list', value: [], kinds: ['unit'], objectKind: 'unit', multiple: true },
    { id: 'charges', name: '使用次数', group: '属性', type: 'number', value: 1, kinds: ['item'], common: true },
    { id: 'cooldown', name: '冷却时间', group: '数据', type: 'number', value: 0, kinds: ['ability'], common: true, unit: '秒', levels: true },
    { id: 'duration', name: '持续时间', group: '数据', type: 'number', value: 0, kinds: ['ability', 'buff'], common: true, unit: '秒', levels: true },
    { id: 'radius', name: '作用范围', group: '数据', type: 'number', value: 0, kinds: ['ability'], common: true, levels: true },
    { id: 'targets', name: '允许目标', group: '数据', type: 'list', value: ['地面', '敌人'], kinds: ['ability'], common: true },
    { id: 'scale', name: '模型缩放', group: '表现', type: 'number', value: 1, kinds: ['unit', 'item', 'destructable', 'doodad'], common: true },
    { id: 'model', name: '模型文件', group: '表现', type: 'string', value: '', common: true },
    { id: 'icon', name: '图标', group: '表现', type: 'string', value: '', common: true },
    { id: 'tooltip', name: '提示文本', group: '文本', type: 'string', value: '', common: true },
    { id: 'enabled', name: '在编辑器中显示', group: '编辑器', type: 'boolean', value: true },
    { id: 'category', name: '分类', group: '编辑器', type: 'string', value: '自定义' },
    { id: 'race', name: '种族', group: '编辑器', type: 'string', value: '其他' },
];

export const topLevelFields = new Set(['name', 'health', 'speed', 'enabled', 'category', 'race']);

export function fieldDefinitions(object: EditorObject): ObjectField[] {
    const base = objectFields.filter(field => !field.kinds || field.kinds.includes(object.kind));
    const known = new Set(base.map(field => field.id));
    const extras = [...new Set([...Object.keys(object.defaults), ...Object.keys(object.fields), ...Object.keys(object.levels)])]
        .filter(id => !known.has(id) && !topLevelFields.has(id))
        .map(id => {
            const value = object.fields[id] ?? object.defaults[id] ?? object.levels[id]?.[0] ?? '';
            return { id, name: id, group: '扩展字段', type: Array.isArray(value) ? 'list' : typeof value, value, levels: !!object.levels[id] } as ObjectField;
        });
    return [...base, ...extras];
}

export function readObjectValue(object: EditorObject, field: ObjectField): FieldValue {
    if (topLevelFields.has(field.id)) return object[field.id] as FieldValue;
    return object.fields[field.id] ?? object.defaults[field.id] ?? field.value;
}

export function baseObjectValue(object: EditorObject, field: ObjectField): FieldValue {
    if (topLevelFields.has(field.id)) return object.defaults[field.id] ?? object[field.id] as FieldValue;
    return object.defaults[field.id] ?? field.value;
}

export function sameValue(a: FieldValue, b: FieldValue): boolean {
    return JSON.stringify(a) === JSON.stringify(b);
}

export function writeObjectValue(object: EditorObject, field: ObjectField, value: FieldValue): EditorObject {
    if (topLevelFields.has(field.id)) return { ...object, [field.id]: value, defaults: { ...object.defaults, [field.id]: object.defaults[field.id] ?? object[field.id] as FieldValue } };
    const fields = { ...object.fields };
    if (sameValue(value, baseObjectValue(object, field))) delete fields[field.id];
    else fields[field.id] = value;
    return { ...object, fields };
}

export function displayValue(value: FieldValue): string {
    return Array.isArray(value) ? value.join(', ') : String(value);
}

export function originalLevels(object: EditorObject): Record<string, number[]> {
    const stored = object.levelDefaults;
    if (stored && typeof stored === 'object' && !Array.isArray(stored) && Object.values(stored).every(value => Array.isArray(value) && value.every(item => typeof item === 'number' && Number.isFinite(item)))) {
        return stored as Record<string, number[]>;
    }
    return object.levels;
}

export function writeObjectLevels(object: EditorObject, levels: Record<string, number[]>): EditorObject {
    return { ...object, levelDefaults: structuredClone(originalLevels(object)), levels };
}

export function containsReference(value: unknown, keys: string[]): boolean {
    if (typeof value === 'string') return keys.includes(value);
    if (Array.isArray(value)) return value.some(item => containsReference(item, keys));
    if (value && typeof value === 'object') return Object.values(value).some(item => containsReference(item, keys));
    return false;
}
