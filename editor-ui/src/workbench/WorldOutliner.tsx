import { useMemo, useState } from 'react';
import { Box, Camera, ChevronDown, ChevronRight, Eye, EyeOff, Gem, Lock, LockOpen, SquareDashed, Trees } from 'lucide-react';
import type { WorkbenchProject } from './types';
import type { EntityKind, WorldEntity } from './worldModel';
import { entityKindLabels, entityName, entityPosition, objectIcon, playerOf, worldEntities } from './worldModel';
import { frameSelection, select, toggleIn, useWorld } from './worldStore';
import { SearchBox } from './ui';

// 场景大纲：地图上全部实例/区域/镜头的可搜索树，与视口选择双向同步。
// 单位按所属玩家分组（与原版“单位面板-玩家”一致）；眼睛/锁只影响编辑器显示与点选，不写进地图。

type Group = { key: string; label: string; color?: string; items: WorldEntity[] };
const kindOrder: EntityKind[] = ['unit', 'item', 'destructable', 'doodad', 'region', 'camera'];

export function WorldOutliner({ project }: { project: WorkbenchProject }) {
  const world = useWorld();
  const [query, setQuery] = useState('');
  const [kinds, setKinds] = useState<Set<EntityKind>>(() => new Set(kindOrder));
  const [collapsed, setCollapsed] = useState<Set<string>>(() => new Set(['destructable', 'doodad']));
  const entities = useMemo(() => worldEntities(project), [project]);
  const selected = new Set(world.selection), hidden = new Set(world.hidden), locked = new Set(world.locked);
  const q = query.trim().toLowerCase();
  const matches = (e: WorldEntity) => kinds.has(e.kind) && (!q || `${entityName(e)} ${e.kind === 'region' || e.kind === 'camera' ? e.id : `${e.instance.objectId} ${e.object?.rawcode ?? ''}`}`.toLowerCase().includes(q));
  const visible = entities.filter(matches);
  const groups: Group[] = [];
  const units = visible.filter(e => e.kind === 'unit');
  for (const owner of [...new Set(units.map(e => e.kind === 'unit' ? e.instance.owner : 0))].sort((a, b) => a - b)) {
    const player = playerOf(owner);
    groups.push({ key: `unit:${owner}`, label: `单位 · ${player.name}`, color: player.color, items: units.filter(e => e.kind === 'unit' && e.instance.owner === owner) });
  }
  for (const kind of kindOrder.slice(1)) { const items = visible.filter(e => e.kind === kind); if (items.length) groups.push({ key: kind, label: entityKindLabels[kind], items }); }
  const counts = Object.fromEntries(kindOrder.map(kind => [kind, entities.filter(e => e.kind === kind).length])) as Record<EntityKind, number>;
  const flat = groups.flatMap(g => collapsed.has(g.key) && !q ? [] : g.items);

  function click(event: React.MouseEvent, id: string) {
    if (event.shiftKey && world.selection.length) {
      // Shift：从最后选中的一项到当前项连续选择（按大纲当前顺序）。
      const last = flat.findIndex(e => e.id === world.selection.at(-1)), here = flat.findIndex(e => e.id === id);
      if (last >= 0 && here >= 0) { select(flat.slice(Math.min(last, here), Math.max(last, here) + 1).map(e => e.id), 'add'); return; }
    }
    select([id], event.ctrlKey || event.metaKey ? 'toggle' : 'replace');
  }
  function keyNav(event: React.KeyboardEvent) {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
    event.preventDefault();
    const index = flat.findIndex(e => e.id === world.selection.at(-1)), next = flat[Math.max(0, Math.min(flat.length - 1, index + (event.key === 'ArrowDown' ? 1 : -1)))];
    if (next) { select([next.id]); document.getElementById(`wo-${next.id}`)?.scrollIntoView({ block: 'nearest' }); }
  }
  const icon = (e: WorldEntity) => {
    if (e.kind === 'region') return <span className="wo-swatch" style={{ background: e.region.color ?? '#8ad6b0' }} />;
    if (e.kind === 'camera') return <Camera size={14} />;
    const src = objectIcon(e.object);
    if (src) return <img src={src} alt="" loading="lazy" draggable={false} />;
    return e.kind === 'destructable' ? <Trees size={14} /> : e.kind === 'item' ? <Gem size={14} /> : e.kind === 'doodad' ? <SquareDashed size={14} /> : <Box size={14} />;
  };
  const detail = (e: WorldEntity) => { const p = entityPosition(e); return e.kind === 'region' ? `${e.region.width}×${e.region.height}` : e.kind === 'camera' ? `${Math.round(p.x)}, ${Math.round(p.y)}` : e.object?.rawcode ?? e.instance.objectId; };

  return <aside className="wo-root" aria-label="场景大纲">
    <div className="wb-panel-title">场景大纲<span>{entities.length}</span></div>
    <div className="wo-search"><SearchBox value={query} onChange={setQuery} placeholder="名称、rawcode" label="搜索场景对象" /></div>
    <div className="wo-kinds" role="group" aria-label="对象类别">{kindOrder.map(kind => <button key={kind} className={kinds.has(kind) ? 'active' : ''} aria-pressed={kinds.has(kind)} title={`${entityKindLabels[kind]}：${counts[kind]}`} onClick={() => setKinds(set => { const next = new Set(set); if (next.has(kind)) next.delete(kind); else next.add(kind); return next; })}>{entityKindLabels[kind]}<small>{counts[kind]}</small></button>)}</div>
    <div className="wo-tree" role="tree" aria-multiselectable="true" tabIndex={0} onKeyDown={keyNav}>
      {groups.map(group => {
        const open = !collapsed.has(group.key) || !!q, ids = group.items.map(e => e.id);
        return <div key={group.key} role="group">
          <div className="wo-group">
            <button className="wo-group-toggle" aria-expanded={open} onClick={() => setCollapsed(set => { const next = new Set(set); if (next.has(group.key)) next.delete(group.key); else next.add(group.key); return next; })}>{open ? <ChevronDown size={13} /> : <ChevronRight size={13} />}{group.color && <i className="wo-player" style={{ background: group.color }} />}<span>{group.label}</span><small>{group.items.length}</small></button>
            <button className="wo-group-action" title="选择整组" onClick={() => select(ids)}>全选</button>
            <button className="wo-group-action" title={ids.every(id => hidden.has(id)) ? '显示整组' : '隐藏整组'} onClick={() => toggleIn('hidden', ids)}>{ids.every(id => hidden.has(id)) ? <EyeOff size={13} /> : <Eye size={13} />}</button>
          </div>
          {open && group.items.map(e => <div key={e.id} id={`wo-${e.id}`} role="treeitem" aria-selected={selected.has(e.id)} className={`wo-row ${selected.has(e.id) ? 'is-selected' : ''} ${hidden.has(e.id) ? 'is-hidden' : ''}`} onClick={event => click(event, e.id)} onDoubleClick={() => { select([e.id]); frameSelection(); }}>
            <span className="wo-icon">{icon(e)}</span><span className="wo-name">{entityName(e)}</span><small>{detail(e)}</small>
            <button className={`wo-flag ${hidden.has(e.id) ? 'is-on' : ''}`} title={hidden.has(e.id) ? '在视口中显示' : '在视口中隐藏'} aria-label={hidden.has(e.id) ? '显示' : '隐藏'} onClick={event => { event.stopPropagation(); toggleIn('hidden', [e.id]); }}>{hidden.has(e.id) ? <EyeOff size={13} /> : <Eye size={13} />}</button>
            <button className={`wo-flag ${locked.has(e.id) ? 'is-on' : ''}`} title={locked.has(e.id) ? '解除锁定' : '锁定：视口中不能点选或移动'} aria-label={locked.has(e.id) ? '解锁' : '锁定'} onClick={event => { event.stopPropagation(); toggleIn('locked', [e.id]); }}>{locked.has(e.id) ? <Lock size={13} /> : <LockOpen size={13} />}</button>
          </div>)}
        </div>;
      })}
      {!groups.length && <div className="wb-empty">{entities.length ? '没有匹配的对象' : '地图上还没有对象。从下方资源浏览器拖入视口即可摆放。'}</div>}
    </div>
  </aside>;
}
