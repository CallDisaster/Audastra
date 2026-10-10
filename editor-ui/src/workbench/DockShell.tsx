import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import type { PointerEvent as ReactPointerEvent, ReactNode, RefObject } from 'react';
import type { LucideIcon } from 'lucide-react';
import { AppWindow, Check, ChevronDown, ChevronUp, LayoutPanelLeft, Maximize2, Minimize2, PanelRight, PanelTop, X } from 'lucide-react';
import type { ModuleId } from './types';
import type { DockLayout, FloatWindow } from './dockLayout';
import { activate, clampFloat, layoutPresets, mainShown, place, updateFloat, whereIs } from './dockLayout';
import { restoreScroll, saveScroll } from './moduleHost';

// 工作台外壳：顶栏模块切换器 + 主编辑区 + 右侧停靠组 + 浮动窗口。
// 模块切换器即全部模块（没有侧栏和标签栏两层重复）：点击切换，拖到空白处成为独立窗口，拖到右侧提示区停靠到右侧；
// 窗口或右侧标签拖回切换器即放回主编辑区。拖动过程中直接改 DOM 位置，松开时才提交布局，避免每次移动都重绘所有已打开的编辑器。

export type ModuleMeta = { id: ModuleId; label: string; icon: LucideIcon; shortcut: string };
type Drop = { type: 'main' } | { type: 'side'; index: number } | { type: 'edge' } | { type: 'float'; x: number; y: number } | null;
type Drag =
  | { kind: 'module'; id: ModuleId; startX: number; startY: number; started: boolean }
  | { kind: 'window'; id: ModuleId; offsetX: number; offsetY: number; element: HTMLElement; startX: number; startY: number; started: boolean };
type Resize = { id: ModuleId; dir: string; startX: number; startY: number; origin: FloatWindow; element: HTMLElement };
const handles = ['n', 's', 'e', 'w', 'ne', 'nw', 'se', 'sw'];

/** 模块的挂载点：把宿主节点搬到这里并恢复滚动位置；卸载时搬回停放区。 */
export function ModuleSlot({ host, park }: { host: HTMLElement; park: RefObject<HTMLElement | null> }) {
  const slot = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const element = slot.current!, parking = park.current;
    element.appendChild(host);
    restoreScroll(host);
    return () => { saveScroll(host); parking?.appendChild(host); };
  }, [host, park]);
  return <div ref={slot} className="wb-module-slot"/>;
}

export function DockShell({ modules, layout, onLayout, focused, onActivate, onFocus, hosts, park, headerStart, headerEnd, aside }: {
  modules: readonly ModuleMeta[]; layout: DockLayout; onLayout: (layout: DockLayout, focus?: ModuleId) => void;
  focused: ModuleId; onActivate: (id: ModuleId) => void;
  /** 点击某个已显示的编辑器（不改变布局），用于标记当前焦点模块。 */
  onFocus: (id: ModuleId) => void;
  hosts: Record<ModuleId, HTMLElement>; park: RefObject<HTMLElement | null>;
  headerStart?: ReactNode; headerEnd?: ReactNode;
  /** 右侧全局工具栏位（修改/测试/助手），不属于任何模块，推开主区而不遮挡。 */
  aside?: ReactNode;
}) {
  const root = useRef<HTMLDivElement>(null);
  const ghost = useRef<HTMLDivElement>(null);
  const dragRef = useRef<Drag | null>(null);
  const dropRef = useRef<Drop>(null);
  const layoutRef = useRef(layout);
  useEffect(() => { layoutRef.current = layout; }, [layout]);
  const [dragging, setDragging] = useState<{ id: ModuleId; kind: Drag['kind'] } | null>(null);
  const [drop, setDrop] = useState<Drop>(null);
  const [menu, setMenu] = useState<{ x: number; y: number; id: ModuleId } | null>(null);
  const [layoutMenu, setLayoutMenu] = useState(false);
  useEffect(() => { if (!menu && !layoutMenu) return; const close = () => { setMenu(null); setLayoutMenu(false); }; window.addEventListener('pointerdown', close); window.addEventListener('blur', close); return () => { window.removeEventListener('pointerdown', close); window.removeEventListener('blur', close); }; }, [menu, layoutMenu]);
  const all = modules.map(item => item.id);
  const meta = (id: ModuleId) => modules.find(item => item.id === id)!;
  const bounds = () => { const rect = root.current!.getBoundingClientRect(); return { left: rect.left, top: rect.top, width: rect.width, height: rect.height }; };

  // 点击窗口或停靠区即成为当前模块，浮动窗口同时提到最前（与 Windows 窗口次序一致）。
  // 编辑器内容经 React Portal 渲染，合成事件不会冒泡到这里，所以用原生捕获监听。
  const focusRef = useRef({ focused, onFocus, onLayout });
  useEffect(() => { focusRef.current = { focused, onFocus, onLayout }; });
  useEffect(() => {
    const element = root.current!;
    const handler = (event: PointerEvent) => {
      const target = event.target as HTMLElement;
      const { focused, onFocus, onLayout } = focusRef.current;
      const window = target.closest<HTMLElement>('[data-window]')?.dataset.window as ModuleId | undefined;
      if (window) {
        if (layoutRef.current.floats.at(-1)?.id !== window) onLayout(activate(layoutRef.current, window), window);
        else if (focused !== window) onFocus(window);
        return;
      }
      const docked = target.closest<HTMLElement>('[data-module]')?.dataset.module as ModuleId | undefined;
      if (docked && focused !== docked) onFocus(docked);
    };
    element.addEventListener('pointerdown', handler, true);
    return () => element.removeEventListener('pointerdown', handler, true);
  }, []);

  function hitTest(x: number, y: number, dragged?: ModuleId): Drop {
    for (const element of document.elementsFromPoint(x, y)) {
      if (dragged && element.closest(`[data-window="${dragged}"]`)) continue;
      const target = element.closest<HTMLElement>('[data-drop]');
      if (!target) continue;
      const kind = target.dataset.drop!;
      if (kind === 'main') return { type: 'main' };
      if (kind === 'edge') return { type: 'edge' };
      if (kind === 'side') {
        const tabs = [...target.querySelectorAll<HTMLElement>('[data-tab]')].filter(tab => tab.dataset.tab !== dragged);
        return { type: 'side', index: tabs.filter(tab => { const rect = tab.getBoundingClientRect(); return rect.left + rect.width / 2 < x; }).length };
      }
      break;
    }
    const area = bounds();
    return { type: 'float', x: x - area.left, y: y - area.top };
  }
  const sameDrop = (a: Drop, b: Drop) => JSON.stringify(a && a.type !== 'float' ? a : a?.type) === JSON.stringify(b && b.type !== 'float' ? b : b?.type);

  function move(event: PointerEvent) {
    const drag = dragRef.current;
    if (!drag) return;
    if (!drag.started) {
      if (Math.hypot(event.clientX - drag.startX, event.clientY - drag.startY) < 6) return;
      drag.started = true;
      setDragging({ id: drag.id, kind: drag.kind });
      setMenu(null); setLayoutMenu(false);
    }
    const area = bounds();
    if (drag.kind === 'window') {
      drag.element.style.left = `${event.clientX - area.left - drag.offsetX}px`;
      drag.element.style.top = `${Math.max(0, event.clientY - area.top - drag.offsetY)}px`;
    } else if (ghost.current) {
      ghost.current.style.transform = `translate(${event.clientX + 12}px, ${event.clientY + 12}px)`;
    }
    const next = hitTest(event.clientX, event.clientY, drag.id);
    if (!sameDrop(next, dropRef.current)) setDrop(next);
    dropRef.current = next;
  }
  // 本次拖动注册的监听函数：组件在拖动中会重绘，必须移除注册时的那一个。
  const listening = useRef<((event: PointerEvent) => void) | null>(null);
  function up(event: PointerEvent) {
    if (listening.current) window.removeEventListener('pointermove', listening.current);
    listening.current = null;
    const drag = dragRef.current;
    // 松开时重新判定目标：快速甩动时提示区可能在最后一次移动后才出现。
    const target = drag?.started ? hitTest(event.clientX, event.clientY, drag.id) : dropRef.current;
    dragRef.current = null; dropRef.current = null;
    setDragging(null); setDrop(null);
    if (!drag) return;
    const current = layoutRef.current;
    if (!drag.started) { if (drag.kind === 'module') onActivate(drag.id); return; }
    const area = bounds();
    if (target?.type === 'main') { onLayout(place(current, drag.id, { area: 'main' }), drag.id); return; }
    if (target?.type === 'side') { onLayout(place(current, drag.id, { area: 'side', index: target.index }), drag.id); return; }
    if (target?.type === 'edge') { onLayout(place(current, drag.id, { area: 'side' }), drag.id); return; }
    if (drag.kind === 'window') {
      const x = event.clientX - area.left - drag.offsetX, y = Math.max(0, event.clientY - area.top - drag.offsetY);
      const window = current.floats.find(item => item.id === drag.id)!;
      onLayout(updateFloat(current, drag.id, clampFloat({ ...window, x, y, maximized: false }, area)), drag.id);
      return;
    }
    if (!target) return;
    const previous = current.floats.find(item => item.id === drag.id) ?? current.sizes?.[drag.id];
    const w = Math.min(previous?.w ?? 960, area.width - 40), h = Math.min(previous?.h ?? 620, area.height - 40);
    // 新窗口的标题栏落在指针处，并尽量完整放进工作区。
    const x = Math.max(0, Math.min(target.x - 120, area.width - w)), y = Math.max(0, Math.min(target.y - 14, area.height - h));
    onLayout(place(current, drag.id, { area: 'float', x, y, w, h }), drag.id);
  }
  function begin(drag: Drag) { dragRef.current = drag; listening.current = move; window.addEventListener('pointermove', move); window.addEventListener('pointerup', up, { once: true }); }
  const beginModule = (event: ReactPointerEvent, id: ModuleId) => { if (event.button !== 0) return; event.preventDefault(); begin({ kind: 'module', id, startX: event.clientX, startY: event.clientY, started: false }); };
  const beginWindow = (event: ReactPointerEvent, id: ModuleId) => {
    if (event.button !== 0 || (event.target as HTMLElement).closest('button')) return;
    const element = (event.currentTarget as HTMLElement).closest<HTMLElement>('[data-window]')!;
    if (layoutRef.current.floats.find(item => item.id === id)?.maximized) return;
    const rect = element.getBoundingClientRect();
    event.preventDefault();
    begin({ kind: 'window', id, element, offsetX: event.clientX - rect.left, offsetY: event.clientY - rect.top, startX: event.clientX, startY: event.clientY, started: false });
  };
  useEffect(() => () => { if (listening.current) window.removeEventListener('pointermove', listening.current); }, []);

  // ---- 窗口缩放 ----
  const resizeRef = useRef<Resize | null>(null);
  function beginResize(event: ReactPointerEvent, window: FloatWindow, dir: string) {
    event.preventDefault(); event.stopPropagation();
    const element = (event.currentTarget as HTMLElement).closest<HTMLElement>('[data-window]')!;
    resizeRef.current = { id: window.id, dir, startX: event.clientX, startY: event.clientY, origin: window, element };
    (event.currentTarget as HTMLElement).setPointerCapture(event.pointerId);
  }
  function resizeRect(event: ReactPointerEvent) {
    const state = resizeRef.current!;
    const dx = event.clientX - state.startX, dy = event.clientY - state.startY, o = state.origin;
    let { x, y, w, h } = o;
    if (state.dir.includes('e')) w = o.w + dx;
    if (state.dir.includes('s')) h = o.h + dy;
    if (state.dir.includes('w')) { w = o.w - dx; x = o.x + Math.min(dx, o.w - 360); }
    if (state.dir.includes('n')) { h = o.h - dy; y = o.y + Math.min(dy, o.h - 240); }
    return clampFloat({ ...o, x, y, w: Math.max(360, w), h: Math.max(240, h) }, bounds());
  }
  const resizeMove = (event: ReactPointerEvent) => { if (!resizeRef.current) return; const rect = resizeRect(event); Object.assign(resizeRef.current.element.style, { left: `${rect.x}px`, top: `${rect.y}px`, width: `${rect.w}px`, height: `${rect.h}px` }); };
  const resizeEnd = (event: ReactPointerEvent) => { if (!resizeRef.current) return; const rect = resizeRect(event); resizeRef.current = null; onLayout(updateFloat(layoutRef.current, rect.id, rect)); };

  // ---- 分隔条 ----
  const splitRef = useRef<{ startX: number; width: number } | null>(null);
  const dockRef = useRef<HTMLDivElement>(null);
  const splitWidth = (event: ReactPointerEvent) => Math.max(320, Math.min(bounds().width - 420, splitRef.current!.width - (event.clientX - splitRef.current!.startX)));

  const backToMain = (id: ModuleId) => onLayout(place(layoutRef.current, id, { area: 'main' }), id);
  const floatOut = (id: ModuleId) => { const area = bounds(), size = layout.sizes?.[id]; onLayout(place(layout, id, { area: 'float', ...clampFloat({ id, x: 60 + layout.floats.length * 28, y: 40 + layout.floats.length * 28, w: Math.min(size?.w ?? 960, area.width - 120), h: Math.min(size?.h ?? 620, area.height - 80) }, area) }), id); };
  const shown = mainShown(layout, all);
  const front = layout.floats.at(-1)?.id;
  const dropHint = drop?.type === 'main' ? '放回主编辑区' : drop?.type === 'side' || drop?.type === 'edge' ? '停靠到右侧' : '独立窗口';

  return <div className={`wb-shell ${dragging ? 'is-dragging' : ''}`}>
    <header className={`wb-app-header ${drop?.type === 'main' ? 'is-hot' : ''}`} data-drop="main">
      {headerStart}
      <nav className="wb-switcher" role="tablist" aria-label="编辑器模块">{modules.map(item => {
        const where = whereIs(layout, item.id);
        const title = `${item.label} · Alt ${item.shortcut}\n${where === 'float' ? '在独立窗口中 · 点击提到最前' : where === 'side' ? '停靠在右侧' : '拖出成为独立窗口，拖到右侧停靠'}`;
        return <button key={item.id} role="tab" aria-selected={shown === item.id} aria-label={`${item.label}模块`} title={title}
          className={`wb-switch ${shown === item.id ? 'is-shown' : ''} ${focused === item.id ? 'is-focused' : ''} is-${where} ${dragging?.id === item.id ? 'is-dragging' : ''}`}
          onPointerDown={event => beginModule(event, item.id)} onClick={event => { if (event.detail === 0) onActivate(item.id); }}
          onContextMenu={event => { event.preventDefault(); setMenu({ x: event.clientX, y: event.clientY, id: item.id }); }}>
          <item.icon size={15} strokeWidth={1.7}/><span>{item.label}</span>{where === 'float' ? <AppWindow className="wb-switch-badge" size={10}/> : where === 'side' ? <PanelRight className="wb-switch-badge" size={10}/> : null}
        </button>;
      })}</nav>
      {drop?.type === 'main' && <span className="wb-drop-pill">松开放回主编辑区</span>}
      <div className="wb-spacer"/>
      {headerEnd}
      <div className="wb-layout-menu"><button title="布局：预设与重置" aria-label="布局" aria-expanded={layoutMenu} onPointerDown={event => event.stopPropagation()} onClick={() => setLayoutMenu(open => !open)}><LayoutPanelLeft size={16}/></button>
        {layoutMenu && <div className="et-menu wb-layout-list" role="menu" onPointerDown={event => event.stopPropagation()}>
          <div className="wb-menu-label">布局预设</div>
          {layoutPresets.map(preset => <button key={preset.id} role="menuitem" onClick={() => { setLayoutMenu(false); const next = preset.make(layout); onLayout(next, next.mainActive); }}><span>{preset.label}<small>{preset.detail}</small></span></button>)}
          <div className="wb-menu-label">当前</div>
          {modules.filter(item => whereIs(layout, item.id) !== 'main').map(item => <button key={item.id} role="menuitem" onClick={() => { setLayoutMenu(false); backToMain(item.id); }}><item.icon size={14}/><span>{item.label}<small>{whereIs(layout, item.id) === 'float' ? '独立窗口' : '右侧'} · 点击放回主编辑区</small></span></button>)}
          {modules.every(item => whereIs(layout, item.id) === 'main') && <div className="wb-menu-note">全部在主编辑区。拖动顶栏模块到空白处可以拆成独立窗口。</div>}
        </div>}
      </div>
    </header>
    <div className="wb-dock-root" ref={root}>
      <div className="wb-dock" ref={dockRef}>
        <section className={`wb-group is-main ${shown && focused === shown ? 'is-focused' : ''}`} data-module={shown}>
          <div className="wb-group-body">
            {shown ? <ModuleSlot host={hosts[shown]} park={park}/> : <div className="wb-group-empty">所有编辑器都在独立窗口或右侧。把窗口拖回顶栏即可放回这里。</div>}
            {dragging && <div className={`wb-edge-zone ${drop?.type === 'edge' ? 'is-hot' : ''}`} data-drop="edge"><PanelRight size={18}/><span>停靠到右侧</span></div>}
          </div>
        </section>
        {layout.side.length > 0 && <><div className="wb-splitter" role="separator" aria-orientation="vertical" aria-label="调整右侧停靠区宽度" onPointerDown={event => { event.preventDefault(); splitRef.current = { startX: event.clientX, width: layout.sideWidth }; event.currentTarget.setPointerCapture(event.pointerId); }} onPointerMove={event => { if (!splitRef.current) return; const side = dockRef.current?.querySelector<HTMLElement>('.wb-group.is-side'); if (side) side.style.width = `${splitWidth(event)}px`; }} onPointerUp={event => { if (!splitRef.current) return; const width = splitWidth(event); splitRef.current = null; onLayout({ ...layoutRef.current, sideWidth: width }); }}/>
          <section className={`wb-group is-side ${layout.sideActive && focused === layout.sideActive ? 'is-focused' : ''}`} style={{ width: layout.sideWidth }} data-module={layout.sideActive}>
            <div className="wb-side-tabs" data-drop="side" role="tablist" aria-label="右侧停靠区">
              {layout.side.map((id, index) => { const item = meta(id); return <button key={id} data-tab={id} role="tab" aria-selected={layout.sideActive === id} className={`wb-tab ${layout.sideActive === id ? 'is-active' : ''} ${drop?.type === 'side' && drop.index === index ? 'drop-before' : ''}`} title={`${item.label} · 拖回顶栏放回主编辑区`}
                onPointerDown={event => { if (event.button === 1) { event.preventDefault(); backToMain(id); return; } beginModule(event, id); }} onClick={event => { if (event.detail === 0) onActivate(id); }}
                onContextMenu={event => { event.preventDefault(); setMenu({ x: event.clientX, y: event.clientY, id }); }}>
                <item.icon size={13}/><span>{item.label}</span><i role="button" aria-label={`把 ${item.label} 放回主编辑区`} title="放回主编辑区" onPointerDown={event => { event.stopPropagation(); if (event.button === 0) backToMain(id); }}><X size={12}/></i></button>; })}
              {drop?.type === 'side' && drop.index === layout.side.length && <span className="wb-tab-drop-end"/>}
            </div>
            <div className="wb-group-body">{layout.sideActive && <ModuleSlot host={hosts[layout.sideActive]} park={park}/>}</div>
          </section></>}
      </div>
      {aside}
      <div className="wb-float-layer">{layout.floats.map((window, index) => {
        const item = meta(window.id);
        const rect = window.maximized ? { left: 0, top: 0, width: '100%', height: '100%' } : { left: window.x, top: window.y, width: window.w, height: window.collapsed ? 33 : window.h };
        return <section key={window.id} data-window={window.id} className={`wb-float ${window.collapsed && !window.maximized ? 'is-collapsed' : ''} ${front === window.id ? 'is-front' : ''} ${focused === window.id ? 'is-focused' : ''} ${window.maximized ? 'is-maximized' : ''}`} style={{ ...rect, zIndex: 40 + index }} role="dialog" aria-modal="false" aria-label={`${item.label}窗口`}>
          <header onPointerDown={event => beginWindow(event, window.id)} onDoubleClick={event => { if (!(event.target as HTMLElement).closest('button')) onLayout(updateFloat(layout, window.id, { maximized: !window.maximized, collapsed: false })); }}>
            <item.icon size={14}/><strong>{item.label}</strong><span className="wb-float-hint">拖回顶栏放回主编辑区</span><div className="wb-spacer"/>
            <button title="停靠到右侧" aria-label="停靠到右侧" onClick={() => onLayout(place(layout, window.id, { area: 'side' }), window.id)}><PanelRight size={14}/></button>
            {!window.maximized && <button title={window.collapsed ? '展开' : '收起为标题栏'} aria-label={window.collapsed ? '展开窗口' : '收起窗口'} onClick={() => onLayout(updateFloat(layout, window.id, { collapsed: !window.collapsed }))}>{window.collapsed ? <ChevronDown size={14}/> : <ChevronUp size={14}/>}</button>}
            <button title={window.maximized ? '还原' : '最大化'} aria-label={window.maximized ? '还原' : '最大化'} onClick={() => onLayout(updateFloat(layout, window.id, { maximized: !window.maximized }))}>{window.maximized ? <Minimize2 size={13}/> : <Maximize2 size={13}/>}</button>
            <button className="wb-float-close" title="放回主编辑区（状态保留）" aria-label={`把 ${item.label} 放回主编辑区`} onClick={() => backToMain(window.id)}><PanelTop size={14}/></button>
          </header>
          <div className="wb-float-body"><ModuleSlot host={hosts[window.id]} park={park}/></div>
          {!window.maximized && !window.collapsed && handles.map(dir => <i key={dir} className={`wb-resize is-${dir}`} onPointerDown={event => beginResize(event, window, dir)} onPointerMove={resizeMove} onPointerUp={resizeEnd}/>)}
        </section>;
      })}</div>
    </div>
    {dragging?.kind === 'module' && <div className="wb-drag-ghost" ref={ghost}>{(() => { const item = meta(dragging.id); return <><item.icon size={14}/>{item.label}<small>{dropHint}</small></>; })()}</div>}
    {menu && <div className="et-menu wb-tab-menu" role="menu" style={{ left: Math.min(menu.x, window.innerWidth - 230), top: menu.y }} onPointerDown={event => event.stopPropagation()}>
      {whereIs(layout, menu.id) !== 'float' && <button role="menuitem" onClick={() => { setMenu(null); floatOut(menu.id); }}><AppWindow size={14}/>在独立窗口中打开</button>}
      {whereIs(layout, menu.id) !== 'side' && <button role="menuitem" onClick={() => { setMenu(null); onLayout(place(layout, menu.id, { area: 'side' }), menu.id); }}><PanelRight size={14}/>停靠到右侧</button>}
      {whereIs(layout, menu.id) !== 'main' ? <button role="menuitem" onClick={() => { setMenu(null); backToMain(menu.id); }}><PanelTop size={14}/>放回主编辑区</button> : <button role="menuitem" disabled><Check size={14}/>在主编辑区</button>}
    </div>}
  </div>;
}
