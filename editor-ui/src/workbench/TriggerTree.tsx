import { useEffect, useRef, useState } from 'react';
import type { DragEvent as ReactDragEvent, KeyboardEvent as ReactKeyboardEvent, MouseEvent as ReactMouseEvent } from 'react';
import { Braces, Check, ChevronDown, ChevronRight, Copy, CopyPlus, FileCode2, Folder, FolderInput, FolderOpen, FolderPlus, GitBranch, Hash, Pencil, Plus, Power, Search, Trash2, X } from 'lucide-react';
import type { Graph } from '../studio/model';
import type { ModuleProps, WorkbenchProject } from './types';
import { cloneTrigger, createFolder, createTrigger, deleteFolder, deleteTrigger, folderList, moveFolder, moveTrigger, renameFolder, renameTrigger } from './triggerFolders';
import { buildReferenceIndex, usagesOf } from './referenceIndex';
import type { Usage } from './referenceIndex';
import { keepEditorSave } from './parameterFields';

// 触发器目录：文件夹（原版“类别”）与触发器的新建、重命名、删除、复制、拖动排序，以及允许 / 初始打开。
// 键盘：↑↓ 选择，←→ 折叠展开，F2 重命名，Delete 删除，Ctrl+C / Ctrl+V 复制粘贴触发器，Ctrl+D 创建副本，Insert 新建触发器，Ctrl+G 新建文件夹。

type RowRef = { type: 'folder'; folder: string } | { type: 'trigger'; id: string; folder: string };
type Menu = { x: number; y: number; row: RowRef } | null;
type Confirm = { title: string; body: string; usages?: Usage[]; usageTitle?: string; action: string; run: () => void } | null;
type Editing = { key: string; draft: string; error?: string } | null;
const keyOf = (row: RowRef) => row.type === 'folder' ? `folder:${row.folder}` : `trigger:${row.id}`;
const collapsedKey = 'forge-trigger-collapsed';
function readCollapsed(): Set<string> { try { return new Set(JSON.parse(localStorage.getItem(collapsedKey) ?? '[]') as string[]); } catch { return new Set(); } }

export function TriggerTree({ project, activeId, scriptId, onSelect, onSelectScript, onChange, notify, onNavigate, onDeclare }: {
  project: WorkbenchProject; activeId?: string; scriptId: string;
  onSelect: (id: string) => void; onSelectScript: (id: string) => void;
  onChange: (project: WorkbenchProject, label: string) => void; notify: ModuleProps['notify']; onNavigate: ModuleProps['onNavigate']; onDeclare: () => void;
}) {
  const [search, setSearch] = useState('');
  const [collapsed, setCollapsed] = useState<Set<string>>(readCollapsed);
  const [cursor, setCursor] = useState<string | undefined>(activeId ? `trigger:${activeId}` : undefined);
  const [editing, setEditing] = useState<Editing>(null);
  const [menu, setMenu] = useState<Menu>(null);
  const [confirm, setConfirm] = useState<Confirm>(null);
  const [drag, setDrag] = useState<{ source: RowRef; over?: string; into?: boolean } | null>(null);
  const [clipboard, setClipboard] = useState<Graph>();
  const tree = useRef<HTMLDivElement>(null);
  // 提交后立即把焦点移回目录会触发输入框 blur，这里防止同一次重命名提交两次。
  const settled = useRef(false);
  const startEditing = (key: string, draft: string) => { settled.current = false; setEditing({ key, draft }); };
  useEffect(() => { try { localStorage.setItem(collapsedKey, JSON.stringify([...collapsed])); } catch { /* 仅为界面便利 */ } }, [collapsed]);
  useEffect(() => { if (!menu) return; const close = () => setMenu(null); window.addEventListener('pointerdown', close); window.addEventListener('blur', close); return () => { window.removeEventListener('pointerdown', close); window.removeEventListener('blur', close); }; }, [menu]);

  const folders = folderList(project);
  const q = search.trim().toLowerCase();
  const rows = (() => {
    const list: RowRef[] = [];
    for (const folder of folders) {
      const triggers = project.graphs.filter(graph => graph.folder === folder && (!q || graph.name.toLowerCase().includes(q)));
      if (q && !triggers.length && !folder.toLowerCase().includes(q)) continue;
      list.push({ type: 'folder', folder });
      if (q || !collapsed.has(folder)) for (const graph of triggers) list.push({ type: 'trigger', id: graph.id, folder });
    }
    return list;
  })();
  const current = rows.find(row => keyOf(row) === cursor) ?? (activeId ? rows.find(row => row.type === 'trigger' && row.id === activeId) : undefined);
  const currentFolder = current?.folder ?? project.graphs.find(graph => graph.id === activeId)?.folder ?? folders[0] ?? '触发器';

  // 同步把焦点还给目录：后台标签页里 requestAnimationFrame 可能不执行，焦点会掉到 body，后续快捷键失效。
  const focusTree = () => tree.current?.focus();
  const reveal = (key: string) => setTimeout(() => tree.current?.querySelector(`[data-key="${CSS.escape(key)}"]`)?.scrollIntoView({ block: 'nearest' }), 0);
  const choose = (row: RowRef) => { setCursor(keyOf(row)); if (row.type === 'trigger') onSelect(row.id); reveal(keyOf(row)); };
  const toggle = (folder: string, open?: boolean) => setCollapsed(set => { const next = new Set(set); if (open ?? next.has(folder)) next.delete(folder); else next.add(folder); return next; });

  function newTrigger(folder = currentFolder, after = current?.type === 'trigger' ? current.id : undefined) {
    const result = createTrigger(project, folder, after);
    onChange(result.project, '新建触发器');
    toggle(folder, true);
    setCursor(`trigger:${result.id}`); onSelect(result.id);
    startEditing(`trigger:${result.id}`, result.project.graphs.find(graph => graph.id === result.id)!.name);
  }
  function newFolder(after = current?.folder) {
    const result = createFolder(project, after);
    onChange(result.project, '新建文件夹');
    setCursor(`folder:${result.name}`);
    startEditing(`folder:${result.name}`, result.name);
  }
  function commitRename() {
    if (!editing || settled.current) return;
    const row = rows.find(item => keyOf(item) === editing.key);
    const draft = editing.draft.trim();
    if (!row) { setEditing(null); return; }
    try {
      if (row.type === 'folder') { if (draft !== row.folder) { onChange(renameFolder(project, row.folder, draft), '重命名文件夹'); setCursor(`folder:${draft}`); setCollapsed(set => { if (!set.has(row.folder)) return set; const next = new Set(set); next.delete(row.folder); next.add(draft); return next; }); } }
      else if (draft !== project.graphs.find(graph => graph.id === row.id)?.name) onChange(renameTrigger(project, row.id, draft), '重命名触发器');
      settled.current = true; setEditing(null); focusTree();
    } catch (error) { setEditing({ ...editing, error: error instanceof Error ? error.message : '名称无效' }); }
  }
  function neighbour(id: string) {
    const list = project.graphs;
    const at = list.findIndex(graph => graph.id === id);
    return list.slice(at + 1).find(graph => graph.folder === list[at].folder) ?? list.slice(0, at).reverse().find(graph => graph.folder === list[at].folder) ?? list.find(graph => graph.id !== id);
  }
  function remove(row: RowRef) {
    if (row.type === 'folder') {
      const inside = project.graphs.filter(graph => graph.folder === row.folder);
      const run = () => {
        onChange(deleteFolder(project, row.folder), `删除文件夹 ${row.folder}`);
        const next = project.graphs.find(graph => graph.folder !== row.folder);
        if (inside.some(graph => graph.id === activeId) && next) onSelect(next.id);
        setCursor(next ? `trigger:${next.id}` : undefined); notify(`已删除文件夹 ${row.folder} · Ctrl+Z 撤销`); focusTree();
      };
      if (!inside.length) { run(); return; }
      const index = buildReferenceIndex(project);
      const outside = inside.flatMap(graph => usagesOf(index, [graph.id], graph.id)).filter(usage => !inside.some(graph => graph.id === usage.ownerId));
      setConfirm({ title: '删除文件夹', body: `删除文件夹“${row.folder}”及其中 ${inside.length} 个触发器？可以用 Ctrl+Z 撤销。`, usages: outside, usageTitle: '文件夹外的引用', action: `删除 ${inside.length + 1} 项`, run });
      return;
    }
    const graph = project.graphs.find(item => item.id === row.id);
    if (!graph) return;
    const run = () => {
      const next = neighbour(graph.id);
      onChange(deleteTrigger(project, graph.id), `删除触发器 ${graph.name}`);
      if (activeId === graph.id && next) onSelect(next.id);
      setCursor(next ? `trigger:${next.id}` : `folder:${graph.folder}`); notify(`已删除 ${graph.name} · Ctrl+Z 撤销`); focusTree();
    };
    const usages = usagesOf(buildReferenceIndex(project), [graph.id], graph.id);
    // 被其他触发器引用（运行触发器、开关触发器等）时先确认，否则直接删除并可撤销，不打断连续整理。
    if (usages.length) setConfirm({ title: '删除触发器', body: `“${graph.name}”被 ${usages.length} 处引用，删除后这些位置会变成无效引用。`, usages, usageTitle: '引用位置', action: '仍然删除', run });
    else run();
  }
  const setFlag = (id: string, patch: Partial<Graph>, label: string) => onChange({ ...project, graphs: project.graphs.map(graph => graph.id === id ? { ...graph, ...patch } : graph) }, label);
  function paste(source = clipboard) {
    if (!source) return;
    const result = cloneTrigger(project, source, currentFolder, current?.type === 'trigger' ? current.id : undefined);
    onChange(result.project, '粘贴触发器'); setCursor(`trigger:${result.id}`); onSelect(result.id); toggle(currentFolder, true);
  }

  function keyDown(event: ReactKeyboardEvent) {
    if (editing || (event.target as HTMLElement).closest('input,textarea,select')) return;
    const at = current ? rows.findIndex(row => keyOf(row) === keyOf(current)) : -1;
    const key = event.key, ctrl = event.ctrlKey || event.metaKey;
    const handled = () => event.preventDefault();
    if (key === 'ArrowDown' || key === 'ArrowUp') { handled(); const next = rows[Math.max(0, Math.min(rows.length - 1, at + (key === 'ArrowDown' ? 1 : -1)))]; if (next) choose(next); return; }
    if (key === 'Home' || key === 'End') { handled(); const next = key === 'Home' ? rows[0] : rows.at(-1); if (next) choose(next); return; }
    if (!current) { if (key === 'Insert') { handled(); newTrigger(); } return; }
    if (key === 'ArrowLeft') { handled(); if (current.type === 'trigger') choose({ type: 'folder', folder: current.folder }); else toggle(current.folder, false); return; }
    if (key === 'ArrowRight' && current.type === 'folder') { handled(); if (collapsed.has(current.folder)) toggle(current.folder, true); else { const child = rows[at + 1]; if (child?.type === 'trigger') choose(child); } return; }
    if (key === 'Enter' && current.type === 'folder') { handled(); toggle(current.folder); return; }
    if (key === 'F2') { handled(); startEditing(keyOf(current), current.type === 'folder' ? current.folder : project.graphs.find(graph => graph.id === current.id)?.name ?? ''); return; }
    if (key === 'Delete') { handled(); remove(current); return; }
    if (key === 'Insert' || (ctrl && key.toLowerCase() === 't')) { handled(); newTrigger(); return; }
    if (ctrl && key.toLowerCase() === 'g') { handled(); newFolder(); return; }
    if (ctrl && key.toLowerCase() === 'c' && current.type === 'trigger') { handled(); const graph = project.graphs.find(item => item.id === current.id); if (graph) { setClipboard(structuredClone(graph)); notify(`已复制触发器 ${graph.name}`); } return; }
    if (ctrl && key.toLowerCase() === 'v') { handled(); paste(); return; }
    if (ctrl && key.toLowerCase() === 'd' && current.type === 'trigger') { handled(); const graph = project.graphs.find(item => item.id === current.id); if (graph) paste(graph); }
  }

  // ---- 拖动：拖到触发器上插在它之前；拖到文件夹上放进该文件夹末尾（文件夹则排到它之前）。 ----
  const dragStart = (event: ReactDragEvent, row: RowRef) => { event.dataTransfer.effectAllowed = 'move'; event.dataTransfer.setData('text/plain', keyOf(row)); setDrag({ source: row }); };
  const dragOver = (event: ReactDragEvent, row: RowRef) => { if (!drag) return; if (drag.source.type === 'folder' && keyOf(drag.source) === keyOf(row)) return; event.preventDefault(); const over = keyOf(row); if (drag.over !== over) setDrag({ ...drag, over, into: row.type === 'folder' && drag.source.type === 'trigger' }); };
  const drop = (event: ReactDragEvent, row: RowRef) => {
    event.preventDefault();
    if (!drag) return;
    const source = drag.source;
    setDrag(null);
    if (source.type === 'trigger') { const next = row.type === 'folder' ? moveTrigger(project, source.id, row.folder) : moveTrigger(project, source.id, row.folder, row.id); if (next !== project) { onChange(next, '移动触发器'); if (row.type === 'folder') toggle(row.folder, true); } }
    else { const next = moveFolder(project, source.folder, row.folder); if (next !== project) onChange(next, '移动文件夹'); }
  };

  const openMenu = (event: ReactMouseEvent, row: RowRef) => { event.preventDefault(); event.stopPropagation(); setCursor(keyOf(row)); if (row.type === 'trigger') onSelect(row.id); setMenu({ x: event.clientX, y: event.clientY, row }); };
  const menuItems = (() => {
    if (!menu) return [];
    const row = menu.row;
    const rename = { label: '重命名', hint: 'F2', icon: Pencil, run: () => startEditing(keyOf(row), row.type === 'folder' ? row.folder : project.graphs.find(graph => graph.id === row.id)?.name ?? '') };
    if (row.type === 'folder') return [
      { label: '新建触发器', hint: 'Insert', icon: Plus, run: () => newTrigger(row.folder, undefined) },
      { label: '新建文件夹', hint: 'Ctrl+G', icon: FolderPlus, run: () => newFolder(row.folder) },
      ...(clipboard ? [{ label: `粘贴 ${clipboard.name}`, hint: 'Ctrl+V', icon: Copy, run: () => { const result = cloneTrigger(project, clipboard, row.folder); onChange(result.project, '粘贴触发器'); onSelect(result.id); setCursor(`trigger:${result.id}`); toggle(row.folder, true); } }] : []),
      rename,
      { label: '删除文件夹', hint: 'Delete', icon: Trash2, danger: true, run: () => remove(row) },
    ];
    const graph = project.graphs.find(item => item.id === row.id);
    if (!graph) return [];
    return [
      { label: '新建触发器', hint: 'Insert', icon: Plus, run: () => newTrigger(row.folder, row.id) },
      { label: '创建副本', hint: 'Ctrl+D', icon: CopyPlus, run: () => paste(graph) },
      { label: '复制', hint: 'Ctrl+C', icon: Copy, run: () => { setClipboard(structuredClone(graph)); notify(`已复制触发器 ${graph.name}`); } },
      rename,
      { label: '允许', icon: graph.enabled === false ? Power : Check, checked: graph.enabled !== false, run: () => setFlag(graph.id, { enabled: graph.enabled === false }, graph.enabled === false ? '允许触发器' : '禁用触发器') },
      { label: '初始打开', icon: graph.initiallyOn === false ? Power : Check, checked: graph.initiallyOn !== false, run: () => setFlag(graph.id, { initiallyOn: graph.initiallyOn === false }, '触发器初始状态') },
      ...folders.filter(folder => folder !== graph.folder).slice(0, 8).map(folder => ({ label: `移到 ${folder}`, icon: FolderInput, run: () => onChange(moveTrigger(project, graph.id, folder), '移动触发器') })),
      { label: '在物体管理器中查看引用', icon: Search, run: () => onNavigate('manager', graph.id) },
      { label: '删除触发器', hint: 'Delete', icon: Trash2, danger: true, run: () => remove(row) },
    ];
  })();

  const graphOf = (id: string) => project.graphs.find(graph => graph.id === id);
  return <aside className="tw-directory tt-directory">
    <header><span>触发器</span><div className="tt-actions"><button title="新建触发器 · Insert / Ctrl+T" aria-label="新建触发器" onClick={() => newTrigger()}><Plus size={15}/></button><button title="新建文件夹 · Ctrl+G" aria-label="新建文件夹" onClick={() => newFolder()}><FolderPlus size={15}/></button></div></header>
    <label className="tw-search"><Search size={13}/><input placeholder="搜索触发器" value={search} onChange={event => setSearch(event.target.value)} onKeyDown={event => { if (event.key === 'ArrowDown') { event.preventDefault(); tree.current?.focus(); if (rows[0] && !current) choose(rows[0]); } if (event.key === 'Escape') setSearch(''); }}/>{search && <button className="tt-clear" aria-label="清除搜索" onClick={() => setSearch('')}><X size={12}/></button>}</label>
    <div className="tw-directory-scroll tt-tree" ref={tree} tabIndex={0} role="tree" aria-label="触发器目录" onKeyDown={keyDown} onContextMenu={event => { if (event.target === event.currentTarget) { event.preventDefault(); newFolder(); } }}>
      {rows.map(row => {
        const key = keyOf(row), selected = current && keyOf(current) === key, isEditing = editing?.key === key;
        const dropClass = drag?.over === key ? drag.into ? 'drop-into' : 'drop-before' : '';
        const common = { 'data-key': key, draggable: !isEditing, onDragStart: (event: ReactDragEvent) => dragStart(event, row), onDragOver: (event: ReactDragEvent) => dragOver(event, row), onDrop: (event: ReactDragEvent) => drop(event, row), onDragEnd: () => setDrag(null), onContextMenu: (event: ReactMouseEvent) => openMenu(event, row) };
        const renameInput = isEditing && <span className="tt-rename"><input autoFocus aria-label="名称" value={editing.draft} aria-invalid={!!editing.error} onFocus={event => event.currentTarget.select()} onChange={event => setEditing({ key, draft: event.target.value })} onBlur={commitRename} onKeyDown={event => { keepEditorSave(event); if (event.key === 'Enter') { event.preventDefault(); commitRename(); } if (event.key === 'Escape') { event.preventDefault(); settled.current = true; setEditing(null); focusTree(); } }}/>{editing.error && <small role="alert">{editing.error}</small>}</span>;
        if (row.type === 'folder') {
          const open = !!q || !collapsed.has(row.folder), count = project.graphs.filter(graph => graph.folder === row.folder).length;
          return <div key={key} role="treeitem" aria-expanded={open} aria-selected={selected} className={`tt-row tt-folder ${selected ? 'is-selected' : ''} ${dropClass}`} {...common} onClick={() => { setCursor(key); tree.current?.focus(); }} onDoubleClick={() => toggle(row.folder)}>
            <button className="tt-twisty" tabIndex={-1} aria-label={open ? '折叠' : '展开'} onClick={event => { event.stopPropagation(); toggle(row.folder); }}>{open ? <ChevronDown size={13}/> : <ChevronRight size={13}/>}</button>
            {open ? <FolderOpen size={14}/> : <Folder size={14}/>}{renameInput || <span className="tt-name">{row.folder}</span>}<small>{count}</small>
          </div>;
        }
        const graph = graphOf(row.id)!;
        const off = graph.enabled === false, closed = graph.initiallyOn === false, text = !!graph.textDocument;
        return <div key={key} role="treeitem" aria-selected={selected} title={`${graph.name}${off ? ' · 已禁用（不生成代码）' : closed ? ' · 初始关闭' : ''}`} className={`tt-row tt-trigger ${selected ? 'is-selected' : ''} ${graph.id === activeId && !scriptId ? 'is-active' : ''} ${off ? 'is-off' : ''} ${dropClass}`} {...common} onClick={() => { choose(row); tree.current?.focus(); }} onDoubleClick={() => startEditing(key, graph.name)}>
          {text ? <FileCode2 size={14}/> : <GitBranch size={14}/>}{renameInput || <span className="tt-name">{graph.name}</span>}
          {off ? <em className="tt-flag is-off">禁用</em> : closed ? <em className="tt-flag">关</em> : null}
        </div>;
      })}
      {!rows.length && <div className="tt-empty">{q ? '没有匹配的触发器' : <><span>还没有触发器</span><button onClick={() => newTrigger(folders[0] ?? '触发器')}><Plus size={13}/>新建触发器</button></>}</div>}
      <div className="tt-section"><Braces size={13}/>脚本<small>{project.scripts.length}</small></div>
      {project.scripts.map(script => <button key={script.id} className={`tt-row tt-trigger tt-script ${scriptId === script.id ? 'is-active' : ''}`} onClick={() => onSelectScript(script.id)}><FileCode2 size={14}/><span className="tt-name">{script.name}</span></button>)}
    </div>
    <footer><button onClick={() => onNavigate('variables')}><Hash size={14}/>全局变量<span>{project.variables.length}</span></button><button onClick={onDeclare}><Plus size={14}/>函数声明</button></footer>
    {menu && <div className="et-menu tt-menu" role="menu" style={{ left: Math.min(menu.x, window.innerWidth - 240), top: Math.max(8, Math.min(menu.y, window.innerHeight - 16 - menuItems.length * 31)) }} onPointerDown={event => event.stopPropagation()} onContextMenu={event => event.preventDefault()}>
      {menuItems.map(item => <button key={item.label} role={'checked' in item ? 'menuitemcheckbox' : 'menuitem'} aria-checked={'checked' in item ? item.checked : undefined} className={'danger' in item && item.danger ? 'is-danger' : ''} onClick={() => { setMenu(null); item.run(); }}>{'checked' in item ? <span className="tt-check">{item.checked && <Check size={13}/>}</span> : <item.icon size={14}/>}<span>{item.label}</span>{'hint' in item && item.hint && <kbd>{item.hint}</kbd>}</button>)}
    </div>}
    {confirm && <div className="tw-backdrop" onPointerDown={event => { if (event.target === event.currentTarget) setConfirm(null); }}><section className="tw-dialog tt-confirm" role="alertdialog" aria-label={confirm.title} onKeyDown={event => { if (event.key === 'Escape') { setConfirm(null); focusTree(); } }}>
      <header><h3>{confirm.title}</h3><button aria-label="关闭" onClick={() => setConfirm(null)}><X size={17}/></button></header>
      <p>{confirm.body}</p>
      {!!confirm.usages?.length && <div className="tt-confirm-refs"><strong>{confirm.usageTitle}</strong>{confirm.usages.slice(0, 8).map((usage, i) => <span key={i}><em>{usage.owner}</em>{usage.text}</span>)}{confirm.usages.length > 8 && <small>还有 {confirm.usages.length - 8} 处</small>}</div>}
      <footer><button autoFocus onClick={() => { setConfirm(null); focusTree(); }}>取消</button><button className="tw-danger-button" onClick={() => { const run = confirm.run; setConfirm(null); run(); }}><Trash2 size={13}/>{confirm.action}</button></footer>
    </section></div>}
  </aside>;
}
