import { useEffect, useMemo, useRef, useState } from 'react';
import type { KeyboardEvent as ReactKeyboardEvent, ReactNode } from 'react';
import { AlertTriangle, ArrowUpRight, AudioLines, Box, ChevronDown, ChevronRight, Code2, GitBranch, Hash, LayoutDashboard, Map as MapIcon, Search, Shield, SquareDashed, X } from 'lucide-react';
import type { ModuleId, ModuleProps, ObjectKind, WorkbenchProject } from './types';
import { objectKinds } from './types';
import { useWar3, iconUrl, standardObject } from './war3Data';
import { withCatalog } from './triggerCatalog';
import { buildReferenceIndex, functionCatalog, groupUsages, usagesOf } from './referenceIndex';
import type { FunctionKind, ReferenceIndex, Usage } from './referenceIndex';
import { findObject } from './objectCatalog';
import { buildEca } from './ecaModel';
import type { EcaItem } from './ecaModel';
import './objectManager.css';

// 物体管理器：对应原版“物体管理器”（地图概况 / 单位 / 物品 / 装饰物 / 区域 / 触发器 / 函数 / 变量 / 声音），
// 在每一页列出地图中实际用到的条目，并回答“它在哪里被用到、它用到了什么”。

type Tab = 'overview' | ObjectKind | 'region' | 'trigger' | 'function' | 'variable' | 'sound';
type Tone = 'custom' | 'modified' | 'standard' | 'off' | 'warn' | undefined;
type Row = {
  id: string; name: string; sub?: string; icon?: ReactNode; cells: (string | number)[]; tone?: Tone; status?: string;
  open?: { module: ModuleId; target?: string }; usages: Usage[]; placed?: Usage[]; unused?: boolean;
  outgoing?: string; functionOf?: string;
};
type Column = { label: string; width: string; numeric?: boolean };
const tabs: { id: Tab; label: string }[] = [
  { id: 'overview', label: '地图概况' },
  ...objectKinds.map(kind => ({ id: kind.id as Tab, label: kind.name })),
  { id: 'region', label: '区域' }, { id: 'trigger', label: '触发器' }, { id: 'function', label: '函数' }, { id: 'variable', label: '变量' }, { id: 'sound', label: '声音' },
];
const filtersOf: Partial<Record<Tab, [string, string][]>> = {
  trigger: [['all', '全部'], ['off', '已禁用'], ['closed', '初始关闭'], ['referenced', '被其他触发器引用']],
  function: [['used', '已使用'], ['all', '全部函数']],
  variable: [['all', '全部'], ['unused', '未使用']],
  region: [['all', '全部'], ['unused', '未使用']],
  sound: [['all', '全部'], ['unused', '未使用']],
};
const objectFilters: [string, string][] = [['all', '全部'], ['custom', '自定义'], ['modified', '已修改'], ['standard', '引用的原版'], ['unused', '未使用']];
const functionKinds: (FunctionKind | '全部')[] = ['全部', '事件', '条件', '动作', '流程', '取值'];
const placeable = new Set<Tab>(['unit', 'item', 'destructable', 'doodad']);
const moduleIcon: Record<string, typeof Box> = { triggers: GitBranch, objects: Box, world: MapIcon, ai: Shield, sounds: AudioLines, variables: Hash };

// 会话内记住上次的页签与选中项：从引用跳转出去再回来（Alt+6 或返回）时回到原处。
const memory: { tab: Tab; selection: Partial<Record<Tab, string>>; filter: Partial<Record<Tab, string>> } = { tab: 'overview', selection: {}, filter: {} };

function MiniIcon({ icon, size = 18 }: { icon?: string; size?: number }) {
  return icon ? <img className="om-icon" src={iconUrl(icon)} alt="" width={size} height={size} loading="lazy" draggable={false}/> : <span className="om-icon om-icon-empty" style={{ width: size, height: size }}/>;
}

/** 把“使用了”的记号解析成工程中的条目；普通字符串（坐标、玩家常量等）不会命中。 */
function resolveToken(project: WorkbenchProject, token: string): { type: string; label: string; module: ModuleId; target: string; icon?: string } | undefined {
  const graph = project.graphs.find(item => item.id === token);
  if (graph) return { type: '触发器', label: graph.name, module: 'triggers', target: graph.id };
  const variable = project.variables.find(item => item.id === token || item.name === token);
  if (variable) return { type: '变量', label: variable.name, module: 'variables', target: variable.id };
  const region = project.world.regions.find(item => item.id === token || item.name === token);
  if (region) return { type: '区域', label: region.name, module: 'world', target: region.id };
  const sound = project.sounds.find(item => item.id === token);
  if (sound) return { type: '声音', label: sound.name, module: 'sounds', target: sound.id };
  const object = findObject(project, token);
  if (object) {
    const own = project.objects.some(item => item.id === object.id);
    return { type: objectKinds.find(kind => kind.id === object.kind)?.name ?? '物体', label: object.name, module: 'objects', target: own ? object.id : `${object.kind}:${object.rawcode}`, icon: standardObject(object.kind, object.base ?? object.rawcode)?.icon };
  }
  return undefined;
}

function focusTarget(project: WorkbenchProject, focusId: string | undefined): { tab: Tab; row: string } | undefined {
  if (!focusId) return undefined;
  const standard = focusId.match(/^(unit|item|destructable|doodad|ability|buff|upgrade):(.+)$/);
  if (standard) { const own = project.objects.find(item => item.kind === standard[1] && item.rawcode === standard[2]); return { tab: standard[1] as Tab, row: own?.id ?? focusId }; }
  const object = project.objects.find(item => item.id === focusId);
  if (object) return { tab: object.kind, row: object.id };
  if (project.graphs.some(item => item.id === focusId)) return { tab: 'trigger', row: focusId };
  if (project.variables.some(item => item.id === focusId)) return { tab: 'variable', row: focusId };
  if (project.world.regions.some(item => item.id === focusId)) return { tab: 'region', row: focusId };
  if (project.sounds.some(item => item.id === focusId)) return { tab: 'sound', row: focusId };
  if (focusId.startsWith('function:')) return { tab: 'function', row: focusId.slice(9) };
  return undefined;
}

function UsageList({ usages, onNavigate, empty }: { usages: Usage[]; onNavigate: ModuleProps['onNavigate']; empty: string }) {
  const [collapsed, setCollapsed] = useState<Set<string>>(new Set());
  if (!usages.length) return <p className="om-empty-note">{empty}</p>;
  return <div className="om-usages">{groupUsages(usages).map(group => {
    const Icon = moduleIcon[group.module] ?? Box, key = `${group.module}:${group.ownerId}`, closed = collapsed.has(key);
    return <div className="om-usage-group" key={key}>
      <button className="om-usage-owner" onClick={() => setCollapsed(set => { const next = new Set(set); if (next.has(key)) next.delete(key); else next.add(key); return next; })}>{closed ? <ChevronRight size={12}/> : <ChevronDown size={12}/>}<Icon size={13}/><span>{group.owner}</span><small>{group.items.length}</small></button>
      {!closed && group.items.map((usage, i) => <button key={i} className={`om-usage ${usage.disabled ? 'is-disabled' : ''}`} title={`${usage.text}${usage.disabled ? '（已禁用，不生成代码）' : ''}\n点击跳转`} onClick={() => onNavigate(usage.module, usage.target)}><em>{usage.place}</em><span>{usage.text}</span><ArrowUpRight size={12}/></button>)}
    </div>;
  })}</div>;
}

export function ObjectManager({ project: stored, onNavigate, focusId }: ModuleProps) {
  const war3 = useWar3();
  const project = useMemo(() => withCatalog(stored), [stored]);
  const index = war3.status === 'ready' ? war3.index : undefined;
  const refs: ReferenceIndex = useMemo(() => {
    const labels = new Map<string, string>();
    if (index) for (const [kind, data] of Object.entries(index.kinds)) for (const field of data.fields) labels.set(`${kind}:${field.id}`, field.name);
    return buildReferenceIndex(project, (kind, field) => labels.get(`${kind}:${field}`));
  }, [project, index]);
  const initial = focusTarget(project, focusId);
  const [tab, setTabState] = useState<Tab>(initial?.tab ?? memory.tab);
  const [selection, setSelection] = useState<Partial<Record<Tab, string>>>(() => initial ? { ...memory.selection, [initial.tab]: initial.row } : memory.selection);
  const [filters, setFilters] = useState<Partial<Record<Tab, string>>>(() => initial?.tab === 'function' ? { ...memory.filter, function: 'all' } : memory.filter);
  const [functionKind, setFunctionKind] = useState<FunctionKind | '全部'>('全部');
  const [query, setQuery] = useState('');
  const [sort, setSort] = useState<{ column: number; descending: boolean } | null>(null);
  const table = useRef<HTMLDivElement>(null);
  useEffect(() => { memory.tab = tab; memory.selection = selection; memory.filter = filters; }, [tab, selection, filters]);
  const setTab = (next: Tab) => { setTabState(next); setQuery(''); setSort(null); };
  const filter = filters[tab] ?? (tab === 'function' ? 'used' : 'all');
  const kindName = (kind: ObjectKind) => objectKinds.find(item => item.id === kind)?.name ?? kind;

  const objectRows = (kind: ObjectKind): Row[] => {
    const own = project.objects.filter(object => object.kind === kind);
    const codes = new Set(own.map(object => object.rawcode));
    const make = (id: string, keys: string[], name: string, rawcode: string, icon: string | undefined, tone: Tone, status: string, exclude?: string): Row => {
      const all = usagesOf(refs, keys, exclude), placed = all.filter(usage => usage.module === 'world'), usages = all.filter(usage => usage.module !== 'world');
      const unused = tone === 'custom' && !all.length;
      return { id, name, sub: rawcode, icon: <MiniIcon icon={icon}/>, cells: [name, rawcode, status, ...(placeable.has(kind) ? [placed.length] : []), usages.length], tone: unused ? 'warn' : tone, status, usages, placed, unused, outgoing: exclude, open: { module: 'objects', target: exclude ?? `${kind}:${rawcode}` } };
    };
    const rows = own.map(object => make(object.id, [object.id, object.rawcode], object.name, object.rawcode, standardObject(kind, object.base ?? object.rawcode)?.icon, object.custom ? 'custom' : 'modified', object.custom ? '自定义' : '已修改', object.id));
    // 从物编跳来的原版物体即使没有引用也列出，便于确认“确实没被用到”。
    const focused = selection[kind]?.startsWith(`${kind}:`) ? [selection[kind]!.slice(kind.length + 1)] : [];
    for (const token of [...focused, ...refs.tokens.keys()]) {
      if (codes.has(token) || !/^[A-Za-z0-9]{4}$/.test(token)) continue;
      const standard = standardObject(kind, token);
      if (!standard) continue;
      codes.add(token);
      rows.push(make(`${kind}:${token}`, [token], [standard.name.trim(), standard.suffix.trim()].filter(Boolean).join(' '), token, standard.icon, 'standard', '原版'));
    }
    return rows;
  };
  const triggerRows = (): Row[] => project.graphs.map(graph => {
    const usages = usagesOf(refs, [graph.id], graph.id), eca = buildEca(graph, project.definitions);
    const count = (items: EcaItem[]): number => items.reduce((sum, item) => sum + 1 + item.blocks.reduce((inner, block) => inner + count(block.items), 0), 0);
    const actions = eca.actions.reduce((sum, block) => sum + count(block.items), 0);
    const off = graph.enabled === false, closed = graph.initiallyOn === false;
    const status = off ? '已禁用' : closed ? '初始关闭' : '允许';
    return { id: graph.id, name: graph.name, sub: graph.folder, icon: <GitBranch size={14}/>, cells: [graph.name, graph.folder, status, eca.events.length, actions, usages.length], tone: off ? 'off' : closed ? 'modified' : undefined, status, usages, outgoing: graph.id, open: { module: 'triggers', target: graph.id } };
  });
  const functionRows = (): Row[] => functionCatalog(project).map(entry => {
    const usages = refs.functions.get(entry.id) ?? [];
    const owners = new Set(usages.map(usage => usage.ownerId)).size;
    return { id: entry.id, name: entry.title, sub: entry.category, icon: <Code2 size={14}/>, cells: [entry.title, entry.category, entry.kind, usages.length, owners], usages, functionOf: entry.kind };
  });
  const variableRows = (): Row[] => project.variables.map(variable => {
    const usages = usagesOf(refs, [variable.id, variable.name]);
    return { id: variable.id, name: variable.name, sub: variable.type, icon: <Hash size={14}/>, cells: [variable.name, `${variable.type}${variable.array ? `[${variable.size}]` : ''}`, variable.initial || '—', usages.length], tone: usages.length ? undefined : 'warn', usages, unused: !usages.length, open: { module: 'variables', target: variable.id } };
  });
  const regionRows = (): Row[] => project.world.regions.map(region => {
    const usages = usagesOf(refs, [region.id, region.name]);
    return { id: region.id, name: region.name, icon: <SquareDashed size={14}/>, cells: [region.name, `${region.x}, ${region.y}`, `${region.width} × ${region.height}`, usages.length], tone: usages.length ? undefined : 'warn', usages, unused: !usages.length, open: { module: 'world', target: region.id } };
  });
  const soundRows = (): Row[] => project.sounds.map(sound => {
    const usages = usagesOf(refs, [sound.id]);
    return { id: sound.id, name: sound.name, icon: <AudioLines size={14}/>, cells: [sound.name, sound.music ? '音乐' : sound.spatial ? '3D 音效' : '音效', project.assets.find(asset => asset.id === sound.assetId)?.path ?? '缺少文件', usages.length], tone: usages.length ? undefined : 'warn', usages, unused: !usages.length, open: { module: 'sounds', target: sound.id } };
  });

  const columns: Column[] = tab === 'trigger' ? [{ label: '名称', width: 'minmax(160px,2fr)' }, { label: '文件夹', width: 'minmax(90px,1fr)' }, { label: '状态', width: '76px' }, { label: '事件', width: '52px', numeric: true }, { label: '动作', width: '52px', numeric: true }, { label: '被引用', width: '64px', numeric: true }]
    : tab === 'function' ? [{ label: '函数', width: 'minmax(180px,2fr)' }, { label: '类别', width: 'minmax(80px,1fr)' }, { label: '种类', width: '56px' }, { label: '调用', width: '56px', numeric: true }, { label: '触发器', width: '64px', numeric: true }]
    : tab === 'variable' ? [{ label: '名称', width: 'minmax(140px,2fr)' }, { label: '类型', width: 'minmax(90px,1fr)' }, { label: '初始值', width: 'minmax(80px,1fr)' }, { label: '引用', width: '56px', numeric: true }]
    : tab === 'region' ? [{ label: '名称', width: 'minmax(140px,2fr)' }, { label: '位置', width: '90px' }, { label: '尺寸', width: '90px' }, { label: '引用', width: '56px', numeric: true }]
    : tab === 'sound' ? [{ label: '名称', width: 'minmax(140px,2fr)' }, { label: '类型', width: '80px' }, { label: '文件', width: 'minmax(120px,2fr)' }, { label: '引用', width: '56px', numeric: true }]
    : [{ label: '名称', width: 'minmax(180px,2fr)' }, { label: 'ID', width: '64px' }, { label: '状态', width: '64px' }, ...(placeable.has(tab) ? [{ label: '放置', width: '52px', numeric: true }] : []), { label: '引用', width: '56px', numeric: true }];

  const baseRows = tab === 'overview' ? [] : tab === 'trigger' ? triggerRows() : tab === 'function' ? functionRows() : tab === 'variable' ? variableRows() : tab === 'region' ? regionRows() : tab === 'sound' ? soundRows() : objectRows(tab);
  const q = query.trim().toLowerCase();
  let rows = baseRows.filter(row => {
    if (q && !`${row.name} ${row.sub ?? ''} ${row.id}`.toLowerCase().includes(q)) return false;
    if (tab === 'function') return (filter === 'all' || row.usages.length > 0) && (functionKind === '全部' || row.functionOf === functionKind);
    if (tab === 'trigger') return filter === 'all' || (filter === 'off' ? row.tone === 'off' : filter === 'closed' ? row.status === '初始关闭' : row.usages.length > 0);
    if (filter === 'unused') return !!row.unused;
    if (filter === 'custom') return row.status === '自定义';
    if (filter === 'modified') return row.status === '已修改';
    if (filter === 'standard') return row.status === '原版';
    return true;
  });
  if (sort) rows = [...rows].sort((a, b) => { const x = a.cells[sort.column], y = b.cells[sort.column]; const order = typeof x === 'number' && typeof y === 'number' ? x - y : String(x).localeCompare(String(y), 'zh-CN'); return sort.descending ? -order : order; });
  else if (tab === 'function') rows = [...rows].sort((a, b) => b.usages.length - a.usages.length);
  const current = rows.find(row => row.id === selection[tab]) ?? baseRows.find(row => row.id === selection[tab]);
  const select = (id: string) => setSelection(items => ({ ...items, [tab]: id }));
  const open = (row: Row) => { if (row.open) onNavigate(row.open.module, row.open.target); };

  function tableKey(event: ReactKeyboardEvent) {
    const at = rows.findIndex(row => row.id === current?.id);
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      const next = rows[Math.max(0, Math.min(rows.length - 1, at + (event.key === 'ArrowDown' ? 1 : -1)))];
      if (next) { select(next.id); requestAnimationFrame(() => table.current?.querySelector(`[data-row="${CSS.escape(next.id)}"]`)?.scrollIntoView({ block: 'nearest' })); }
    }
    if (event.key === 'Enter' && current) { event.preventDefault(); open(current); }
  }

  // ---- 地图概况 ----
  const overview = () => {
    const kindsSummary = objectKinds.map(kind => { const own = project.objects.filter(object => object.kind === kind.id); return { kind, custom: own.filter(o => o.custom).length, modified: own.filter(o => !o.custom).length }; });
    const unusedVariables = variableRows().filter(row => row.unused).length;
    const unusedObjects = objectKinds.flatMap(kind => objectRows(kind.id)).filter(row => row.unused);
    const disabled = project.graphs.filter(graph => graph.enabled === false).length, closed = project.graphs.filter(graph => graph.enabled !== false && graph.initiallyOn === false).length;
    const usedFunctions = functionRows().filter(row => row.usages.length).length;
    const line = (label: string, value: ReactNode, go?: () => void) => <button className="om-overview-row" disabled={!go} onClick={go} key={label}><span>{label}</span><b>{value}</b>{go && <ChevronRight size={12}/>}</button>;
    return <div className="om-overview">
      <section><h3>地图</h3>
        {line('地图名', project.settings.name || project.name)}
        {line('目标版本', project.settings.target)}
        {line('脚本后端', project.settings.language)}
        {line('玩家数量', project.settings.players)}
        {line('放置实例', project.world.instances.length, () => onNavigate('world'))}
        {line('区域', project.world.regions.length, () => setTab('region'))}
        {line('触发器', `${project.graphs.length}${disabled ? ` · ${disabled} 个已禁用` : ''}${closed ? ` · ${closed} 个初始关闭` : ''}`, () => setTab('trigger'))}
        {line('使用的函数', usedFunctions, () => setTab('function'))}
        {line('变量', project.variables.length, () => setTab('variable'))}
        {line('声音', project.sounds.length, () => setTab('sound'))}
      </section>
      <section><h3>物体数据</h3>
        {kindsSummary.map(item => line(`${item.kind.name}`, `${item.modified} 已修改 · ${item.custom} 自定义`, () => setTab(item.kind.id)))}
      </section>
      <section><h3>清理建议</h3>
        {line('未使用的变量', unusedVariables, unusedVariables ? () => { setFilters(items => ({ ...items, variable: 'unused' })); setTab('variable'); } : undefined)}
        {line('未使用的自定义物体', unusedObjects.length, unusedObjects.length ? () => { const kind = project.objects.find(object => object.id === unusedObjects[0].id)?.kind ?? 'unit'; setFilters(items => ({ ...items, [kind]: 'unused' })); setTab(kind); } : undefined)}
        {line('已禁用的触发器', disabled, disabled ? () => { setFilters(items => ({ ...items, trigger: 'off' })); setTab('trigger'); } : undefined)}
        <p className="om-empty-note">“未使用”只按工程数据中的直接引用判断；自定义脚本、字符串拼接等动态引用无法统计，删除前请确认。</p>
      </section>
    </div>;
  };

  // ---- 右侧详情 ----
  const detail = () => {
    if (!current) return <div className="om-detail-empty"><Search size={22}/><span>选择一个条目查看引用</span></div>;
    const outgoing = current.outgoing ? [...(refs.outgoing.get(current.outgoing) ?? [])].map(token => resolveToken(project, token)).filter((item): item is NonNullable<ReturnType<typeof resolveToken>> => !!item && item.target !== current.id && item.target !== current.open?.target) : [];
    const uniqueOut = [...new Map(outgoing.map(item => [`${item.module}:${item.target}`, item])).values()];
    const calledFunctions = tab === 'trigger' ? [...refs.functions.entries()].filter(([, list]) => list.some(usage => usage.ownerId === current.id)).map(([id, list]) => ({ id, count: list.filter(usage => usage.ownerId === current.id).length })) : [];
    const catalog = tab === 'trigger' ? functionCatalog(project) : [];
    return <div className="om-detail">
      <header><span className="om-detail-icon">{current.icon}</span><div><h3>{current.name}</h3><small>{[current.sub && current.sub !== current.name ? current.sub : '', current.status].filter(Boolean).join(' · ')}</small></div>{current.open && <button className="om-open" onClick={() => open(current)}>打开<ArrowUpRight size={13}/></button>}</header>
      {current.unused && <div className="om-warning"><AlertTriangle size={13}/>未发现静态引用{placeable.has(tab) ? '，也没有放置在地图上' : ''}</div>}
      <section><h4>{tab === 'function' ? '调用位置' : '被引用'}<small>{current.usages.length}</small></h4><UsageList usages={current.usages} onNavigate={onNavigate} empty={tab === 'function' ? '地图中没有使用这个函数' : '没有其他内容引用它'}/></section>
      {current.placed && placeable.has(tab) && <section><h4>放置在地图上<small>{current.placed.length}</small></h4>{current.placed.length ? <div className="om-usages">{current.placed.map(usage => <button key={usage.target} className="om-usage" onClick={() => onNavigate(usage.module, usage.target)}><em>{usage.place}</em><span>{usage.text}</span><ArrowUpRight size={12}/></button>)}</div> : <p className="om-empty-note">没有放置实例</p>}</section>}
      {!!uniqueOut.length && <section><h4>使用了<small>{uniqueOut.length}</small></h4><div className="om-usages">{uniqueOut.map(item => <button key={`${item.module}:${item.target}`} className="om-usage" onClick={() => onNavigate(item.module, item.target)}><em>{item.type}</em>{item.icon && <MiniIcon icon={item.icon} size={15}/>}<span>{item.label}</span><ArrowUpRight size={12}/></button>)}</div></section>}
      {!!calledFunctions.length && <section><h4>调用的函数<small>{calledFunctions.length}</small></h4><div className="om-usages">{calledFunctions.map(item => { const entry = catalog.find(fn => fn.id === item.id); return <button key={item.id} className="om-usage" title="在函数页查看全部调用位置" onClick={() => { setSelection(items => ({ ...items, function: item.id })); setFilters(items => ({ ...items, function: 'used' })); setFunctionKind('全部'); setTab('function'); }}><em>{entry?.kind ?? '函数'}</em><span>{entry?.title ?? item.id}</span><small>{item.count}</small></button>; })}</div></section>}
    </div>;
  };

  const counts = useMemo(() => {
    const result: Partial<Record<Tab, number>> = { trigger: project.graphs.length, variable: project.variables.length, region: project.world.regions.length, sound: project.sounds.length, function: refs.functions.size };
    for (const kind of objectKinds) result[kind.id] = project.objects.filter(object => object.kind === kind.id).length;
    return result;
  }, [project, refs]);
  const tabFilters = tab === 'overview' ? [] : filtersOf[tab] ?? objectFilters;
  return <div className="object-manager">
    <nav className="om-tabs" role="tablist" aria-label="物体管理器分类">{tabs.map(item => <button key={item.id} role="tab" aria-selected={tab === item.id} className={tab === item.id ? 'is-active' : ''} onClick={() => setTab(item.id)}>{item.id === 'overview' && <LayoutDashboard size={13}/>}{item.label}{!!counts[item.id] && <small>{counts[item.id]}</small>}</button>)}</nav>
    {tab === 'overview' ? overview() : <>
      <section className="om-list">
        <div className="om-toolbar">
          <label className="om-search"><Search size={13}/><input value={query} placeholder={`搜索${tabs.find(item => item.id === tab)?.label ?? ''}`} aria-label="搜索" onChange={event => setQuery(event.target.value)} onKeyDown={event => { if (event.key === 'ArrowDown') { event.preventDefault(); table.current?.focus(); if (!current && rows[0]) select(rows[0].id); } if (event.key === 'Escape') setQuery(''); }}/>{query && <button aria-label="清除搜索" onClick={() => setQuery('')}><X size={12}/></button>}</label>
          <div className="om-segment">{tabFilters.map(([id, label]) => <button key={id} className={filter === id ? 'is-active' : ''} onClick={() => setFilters(items => ({ ...items, [tab]: id }))}>{label}</button>)}</div>
          {tab === 'function' && <select aria-label="函数种类" value={functionKind} onChange={event => setFunctionKind(event.target.value as FunctionKind | '全部')}>{functionKinds.map(kind => <option key={kind}>{kind}</option>)}</select>}
          <span className="om-count">{rows.length} 项</span>
        </div>
        <div className="om-table" ref={table} tabIndex={0} role="grid" aria-label="条目" onKeyDown={tableKey} style={{ ['--om-columns' as string]: columns.map(column => column.width).join(' ') }}>
          <div className="om-row om-head" role="row">{columns.map((column, i) => <button key={column.label} role="columnheader" className={column.numeric ? 'is-numeric' : ''} onClick={() => setSort(value => value?.column === i ? (value.descending ? null : { column: i, descending: true }) : { column: i, descending: column.numeric ?? false })}>{column.label}{sort?.column === i && (sort.descending ? ' ↓' : ' ↑')}</button>)}</div>
          {rows.map(row => <div key={row.id} data-row={row.id} role="row" aria-selected={current?.id === row.id} className={`om-row ${current?.id === row.id ? 'is-selected' : ''} ${row.tone ? `tone-${row.tone}` : ''}`} onClick={() => select(row.id)} onDoubleClick={() => open(row)}>
            {row.cells.map((cell, i) => <span key={i} className={columns[i]?.numeric ? 'is-numeric' : ''}>{i === 0 && row.icon}{i === 0 ? <b>{cell}</b> : columns[i]?.label === 'ID' ? <code>{cell}</code> : columns[i]?.numeric && cell === 0 ? <i>0</i> : cell}</span>)}
          </div>)}
          {!rows.length && <div className="om-table-empty">{baseRows.length ? '没有符合条件的条目' : tab === 'function' ? '没有函数' : placeable.has(tab) || objectKinds.some(kind => kind.id === tab) ? `地图中没有使用${kindName(tab as ObjectKind)}` : '没有条目'}</div>}
        </div>
        <footer className="om-foot">静态引用：触发器语句、条件、物体字段、放置实例与 AI；自定义脚本与动态拼接不在统计内。双击或回车打开。</footer>
      </section>
      <aside className="om-side">{detail()}</aside>
    </>}
  </div>;
}
