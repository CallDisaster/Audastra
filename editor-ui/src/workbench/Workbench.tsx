import { Fragment, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { ArrowLeft, AudioLines, Box, Camera, Check, ChevronDown, Crosshair, Download, FilePlus2, Flag, FolderOpen, GitBranch, GitCompareArrows, Grid3x3, Hexagon, Layers, ListTree, Magnet, Map, MousePointer2, Play, Plus, Redo2, Save, Search, Settings2, Shield, Sparkles, SquareDashed, Undo2, Upload, Variable, Puzzle, AlertTriangle } from 'lucide-react';
import { TriggerWorkspace } from './TriggerWorkspace';
import { ObjectWorkspace } from './ObjectWorkspace';
import { ObjectEditor } from './ObjectEditor';
import { useWar3 } from './war3Data';
import { VariablesWorkspace } from './VariablesWorkspace';
import { ResourcesWorkspace } from './ResourcesWorkspace';
import { SoundWorkspace } from './SoundWorkspace';
import { AiWorkspace, CampaignWorkspace } from './ProjectWorkspaces';
import { WorldWorkspace } from './WorldWorkspace';
import { CommandPalette } from './CommandPalette';
import type { PaletteCommand, PaletteEntry } from './CommandPalette';
import { ToolPanel } from './ToolPanel';
import type { ToolId } from './ToolPanel';
import { ChangesPanel } from './ChangesPanel';
import { TestRunPanel } from './TestRunPanel';
import { AssistantPanel } from './AssistantPanel';
import { changeCount, diffProjects } from './projectDiff';
import type { ChangeGroup } from './projectDiff';
import { entityKindLabels, entityName, entityPosition, instanceObject, playerOf, worldEntities } from './worldModel';
import { frameSelection, select, setWorld, worldState } from './worldStore';
import { ObjectManager } from './ObjectManager';
import { DockShell } from './DockShell';
import { createModuleHost } from './moduleHost';
import { activate, defaultLayout, mainShown, readLayout, visibleModules } from './dockLayout';
import type { DockLayout } from './dockLayout';
import { Modal, CommitInput } from './ui';
import { createEmpty, createSample, readWorkbench } from './sample';
import { withCatalog } from './triggerCatalog';
import { downloadJSON, parseDefinitions, validateProject } from '../studio/model';
import type { ModuleId, WorkbenchProject } from './types';
import './toolPanels.css';
import '../studio/studio.css';
import './workbench.css';

const storageKey = 'forge-workbench-r02';
const kindLabels = { unit: '单位', item: '物品', destructable: '可破坏物', doodad: '装饰物', ability: '技能', buff: '魔法效果', upgrade: '科技' };
const modules = [
  { id: 'triggers', label: '触发器', icon: GitBranch, shortcut: '1' },
  { id: 'objects', label: '物体', icon: Box, shortcut: '2' },
  { id: 'variables', label: '变量', icon: Variable, shortcut: '3' },
  { id: 'sounds', label: '声音', icon: AudioLines, shortcut: '4' },
  { id: 'assets', label: '资源', icon: Layers, shortcut: '5' },
  { id: 'manager', label: '物体管理', icon: ListTree, shortcut: '6' },
  { id: 'campaign', label: '战役', icon: Flag, shortcut: '7' },
  { id: 'ai', label: 'AI', icon: Shield, shortcut: '8' },
  { id: 'world', label: '世界', icon: Map, shortcut: '9' },
] as const;
const layoutKey = 'forge-layout-r07';
const moduleIds = modules.map(m => m.id);
// 布局只是本机界面偏好：读不到或损坏时回到默认布局，不影响工程数据。
function initialLayout(): DockLayout { try { return readLayout(JSON.parse(localStorage.getItem(layoutKey) ?? 'null'), modules.map(m => m.id)); } catch { return defaultLayout(); } }
function initial() {
  const saved = localStorage.getItem(storageKey);
  // 以读取后的规范化结果作为已保存基线：校验会重排字段顺序，直接比较原始字符串会让刚打开的工程显示“未保存”。
  try { const project = saved ? readWorkbench(JSON.parse(saved)) : createSample(); return { project, error: '', saved: saved ? JSON.stringify(project) : '' }; }
  catch { return { project: createSample(), error: '本地存档无法读取，已打开示例；原存档未覆盖', saved: '' }; }
}
export default function Workbench() {
  const [loaded] = useState(initial);
  const war3 = useWar3();
  const [project, setProject] = useState(loaded.project);
  const [past, setPast] = useState<WorkbenchProject[]>([]), [future, setFuture] = useState<WorkbenchProject[]>([]);
  const [saved, setSaved] = useState(loaded.saved), [message, setMessage] = useState(loaded.error);
  // 停靠布局与模块保活：打开过的编辑器一直挂载在各自的宿主节点里，切换、停靠、浮动都只搬动节点，切回来时保持离开时的状态。
  const [layout, setLayout] = useState(initialLayout);
  const [mode, setMode] = useState<ModuleId>(() => layout.floats.at(-1)?.id ?? mainShown(layout, moduleIds) ?? layout.sideActive ?? 'triggers');
  const [mounted, setMounted] = useState<Set<ModuleId>>(() => new Set(visibleModules(layout, moduleIds)));
  const [focus, setFocus] = useState<Partial<Record<ModuleId, string>>>({}), [generation, setGeneration] = useState<Partial<Record<ModuleId, number>>>({});
  const [hosts] = useState(() => Object.fromEntries(modules.map(m => [m.id, createModuleHost()])) as unknown as Record<ModuleId, HTMLElement>);
  const park = useRef<HTMLDivElement>(null);
  useEffect(() => { try { localStorage.setItem(layoutKey, JSON.stringify(layout)); } catch { /* 仅为界面偏好 */ } }, [layout]);
  const [modal, setModal] = useState<'project' | 'settings' | 'definitions' | 'problems' | null>(null);
  const [palette, setPalette] = useState<string | null>(null);
  const [theme, setTheme] = useState('graphite'), [density, setDensity] = useState('standard');
  const [error, setError] = useState(''), [definitionDraft, setDefinitionDraft] = useState('[]');
  const importRef = useRef<HTMLInputElement>(null);
  const projectRef = useRef(project);
  useEffect(() => { projectRef.current = project; }, [project]);
  const serialized = JSON.stringify(project), dirty = serialized !== saved;
  const issues = validateProject(withCatalog(project));
  const notify = (text: string) => setMessage(text);
  // 修订号：每次工程变化递增。助手提议绑定生成时的修订，修订变化后提议过期（原生端对应 EditRevision）。
  const [revision, setRevision] = useState(1);
  function update(next: WorkbenchProject, label: string) {
    if (JSON.stringify(next) === JSON.stringify(project)) return;
    setPast(history => [...history.slice(-39), project]); setFuture([]); setProject(next); setRevision(r => r + 1); notify(label);
  }
  function undo() { if (!past.length) return; setFuture(list => [project, ...list]); setProject(past.at(-1)!); setPast(past.slice(0, -1)); setRevision(r => r + 1); notify('已撤销'); }
  function redo() { if (!future.length) return; setPast(list => [...list, project]); setProject(future[0]); setFuture(future.slice(1)); setRevision(r => r + 1); notify('已重做'); }
  function save() { try { const data = JSON.stringify(projectRef.current); localStorage.setItem(storageKey, data); setSaved(data); setAttribution({}); notify('已保存到本机'); } catch { notify('本地空间不足，请导出工程 JSON'); } }
  // ---- 本次修改（与已保存基线比较）与右侧工具面板 ----
  const [tool, setTool] = useState<ToolId | null>(null);
  const [attribution, setAttribution] = useState<Record<string, string>>({});
  // 从未保存过的工程（示例、导入）以打开时的内容为基线，列出“自打开以来的修改”。
  const [openedAs] = useState(() => JSON.stringify(loaded.project));
  const baseline = useMemo(() => { try { return readWorkbench(JSON.parse(saved || openedAs)); } catch { return undefined; } }, [saved, openedAs]);
  const changes = useMemo<ChangeGroup[]>(() => baseline ? diffProjects(baseline, project) : [], [baseline, project]);
  const pendingCount = changeCount(changes);
  const toggleTool = (id: ToolId) => setTool(current => current === id ? null : id);
  // 跳转历史：从引用列表跳到触发器/物体后，Alt+← 或工具条返回按钮回到原处（物体管理器会记住页签与选中项）。
  const [history, setHistory] = useState<ModuleId[]>([]);
  function applyLayout(next: DockLayout, focused?: ModuleId) {
    setLayout(next);
    const visible = visibleModules(next, moduleIds);
    setMounted(set => visible.every(id => set.has(id)) ? set : new Set([...set, ...visible]));
    if (focused) setMode(focused);
    else if (!visible.includes(mode)) setMode(mainShown(next, moduleIds) ?? next.floats.at(-1)?.id ?? next.sideActive ?? mode);
  }
  // 带目标的跳转（引用、搜索结果）重建该模块以定位目标；只切换模块时保留原状态。
  function navigate(next: ModuleId, targetId?: string) {
    if (next !== mode || targetId) setHistory(list => [...list.slice(-29), mode]);
    if (targetId !== undefined) { setFocus(map => ({ ...map, [next]: targetId })); setGeneration(map => ({ ...map, [next]: (map[next] ?? 0) + 1 })); }
    applyLayout(activate(layout, next, true), next); setModal(null);
  }
  function back() { const last = history.at(-1); if (!last) return; setHistory(history.slice(0, -1)); applyLayout(activate(layout, last, true), last); setModal(null); }
  const commandRef = useRef({ save, undo, redo, navigate, back, toggleTool });
  useEffect(() => { commandRef.current = { save, undo, redo, navigate, back, toggleTool }; });
  useEffect(() => {
    function key(event: KeyboardEvent) {
      const input = event.target instanceof HTMLElement && !!event.target.closest('input,textarea,select,[contenteditable=true]');
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') { event.preventDefault(); if (event.target instanceof HTMLElement) event.target.blur(); setTimeout(() => commandRef.current.save(), 0); }
      if ((event.ctrlKey || event.metaKey) && (event.key.toLowerCase() === 'k' || event.key.toLowerCase() === 'p')) { event.preventDefault(); setPalette(event.shiftKey ? '>' : ''); }
      if (event.key === 'F5') { event.preventDefault(); commandRef.current.toggleTool('test'); }
      if (!input && (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'z') { event.preventDefault(); if (event.shiftKey) commandRef.current.redo(); else commandRef.current.undo(); }
      if (!input && (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'y') { event.preventDefault(); commandRef.current.redo(); }
      if (!input && event.altKey && event.key === 'ArrowLeft') { event.preventDefault(); commandRef.current.back(); }
      if (!input && event.altKey && /^[1-9]$/.test(event.key)) { event.preventDefault(); commandRef.current.navigate(modules[Number(event.key) - 1].id); }
    }
    window.addEventListener('keydown', key); return () => window.removeEventListener('keydown', key);
  }, []);
  useEffect(() => { if (!message) return; const timer = window.setTimeout(() => setMessage(''), 3500); return () => clearTimeout(timer); }, [message]);
  useEffect(() => { const handler = (e: BeforeUnloadEvent) => { if (dirty) e.preventDefault(); }; window.addEventListener('beforeunload', handler); return () => window.removeEventListener('beforeunload', handler); }, [dirty]);
  const moduleView = (id: ModuleId) => {
    const props = { project, onChange: update, notify, onNavigate: navigate, focusId: focus[id] };
    switch (id) {
      case 'triggers': return <TriggerWorkspace {...props} />;
      case 'objects': return war3.status === 'ready' ? <ObjectEditor {...props} /> : war3.status === 'missing' ? <div className="wb-objects-fallback"><div className="wb-data-banner">未找到本机原版数据，当前为示例物编。运行 <code>node tools/import-war3-data.mjs --game &lt;Warcraft III 目录&gt;</code> 后刷新。</div><ObjectWorkspace {...props} /></div> : <div className="oe-loading">正在读取原版数据…</div>;
      case 'variables': return <VariablesWorkspace {...props} />;
      case 'sounds': return <SoundWorkspace {...props} />;
      case 'assets': return <ResourcesWorkspace {...props} />;
      case 'manager': return <ObjectManager {...props} />;
      case 'campaign': return <CampaignWorkspace {...props} />;
      case 'ai': return <AiWorkspace {...props} />;
      case 'world': return <WorldWorkspace {...props} />;
    }
  };
  // ---- 命令表：命令面板、快捷键与（原生端）Agent 共用 ----
  const worldCommand = (patch: Parameters<typeof setWorld>[0], label: string) => () => { setWorld(patch); navigate('world'); notify(label); };
  const worldNow = worldState();
  const commands: PaletteCommand[] = [
    { id: 'file.save', title: '保存', group: '工程', shortcut: 'Ctrl S', icon: Save, run: save },
    { id: 'edit.undo', title: '撤销', group: '编辑', shortcut: 'Ctrl Z', icon: Undo2, disabled: !past.length, run: undo },
    { id: 'edit.redo', title: '重做', group: '编辑', shortcut: 'Ctrl Shift Z', icon: Redo2, disabled: !future.length, run: redo },
    { id: 'nav.back', title: '返回上一位置', group: '导航', shortcut: 'Alt ←', icon: ArrowLeft, disabled: !history.length, run: back },
    ...modules.map(m => ({ id: `module.${m.id}`, title: `打开${m.label}`, group: '模块', shortcut: `Alt ${m.shortcut}`, icon: m.icon, keywords: m.id, run: () => navigate(m.id) })),
    { id: 'tool.changes', title: '审阅本次修改', group: '工具', icon: GitCompareArrows, keywords: 'diff 差异 保存预览', run: () => setTool('changes') },
    { id: 'tool.test', title: '测试地图', group: '工具', shortcut: 'F5', icon: Play, keywords: 'run play 运行 进游戏', run: () => setTool('test') },
    { id: 'tool.assistant', title: '打开助手', group: '工具', icon: Sparkles, keywords: 'agent ai', run: () => setTool('assistant') },
    { id: 'world.select', title: '选择工具', group: '世界', shortcut: 'V', icon: MousePointer2, run: worldCommand({ tool: 'select' }, '选择工具') },
    { id: 'world.region', title: '区域工具', group: '世界', shortcut: 'R', icon: SquareDashed, run: worldCommand({ tool: 'region' }, '区域工具') },
    { id: 'world.camera', title: '镜头工具', group: '世界', shortcut: 'C', icon: Camera, run: worldCommand({ tool: 'camera' }, '镜头工具') },
    { id: 'world.snap', title: worldNow.snap ? '关闭网格吸附' : '开启网格吸附', group: '世界', icon: Magnet, run: worldCommand(s => ({ snap: !s.snap }), '已切换吸附') },
    { id: 'world.grid', title: worldNow.layers.grid ? '隐藏网格' : '显示网格', group: '世界', icon: Grid3x3, run: worldCommand(s => ({ layers: { ...s.layers, grid: !s.layers.grid } }), '已切换网格') },
    { id: 'world.names', title: worldNow.layers.names ? '隐藏单位名称' : '显示单位名称', group: '世界', icon: Grid3x3, run: worldCommand(s => ({ layers: { ...s.layers, names: !s.layers.names } }), '已切换单位名称') },
    { id: 'world.frame', title: '对准选择', group: '世界', shortcut: 'F', icon: Crosshair, disabled: !worldNow.selection.length, run: () => { navigate('world'); frameSelection(); } },
    { id: 'world.same', title: '选择同类对象', group: '世界', icon: MousePointer2, keywords: '相同 物体 全选', disabled: !worldNow.selection.length, run: () => { const ids = new Set(worldState().selection); const codes = new Set(project.world.instances.filter(i => ids.has(i.id)).map(i => instanceObject(project, i)?.rawcode ?? i.objectId)); select(project.world.instances.filter(i => codes.has(instanceObject(project, i)?.rawcode ?? i.objectId)).map(i => i.id)); navigate('world'); } },
    { id: 'world.clear', title: '取消选择', group: '世界', icon: MousePointer2, disabled: !worldNow.selection.length, run: () => select([]) },
    { id: 'project.open', title: '新建 / 打开 / 导出工程', group: '工程', icon: FolderOpen, run: () => { setError(''); setModal('project'); } },
    { id: 'project.check', title: '工程检查', group: '工程', icon: Check, run: () => setModal('problems') },
    { id: 'project.definitions', title: '函数定义', group: '工程', icon: Puzzle, run: () => { setError(''); setDefinitionDraft('[]'); setModal('definitions'); } },
    { id: 'app.settings', title: '设置', group: '界面', icon: Settings2, keywords: '主题 密度', run: () => setModal('settings') },
  ];
  const navigateRef = useRef(navigate);
  useEffect(() => { navigateRef.current = navigate; });
  const entries = useCallback((query: string): PaletteEntry[] => {
    const go = (module: ModuleId, id?: string) => () => navigateRef.current(module, id);
    return [
      ...project.graphs.map(g => ({ id: `g:${g.id}`, title: g.name, detail: `触发器 · ${g.folder}`, icon: GitBranch, run: go('triggers', g.id) })),
      ...project.objects.map(o => ({ id: `o:${o.id}`, title: o.name, detail: `${o.category} · ${o.rawcode}`, icon: Box, keywords: o.rawcode, run: go('objects', o.id) })),
      ...project.variables.map(v => ({ id: `v:${v.id}`, title: `udg_${v.name}`, detail: `变量 · ${v.type}`, icon: Variable, run: go('variables', v.id) })),
      ...project.sounds.map(sd => ({ id: `s:${sd.id}`, title: sd.name, detail: '声音定义', icon: AudioLines, run: go('sounds', sd.id) })),
      ...project.assets.map(a => ({ id: `a:${a.id}`, title: a.name, detail: `导入文件 · ${a.path}`, icon: Layers, run: go('assets', a.id) })),
      ...worldEntities(project).map(e => { const p = entityPosition(e); return { id: `w:${e.id}`, title: entityName(e), detail: `世界 · ${entityKindLabels[e.kind]}${e.kind !== 'region' && e.kind !== 'camera' ? ` · ${playerOf(e.instance.owner).name}` : ''} · ${Math.round(p.x)}, ${Math.round(p.y)}`, icon: Map, run: go('world', e.id) }; }),
      // 原版物体：输入两个字符以上才参与，避免数千项淹没工程内容；地图中已有的同 rawcode 物体以地图为准。
      ...(war3.status === 'ready' && war3.index && query.trim().length >= 2 ? Object.entries(war3.index.kinds).flatMap(([kind, data]) => data.objects.filter(o => !o.hidden && !project.objects.some(own => own.kind === kind && own.rawcode === o.id)).map(o => ({ id: `std:${kind}:${o.id}`, title: [o.name.trim(), o.suffix.trim()].filter(Boolean).join(' '), detail: `原版${kindLabels[kind as keyof typeof kindLabels]} · ${o.id}`, icon: Box, keywords: o.id, run: go('objects', `${kind}:${o.id}`) }))) : []),
    ];
  }, [project, war3]);
  const toolBody = tool === 'changes' ? <ChangesPanel project={project} baseline={baseline} groups={changes} attribution={attribution} onChange={update} onNavigate={navigate} onSave={save} onRevertAll={() => { if (baseline) { update(baseline, '撤销全部未保存修改'); setAttribution({}); } }} />
    : tool === 'test' ? <TestRunPanel project={project} onNavigate={navigate} />
    : tool === 'assistant' ? <AssistantPanel project={project} revision={`r${revision}`} onNavigate={navigate} onApply={(next, label, diff) => { update(next, label); setAttribution(map => ({ ...map, ...Object.fromEntries(diff.flatMap(g => g.items.map(item => [item.key, '助手']))) })); }} />
    : null;
  return <div className={`studio workbench theme-${theme} density-${density}`}>
    <div className="wb-park" ref={park} hidden />
    <DockShell modules={modules} layout={layout} onLayout={applyLayout} focused={mode} onActivate={id => navigate(id)} onFocus={setMode} hosts={hosts} park={park}
      headerStart={<><button className="wb-brand" onPointerDown={event => event.stopPropagation()} onClick={() => { setError(''); setModal('project'); }} aria-label="打开工程" title="工程：新建、打开、导出"><Hexagon size={22} strokeWidth={1.8} /><span><strong>{project.name}</strong></span><ChevronDown size={12} /></button><button className="wb-back" disabled={!history.length} title={history.length ? `返回 ${modules.find(m => m.id === history.at(-1))?.label} · Alt ←` : '没有可返回的位置'} aria-label="返回上一位置" onPointerDown={event => event.stopPropagation()} onClick={back}><ArrowLeft size={15} /></button></>}
      headerEnd={<div className="wb-header-actions" onPointerDown={event => event.stopPropagation()}><button className="wb-command-button" onClick={() => setPalette('')} title="命令面板：搜索命令与工程内容 · Ctrl K"><Search size={14} /><span>搜索或执行命令</span><kbd>Ctrl K</kbd></button><span className="wb-divider" /><button title="撤销 · Ctrl Z" aria-label="撤销" disabled={!past.length} onClick={undo}><Undo2 size={16} /></button><button title="重做 · Ctrl Shift Z" aria-label="重做" disabled={!future.length} onClick={redo}><Redo2 size={16} /></button><button className="wb-save" onClick={save} aria-label="保存工程" title="保存 · Ctrl S"><Save size={15} /><span>保存</span>{dirty && <i />}</button><button className={`wb-tool-button ${tool === 'changes' ? 'is-open' : ''}`} onClick={() => toggleTool('changes')} title="审阅本次修改" aria-pressed={tool === 'changes'}><GitCompareArrows size={15} /><span>修改</span>{pendingCount > 0 && <b>{pendingCount}</b>}</button><span className="wb-divider" /><button className={`wb-tool-button wb-test-button ${tool === 'test' ? 'is-open' : ''}`} onClick={() => toggleTool('test')} title="测试地图 · F5" aria-pressed={tool === 'test'}><Play size={15} /><span>测试</span></button><button className={`wb-tool-button ${tool === 'assistant' ? 'is-open' : ''}`} onClick={() => toggleTool('assistant')} title="助手" aria-label="助手" aria-pressed={tool === 'assistant'}><Sparkles size={15} /></button><span className="wb-divider" /><button title={issues.length ? `工程检查：${issues.length} 个问题` : '工程检查'} aria-label="检查工程结构" onClick={() => setModal('problems')} className={issues.some(i => i.level === 'error') ? 'wb-danger' : ''}><Check size={16} /></button><button title="函数定义" aria-label="函数定义管理" onClick={() => { setError(''); setDefinitionDraft('[]'); setModal('definitions'); }}><Puzzle size={16} /></button><button title="工程与界面设置" aria-label="设置" onClick={() => setModal('settings')}><Settings2 size={16} /></button></div>}
      aside={tool && <ToolPanel tool={tool} onTool={setTool} onClose={() => setTool(null)} badges={{ changes: pendingCount }}>{toolBody}</ToolPanel>} />
    {[...mounted].map(id => createPortal(<Fragment key={generation[id] ?? 0}>{moduleView(id)}</Fragment>, hosts[id], id))}
    <footer className="wb-status"><span><i className="wb-status-light" />{dirty ? '未保存' : '已保存'}</span><span>{project.settings.target}</span><span>{project.settings.language}</span><div className="wb-spacer" /><button onClick={() => setModal('problems')}>{issues.length ? `${issues.length} 个问题` : '结构检查就绪'}</button><button onClick={() => toggleTool('changes')}>{pendingCount ? `${pendingCount} 项未保存修改` : '无未保存修改'}</button><span>C++ Core 未连接</span><span>R08</span></footer>
    {message && <div className="wb-toast" role="status"><Check size={14} />{message}</div>}
    <input ref={importRef} hidden type="file" accept="application/json,.json" onChange={async e => { const file = e.target.files?.[0]; e.target.value = ''; if (!file) return; try { if (file.size > 8 * 1024 * 1024) throw new Error('工程文件超过 8 MiB'); const next = readWorkbench(JSON.parse(await file.text())); update(next, '已打开工程'); setModal(null); setFocus({}); } catch (err) { setError(err instanceof Error ? err.message : '工程读取失败'); } }} />
    {modal === 'project' && <Modal title="工程" close={() => setModal(null)}><div className="wb-modal-body"><label>工程名称<CommitInput label="工程名称" value={project.name} onCommit={name => { if (name.trim()) update({ ...project, name: name.trim() }, '重命名工程'); }} /></label><div className="wb-project-actions"><button onClick={() => { update(createEmpty(), '新建工程'); setModal(null); setFocus({}); }}><FilePlus2 size={18} /><span>新建工程</span></button><button onClick={() => importRef.current?.click()}><FolderOpen size={18} /><span>打开工程 JSON</span></button><button onClick={() => downloadJSON(project, `${project.name}.forge.json`)}><Download size={18} /><span>导出工程 JSON</span></button><button onClick={() => { update(createSample(), '已打开示例工程'); setModal(null); navigate('triggers'); }}><GitBranch size={18} /><span>打开示例工程</span></button></div>{error && <p className="wb-error" role="alert">{error}</p>}</div></Modal>}
    {modal === 'settings' && <Modal title="设置" close={() => setModal(null)}><div className="wb-modal-body"><span className="wb-eyebrow">界面</span><label>主题<select aria-label="界面主题" value={theme} onChange={e => setTheme(e.target.value)}><option value="graphite">石墨</option><option value="midnight">深海</option><option value="paper">浅色</option></select></label><label>密度<select aria-label="界面密度" value={density} onChange={e => setDensity(e.target.value)}><option value="standard">标准</option><option value="compact">紧凑</option></select></label><hr /><span className="wb-eyebrow">地图设置</span><label>目标版本<select aria-label="目标版本" value={project.settings.target} onChange={e => update({ ...project, settings: { ...project.settings, target: e.target.value } }, '修改目标版本')}><option>Warcraft III 1.27</option><option>Warcraft III 1.31+</option><option>自定义扩展环境</option></select></label><label>脚本后端<select aria-label="脚本后端" value={project.settings.language} onChange={e => update({ ...project, settings: { ...project.settings, language: e.target.value } }, '修改脚本后端')}><option>JASS</option><option>vJass</option><option>Lua</option></select></label>{project.settings.language === 'Lua' && project.settings.target === 'Warcraft III 1.27' && <span className="wb-error">此目标需要 Lua 扩展后端</span>}<label>玩家数量<CommitInput label="玩家数量" type="number" min={1} max={24} value={project.settings.players} onCommit={v => update({ ...project, settings: { ...project.settings, players: Number(v) } }, '修改玩家数量')} /></label><label className="pw-check"><input type="checkbox" checked={project.settings.fog} onChange={e => update({ ...project, settings: { ...project.settings, fog: e.target.checked } }, '修改战争迷雾')} />战争迷雾</label></div></Modal>}
    {palette !== null && <CommandPalette initial={palette} commands={commands} entries={entries} onClose={() => setPalette(null)} />}
    {modal === 'definitions' && <Modal title="函数定义" close={() => setModal(null)} wide><div className="wb-modal-body"><div className="wb-definition-summary"><Puzzle size={18} /><span>{project.definitions.length} 个定义</span><div className="wb-spacer" /><button onClick={() => downloadJSON(project.definitions, 'definitions.json')}><Download size={14} />导出定义</button></div><textarea className="wb-definition-json" aria-label="扩展定义 JSON" spellCheck={false} value={definitionDraft} onChange={e => setDefinitionDraft(e.target.value)} /><div className="wb-inline"><button className="wb-primary" onClick={() => { try { const defs = parseDefinitions(JSON.parse(definitionDraft)); const merged = [...project.definitions.filter(d => !defs.some(next => next.id === d.id)), ...defs]; update({ ...project, definitions: merged }, `已导入 ${defs.length} 个定义`); setModal(null); } catch (err) { setError(err instanceof Error ? err.message : '定义无效'); } }}><Upload size={14} />导入 / 更新定义</button><button onClick={() => setDefinitionDraft(JSON.stringify([{ id: 'custom.action', title: '自定义动作', namespace: 'custom', version: '1.0', kind: 'action', ports: [{ id: 'in', label: '执行', type: 'exec', direction: 'in' }, { id: 'then', label: '完成', type: 'exec', direction: 'out' }], fields: [{ id: 'value', label: '数值', type: 'number', default: 1 }] }], null, 2))}><Plus size={14} />定义模板</button></div>{error && <pre className="wb-error">{error}</pre>}</div></Modal>}
    {modal === 'problems' && <Modal title="工程检查" close={() => setModal(null)} wide><div className="wb-modal-body"><div className="wb-check-summary">{issues.length ? <AlertTriangle size={25} /> : <Check size={25} />}<div><strong>{issues.length ? `${issues.length} 个结构问题` : '蓝图结构检查通过'}</strong><small>节点定义、参数类型、端口连接</small></div></div>{issues.map((issue, i) => <button key={i} className="pw-link-row" onClick={() => navigate('triggers', issue.graph)}><AlertTriangle size={15} /><span>{issue.message}</span></button>)}<div className="wb-check-note">脚本编译、原生地图保存及游戏测试尚未连接</div></div></Modal>}
  </div>;
}
