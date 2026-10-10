import { useSyncExternalStore } from 'react';

// 世界页的会话状态：选择集、工具、吸附、显示层、隐藏/锁定、视图书签。
// 与工程数据分开：它是编辑器状态，不进撤销历史、不写进地图；但命令面板、助手和资源浏览器都要读写，
// 所以放在模块外的共享存储里（原生端对应 SelectionService / WorldViewState，供 UI 与 Agent 适配器共用）。

export type WorldTool = 'select' | 'place' | 'region' | 'camera';
export interface WorldView { cx: number; cy: number; zoom: number }
export interface WorldState {
  selection: string[];
  tool: WorldTool;
  /** 摆放工具的物体：地图物体 ID 或原版 rawcode。 */
  template: string | null;
  owner: number;
  snap: boolean; step: number; angleStep: number;
  layers: { grid: boolean; regions: boolean; cameras: boolean; names: boolean; doodads: boolean; pathing: boolean };
  hidden: string[]; locked: string[];
  bookmarks: { name: string; view: WorldView }[];
  /** 视图请求：命令面板/大纲要求视口“对准选择”时递增。 */
  frameRequest: number;
}
const storageKey = 'forge-world-r08';
const initial: WorldState = {
  selection: [], tool: 'select', template: null, owner: 1, snap: true, step: 32, angleStep: 15,
  layers: { grid: true, regions: true, cameras: true, names: false, doodads: true, pathing: false },
  hidden: [], locked: [], bookmarks: [], frameRequest: 0,
};
function load(): WorldState {
  try {
    const saved = JSON.parse(localStorage.getItem(storageKey) ?? 'null') as Partial<WorldState> | null;
    // 只恢复界面偏好；选择集和工具每次打开都从默认开始。
    return saved ? { ...initial, snap: saved.snap ?? initial.snap, step: saved.step ?? initial.step, angleStep: saved.angleStep ?? initial.angleStep, layers: { ...initial.layers, ...saved.layers }, bookmarks: Array.isArray(saved.bookmarks) ? saved.bookmarks : [] } : initial;
  } catch { return initial; }
}
let state = load();
const listeners = new Set<() => void>();
export function setWorld(patch: Partial<WorldState> | ((current: WorldState) => Partial<WorldState>)) {
  state = { ...state, ...(typeof patch === 'function' ? patch(state) : patch) };
  try { localStorage.setItem(storageKey, JSON.stringify({ snap: state.snap, step: state.step, angleStep: state.angleStep, layers: state.layers, bookmarks: state.bookmarks })); } catch { /* 界面偏好 */ }
  listeners.forEach(listener => listener());
}
export const worldState = () => state;
export function subscribeWorld(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener); }; }
export function useWorld() { return useSyncExternalStore(subscribeWorld, worldState); }

export function select(ids: string[], mode: 'replace' | 'add' | 'toggle' = 'replace') {
  setWorld(current => {
    if (mode === 'replace') return { selection: ids };
    if (mode === 'add') return { selection: [...new Set([...current.selection, ...ids])] };
    const set = new Set(current.selection); for (const id of ids) { if (set.has(id)) set.delete(id); else set.add(id); }
    return { selection: [...set] };
  });
}
export function toggleIn(key: 'hidden' | 'locked', ids: string[]) {
  setWorld(current => { const set = new Set(current[key]); const all = ids.every(id => set.has(id)); for (const id of ids) { if (all) set.delete(id); else set.add(id); } return { [key]: [...set] } as Partial<WorldState>; });
}
export function frameSelection() { setWorld(current => ({ frameRequest: current.frameRequest + 1 })); }
export function startPlacing(template: string) { setWorld({ tool: 'place', template }); }
