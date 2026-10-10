import type { ModuleId } from './types';

// 工作台停靠布局。所有模块常驻在顶栏的模块切换器里，没有“打开/关闭”这一层；每个模块只在一处：
// 主编辑区（默认）、右侧停靠组或浮动窗口。主编辑区一次显示一个模块，其余主区模块保持挂载但不显示。
// 纯函数，界面只把拖动/点击翻译成这些操作；原生端对应 Qt 停靠系统（中央区 + 停靠组 + 顶层浮动窗口）。

/** collapsed：收起为标题栏，浮动窗口较大时用来露出下面的编辑区。 */
export type FloatWindow = { id: ModuleId; x: number; y: number; w: number; h: number; maximized?: boolean; collapsed?: boolean };
export type DockLayout = {
  /** 主编辑区当前显示的模块；若它已浮动或停靠到右侧，显示时回退到主区第一个模块。 */
  mainActive: ModuleId;
  side: ModuleId[]; sideActive?: ModuleId; sideWidth: number;
  /** 数组顺序即前后次序，最后一个在最前。 */
  floats: FloatWindow[];
  /** 每个模块上次浮动时的尺寸：放回后再拖出时沿用。 */
  sizes?: Partial<Record<ModuleId, { w: number; h: number }>>;
};
export type Placement = { area: 'main' } | { area: 'side'; index?: number } | { area: 'float'; x: number; y: number; w?: number; h?: number };

export const defaultLayout = (): DockLayout => ({ mainActive: 'triggers', side: [], sideWidth: 520, floats: [] });

export function whereIs(layout: DockLayout, id: ModuleId): 'main' | 'side' | 'float' {
  if (layout.side.includes(id)) return 'side';
  if (layout.floats.some(item => item.id === id)) return 'float';
  return 'main';
}
export const mainModules = (layout: DockLayout, all: readonly ModuleId[]) => all.filter(id => whereIs(layout, id) === 'main');
/** 主编辑区实际显示的模块；全部模块都浮动或停靠到右侧时为 undefined。 */
export function mainShown(layout: DockLayout, all: readonly ModuleId[]): ModuleId | undefined {
  return whereIs(layout, layout.mainActive) === 'main' ? layout.mainActive : mainModules(layout, all)[0];
}
/** 当前可见（需要挂载）的模块。 */
export const visibleModules = (layout: DockLayout, all: readonly ModuleId[]): ModuleId[] => [mainShown(layout, all), layout.sideActive, ...layout.floats.map(item => item.id)].filter((id): id is ModuleId => !!id);

/** 从右侧组与浮动窗口中移出（即回到主编辑区，但不切换主区显示）。 */
export function detach(layout: DockLayout, id: ModuleId): DockLayout {
  const index = layout.side.indexOf(id);
  const side = layout.side.filter(item => item !== id);
  // 移走右侧活动标签时激活相邻标签（优先右侧）。
  const sideActive = layout.sideActive === id ? side[Math.min(index, side.length - 1)] : layout.sideActive;
  const window = layout.floats.find(item => item.id === id);
  const sizes = window ? { ...layout.sizes, [id]: { w: window.w, h: window.h } } : layout.sizes;
  return { ...layout, side, sideActive, floats: layout.floats.filter(item => item.id !== id), sizes };
}
export function place(layout: DockLayout, id: ModuleId, target: Placement): DockLayout {
  const next = detach(layout, id);
  if (target.area === 'main') return { ...next, mainActive: id };
  if (target.area === 'float') { const size = next.sizes?.[id]; return { ...next, floats: [...next.floats, { id, x: target.x, y: target.y, w: target.w ?? size?.w ?? 900, h: target.h ?? size?.h ?? 600 }] }; }
  const side = [...next.side];
  side.splice(Math.max(0, Math.min(target.index ?? side.length, side.length)), 0, id);
  return { ...next, side, sideActive: id };
}
/** 切到模块：浮动窗口提到最前，右侧组中则激活，主区模块在主编辑区显示。
 *  reveal：从切换器、搜索或引用打开时同时展开收起的窗口；单纯点击窗口只调整前后次序。 */
export function activate(layout: DockLayout, id: ModuleId, reveal = false): DockLayout {
  const where = whereIs(layout, id);
  if (where === 'float') { const window = layout.floats.find(item => item.id === id)!; return { ...layout, floats: [...layout.floats.filter(item => item.id !== id), reveal ? { ...window, collapsed: false } : window] }; }
  if (where === 'side') return { ...layout, sideActive: id };
  return { ...layout, mainActive: id };
}
export function updateFloat(layout: DockLayout, id: ModuleId, patch: Partial<FloatWindow>): DockLayout {
  return { ...layout, floats: layout.floats.map(item => item.id === id ? { ...item, ...patch } : item) };
}
/** 浮动窗口限制在工作区内：至少保留标题栏可见，避免拖出屏幕后找不回来。 */
export function clampFloat(window: FloatWindow, bounds: { width: number; height: number }): FloatWindow {
  const w = Math.max(360, Math.min(window.w, bounds.width)), h = Math.max(240, Math.min(window.h, bounds.height));
  return { ...window, w, h, x: Math.max(80 - w, Math.min(window.x, bounds.width - 80)), y: Math.max(0, Math.min(window.y, bounds.height - 36)) };
}
/** 布局预设（类似 Photoshop 的“工作区”）：保留窗口尺寸记忆与右侧宽度。 */
export const layoutPresets: { id: string; label: string; detail: string; make: (current: DockLayout) => DockLayout }[] = [
  { id: 'single', label: '单编辑区', detail: '全部放回主编辑区', make: current => ({ ...defaultLayout(), mainActive: current.mainActive, sideWidth: current.sideWidth, sizes: current.sizes }) },
  { id: 'triggers-manager', label: '触发器 + 物体管理器', detail: '右侧查引用', make: current => ({ ...defaultLayout(), mainActive: 'triggers', side: ['manager'], sideActive: 'manager', sideWidth: current.sideWidth, sizes: current.sizes }) },
  { id: 'objects-manager', label: '物体 + 物体管理器', detail: '改物编时看引用', make: current => ({ ...defaultLayout(), mainActive: 'objects', side: ['manager'], sideActive: 'manager', sideWidth: current.sideWidth, sizes: current.sizes }) },
  { id: 'triggers-variables', label: '触发器 + 变量', detail: '写触发时管理变量', make: current => ({ ...defaultLayout(), mainActive: 'triggers', side: ['variables'], sideActive: 'variables', sideWidth: current.sideWidth, sizes: current.sizes }) },
];
/** 读取本机保存的布局；未知模块、重复位置与非法尺寸丢弃。R07 初版的标签组格式只保留右侧组与浮动窗口。 */
export function readLayout(value: unknown, known: readonly ModuleId[]): DockLayout {
  const base = defaultLayout();
  if (!value || typeof value !== 'object') return base;
  const raw = value as Partial<DockLayout>;
  const seen = new Set<ModuleId>();
  const side = (Array.isArray(raw.side) ? raw.side : []).filter((id): id is ModuleId => known.includes(id) && !seen.has(id) && !!seen.add(id));
  const floats = (Array.isArray(raw.floats) ? raw.floats : []).filter(item => item && known.includes(item.id) && !seen.has(item.id) && [item.x, item.y, item.w, item.h].every(Number.isFinite) && !!seen.add(item.id)).map(item => ({ id: item.id, x: item.x, y: item.y, w: item.w, h: item.h, maximized: !!item.maximized, collapsed: !!item.collapsed }));
  return {
    mainActive: known.includes(raw.mainActive as ModuleId) ? raw.mainActive as ModuleId : base.mainActive,
    side, sideActive: side.includes(raw.sideActive as ModuleId) ? raw.sideActive : side[0],
    sideWidth: Number.isFinite(raw.sideWidth) ? Math.max(320, Number(raw.sideWidth)) : base.sideWidth,
    floats,
    sizes: Object.fromEntries(Object.entries(raw.sizes && typeof raw.sizes === 'object' ? raw.sizes : {}).filter(([id, size]) => known.includes(id as ModuleId) && size && Number.isFinite(size.w) && Number.isFinite(size.h))),
  };
}
