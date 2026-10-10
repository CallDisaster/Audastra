import { useMemo, useRef, useState } from 'react';
import { ChevronDown, ChevronUp, FileAudio, FileBox, FileImage, FileText, LayoutGrid, List } from 'lucide-react';
import type { EditorObject, ModuleId, ObjectKind, WorkbenchProject } from './types';
import { pickableObjects } from './objectCatalog';
import { placeableKinds } from './worldModel';
import { startPlacing, useWorld } from './worldStore';
import { ObjectPortrait } from './ObjectPortrait';
import { SearchBox } from './ui';

// 资源浏览器：可摆放物体（地图物体在前、原版目录在后）按类别/分组/来源浏览，缩略图拖进视口即摆放，单击进入摆放工具。
// 模型缩略图在原生端由离屏渲染生成；原型用物体图标代替，没有图标的装饰物与可破坏物显示类别图形。

const kindLabels: Record<string, string> = { unit: '单位', item: '物品', destructable: '可破坏物', doodad: '装饰物', files: '地图文件' };
const limit = 360;
const templateId = (object: EditorObject) => object.custom ? object.id : object.rawcode;

export function AssetBrowser({ project, onNavigate }: { project: WorkbenchProject; onNavigate: (module: ModuleId, id?: string) => void }) {
  const world = useWorld();
  const [tab, setTab] = useState<ObjectKind | 'files'>('unit');
  const [query, setQuery] = useState(''), [source, setSource] = useState<'all' | 'map' | 'standard'>('all'), [group, setGroup] = useState('');
  const [grid, setGrid] = useState(true), [open, setOpen] = useState(true), [height, setHeight] = useState(232);
  const drag = useRef<{ y: number; h: number } | null>(null);
  const objects = useMemo(() => tab === 'files' ? [] : pickableObjects(project, tab), [project, tab]);
  const groups = useMemo(() => [...new Set(objects.map(o => o.custom ? '地图物体' : o.category.split(' / ').slice(0, 2).join(' / ')))].filter(Boolean), [objects]);
  const q = query.trim().toLowerCase();
  const list = objects.filter(o => (source === 'all' || (source === 'map') === o.custom) && (!group || (o.custom ? '地图物体' : o.category.split(' / ').slice(0, 2).join(' / ')) === group) && (!q || `${o.name} ${o.rawcode}`.toLowerCase().includes(q)));
  const files = project.assets.filter(a => !q || `${a.name} ${a.path}`.toLowerCase().includes(q));

  return <section className={`ab-root ${open ? '' : 'is-closed'}`} style={open ? { height } : undefined} aria-label="资源浏览器">
    {open && <div className="ab-resize" role="separator" aria-orientation="horizontal" aria-label="调整资源浏览器高度" onPointerDown={event => { drag.current = { y: event.clientY, h: height }; event.currentTarget.setPointerCapture(event.pointerId); }} onPointerMove={event => { if (drag.current) setHeight(Math.max(140, Math.min(520, drag.current.h - (event.clientY - drag.current.y)))); }} onPointerUp={() => { drag.current = null; }} />}
    <header className="ab-header">
      <strong>资源浏览器</strong>
      <nav className="ab-tabs" role="tablist">{[...placeableKinds, 'files' as const].map(kind => <button key={kind} role="tab" aria-selected={tab === kind} className={tab === kind ? 'active' : ''} onClick={() => { setTab(kind); setGroup(''); setOpen(true); }}>{kindLabels[kind]}</button>)}</nav>
      {open && <>
        <SearchBox value={query} onChange={setQuery} placeholder="名称或 rawcode" label="搜索资源" />
        {tab !== 'files' && <><select aria-label="来源" value={source} onChange={event => setSource(event.target.value as typeof source)}><option value="all">全部来源</option><option value="map">地图物体</option><option value="standard">原版</option></select>
          <select aria-label="分组" value={group} onChange={event => setGroup(event.target.value)}><option value="">全部分组</option>{groups.map(g => <option key={g}>{g}</option>)}</select></>}
      </>}
      <div className="wb-spacer" />
      {open && tab !== 'files' && <div className="wb-segment"><button className={grid ? 'active' : ''} aria-label="缩略图" onClick={() => setGrid(true)}><LayoutGrid size={13} /></button><button className={!grid ? 'active' : ''} aria-label="列表" onClick={() => setGrid(false)}><List size={13} /></button></div>}
      <button className="ab-toggle" aria-label={open ? '收起资源浏览器' : '展开资源浏览器'} onClick={() => setOpen(!open)}>{open ? <ChevronDown size={14} /> : <ChevronUp size={14} />}</button>
    </header>
    {open && (tab === 'files' ? <div className="ab-files">{files.map(asset => <button key={asset.id} className="ab-file" onClick={() => onNavigate('assets', asset.id)} title={asset.path}>{asset.type.startsWith('audio') ? <FileAudio size={16} /> : asset.type.startsWith('image') ? <FileImage size={16} /> : /mdx|mdl/i.test(asset.path) ? <FileBox size={16} /> : <FileText size={16} />}<span>{asset.name}</span><small>{asset.path}</small></button>)}{!files.length && <div className="wb-empty">没有地图文件</div>}</div>
      : <div className={`ab-items ${grid ? 'is-grid' : 'is-list'}`} role="listbox" aria-label={`${kindLabels[tab]}列表`}>
        {list.slice(0, limit).map(object => { const id = templateId(object), active = world.tool === 'place' && world.template === id; return <button key={`${object.custom ? 'm' : 's'}:${object.id}`} role="option" aria-selected={active} className={`ab-item ${active ? 'is-active' : ''}`} draggable title={`${object.name} · ${object.rawcode}\n单击进入摆放 · 拖进视口放到指定位置 · 双击打开物体定义`}
          onDragStart={event => { event.dataTransfer.setData('application/x-forge-object', id); event.dataTransfer.effectAllowed = 'copy'; }}
          onClick={() => startPlacing(id)} onDoubleClick={() => onNavigate('objects', object.custom ? object.id : `${object.kind}:${object.rawcode}`)}>
          <ObjectPortrait object={object} project={project} size={grid ? 44 : 22} /><span>{object.name}</span><small>{object.rawcode}{object.custom && <b>地图</b>}</small>
        </button>; })}
        {list.length > limit && <div className="ab-more">还有 {list.length - limit} 项，输入名称或选择分组缩小范围</div>}
        {!list.length && <div className="wb-empty">{objects.length ? '没有匹配的物体' : '没有可摆放的物体；导入本机原版数据后显示原版目录'}</div>}
      </div>)}
  </section>;
}
