import { useEffect, useMemo, useRef, useState } from 'react';
import type { KeyboardEvent } from 'react';
import { ChevronDown, ChevronRight, CopyPlus, Eye, EyeOff, Hash, Link2, RotateCcw, Search, Trash2, X } from 'lucide-react';
import type { ModuleProps, ObjectKind } from './types';
import type { War3Field, War3Index, War3Value } from './war3Data';
import { iconUrl, loadKindValues, useWar3 } from './war3Data';
import { abilityColumnLookup, applicableFields, baseFieldType, createCustom, deleteCustom, displayValue, effectiveValues, levelCount, listObjects, resetFields, resolvePlaceholders, setBase, setFieldValue } from './objectModel';
import type { ObjectRow } from './objectModel';
import { ObjectFieldGrid } from './ObjectFieldGrid';
import { ObjectDrawer } from './ObjectDrawer';
import type { DrawerTarget } from './ObjectDrawer';
import { keepEditorSave } from './parameterFields';
import { buildReferenceIndex, usagesOf } from './referenceIndex';
import { withCatalog } from './triggerCatalog';
import { AnchoredPopover } from './AnchoredPopover';
import './objectEditor.css';

// 物体编辑器（原版数据）：七类标签 → 左侧按原版分组的目录 → 高密度字段表 → 按需右侧抽屉。
const kinds: { id: ObjectKind; name: string }[] = [
  { id: 'unit', name: '单位' }, { id: 'item', name: '物品' }, { id: 'destructable', name: '可破坏物' }, { id: 'doodad', name: '装饰物' },
  { id: 'ability', name: '技能' }, { id: 'buff', name: '魔法效果' }, { id: 'upgrade', name: '科技' },
];
type Filter = 'all' | 'modified' | 'custom';
type TreeNode = { key: string; label: string; depth: number; children: TreeNode[]; rows: ObjectRow[]; count: number };
// 与原版物编目录一致：两个根文件夹“标准X / 自定义X”（自定义即使为空也显示），其下按种族 → 对战/战役 → 单位/建筑/英雄/特殊。
// 每一层各有顺序（“特殊”在种族层排最后、在类型层也排最后），按同层标签最吻合的那组排序，未知标签保持原顺序排在后面。
const levelOrders = [
  ['人族', '兽族', '不死族', '暗夜精灵族', '中立 - 娜迦', '中立敌对', '中立被动', '特殊', '未关联原版'],
  ['对战', '战役'], ['单位', '建筑', '英雄', '特殊'], ['单位', '英雄', '物品'], ['魔法效果', '效果'],
];
function sortSiblings(list: TreeNode[]) {
  const order = levelOrders.reduce((best, item) => list.filter(node => item.includes(node.label)).length > list.filter(node => best.includes(node.label)).length ? item : best, [] as string[]);
  const rank = (label: string) => { const at = order.indexOf(label); return at < 0 ? order.length : at; };
  list.sort((a, b) => rank(a.label) - rank(b.label));
}
function buildTree(rows: ObjectRow[], kindName: string): TreeNode[] {
  const standard: TreeNode = { key: '/standard', label: `标准${kindName}`, depth: 0, children: [], rows: [], count: 0 };
  const custom: TreeNode = { key: '/custom', label: `自定义${kindName}`, depth: 0, children: [], rows: [], count: 0 };
  for (const row of rows) {
    let node = row.custom ? custom : standard;
    node.count++;
    (row.custom ? row.group.slice(1) : row.group).forEach(label => {
      const key = `${node.key}/${label}`;
      let child = node.children.find(item => item.key === key);
      if (!child) { child = { key, label, depth: node.depth + 1, children: [], rows: [], count: 0 }; node.children.push(child); }
      child.count++; node = child;
    });
    node.rows.push(row);
  }
  const sort = (node: TreeNode) => { sortSiblings(node.children); node.children.forEach(sort); };
  sort(standard); sort(custom);
  return [standard, custom];
}
/** 物体所在的全部文件夹键（从根到叶）。 */
function pathKeys(row: ObjectRow): string[] {
  const keys: string[] = [];
  let key = row.custom ? '/custom' : '/standard';
  keys.push(key);
  for (const label of row.custom ? row.group.slice(1) : row.group) { key = `${key}/${label}`; keys.push(key); }
  return keys;
}
function Icon({ row, size }: { row: Pick<ObjectRow, 'icon' | 'kind'>; size: number }) {
  return row.icon ? <img className="oe-icon" src={iconUrl(row.icon)} alt="" width={size} height={size} loading="lazy" draggable={false}/> : <span className={`oe-icon oe-icon-empty kind-${row.kind}`} style={{ width: size, height: size }}/>;
}

export function ObjectEditor({ project, onChange, notify, focusId, onNavigate }: ModuleProps) {
  const war3 = useWar3();
  const index = war3.index as War3Index;
  const focus = focusId?.match(/^(unit|item|destructable|doodad|ability|buff|upgrade):(.+)$/);
  const legacy = focusId ? project.objects.find(object => object.id === focusId) : undefined;
  const [kind, setKind] = useState<ObjectKind>((focus?.[1] as ObjectKind) ?? legacy?.kind ?? 'unit');
  // 选择历史：当前选中项消失（撤销新建、删除）时回到最近一个仍存在的物体，而不是跳到列表开头。
  const [trail, setTrail] = useState<Partial<Record<ObjectKind, string[]>>>(focus ? { [focus[1]]: [focus[2]] } : legacy ? { [legacy.kind]: [legacy.rawcode] } : {});
  const choose = (target: ObjectKind, rawcode: string) => setTrail(items => ({ ...items, [target]: [...(items[target] ?? []).filter(code => code !== rawcode), rawcode].slice(-30) }));
  const [query, setQuery] = useState('');
  const [filter, setFilter] = useState<Filter>('all');
  // 原版目录也列出编辑器中隐藏的物体（计数包含它们），默认显示，可切换隐藏。
  const [showHidden, setShowHidden] = useState(true);
  // 文件夹展开：用户点过的文件夹以用户为准；没点过的，根文件夹与当前物体所在路径默认展开。
  // （此前当前物体的路径每次渲染都强制展开，导致“人族”等文件夹无法折叠。）
  const [folderState, setFolderState] = useState<Partial<Record<ObjectKind, Record<string, boolean>>>>({});
  const [fieldQuery, setFieldQuery] = useState('');
  const [onlyModified, setOnlyModified] = useState(false);
  const [raw, setRaw] = useState(false);
  const [drawer, setDrawer] = useState<DrawerTarget | null>(null);
  const [refsOpen, setRefsOpen] = useState(false);
  const refsButton = useRef<HTMLButtonElement>(null);
  const tree = useRef<HTMLDivElement>(null);
  const values = war3.values[kind];
  useEffect(() => { loadKindValues(kind); }, [kind, war3.status]);
  // 提示文字的 <ID,数据> 占位符按技能数据解析，任何类别打开文本时都需要技能数据。
  useEffect(() => { if (drawer) loadKindValues('ability'); }, [drawer, war3.status]);

  const rows = useMemo(() => index ? listObjects(project, index, kind) : [], [project, index, kind]);
  // 与物体管理器共用同一份静态引用索引，两处结果一致。
  const referenceIndex = useMemo(() => { const labels = new Map<string, string>(); if (index) for (const [kind, data] of Object.entries(index.kinds)) for (const field of data.fields) labels.set(`${kind}:${field.id}`, field.name); return buildReferenceIndex(withCatalog(project), (kind, field) => labels.get(`${kind}:${field}`)); }, [project, index]);
  const filtered = useMemo(() => rows.filter(row => (showHidden || !row.hidden || row.custom || row.modified) && (filter === 'all' || (filter === 'custom' ? row.custom : row.custom || row.modified > 0))), [rows, showHidden, filter]);
  const words = query.trim().toLowerCase().split(/\s+/).filter(Boolean);
  const matches = words.length ? filtered.filter(row => words.every(word => `${row.name} ${row.suffix} ${row.rawcode} ${row.group.join(' ')}`.toLowerCase().includes(word))) : filtered;
  const kindName = kinds.find(item => item.id === kind)?.name ?? '';
  const nodes = useMemo(() => buildTree(filtered, kindName), [filtered, kindName]);
  const byCode = new Map(rows.map(row => [row.rawcode, row]));
  const current = [...(trail[kind] ?? [])].reverse().map(code => byCode.get(code)).find(Boolean) ?? filtered.find(row => !row.hidden) ?? rows[0];
  const folders = folderState[kind] ?? {};
  const currentPath = current ? pathKeys(current) : [];
  const isOpen = (key: string) => folders[key] ?? (key === '/standard' || key === '/custom' || currentPath.includes(key));
  const openKeys = { has: isOpen };
  const visibleRows: ObjectRow[] = [];
  const walk = (list: TreeNode[]) => list.forEach(node => { if (openKeys.has(node.key)) { walk(node.children); visibleRows.push(...node.rows); } });
  if (words.length) visibleRows.push(...matches.slice(0, 400)); else walk(nodes);

  // 选中物体时展开它所在的路径（例如从搜索结果或引用跳来），之后仍可自由折叠。
  const reveal = (row: ObjectRow) => setFolderState(state => { const next = { ...(state[kind] ?? {}) }; for (const key of pathKeys(row)) delete next[key]; return { ...state, [kind]: next }; });
  const select = (row: ObjectRow) => { choose(kind, row.rawcode); reveal(row); setDrawer(null); };
  useEffect(() => { tree.current?.querySelector('.oe-tree-row.is-selected')?.scrollIntoView({ block: 'nearest' }); }, [current?.key, kind]);
  const toggleNode = (key: string) => setFolderState(state => ({ ...state, [kind]: { ...(state[kind] ?? {}), [key]: !isOpen(key) } }));
  const treeKeys = (event: KeyboardEvent<HTMLDivElement>) => {
    if ((event.target as HTMLElement).closest('input')) return;
    const at = visibleRows.findIndex(row => row.key === current?.key);
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); const next = visibleRows[Math.max(0, Math.min(visibleRows.length - 1, at + (event.key === 'ArrowDown' ? 1 : -1)))]; if (next) select(next); }
    if (event.key === 'Enter' || event.key === 'ArrowRight') { event.preventDefault(); (document.querySelector('.oe-grid') as HTMLElement | null)?.focus(); }
  };

  if (war3.status !== 'ready' || !index) return <div className="oe-loading">正在读取原版数据…</div>;
  const ready = !!values && !!current;
  const effective = ready ? effectiveValues(index, kind, values, current) : {};
  const base = ready ? values[current.base] ?? {} : {};
  const overrides = current?.edit?.values ?? {};
  const levels = ready ? levelCount(kind, effective, current) : 1;
  const allFields = ready ? applicableFields(index, kind, values, current) : [];
  const fieldWords = fieldQuery.trim().toLowerCase().split(/\s+/).filter(Boolean);
  const fields = allFields.filter(field => (!onlyModified || field.id in overrides) && fieldWords.every(word => `${field.name} ${field.id} ${displayValue(project, index, field, effective[field.id])}`.toLowerCase().includes(word)));
  const commit = (field: War3Field, value: War3Value) => { if (!ready) return; onChange(setFieldValue(project, index, values, current, field.id, value), `修改 ${current.name} · ${field.name}`); };
  const reset = (field: War3Field) => { if (current) onChange(resetFields(project, current, [field.id]), `恢复 ${field.name}`); };
  const copy = () => { if (!current) return; try { const created = createCustom(project, index, kind, current); onChange(created.project, `复制为自定义 ${created.rawcode}`); choose(kind, created.rawcode); setFilter(value => value === 'modified' ? 'all' : value); setQuery(''); notify(`已创建 ${current.name}（${created.rawcode}）`); } catch (error) { notify(error instanceof Error ? error.message : '无法复制'); } };
  const remove = () => { if (!current?.custom) return; onChange(deleteCustom(project, current), `删除 ${current.name}`); if (current.base) choose(kind, current.base); notify(`已删除 ${current.name}（${current.rawcode}），可撤销`); };
  // 未关联原版的自定义物体：用同一个引用抽屉选择基础物体。
  const baseField: War3Field = { id: '__base', field: 'base', slk: '', index: -1, repeat: 0, data: 0, category: '', name: '基础物体', sort: '', type: baseFieldType[kind], canBeEmpty: false };
  const refs = current ? usagesOf(referenceIndex, [current.edit?.id, current.rawcode], current.edit?.id) : [];
  const drawerField = drawer && (drawer.field.id === '__base' ? baseField : allFields.find(field => field.id === drawer.field.id));

  const renderNode = (node: TreeNode): React.ReactNode => {
    const open = openKeys.has(node.key);
    return <div key={node.key} role="group">
      <button className="oe-tree-group" style={{ paddingLeft: 8 + node.depth * 14 }} aria-expanded={open} onClick={() => toggleNode(node.key)}>{open ? <ChevronDown size={12}/> : <ChevronRight size={12}/>}<span>{node.label}</span><small>{node.count}</small></button>
      {open && <>{node.children.map(renderNode)}{node.rows.map(row => renderRow(row, node.depth + 1))}</>}
    </div>;
  };
  const renderRow = (row: ObjectRow, depth: number, path?: string) => <button key={row.key} role="treeitem" aria-selected={row.key === current?.key} className={`oe-tree-row ${row.key === current?.key ? 'is-selected' : ''} ${row.hidden ? 'is-hidden' : ''}`} style={{ paddingLeft: 10 + depth * 14 }} onClick={() => select(row)} title={`${row.name}${row.suffix ? ` ${row.suffix}` : ''} · ${row.rawcode}${row.hidden ? ' · 编辑器中隐藏' : ''}`}>
    <Icon row={row} size={20}/><span className="oe-tree-name">{row.name}{row.suffix && <em>{row.suffix}</em>}{path && <small>{path}</small>}</span>
    {(row.modified > 0 || row.custom) && <i className={row.custom ? 'is-custom' : ''} aria-label={row.custom ? '自定义' : '已修改'}/>}<code>{row.rawcode}</code>
  </button>;

  return <div className={`object-editor ${drawerField ? 'has-drawer' : ''}`}>
    <nav className="oe-kinds" aria-label="物体类别">{kinds.map(item => { const edits = project.objects.filter(object => object.kind === item.id).length; return <button key={item.id} className={kind === item.id ? 'is-active' : ''} aria-current={kind === item.id ? 'page' : undefined} title={`原版 ${index.kinds[item.id].objects.length} 个${edits ? `，地图中 ${edits} 个修改或自定义` : ''}`} onClick={() => { setKind(item.id); setDrawer(null); setQuery(''); }}>{item.name}{edits > 0 && <i className="oe-kind-dot" aria-label={`${edits} 个地图修改`}/>}</button>; })}</nav>
    <aside className="oe-tree-pane">
      <label className="oe-search"><Search size={13}/><input aria-label="搜索物体" placeholder="名称、rawcode 或分组" value={query} onChange={event => setQuery(event.target.value)} onKeyDown={event => { keepEditorSave(event); if (event.key === 'ArrowDown') { event.preventDefault(); if (visibleRows[0]) select(visibleRows[0]); tree.current?.focus(); } if (event.key === 'Escape') setQuery(''); }}/>{query && <button aria-label="清除搜索" onClick={() => setQuery('')}><X size={12}/></button>}</label>
      <div className="oe-tree-filters"><div className="oe-segment">{([['all', '全部'], ['modified', '已修改'], ['custom', '自定义']] as const).map(([id, label]) => <button key={id} className={filter === id ? 'is-active' : ''} onClick={() => setFilter(id)}>{label}</button>)}</div><button className="oe-icon-button" aria-pressed={showHidden} title={showHidden ? '当前显示编辑器中隐藏的物体（同原版计数）· 点击隐藏它们' : '已隐藏编辑器中隐藏的物体 · 点击显示'} onClick={() => setShowHidden(value => !value)}>{showHidden ? <Eye size={14}/> : <EyeOff size={14}/>}</button></div>
      <div ref={tree} className="oe-tree" role="tree" aria-label={`${kinds.find(item => item.id === kind)?.name}目录`} tabIndex={0} onKeyDown={treeKeys}>
        {words.length ? matches.slice(0, 400).map(row => renderRow(row, 0, row.group.join(' / '))) : nodes.map(renderNode)}
        {words.length > 0 && matches.length > 400 && <div className="oe-empty">还有 {matches.length - 400} 项，请输入更多关键字</div>}
        {!matches.length && <div className="oe-empty">{filter === 'all' ? '没有匹配的物体' : filter === 'custom' ? '没有自定义物体' : '没有修改过的物体'}</div>}
      </div>
      <footer><span title={`${index.source} · 导出于 ${new Date(index.generatedAt).toLocaleString()}`}>{matches.length} 项 · 本机原版</span><button onClick={copy} disabled={!current?.base} title="以当前物体为基础新建自定义物体"><CopyPlus size={13}/>复制为自定义</button></footer>
    </aside>
    <main className="oe-main">
      {current ? <>
        <header className="oe-head">
          <Icon row={current} size={44}/>
          <div className="oe-title"><h2>{current.name}{current.suffix && <em>{current.suffix}</em>}</h2>
            <div className="oe-meta"><code>{current.rawcode}</code>{current.custom ? <span className="oe-badge is-custom">自定义</span> : <span className="oe-badge">原版</span>}{current.modified > 0 && <span className="oe-badge is-modified">{current.modified} 项已修改</span>}{current.custom && current.base && <button className="oe-link" onClick={() => choose(kind, current.base)}><Link2 size={11}/>基于 {index.kinds[kind].objects.find(object => object.id === current.base)?.name}（{current.base}）</button>}{levels > 1 && <span className="oe-badge">{levels} 级</span>}<span className="oe-path">{current.group.join(' / ')}</span></div>
          </div>
          <div className="oe-head-actions">
            <button ref={refsButton} className="oe-refs" aria-expanded={refsOpen} title="只统计地图数据中的直接引用；脚本文本和动态拼接不在此列" onClick={() => setRefsOpen(open => !open)}><Link2 size={13}/>静态引用 {refs.length}</button>
            <button onClick={copy} disabled={!current.base}><CopyPlus size={14}/>复制为自定义</button>
            {current.modified > 0 && <button onClick={() => onChange(resetFields(project, current), `恢复 ${current.name} 全部字段`)}><RotateCcw size={14}/>{current.custom ? '清除修改' : '恢复原版'}</button>}
            {current.custom && <button className="oe-danger" onClick={remove}><Trash2 size={14}/>删除</button>}
          </div>
        </header>
        <div className="oe-fieldbar">
          <label className="oe-search"><Search size={13}/><input aria-label="搜索字段" placeholder="字段名、ID 或值" value={fieldQuery} onChange={event => setFieldQuery(event.target.value)} onKeyDown={event => { keepEditorSave(event); if (event.key === 'Escape') setFieldQuery(''); if (event.key === 'ArrowDown') { event.preventDefault(); (document.querySelector('.oe-grid') as HTMLElement | null)?.focus(); } }}/>{fieldQuery && <button aria-label="清除字段搜索" onClick={() => setFieldQuery('')}><X size={12}/></button>}</label>
          <button className={`oe-toggle ${onlyModified ? 'is-on' : ''}`} aria-pressed={onlyModified} onClick={() => setOnlyModified(value => !value)}>只看已修改<small>{Object.keys(overrides).length}</small></button>
          <button className={`oe-toggle ${raw ? 'is-on' : ''}`} aria-pressed={raw} title="显示字段 ID 与原始值" onClick={() => setRaw(value => !value)}><Hash size={12}/>原始值</button>
          <span className="oe-count">{fields.length} / {allFields.length} 个字段</span>
        </div>
        {ready && (current.base || !current.custom) && <ObjectFieldGrid key={current.key} project={project} index={index} fields={fields} effective={effective} overrides={overrides} base={base} levels={levels} raw={raw} drawerField={drawerField?.id} onSet={commit} onReset={reset} onDrawer={setDrawer}
          emptyText={!allFields.length ? current.base ? '该物体没有可编辑字段' : '该物体未关联原版基础物体，无法确定字段' : onlyModified ? '没有修改过的字段' : '没有匹配的字段'}/>}
        {ready && current.custom && !current.base && <div className="oe-unlinked"><span>旧工程中的自定义物体没有记录基础物体。指定后按基础物体显示字段，已有修改保留。</span><button className="oe-primary" onClick={() => setDrawer({ field: baseField })}>选择基础物体</button></div>}
        {!ready && <div className="oe-loading">正在读取字段…</div>}
      </> : <div className="oe-loading">没有物体</div>}
    </main>
    {refsOpen && current && <AnchoredPopover anchor={() => refsButton.current} placement="below" width={300} label="静态引用" title={`${current.name} 的静态引用`} onClose={() => setRefsOpen(false)}><div className="sv-result-list oe-refs-list">{refs.map((ref, i) => <button key={`${ref.module}-${ref.target}-${i}`} title={ref.text} onClick={() => { setRefsOpen(false); onNavigate(ref.module, ref.target); }}><span>{ref.owner}<small>{ref.place} · {ref.text}</small></span></button>)}{!refs.length && <div className="sv-empty">地图数据中没有直接引用。脚本和动态引用未统计，不代表可以安全删除。</div>}<button className="oe-refs-more" onClick={() => { setRefsOpen(false); onNavigate('manager', current.edit?.id ?? `${current.kind}:${current.rawcode}`); }}>在物体管理器中查看<small>分组、放置实例与“使用了”</small></button></div></AnchoredPopover>}
    {drawerField && current && ready && <ObjectDrawer key={`${current.key}:${drawerField.id}`} project={project} index={index} target={{ field: drawerField, level: drawer?.level }} value={effective[drawerField.id]} baseValue={base[drawerField.id]} levels={levels} modified={drawerField.id in overrides} onCommit={value => { if (drawerField.id === '__base') { if (value) { onChange(setBase(project, current, String(value)), `${current.name} 基于 ${value}`); setDrawer(null); } return; } commit(drawerField, value); }} onReset={() => reset(drawerField)} onClose={() => setDrawer(null)} resolveText={war3.values.ability ? text => resolvePlaceholders(text, abilityColumnLookup(project, index, war3.values.ability!)) : undefined}/>}
  </div>;
}
