import { useEffect, useSyncExternalStore } from 'react';
import type { ObjectKind } from './types';
import { registerStandardCatalog } from './objectCatalog';

// 本机原版数据（tools/import-war3-data.mjs 导出到 public/local-data/war3/）。不随仓库分发；缺失时各模块回退到示例数据。
export type War3Value = string | number | (string | number)[];
export interface War3Field {
  id: string; field: string; slk: string; index: number; repeat: number; data: number;
  category: string; name: string; sort: string; type: string; min?: number | string; max?: number | string;
  canBeEmpty: boolean; specific?: string[]; notSpecific?: string[];
}
export interface War3Object { id: string; name: string; suffix: string; group: string[]; race: string; hidden: boolean; levels: number; code: string; icon: string }
export interface War3Index {
  generatedAt: string; source: string;
  categories: Record<string, string>;
  enums: Record<string, { value: string; label: string }[]>;
  kinds: Record<ObjectKind, { fields: War3Field[]; objects: War3Object[] }>;
  icons?: { available: boolean; failed: number };
}
export type KindValues = Record<string, Record<string, War3Value>>;
type State = { status: 'idle' | 'loading' | 'ready' | 'missing'; index?: War3Index; values: Partial<Record<ObjectKind, KindValues>>; pending: Partial<Record<ObjectKind, boolean>>; lookup?: Map<string, { kind: ObjectKind; object: War3Object }> };

const root = `${import.meta.env?.BASE_URL ?? './'}local-data/war3/`;
let state: State = { status: 'idle', values: {}, pending: {} };
const listeners = new Set<() => void>();
const set = (next: Partial<State>) => { state = { ...state, ...next }; listeners.forEach(listener => listener()); };
const subscribe = (listener: () => void) => { listeners.add(listener); return () => { listeners.delete(listener); }; };

export function loadWar3Index() {
  if (state.status !== 'idle') return;
  set({ status: 'loading' });
  fetch(`${root}index.json`).then(response => { if (!response.ok) throw new Error(String(response.status)); return response.json() as Promise<War3Index>; }).then(index => {
    const lookup = new Map<string, { kind: ObjectKind; object: War3Object }>();
    for (const [kind, data] of Object.entries(index.kinds) as [ObjectKind, War3Index['kinds'][ObjectKind]][]) for (const object of data.objects) lookup.set(`${kind}:${object.id}`, { kind, object });
    registerStandardCatalog({ objects: kind => index.kinds[kind]?.objects ?? [], iconUrl });
    set({ status: 'ready', index, lookup });
  }).catch(() => set({ status: 'missing' }));
}
export function loadKindValues(kind: ObjectKind) {
  if (state.values[kind] || state.pending[kind] || state.status !== 'ready') return;
  set({ pending: { ...state.pending, [kind]: true } });
  fetch(`${root}values-${kind}.json`).then(response => response.json() as Promise<KindValues>).then(values => set({ values: { ...state.values, [kind]: values }, pending: { ...state.pending, [kind]: false } })).catch(() => set({ pending: { ...state.pending, [kind]: false } }));
}
/** 组件内订阅原版数据；首次调用时开始加载索引。 */
export function useWar3() {
  useEffect(loadWar3Index, []);
  return useSyncExternalStore(subscribe, () => state);
}
export const war3State = () => state;
export const iconUrl = (icon: string | undefined) => icon ? `${root}icons/${icon}` : undefined;
/** 按类别与 rawcode 查找原版物体（未加载时返回 undefined）。 */
export function standardObject(kind: ObjectKind, rawcode: string) { return state.lookup?.get(`${kind}:${rawcode}`)?.object; }
export function standardAnyKind(rawcode: string) { for (const kind of ['unit', 'item', 'ability', 'buff', 'upgrade', 'destructable', 'doodad'] as ObjectKind[]) { const found = state.lookup?.get(`${kind}:${rawcode}`); if (found) return found; } return undefined; }
