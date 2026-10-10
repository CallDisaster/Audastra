import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import type { PointerEvent as ReactPointerEvent } from 'react';
import { Bookmark, Camera, Crosshair, Eye, Grid3x3, Magnet, Maximize, MousePointer2, PanelLeft, PanelRight, Plus, SquareDashed } from 'lucide-react';
import type { EditorObject, WorkbenchProject } from './types';
import type { WorldEntity } from './worldModel';
import { CELL, angleTo, isBuilding, objectIcon, clamp, duplicateEntities, entityName, moveEntities, nextWorldId, patchEntities, placeInstance, playerOf, players, removeEntities, selectionBounds, snap, uniqueName, worldBounds, worldEntities } from './worldModel';
import type { WorldView, WorldTool } from './worldStore';
import { select, setWorld, subscribeWorld, useWorld, worldState } from './worldStore';
import { findObject } from './objectCatalog';

// 世界视口（原型）：俯视二维示意，交互与原生 D3D11 视口一致——选择/框选/拖动/Ctrl 拖动转向/摆放预览/区域/镜头、
// 中键或右键平移、滚轮以光标为中心缩放。地形只是示意色块，原生端由 R1–R7 渲染器绘制真实画面。

const toolMeta: { id: WorldTool; label: string; key: string; icon: typeof MousePointer2 }[] = [
  { id: 'select', label: '选择', key: 'V', icon: MousePointer2 }, { id: 'place', label: '摆放', key: 'B', icon: Plus },
  { id: 'region', label: '区域', key: 'R', icon: SquareDashed }, { id: 'camera', label: '镜头', key: 'C', icon: Camera },
];
type Gesture =
  | { kind: 'pan'; x: number; y: number; view: WorldView }
  | { kind: 'press'; id: string; x: number; y: number; wx: number; wy: number; ctrl: boolean; moved: boolean }
  | { kind: 'move'; anchor: { x: number; y: number }; wx: number; wy: number }
  | { kind: 'rotate' }
  | { kind: 'marquee'; wx: number; wy: number; additive: boolean }
  | { kind: 'region'; wx: number; wy: number }
  | { kind: 'resize'; id: string; corner: 'nw' | 'ne' | 'sw' | 'se' };
let clipboard: { project: WorkbenchProject; ids: string[]; center: { x: number; y: number } } | null = null;


export function WorldViewport({ project, onChange, notify, templateObject, panels, onTogglePanel }: { project: WorkbenchProject; onChange: (next: WorkbenchProject, label: string) => void; notify: (text: string) => void; templateObject?: EditorObject; panels: { outliner: boolean; inspector: boolean }; onTogglePanel: (panel: 'outliner' | 'inspector') => void }) {
  const world = useWorld();
  const bounds = worldBounds(project);
  const svg = useRef<SVGSVGElement>(null);
  const [size, setSize] = useState({ w: 800, h: 600 });
  const [view, setView] = useState<WorldView>(() => ({ cx: 0, cy: 0, zoom: 0.12 }));
  const [cursor, setCursor] = useState<{ x: number; y: number } | null>(null);
  const [gesture, setGesture] = useState<Gesture | null>(null);
  const [preview, setPreview] = useState<{ dx: number; dy: number; angle?: number; rect?: { x: number; y: number; width: number; height: number } } | null>(null);
  const [menu, setMenu] = useState<'layers' | 'bookmarks' | null>(null);
  const entities = useMemo(() => worldEntities(project), [project]);
  const selected = useMemo(() => new Set(world.selection), [world.selection]);
  const hidden = useMemo(() => new Set(world.hidden), [world.hidden]), locked = useMemo(() => new Set(world.locked), [world.locked]);

  // 视图适配放在外部回调里（尺寸观察、选择存储订阅），不在 effect 主体里同步 setState。
  const sizeRef = useRef(size), projectRef = useRef(project), fitted = useRef(false), frameSeen = useRef(world.frameRequest);
  useEffect(() => { projectRef.current = project; });
  const fitTo = (box: { minX: number; minY: number; maxX: number; maxY: number }, margin = 1.08) => { const s = sizeRef.current; setView({ cx: (box.minX + box.maxX) / 2, cy: (box.minY + box.maxY) / 2, zoom: clamp(Math.min(s.w / ((box.maxX - box.minX) * margin), s.h / ((box.maxY - box.minY) * margin)), 0.02, 2) }); };
  const fit = () => fitTo(worldBounds(projectRef.current));
  // 对准选择（命令面板、大纲双击、F 键）：选择框四周留 512 的边。
  const frame = () => { const state = worldState(); frameSeen.current = state.frameRequest; const box = selectionBounds(projectRef.current, state.selection); if (box) fitTo({ minX: box.minX - 512, minY: box.minY - 512, maxX: box.maxX + 512, maxY: box.maxY + 512 }, 1); };
  const fitRef = useRef({ fit, frame });
  useEffect(() => { fitRef.current = { fit, frame }; });
  useLayoutEffect(() => {
    const element = svg.current; if (!element) return;
    const observer = new ResizeObserver(() => {
      const r = element.getBoundingClientRect(); sizeRef.current = { w: Math.max(1, r.width), h: Math.max(1, r.height) }; setSize(sizeRef.current);
      if (!fitted.current && r.width > 1) { fitted.current = true; if (worldState().frameRequest !== frameSeen.current) fitRef.current.frame(); else fitRef.current.fit(); }
    });
    observer.observe(element);
    const unsubscribe = subscribeWorld(() => { if (fitted.current && worldState().frameRequest !== frameSeen.current) fitRef.current.frame(); });
    return () => { observer.disconnect(); unsubscribe(); };
  }, []);

  const toWorld = (event: { clientX: number; clientY: number }) => {
    const r = svg.current!.getBoundingClientRect();
    return { x: view.cx + (event.clientX - r.left - r.width / 2) / view.zoom, y: view.cy - (event.clientY - r.top - r.height / 2) / view.zoom };
  };
  const snapped = (p: { x: number; y: number }) => world.snap ? { x: snap(p.x, world.step), y: snap(p.y, world.step) } : { x: Math.round(p.x), y: Math.round(p.y) };
  const pick = (id: string) => entities.find(entity => entity.id === id);
  const movable = world.selection.filter(id => !locked.has(id));

  function begin(event: ReactPointerEvent<SVGSVGElement>) {
    svg.current?.focus();
    const p = toWorld(event);
    if (event.button === 1 || event.button === 2 || (event.button === 0 && event.altKey)) { event.preventDefault(); svg.current!.setPointerCapture(event.pointerId); setGesture({ kind: 'pan', x: event.clientX, y: event.clientY, view }); return; }
    if (event.button !== 0) return;
    const target = (event.target as Element).closest<SVGElement>('[data-entity]');
    const handle = (event.target as Element).closest<SVGElement>('[data-corner]');
    svg.current!.setPointerCapture(event.pointerId);
    if (handle && world.tool === 'select') { setGesture({ kind: 'resize', id: handle.dataset.id!, corner: handle.dataset.corner as 'nw' }); return; }
    if (world.tool === 'place') {
      if (!world.template) { notify('先在资源浏览器里选择要摆放的物体'); return; }
      const at = snapped(p), result = placeInstance(project, world.template, at.x, at.y, world.owner);
      onChange(result.project, `摆放 ${templateObject?.name ?? world.template}`); select([result.id]); return;
    }
    if (world.tool === 'region') { const at = snapped(p); setGesture({ kind: 'region', wx: at.x, wy: at.y }); setPreview({ dx: 0, dy: 0, rect: { x: at.x, y: at.y, width: 0, height: 0 } }); return; }
    if (world.tool === 'camera') {
      const at = snapped(p), id = nextWorldId(project, 'camera'), cameras = project.world.cameras ?? [];
      onChange({ ...project, world: { ...project.world, cameras: [...cameras, { id, name: uniqueName(cameras.map(c => c.name), '镜头 1'), x: at.x, y: at.y, distance: 1650, rotation: 90, aoa: 304, fov: 70 }] } }, '创建镜头'); select([id]); return;
    }
    if (target && !locked.has(target.dataset.entity!)) {
      const id = target.dataset.entity!;
      if (event.shiftKey) { select([id], 'add'); setGesture({ kind: 'press', id, x: event.clientX, y: event.clientY, wx: p.x, wy: p.y, ctrl: false, moved: false }); return; }
      if (event.ctrlKey || event.metaKey) { if (selected.has(id)) setGesture({ kind: 'press', id, x: event.clientX, y: event.clientY, wx: p.x, wy: p.y, ctrl: true, moved: false }); else select([id], 'add'); return; }
      if (!selected.has(id)) select([id]);
      setGesture({ kind: 'press', id, x: event.clientX, y: event.clientY, wx: p.x, wy: p.y, ctrl: false, moved: false });
      return;
    }
    setGesture({ kind: 'marquee', wx: p.x, wy: p.y, additive: event.shiftKey });
    setPreview({ dx: 0, dy: 0, rect: { x: p.x, y: p.y, width: 0, height: 0 } });
  }
  function moveTo(event: ReactPointerEvent<SVGSVGElement>) {
    const p = toWorld(event); setCursor(p);
    if (!gesture) return;
    if (gesture.kind === 'pan') { setView({ ...gesture.view, cx: gesture.view.cx - (event.clientX - gesture.x) / view.zoom, cy: gesture.view.cy + (event.clientY - gesture.y) / view.zoom }); return; }
    if (gesture.kind === 'press') {
      if (Math.hypot(event.clientX - gesture.x, event.clientY - gesture.y) < 4) return;
      if (gesture.ctrl) { setGesture({ kind: 'rotate' }); return; }
      const anchorEntity = pick(gesture.id); if (!anchorEntity || !movable.length) return;
      const anchor = anchorEntity.kind === 'region' ? { x: anchorEntity.region.x, y: anchorEntity.region.y } : anchorEntity.kind === 'camera' ? anchorEntity.camera : anchorEntity.instance;
      setGesture({ kind: 'move', anchor: { x: anchor.x, y: anchor.y }, wx: gesture.wx, wy: gesture.wy }); return;
    }
    if (gesture.kind === 'move') { const target = snapped({ x: gesture.anchor.x + p.x - gesture.wx, y: gesture.anchor.y + p.y - gesture.wy }); setPreview({ dx: target.x - gesture.anchor.x, dy: target.y - gesture.anchor.y }); return; }
    if (gesture.kind === 'rotate') { setPreview({ dx: 0, dy: 0, angle: 0 }); setCursor(p); return; }
    if (gesture.kind === 'marquee') { setPreview({ dx: 0, dy: 0, rect: { x: Math.min(gesture.wx, p.x), y: Math.min(gesture.wy, p.y), width: Math.abs(p.x - gesture.wx), height: Math.abs(p.y - gesture.wy) } }); return; }
    if (gesture.kind === 'region') { const at = snapped(p); setPreview({ dx: 0, dy: 0, rect: { x: Math.min(gesture.wx, at.x), y: Math.min(gesture.wy, at.y), width: Math.abs(at.x - gesture.wx), height: Math.abs(at.y - gesture.wy) } }); return; }
    if (gesture.kind === 'resize') {
      const entity = pick(gesture.id); if (entity?.kind !== 'region') return; const r = entity.region, at = snapped(p);
      const left = gesture.corner.includes('w') ? Math.min(at.x, r.x + r.width - CELL / 4) : r.x, right = gesture.corner.includes('e') ? Math.max(at.x, r.x + CELL / 4) : r.x + r.width;
      const bottom = gesture.corner.startsWith('s') ? Math.min(at.y, r.y + r.height - CELL / 4) : r.y, top = gesture.corner.startsWith('n') ? Math.max(at.y, r.y + CELL / 4) : r.y + r.height;
      setPreview({ dx: 0, dy: 0, rect: { x: left, y: bottom, width: right - left, height: top - bottom } });
    }
  }
  function end(event: ReactPointerEvent<SVGSVGElement>) {
    const g = gesture; setGesture(null);
    if (!g) return;
    if (g.kind === 'press') { if (g.ctrl) select([g.id], 'toggle'); setPreview(null); return; }
    if (g.kind === 'move' && preview && (preview.dx || preview.dy)) onChange(moveEntities(project, movable, preview.dx, preview.dy), movable.length > 1 ? `移动 ${movable.length} 个对象` : `移动 ${entityName(pick(movable[0])!)}`);
    if (g.kind === 'rotate') {
      const p = toWorld(event), ids = movable.filter(id => { const e = pick(id); return e && e.kind !== 'region' && e.kind !== 'camera'; });
      let next = project; for (const id of ids) { const e = pick(id)!; if (e.kind === 'region' || e.kind === 'camera') continue; next = patchEntities(next, [id], { instance: { rotation: angleTo(e.instance, p, world.angleStep) } }); }
      if (ids.length) onChange(next, ids.length > 1 ? `转向 ${ids.length} 个对象` : '修改朝向');
    }
    if (g.kind === 'marquee' && preview?.rect) {
      const r = preview.rect;
      if (r.width * view.zoom < 3 && r.height * view.zoom < 3) { if (!g.additive) select([]); }
      else { const inside = entities.filter(e => !hidden.has(e.id) && !locked.has(e.id) && (e.kind !== 'doodad' || world.layers.doodads) && (() => { const c = e.kind === 'region' ? { x: e.region.x + e.region.width / 2, y: e.region.y + e.region.height / 2 } : e.kind === 'camera' ? e.camera : e.instance; return c.x >= r.x && c.x <= r.x + r.width && c.y >= r.y && c.y <= r.y + r.height; })()).map(e => e.id); select(inside, g.additive ? 'add' : 'replace'); }
    }
    if (g.kind === 'region' && preview?.rect) {
      const r = preview.rect;
      if (r.width >= CELL / 2 && r.height >= CELL / 2) { const id = nextWorldId(project, 'region'); onChange({ ...project, world: { ...project.world, regions: [...project.world.regions, { id, name: uniqueName(project.world.regions.map(x => x.name), '区域 1'), ...r }] } }, '创建区域'); select([id]); }
      else notify('区域至少半格宽；拖动鼠标画出矩形');
    }
    if (g.kind === 'resize' && preview?.rect) onChange(patchEntities(project, [g.id], { region: preview.rect }), '调整区域大小');
    setPreview(null);
  }
  function wheel(event: React.WheelEvent<SVGSVGElement>) {
    const before = toWorld(event), zoom = clamp(view.zoom * Math.exp(-event.deltaY * 0.0015), 0.02, 2);
    const r = svg.current!.getBoundingClientRect();
    setView({ zoom, cx: before.x - (event.clientX - r.left - r.width / 2) / zoom, cy: before.y + (event.clientY - r.top - r.height / 2) / zoom });
  }
  function key(event: React.KeyboardEvent<SVGSVGElement>) {
    const ctrl = event.ctrlKey || event.metaKey, lower = event.key.toLowerCase();
    if (!ctrl && !event.altKey) { const tool = toolMeta.find(t => t.key.toLowerCase() === lower); if (tool) { event.preventDefault(); setWorld({ tool: tool.id }); return; } }
    if (event.key === 'Escape') { if (world.tool !== 'select') setWorld({ tool: 'select' }); else select([]); return; }
    if (lower === 'f' && !ctrl) { event.preventDefault(); setWorld(s => ({ frameRequest: s.frameRequest + 1 })); return; }
    if (event.key === 'Home') { event.preventDefault(); fit(); return; }
    if ((event.key === 'Delete' || event.key === 'Backspace') && movable.length) { event.preventDefault(); onChange(removeEntities(project, movable), `删除 ${movable.length} 个对象`); select([]); return; }
    if (ctrl && lower === 'a') { event.preventDefault(); select(entities.filter(e => !hidden.has(e.id) && !locked.has(e.id)).map(e => e.id)); return; }
    if (ctrl && lower === 'c' && world.selection.length) { event.preventDefault(); const box = selectionBounds(project, world.selection)!; clipboard = { project, ids: [...world.selection], center: { x: (box.minX + box.maxX) / 2, y: (box.minY + box.maxY) / 2 } }; notify(`已复制 ${world.selection.length} 个对象`); return; }
    if (ctrl && lower === 'v' && clipboard) {
      event.preventDefault();
      // 粘贴到光标处（保持相对位置）；复制来源取复制时的数据，粘贴前已删除的对象也能恢复。
      const at = snapped(cursor ?? { x: view.cx, y: view.cy }), source = clipboard.project;
      const merged: WorkbenchProject = { ...project, world: { ...project.world, instances: [...project.world.instances, ...source.world.instances.filter(i => clipboard!.ids.includes(i.id) && !project.world.instances.some(x => x.id === i.id))], regions: [...project.world.regions, ...source.world.regions.filter(r => clipboard!.ids.includes(r.id) && !project.world.regions.some(x => x.id === r.id))], cameras: [...(project.world.cameras ?? []), ...(source.world.cameras ?? []).filter(c => clipboard!.ids.includes(c.id) && !(project.world.cameras ?? []).some(x => x.id === c.id))] } };
      const result = duplicateEntities(merged, clipboard.ids, at.x - clipboard.center.x, at.y - clipboard.center.y);
      const restored = removeEntities(result.project, clipboard.ids.filter(id => !worldEntities(project).some(e => e.id === id)));
      onChange(restored, `粘贴 ${result.ids.length} 个对象`); select(result.ids); return;
    }
    if (ctrl && lower === 'd' && world.selection.length) { event.preventDefault(); const result = duplicateEntities(project, world.selection, world.step * 2, -world.step * 2); onChange(result.project, `创建 ${result.ids.length} 个副本`); select(result.ids); return; }
    const arrows: Record<string, [number, number]> = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, 1], ArrowDown: [0, -1] };
    if (arrows[event.key] && movable.length) { event.preventDefault(); const step = (world.snap ? world.step : 1) * (event.shiftKey ? 4 : 1); onChange(moveEntities(project, movable, arrows[event.key][0] * step, arrows[event.key][1] * step), '微调位置'); }
  }

  // ---- 绘制 ----
  const px = (value: number) => value / view.zoom; // 屏幕像素 → 世界单位
  const vb = `${view.cx - size.w / 2 / view.zoom} ${-view.cy - size.h / 2 / view.zoom} ${size.w / view.zoom} ${size.h / view.zoom}`;
  const offset = (id: string) => preview && selected.has(id) && !locked.has(id) && (preview.dx || preview.dy) ? { dx: preview.dx, dy: preview.dy } : { dx: 0, dy: 0 };
  const rotateTarget = gesture?.kind === 'rotate' ? cursor : null;
  const facing = (e: Extract<WorldEntity, { instance: unknown }>) => rotateTarget && selected.has(e.id) && !locked.has(e.id) ? angleTo(e.instance, rotateTarget, world.angleStep) : e.instance.rotation;
  const gridStep = view.zoom * CELL < 10 ? CELL * 4 : CELL;
  const cell = cursor ? { col: Math.floor((cursor.x - bounds.minX) / CELL), row: Math.floor((cursor.y - bounds.minY) / CELL) } : null;
  const ghost = world.tool === 'place' && cursor && world.template ? snapped(cursor) : null;
  const hint = gesture?.kind === 'rotate' ? '松开确定朝向' : world.tool === 'select' ? '单击选择 · Shift 加选 · Ctrl 单击取消 · Ctrl 拖动转向 · 拖空白处框选' : world.tool === 'place' ? (world.template ? `单击摆放 ${templateObject?.name ?? world.template} · Esc 结束` : '在资源浏览器中选择物体') : world.tool === 'region' ? '拖动画出区域' : '单击创建镜头';

  function drawInstance(e: Extract<WorldEntity, { instance: unknown }>, ghostMode = false) {
    const { dx, dy } = ghostMode ? { dx: 0, dy: 0 } : offset(e.id), x = e.instance.x + dx, y = -(e.instance.y + dy), angle = ghostMode ? e.instance.rotation : facing(e);
    const color = playerOf(e.instance.owner).color, icon = objectIcon(e.object), scale = e.instance.scale ?? 1, isSel = !ghostMode && selected.has(e.id);
    const rad = angle * Math.PI / 180;
    if (e.kind === 'destructable') { const r = Math.max(56 * scale, px(4)); return <g key={e.id} data-entity={ghostMode ? undefined : e.id} className={`wv-dest ${isSel ? 'is-selected' : ''}`}><circle cx={x} cy={y} r={r} /><circle cx={x - r * .25} cy={y - r * .25} r={r * .45} className="wv-dest-core" />{isSel && <circle cx={x} cy={y} r={r + px(4)} className="wv-sel-ring" />}</g>; }
    if (e.kind === 'doodad') { const s = 44 * scale; return <g key={e.id} data-entity={ghostMode ? undefined : e.id} className={`wv-doodad ${isSel ? 'is-selected' : ''}`} transform={`rotate(${-angle} ${x} ${y})`}><rect x={x - s} y={y - s} width={s * 2} height={s * 2} rx={8} />{isSel && <rect x={x - s - px(4)} y={y - s - px(4)} width={s * 2 + px(8)} height={s * 2 + px(8)} className="wv-sel-ring" />}</g>; }
    if (e.kind === 'item') { const s = Math.max(30, px(5)); return <g key={e.id} data-entity={ghostMode ? undefined : e.id} className={`wv-item ${isSel ? 'is-selected' : ''}`}><path d={`M${x} ${y - s}L${x + s} ${y}L${x} ${y + s}L${x - s} ${y}Z`} />{isSel && <circle cx={x} cy={y} r={s + px(5)} className="wv-sel-ring" />}</g>; }
    // 缩到全图时单位仍保持可辨认的最小屏幕尺寸（原生端同样有最小拾取半径）。
    const building = isBuilding(e.object), r = Math.max((building ? 150 : 44) * scale, px(building ? 9 : 6)), clip = `clip-${e.id}`;
    return <g key={e.id} data-entity={ghostMode ? undefined : e.id} className={`wv-unit ${building ? 'is-building' : ''} ${isSel ? 'is-selected' : ''}`}>
      {building ? <rect x={x - r} y={y - r} width={r * 2} height={r * 2} rx={10} className="wv-footprint" style={{ stroke: color }} /> : <circle cx={x} cy={y} r={r + 7} className="wv-team" style={{ fill: color }} />}
      <clipPath id={clip}><circle cx={x} cy={y} r={building ? r * .55 : r} /></clipPath>
      {icon ? <image href={icon} x={x - (building ? r * .55 : r)} y={y - (building ? r * .55 : r)} width={(building ? r * .55 : r) * 2} height={(building ? r * .55 : r) * 2} clipPath={`url(#${clip})`} preserveAspectRatio="xMidYMid slice" /> : <><circle cx={x} cy={y} r={building ? r * .55 : r} className="wv-unit-blank" /><text x={x} y={y} className="wv-initial" fontSize={r * .8}>{(e.object?.name ?? '?').slice(0, 1)}</text></>}
      {!building && <line x1={x + Math.cos(rad) * (r + 4)} y1={y - Math.sin(rad) * (r + 4)} x2={x + Math.cos(rad) * (r + 34)} y2={y - Math.sin(rad) * (r + 34)} className="wv-facing" style={{ stroke: color }} />}
      {isSel && (building ? <rect x={x - r - px(5)} y={y - r - px(5)} width={r * 2 + px(10)} height={r * 2 + px(10)} rx={12} className="wv-sel-ring" /> : <circle cx={x} cy={y} r={r + 7 + px(4)} className="wv-sel-ring" />)}
      {(world.layers.names || (isSel && world.selection.length <= 3)) && !ghostMode && <text x={x} y={y + r + 10 + px(14)} className="wv-label" fontSize={px(11)}>{e.object?.name ?? e.instance.objectId}</text>}
    </g>;
  }

  return <section className="wv-root">
    <div className="wv-toolbar" role="toolbar" aria-label="世界视口工具">
      <button className={panels.outliner ? 'active' : ''} aria-pressed={panels.outliner} title="场景大纲" aria-label="显示场景大纲" onClick={() => onTogglePanel('outliner')}><PanelLeft size={14} /></button>
      <div className="wb-segment" role="radiogroup" aria-label="工具">{toolMeta.map(t => <button key={t.id} role="radio" aria-checked={world.tool === t.id} className={world.tool === t.id ? 'active' : ''} title={`${t.label} · ${t.key}`} onClick={() => setWorld({ tool: t.id })}><t.icon size={14} /><span className="wv-text">{t.label}</span></button>)}</div>
      {world.tool === 'place' && <label className="wv-owner" title="新摆放对象的所属玩家"><i style={{ background: playerOf(world.owner).color }} /><select aria-label="摆放所属玩家" value={world.owner} onChange={event => setWorld({ owner: Number(event.target.value) })}>{players.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}</select></label>}
      <span className="wb-divider" />
      <button className={world.snap ? 'active' : ''} title="吸附到网格" aria-pressed={world.snap} onClick={() => setWorld({ snap: !world.snap })}><Magnet size={14} /><span className="wv-text">吸附</span></button>
      <select aria-label="吸附步长" title="位置吸附步长（游戏单位）" value={world.step} onChange={event => setWorld({ step: Number(event.target.value) })}>{[16, 32, 64, 128].map(v => <option key={v} value={v}>{v}</option>)}</select>
      <select aria-label="角度吸附" title="Ctrl 拖动转向的角度步长" value={world.angleStep} onChange={event => setWorld({ angleStep: Number(event.target.value) })}>{[0, 15, 45, 90].map(v => <option key={v} value={v}>{v ? `${v}°` : '自由角度'}</option>)}</select>
      <div className="wv-menu-anchor"><button className={menu === 'layers' ? 'active' : ''} aria-expanded={menu === 'layers'} onClick={() => setMenu(menu === 'layers' ? null : 'layers')}><Eye size={14} /><span className="wv-text">显示</span></button>
        {menu === 'layers' && <div className="et-menu wv-menu" role="menu">{([['grid', '网格'], ['regions', '区域'], ['cameras', '镜头'], ['doodads', '装饰物'], ['names', '单位名称'], ['pathing', '通行（原生端）']] as const).map(([id, label]) => <label key={id} className="wv-check"><input type="checkbox" checked={world.layers[id]} disabled={id === 'pathing'} onChange={event => setWorld(s => ({ layers: { ...s.layers, [id]: event.target.checked } }))} />{label}</label>)}</div>}
      </div>
      <div className="wv-menu-anchor"><button className={menu === 'bookmarks' ? 'active' : ''} aria-expanded={menu === 'bookmarks'} title="视图书签" onClick={() => setMenu(menu === 'bookmarks' ? null : 'bookmarks')}><Bookmark size={14} /><span className="wv-text">书签</span></button>
        {menu === 'bookmarks' && <div className="et-menu wv-menu" role="menu">
          <button role="menuitem" onClick={() => { setWorld(s => ({ bookmarks: [...s.bookmarks, { name: `视图 ${s.bookmarks.length + 1}`, view }] })); setMenu(null); notify('已保存当前视图'); }}><Plus size={13} />保存当前视图</button>
          {world.bookmarks.map((b, i) => <button role="menuitem" key={i} onClick={() => { setView(b.view); setMenu(null); }}><Bookmark size={13} />{b.name}<small>{Math.round(b.view.cx)}, {Math.round(b.view.cy)}</small></button>)}
          {(project.world.cameras ?? []).map(c => <button role="menuitem" key={c.id} onClick={() => { setView({ cx: c.x, cy: c.y, zoom: clamp(size.h / (c.distance * 1.6), 0.02, 2) }); setMenu(null); }}><Camera size={13} />{c.name}<small>镜头</small></button>)}
        </div>}
      </div>
      <div className="wb-spacer" />
      <button title="对准选择 · F" disabled={!world.selection.length} onClick={() => setWorld(s => ({ frameRequest: s.frameRequest + 1 }))}><Crosshair size={14} /><span className="wv-text">对准</span></button>
      <button title="显示全图 · Home" onClick={() => fit()}><Maximize size={14} /><span className="wv-text">全图</span></button>
      <button className={panels.inspector ? 'active' : ''} aria-pressed={panels.inspector} title="属性" aria-label="显示属性" onClick={() => onTogglePanel('inspector')}><PanelRight size={14} /></button>
    </div>
    <div className="wv-stage" onDragOver={event => { if (event.dataTransfer.types.includes('application/x-forge-object')) { event.preventDefault(); event.dataTransfer.dropEffect = 'copy'; setCursor(toWorld(event)); } }}
      onDrop={event => { const objectId = event.dataTransfer.getData('application/x-forge-object'); if (!objectId) return; event.preventDefault(); const at = snapped(toWorld(event)), result = placeInstance(project, objectId, at.x, at.y, world.owner); onChange(result.project, `摆放 ${findObject(project, objectId)?.name ?? objectId}`); select([result.id]); svg.current?.focus(); }}>
      <svg ref={svg} className={`wv-svg tool-${world.tool} ${gesture?.kind === 'pan' ? 'is-panning' : ''}`} viewBox={vb} tabIndex={0} role="application" aria-label="世界视口"
        onPointerDown={begin} onPointerMove={moveTo} onPointerUp={end} onPointerLeave={() => { if (!gesture) setCursor(null); }} onWheel={wheel} onKeyDown={key} onContextMenu={event => event.preventDefault()}
        onDoubleClick={event => { const target = (event.target as Element).closest<SVGElement>('[data-entity]'); if (target) { select([target.dataset.entity!]); setWorld(s => ({ frameRequest: s.frameRequest + 1 })); } }}>
        <defs>
          <pattern id="wv-grid" width={gridStep} height={gridStep} patternUnits="userSpaceOnUse" x={bounds.minX} y={-bounds.maxY}><path d={`M ${gridStep} 0 L 0 0 0 ${gridStep}`} className="wv-grid-line" /></pattern>
          <pattern id="wv-grid-major" width={CELL * 16} height={CELL * 16} patternUnits="userSpaceOnUse" x={bounds.minX} y={-bounds.maxY}><path d={`M ${CELL * 16} 0 L 0 0 0 ${CELL * 16}`} className="wv-grid-major" /></pattern>
        </defs>
        <rect x={bounds.minX - 100000} y={-bounds.maxY - 100000} width={200000 + bounds.maxX - bounds.minX} height={200000 + bounds.maxY - bounds.minY} className="wv-void" />
        <g className="wv-terrain" aria-hidden="true">
          <rect x={bounds.minX} y={-bounds.maxY} width={bounds.maxX - bounds.minX} height={bounds.maxY - bounds.minY} className="wv-ground" />
          <path d={`M ${bounds.minX} ${-bounds.minY - 900} C -2200 2200, -800 700, 0 200 S 2400 -1200, ${bounds.maxX} ${-900}`} className="wv-road" />
          <path d={`M 1400 ${-bounds.maxY} C 1100 -2600, 2400 -1300, 1900 -300 S 2600 1800, 3000 ${-bounds.minY}`} className="wv-water" />
          <ellipse cx={-2600} cy={-2600} rx={900} ry={700} className="wv-dirt" />
          <rect x={bounds.minX + 512} y={-bounds.maxY + 512} width={bounds.maxX - bounds.minX - 1024} height={bounds.maxY - bounds.minY - 1024} className="wv-playable" />
        </g>
        {world.layers.grid && <><rect x={bounds.minX} y={-bounds.maxY} width={bounds.maxX - bounds.minX} height={bounds.maxY - bounds.minY} fill="url(#wv-grid)" style={{ strokeWidth: px(1) }} className="wv-grid" /><rect x={bounds.minX} y={-bounds.maxY} width={bounds.maxX - bounds.minX} height={bounds.maxY - bounds.minY} fill="url(#wv-grid-major)" className="wv-grid" /></>}
        {world.layers.regions && entities.filter(e => e.kind === 'region' && !hidden.has(e.id)).map(e => { if (e.kind !== 'region') return null; const r = preview?.rect && gesture?.kind === 'resize' && gesture.id === e.id ? preview.rect : e.region, { dx, dy } = offset(e.id), isSel = selected.has(e.id); const color = e.region.color ?? '#8ad6b0'; return <g key={e.id} data-entity={e.id} className={`wv-region ${isSel ? 'is-selected' : ''} ${locked.has(e.id) ? 'is-locked' : ''}`} style={{ '--region': color } as React.CSSProperties}>
          <rect x={r.x + dx} y={-(r.y + dy + r.height)} width={r.width} height={r.height} />
          <rect x={r.x + dx} y={-(r.y + dy + r.height)} width={r.width} height={r.height} className="wv-region-hit" />
          <text x={r.x + dx + px(6)} y={-(r.y + dy + r.height) + px(15)} fontSize={px(11)}>{e.region.name}</text>
          {isSel && world.selection.length === 1 && world.tool === 'select' && !locked.has(e.id) && (['nw', 'ne', 'sw', 'se'] as const).map(corner => <rect key={corner} data-corner={corner} data-id={e.id} className="wv-handle" x={(corner.includes('w') ? r.x : r.x + r.width) + dx - px(5)} y={-((corner.startsWith('n') ? r.y + r.height : r.y) + dy) - px(5)} width={px(10)} height={px(10)} />)}
        </g>; })}
        {entities.filter(e => e.kind !== 'region' && e.kind !== 'camera' && !hidden.has(e.id) && (e.kind !== 'doodad' || world.layers.doodads)).sort((a, b) => (a.kind === 'unit' ? 1 : 0) - (b.kind === 'unit' ? 1 : 0)).map(e => e.kind === 'region' || e.kind === 'camera' ? null : drawInstance(e))}
        {world.layers.cameras && entities.filter(e => e.kind === 'camera' && !hidden.has(e.id)).map(e => { if (e.kind !== 'camera') return null; const c = e.camera, { dx, dy } = offset(e.id), x = c.x + dx, y = -(c.y + dy), a = c.rotation * Math.PI / 180, half = c.fov / 2 * Math.PI / 180, len = c.distance * .45; const isSel = selected.has(e.id); return <g key={e.id} data-entity={e.id} className={`wv-camera ${isSel ? 'is-selected' : ''}`}>
          <path d={`M ${x} ${y} L ${x + Math.cos(a - half) * len} ${y - Math.sin(a - half) * len} L ${x + Math.cos(a + half) * len} ${y - Math.sin(a + half) * len} Z`} className="wv-frustum" />
          <circle cx={x} cy={y} r={px(7)} className="wv-camera-dot" /><text x={x + px(10)} y={y - px(8)} fontSize={px(11)}>{c.name}</text>
        </g>; })}
        {ghost && (() => { const object = findObject(project, world.template!); return <g className="wv-ghost">{drawInstance({ kind: object?.kind === 'item' || object?.kind === 'destructable' || object?.kind === 'doodad' ? object.kind : 'unit', id: '__ghost', instance: { id: '__ghost', objectId: world.template!, x: ghost.x, y: ghost.y, owner: world.owner, rotation: 270 }, object }, true)}</g>; })()}
        {preview?.rect && (gesture?.kind === 'marquee' || gesture?.kind === 'region') && <rect x={preview.rect.x} y={-(preview.rect.y + preview.rect.height)} width={preview.rect.width} height={preview.rect.height} className={gesture.kind === 'marquee' ? 'wv-marquee' : 'wv-region-draft'} />}
        {rotateTarget && <circle cx={rotateTarget.x} cy={-rotateTarget.y} r={px(4)} className="wv-rotate-target" />}
      </svg>
      <div className="wv-badge" title="原型为二维示意；原生端使用 D3D11 渲染器显示真实地形与模型">示意视口</div>
    </div>
    <footer className="wv-status">
      <span className="wv-coord">{cursor ? <>X {Math.round(cursor.x)} · Y {Math.round(cursor.y)}{cell && <small>格 {cell.col}, {cell.row}</small>}</> : '—'}</span>
      <span>{Math.round(view.zoom * 1000) / 10}%</span>
      <span>{world.selection.length ? `已选 ${world.selection.length}` : `${entities.length} 个对象`}</span>
      <div className="wb-spacer" /><span className="wv-hint"><Grid3x3 size={12} />{hint}</span>
    </footer>
  </section>;
}
