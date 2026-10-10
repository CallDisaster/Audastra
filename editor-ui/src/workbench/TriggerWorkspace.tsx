import { useLayoutEffect, useMemo, useRef, useState } from 'react';
import type { PointerEvent as ReactPointerEvent } from 'react';
import { ArrowRight, Box, ChevronDown, ChevronRight, Code2, Copy, FileCode2, GitBranch, Hand, Hash, Link2, ListTree, Maximize, Minus, MousePointer2, Plus, Settings2, Trash2, Unplug, X, Zap } from 'lucide-react';
import { connectPorts, makeNode, removeNodes, uid } from '../studio/model';
import type { Definition, Graph, GraphNode, Port } from '../studio/model';
import type { ModuleProps, VariableRecord, WorkbenchProject } from './types';
import { generateTrigger } from './triggerCode';
import type { ParameterBinding, ScriptLanguage, TriggerText } from './triggerCode';
import { NodeParameterField } from './NodeParameterField';
import { TraditionalView } from './TraditionalView';
import type { InsertRequest } from './TraditionalView';
import { buildEca, conditionText, findBlock, graphConditions, hasExecInput, insertNode, locateItem, nodeConditions } from './ecaModel';
import { withCatalog, withoutCatalog } from './triggerCatalog';
import { StatementChooser } from './StatementChooser';
import { TriggerTree } from './TriggerTree';
import { folderList, renameTrigger } from './triggerFolders';
import './trigger.css';

type Point = { x: number; y: number };
type Field = Definition['fields'][number];
type ParameterFocus = { node: string; field: string; surface: 'canvas' | 'inspector' } | null;
type CommitParameter = (node: GraphNode, field: Field, value: unknown, binding?: ParameterBinding, created?: VariableRecord[]) => void;
type AddRequest = { position: Point; screen: Point; source?: { node: string; port: string; type: string }; filter?: InsertRequest['filter']; insert?: InsertRequest['insert'] };
const icons = { event: Zap, flow: GitBranch, action: Box, value: Hash };
const kindLabels = { event: '事件', flow: '流程', action: '动作', value: '数据' };
const WIDTH = 254;
const portPoint = (node: GraphNode, definition: Definition, port: Port) => ({ x: node.x + (port.direction === 'out' ? WIDTH : 0), y: node.y + 68 + definition.ports.filter(p => p.direction === port.direction).findIndex(p => p.id === port.id) * 28 });
const nodeHeight = (definition?: Definition) => 56 + Math.max(definition?.ports.filter(p => p.direction === 'in').length ?? 0, definition?.ports.filter(p => p.direction === 'out').length ?? 0) * 28 + (definition?.fields.length ?? 0) * 33 + (definition?.conditions ? 33 : 0) + 16;
const curve = (a: Point, b: Point) => `M ${a.x} ${a.y} C ${a.x + Math.max(70, Math.abs(a.x - b.x) * .45)} ${a.y}, ${b.x - Math.max(70, Math.abs(a.x - b.x) * .45)} ${b.y}, ${b.x} ${b.y}`;
const textDoc = (graph: Graph) => graph.textDocument as TriggerText | undefined;
const graphSnapshot = (graph: Graph): Graph => { const snapshot = structuredClone(graph); delete snapshot.textDocument; return snapshot; };

function TriggerCanvas({ graph, project, selected, setSelected, onChange, onAdd, onCommit, onNavigate, parameterFocus, setParameterFocus, readOnly, onCopy, onPaste, onDelete, notify, onShowConditions }: {
  graph: Graph; project: WorkbenchProject; selected: string[]; setSelected: (ids: string[]) => void;
  onChange: (graph: Graph, label: string) => void; onAdd: (request: AddRequest) => void;
  onCommit: CommitParameter; onNavigate: ModuleProps['onNavigate']; parameterFocus: ParameterFocus; setParameterFocus: (focus: ParameterFocus) => void; readOnly: boolean;
  onCopy: () => void; onPaste: () => void; onDelete: () => void; notify: ModuleProps['notify']; onShowConditions: () => void;
}) {
  const host = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState({ width: 1000, height: 600 });
  const [view, setView] = useState({ x: 35, y: 65, zoom: .75 });
  const [mode, setMode] = useState<'select' | 'pan'>('select');
  const [gesture, setGesture] = useState<{ kind: 'node' | 'select' | 'pan'; start: Point; current: Point; ids: string[]; origin: typeof view } | null>(null);
  const [pending, setPending] = useState<AddRequest['source']>();
  const [cursor, setCursor] = useState<Point>({ x: 0, y: 0 });
  const definitions = useMemo(() => new Map(project.definitions.map(d => [d.id, d])), [project.definitions]);
  const initialLayout = useRef({ nodes: graph.nodes, definitions });
  const local = (event: { clientX: number; clientY: number }): Point => { const rect = host.current!.getBoundingClientRect(); return { x: event.clientX - rect.left, y: event.clientY - rect.top }; };
  const world = (point: Point) => ({ x: (point.x - view.x) / view.zoom, y: (point.y - view.y) / view.zoom });
  useLayoutEffect(() => { const element = host.current; if (!element) return; let first = true; const observer = new ResizeObserver(entries => { const graph = initialLayout.current; const definitions = graph.definitions; const rect = entries[0].contentRect; setSize({ width: rect.width, height: rect.height }); if (first && graph.nodes.length) { first = false; const minX = Math.min(...graph.nodes.map(n => n.x)), minY = Math.min(...graph.nodes.map(n => n.y)), maxX = Math.max(...graph.nodes.map(n => n.x + WIDTH)), maxY = Math.max(...graph.nodes.map(n => n.y + nodeHeight(definitions.get(n.definition)))); const zoom = Math.max(.25, Math.min(1, (rect.width - 64) / (maxX - minX), (rect.height - 110) / (maxY - minY))); setView({ x: (rect.width - (maxX - minX) * zoom) / 2 - minX * zoom, y: (rect.height - (maxY - minY) * zoom) / 2 - minY * zoom, zoom }); } }); observer.observe(element); return () => observer.disconnect(); }, []);
  const fit = () => {
    if (!graph.nodes.length) { setView({ x: 40, y: 60, zoom: 1 }); return; }
    const minX = Math.min(...graph.nodes.map(n => n.x)), minY = Math.min(...graph.nodes.map(n => n.y));
    const maxX = Math.max(...graph.nodes.map(n => n.x + WIDTH)), maxY = Math.max(...graph.nodes.map(n => n.y + nodeHeight(definitions.get(n.definition))));
    const zoom = Math.max(.25, Math.min(1, (size.width - 64) / (maxX - minX), (size.height - 110) / (maxY - minY)));
    setView({ x: (size.width - (maxX - minX) * zoom) / 2 - minX * zoom, y: (size.height - (maxY - minY) * zoom) / 2 - minY * zoom, zoom });
  };
  const zoomAt = (point: Point, factor: number) => { const next = Math.max(.25, Math.min(1.7, view.zoom * factor)); setView({ x: point.x - (point.x - view.x) * next / view.zoom, y: point.y - (point.y - view.y) * next / view.zoom, zoom: next }); };
  const moved = (node: GraphNode) => gesture?.kind === 'node' && gesture.ids.includes(node.id) ? { ...node, x: node.x + (gesture.current.x - gesture.start.x) / view.zoom, y: node.y + (gesture.current.y - gesture.start.y) / view.zoom } : node;
  const connect = (node: GraphNode, port: Port) => {
    if (!pending || port.direction !== 'in') return;
    try { onChange(connectPorts(graph, project.definitions, pending.node, pending.port, node.id, port.id), '连接节点'); } catch (error) { notify(error instanceof Error ? error.message : '连接无效'); }
    setPending(undefined);
  };
  const startNode = (event: ReactPointerEvent, node: GraphNode) => {
    if (event.button !== 0 || (event.target as HTMLElement).closest('button,input,textarea,select,[contenteditable],[data-node-parameter]')) return;
    event.stopPropagation();
    setParameterFocus(null);
    host.current?.focus();
    const ids = event.shiftKey ? selected.includes(node.id) ? selected.filter(id => id !== node.id) : [...selected, node.id] : selected.includes(node.id) ? selected : [node.id];
    setSelected(ids);
    const start = local(event);
    setGesture({ kind: 'node', start, current: start, ids, origin: view });
    host.current?.setPointerCapture(event.pointerId);
  };
  const end = (event: ReactPointerEvent) => {
    if ((event.target as HTMLElement).closest('[data-node-parameter]')) return;
    if (gesture) {
      if (gesture.kind === 'node' && (Math.abs(gesture.current.x - gesture.start.x) > 1 || Math.abs(gesture.current.y - gesture.start.y) > 1)) onChange({ ...graph, nodes: graph.nodes.map(n => { const next = moved(n); return { ...next, x: Math.round(next.x), y: Math.round(next.y) }; }) }, '移动节点');
      if (gesture.kind === 'select') {
        const a = world(gesture.start), b = world(gesture.current);
        const ids = graph.nodes.filter(n => n.x + WIDTH >= Math.min(a.x, b.x) && n.x <= Math.max(a.x, b.x) && n.y + nodeHeight(definitions.get(n.definition)) >= Math.min(a.y, b.y) && n.y <= Math.max(a.y, b.y)).map(n => n.id);
        setSelected(event.shiftKey ? Array.from(new Set([...selected, ...ids])) : ids);
      }
      setGesture(null);
      if (host.current?.hasPointerCapture(event.pointerId)) host.current.releasePointerCapture(event.pointerId);
    }
    if (pending && !(event.target as HTMLElement).closest('[data-port]')) {
      onAdd({ position: world(local(event)), screen: { x: event.clientX, y: event.clientY }, source: pending });
      setPending(undefined);
    }
  };
  const pendingNode = pending && graph.nodes.find(n => n.id === pending.node);
  const pendingDefinition = pendingNode && definitions.get(pendingNode.definition);
  const pendingPort = pendingDefinition?.ports.find(p => p.id === pending?.port);
  return <div ref={host} className={`tw-canvas ${mode === 'pan' ? 'is-pan' : ''}`} tabIndex={0} aria-label="蓝图画布" onKeyDown={e => {
    if ((e.target as HTMLElement).closest('input,textarea,select,[contenteditable],[data-node-parameter]')) return;
    if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); onDelete(); }
    if (e.key === 'Escape') { setPending(undefined); setParameterFocus(null); setSelected([]); }
    if (e.key.toLowerCase() === 'f') fit();
    if (e.ctrlKey && e.key.toLowerCase() === 'c') { e.preventDefault(); onCopy(); }
    if (e.ctrlKey && e.key.toLowerCase() === 'v') { e.preventDefault(); onPaste(); }
    if (e.ctrlKey && e.key.toLowerCase() === 'a') { e.preventDefault(); setSelected(graph.nodes.map(n => n.id)); }
  }} onWheel={e => { if ((e.target as HTMLElement).closest('input,textarea,select,[data-node-parameter]')) return; zoomAt(local(e), e.deltaY > 0 ? .92 : 1.08); }} onPointerMove={e => {
    const point = local(e); setCursor(world(point));
    if (gesture) { setGesture({ ...gesture, current: point }); if (gesture.kind === 'pan') setView({ ...gesture.origin, x: gesture.origin.x + point.x - gesture.start.x, y: gesture.origin.y + point.y - gesture.start.y }); }
  }} onPointerUp={end} onPointerCancel={() => setGesture(null)} onPointerDown={e => {
    if ((e.target as HTMLElement).closest('.tw-node,.tw-canvas-tools,.tw-edge-hit,[data-node-parameter]')) return;
    setParameterFocus(null);
    host.current?.focus();
    if (pending) return;
    if (e.button !== 0 && e.button !== 1) return;
    e.preventDefault();
    const start = local(e);
    setGesture({ kind: e.button === 1 || mode === 'pan' ? 'pan' : 'select', start, current: start, ids: [], origin: view });
    e.currentTarget.setPointerCapture(e.pointerId);
  }} onContextMenu={e => { if ((e.target as HTMLElement).closest('input,textarea,select,[contenteditable],[data-node-parameter]')) return; e.preventDefault(); setGesture(null); setParameterFocus(null); onAdd({ position: world(local(e)), screen: { x: e.clientX, y: e.clientY } }); }} onDoubleClick={e => { if (!(e.target as HTMLElement).closest('.tw-node,.tw-canvas-tools,[data-node-parameter]')) onAdd({ position: world(local(e)), screen: { x: e.clientX, y: e.clientY } }); }} style={{ backgroundPosition: `${view.x}px ${view.y}px`, backgroundSize: `${24 * view.zoom}px ${24 * view.zoom}px` }}>
    <div className="tw-canvas-label"><GitBranch size={14}/>{graph.name}<span>{graph.nodes.length} 节点 · {graph.edges.length} 连线</span></div>
    {graphConditions(graph).length > 0 && <div className="tw-condition-strip"><button onClick={onShowConditions} title="触发条件在传统视图中编辑"><GitBranch size={11}/>{graphConditions(graph).length} 项触发条件</button></div>}
    <svg className="tw-connections"><g transform={`translate(${view.x} ${view.y}) scale(${view.zoom})`}>{graph.edges.map(edge => {
      const source = graph.nodes.find(n => n.id === edge.from), target = graph.nodes.find(n => n.id === edge.to);
      const sd = source && definitions.get(source.definition), td = target && definitions.get(target.definition);
      const sp = sd?.ports.find(p => p.id === edge.output), tp = td?.ports.find(p => p.id === edge.input);
      if (!source || !target || !sd || !td || !sp || !tp) return null;
      const d = curve(portPoint(moved(source), sd, sp), portPoint(moved(target), td, tp));
      return <g key={edge.id} className={sp.type === 'exec' ? 'tw-wire-exec' : 'tw-wire-data'}><path d={d}/><path className="tw-edge-hit" d={d} onDoubleClick={e => { e.stopPropagation(); onChange({ ...graph, edges: graph.edges.filter(item => item.id !== edge.id) }, '断开连线'); }}><title>双击断开连线</title></path></g>;
    })}{pendingNode && pendingDefinition && pendingPort && <path className="tw-pending-wire" d={curve(portPoint(moved(pendingNode), pendingDefinition, pendingPort), cursor)}/>}</g></svg>
    <div className="tw-node-layer" style={{ transform: `translate(${view.x}px, ${view.y}px) scale(${view.zoom})` }}>{graph.nodes.map(raw => {
      const node = moved(raw), definition = definitions.get(node.definition);
      const Icon = icons[definition?.kind ?? 'action'];
      const input = definition?.ports.filter(p => p.direction === 'in') ?? [], output = definition?.ports.filter(p => p.direction === 'out') ?? [];
      return <article key={node.id} data-node-id={node.id} data-definition={node.definition} className={`tw-node kind-${definition?.kind ?? 'action'} ${selected.includes(node.id) ? 'selected' : ''} ${parameterFocus?.node === node.id ? 'is-editing' : ''} ${node.enabled === false ? 'is-disabled' : ''}`} style={{ left: node.x, top: node.y, width: WIDTH }} onPointerDown={e => startNode(e, node)} onClick={e => e.stopPropagation()}>
        <header><Icon size={16}/><strong>{definition?.title ?? node.definition}</strong><small>{kindLabels[definition?.kind ?? 'action']}</small></header>
        <div className="tw-node-ports">{Array.from({ length: Math.max(input.length, output.length) }, (_, i) => <div key={i} className="tw-port-row">{[input[i], output[i]].map((port, side) => port ? <button key={port.id} data-port={port.id} className={`tw-port ${port.direction} ${port.type === 'exec' ? 'exec' : 'data'} ${pending?.node === node.id && pending.port === port.id ? 'pending' : ''}`} title={`${port.label} · ${port.type}`} onPointerDown={e => { e.stopPropagation(); if (port.direction === 'out') { setPending({ node: node.id, port: port.id, type: port.type }); setCursor(portPoint(node, definition!, port)); } }} onPointerUp={e => { e.stopPropagation(); connect(node, port); }} onClick={e => { e.stopPropagation(); if (port.direction === 'in') connect(node, port); }}><i/>{port.label}</button> : <span key={side}/>)}</div>)}</div>
        <div className="tw-node-fields" onFocusCapture={() => { setSelected([node.id]); setPending(undefined); }}>{definition?.fields.map(field => <NodeParameterField key={field.id} node={node} field={field} graph={graph} project={project} disabled={readOnly} onCommit={(value, binding, created) => onCommit(node, field, value, binding, created)} onNavigate={onNavigate} open={parameterFocus?.surface === 'canvas' && parameterFocus.node === node.id && parameterFocus.field === field.id} onOpenChange={open => { setPending(undefined); setSelected([node.id]); setParameterFocus(open ? { node: node.id, field: field.id, surface: 'canvas' } : null); }}/>)}</div>
        {!!definition?.conditions && <button className="tw-node-conditions" title="条件在传统视图中编辑" onClick={e => { e.stopPropagation(); setSelected([node.id]); onShowConditions(); }}><GitBranch size={11}/>{conditionText(nodeConditions(node), project) || '无条件 · 总是成立'}</button>}
      </article>;
    })}</div>
    {gesture?.kind === 'select' && <div className="tw-selection-box" style={{ left: Math.min(gesture.start.x, gesture.current.x), top: Math.min(gesture.start.y, gesture.current.y), width: Math.abs(gesture.current.x - gesture.start.x), height: Math.abs(gesture.current.y - gesture.start.y) }}/>}
    <div className="tw-canvas-tools"><button className={mode === 'select' ? 'active' : ''} onClick={() => setMode('select')} title="框选"><MousePointer2 size={16}/></button><button className={mode === 'pan' ? 'active' : ''} onClick={() => setMode('pan')} title="平移"><Hand size={16}/></button><i/><button onClick={() => zoomAt({ x: size.width / 2, y: size.height / 2 }, .8)} title="缩小"><Minus size={15}/></button><button className="tw-zoom-value" onClick={() => zoomAt({ x: size.width / 2, y: size.height / 2 }, 1 / view.zoom)} title="实际大小 · 100%">{Math.round(view.zoom * 100)}%</button><button onClick={() => zoomAt({ x: size.width / 2, y: size.height / 2 }, 1.2)} title="放大"><Plus size={15}/></button><button onClick={fit} title="适应画布 · F"><Maximize size={15}/></button></div>
    <div className="tw-canvas-help">{pending ? '选择输入端口 · 拖到空白处添加' : selected.length ? `已选 ${selected.length} 个 · Shift 多选 · Delete 删除` : '右键添加节点 · 拖动框选 · 中键平移'}</div>
  </div>;
}

export function TriggerWorkspace({ project: storedProject, onChange: storeChange, notify, onNavigate, focusId }: ModuleProps) {
  // 触发器模块内使用“内置目录 + 工程定义”，写回时去掉内置目录，工程文件不保存目录副本。
  const project = useMemo(() => withCatalog(storedProject), [storedProject]);
  const onChange = (next: WorkbenchProject, label: string) => storeChange(withoutCatalog(next), label);
  const initialGraph = project.graphs.find(g => g.id === focusId || g.nodes.some(n => n.id === focusId)) ?? project.graphs[0];
  const [graphId, setGraphId] = useState(initialGraph?.id ?? '');
  const [scriptId, setScriptId] = useState('');
  const [view, setViewState] = useState<'traditional' | 'blueprint' | 'code'>(() => { try { const saved = localStorage.getItem('forge-trigger-view'); return saved === 'traditional' || saved === 'code' ? saved : 'blueprint'; } catch { return 'blueprint'; } });
  const [selected, setSelected] = useState<string[]>(focusId && initialGraph?.nodes.some(n => n.id === focusId) ? [focusId] : []);
  const [add, setAdd] = useState<AddRequest | null>(null);
  const [showAll, setShowAll] = useState(false);
  // 窄窗口时属性区收为抽屉，由工具栏按钮打开。
  const [inspectorOpen, setInspectorOpen] = useState(false);
  const [parameterFocus, setParameterFocus] = useState<ParameterFocus>(null);
  const [language, setLanguage] = useState<ScriptLanguage>('JASS');
  const [conversion, setConversion] = useState(false);
  const [clipboard, setClipboard] = useState<{ nodes: GraphNode[]; edges: Graph['edges'] }>();
  const [declaration, setDeclaration] = useState(false);
  const [declarationName, setDeclarationName] = useState('自定义函数');
  const [declarationSymbol, setDeclarationSymbol] = useState('CustomAction');
  const [declarationArgs, setDeclarationArgs] = useState('value:number');
  const graph = project.graphs.find(g => g.id === graphId) ?? project.graphs[0];
  const selectedScript = project.scripts.find(script => script.id === scriptId);
  const activeText = graph && textDoc(graph);
  const activeLanguage = (selectedScript?.language as ScriptLanguage | undefined) ?? activeText?.language ?? language;
  const generated = graph ? generateTrigger(graph, project, activeLanguage) : { content: '', warnings: [], mapped: 0 };
  const codeContent = selectedScript?.content ?? activeText?.content ?? generated.content;
  const selectedNode = graph?.nodes.find(n => n.id === selected[0]);
  const selectedDefinition = project.definitions.find(d => d.id === selectedNode?.definition);
  const selectGraph = (id: string) => { setGraphId(id); setScriptId(''); setSelected([]); setAdd(null); setParameterFocus(null); };
  const chooseView = (next: typeof view) => { setViewState(next); try { localStorage.setItem('forge-trigger-view', next); } catch { /* 仅为界面便利 */ } };
  // 没有触发器时仍显示目录，可以新建文件夹或触发器。
  if (!graph) return <div className="trigger-workspace"><TriggerTree project={project} activeId={undefined} scriptId={scriptId} onSelect={selectGraph} onSelectScript={id => { setScriptId(id); chooseView('code'); }} onChange={onChange} notify={notify} onNavigate={onNavigate} onDeclare={() => setDeclaration(true)}/><main className="tw-main"><div className="tw-empty-workspace"><span>没有打开的触发器</span><small>在左侧目录中新建触发器（Insert）或文件夹（Ctrl+G）</small></div></main><aside className="tw-inspector"/></div>;
  const changeGraph = (next: Graph, label: string, created?: VariableRecord[]) => onChange({ ...project, graphs: project.graphs.map(g => g.id === graph.id ? next : g), variables: created?.length ? [...project.variables, ...created] : project.variables }, label);
  const commitParameter: CommitParameter = (node, field, value, binding, created) => {
    if (activeText) return;
    const bindings = { ...(node.bindings as Record<string, ParameterBinding> ?? {}) };
    if (binding) bindings[field.id] = binding;
    else delete bindings[field.id];
    const nextGraph = { ...graph, nodes: graph.nodes.map(n => n.id === node.id ? { ...n, values: { ...n.values, [field.id]: value }, bindings } : n) };
    const linkedVariable = node.definition === 'core.number' && field.id === 'value' && !binding && project.variables.find(v => v.name === node.values.name || v.id === node.values.name);
    if (linkedVariable) onChange({ ...project, graphs: project.graphs.map(g => g.id === graph.id ? nextGraph : g), variables: [...project.variables.map(v => v.id === linkedVariable.id ? { ...v, initial: String(value) } : v), ...(created ?? [])] }, '修改变量初始值');
    else changeGraph(nextGraph, created?.length ? `修改参数并新建变量 ${created.map(v => v.name).join('、')}` : '修改参数', created);
  };
  const addAt = (request: AddRequest) => { if (activeText) { notify('当前为自定义文本 · 在代码页编辑'); return; } setShowAll(false); setAdd(request); };
  const addDefault = () => addAt({ position: { x: 100 + graph.nodes.length * 25, y: 120 }, screen: { x: Math.min(window.innerWidth - 390, 550), y: 180 } });
  // 传统视图：在所选行之后插入，否则追加到动作末尾；新节点在蓝图中放到前一节点右侧。
  const requestInsert = (request: InsertRequest) => {
    const anchor = request.insert && (request.insert.index > 0 ? findBlock(buildEca(graph, project.definitions), request.insert)?.items[request.insert.index - 1]?.node : graph.nodes.find(n => n.id === request.insert!.owner));
    const position = anchor ? { x: anchor.x + WIDTH + 56, y: anchor.y + (request.insert!.index > 0 ? 0 : 60) } : { x: request.filter === 'event' ? 40 : 100 + graph.nodes.length * 25, y: 120 + graph.nodes.length * 12 };
    // 下移到不与现有节点重叠的位置，蓝图中不出现叠放节点。
    const definitionsById = new Map(project.definitions.map(d => [d.id, d]));
    const overlaps = (y: number) => graph.nodes.some(n => Math.abs(n.x - position.x) < WIDTH + 16 && y < n.y + nodeHeight(definitionsById.get(n.definition)) + 16 && n.y < y + 170);
    for (let step = 0; step < 40 && overlaps(position.y); step++) position.y += 40;
    addAt({ position, screen: { x: Math.min(request.screen.x, window.innerWidth - 390), y: request.screen.y }, filter: request.filter, insert: request.insert });
  };
  const addStatement = () => {
    const document = buildEca(graph, project.definitions);
    const location = selected[0] ? locateItem(document, selected[0]) : undefined;
    const block = location?.block ?? document.actions[0];
    const screen = { x: Math.min(window.innerWidth - 390, 550), y: 180 };
    if (!block || block.locked) { notify('该分支与其他分支共享后续动作，请在蓝图中调整'); return; }
    requestInsert({ screen, filter: 'statement', insert: { owner: block.owner, port: block.port, index: location ? location.index + 1 : block.items.length } });
  };
  const copy = () => { if (!selected.length) return; setClipboard({ nodes: structuredClone(graph.nodes.filter(n => selected.includes(n.id))), edges: structuredClone(graph.edges.filter(e => selected.includes(e.from) && selected.includes(e.to))) }); notify(`已复制 ${selected.length} 个节点`); };
  const paste = () => {
    if (!clipboard || activeText) return;
    const ids = new Map(clipboard.nodes.map(n => [n.id, uid()]));
    const nodes = clipboard.nodes.map(n => ({ ...structuredClone(n), id: ids.get(n.id)!, x: n.x + 40, y: n.y + 40 }));
    changeGraph({ ...graph, nodes: [...graph.nodes, ...nodes], edges: [...graph.edges, ...clipboard.edges.map(e => ({ ...e, id: uid(), from: ids.get(e.from)!, to: ids.get(e.to)! }))] }, '粘贴节点'); setSelected(nodes.map(n => n.id)); setAdd(null);
  };
  const remove = () => { if (activeText) return; changeGraph(removeNodes(graph, selected), '删除节点'); setSelected([]); };
  const create = (definition: Definition) => {
    if (!add) return;
    const node = makeNode(definition, Math.round(add.position.x), Math.round(add.position.y));
    if (add.insert) {
      const block = findBlock(buildEca(graph, project.definitions), add.insert);
      if (!block) { notify('插入位置已变化，请重试'); setAdd(null); return; }
      try { changeGraph(insertNode(graph, project.definitions, block, add.insert.index, node), '添加动作'); setSelected([node.id]); } catch (error) { notify(error instanceof Error ? error.message : '无法插入'); }
      setAdd(null); return;
    }
    let next: Graph = { ...graph, nodes: [...graph.nodes, node] };
    if (add.source) { const port = definition.ports.find(p => p.direction === 'in' && (p.type === add.source!.type || p.type === 'any' || add.source!.type === 'any')); if (port) { try { next = connectPorts(next, project.definitions, add.source.node, add.source.port, node.id, port.id); } catch (error) { notify(String(error)); } } }
    changeGraph(next, '添加节点'); setSelected([node.id]); setAdd(null);
  };
  const convertToText = () => {
    const document: TriggerText = { language, content: generated.content, generated: generated.content, source: graphSnapshot(graph), warnings: generated.warnings };
    changeGraph({ ...graph, textDocument: document }, '转为自定义文本');
  };
  const restore = () => {
    if (!activeText) return;
    const changed = activeText.content !== activeText.generated;
    const scripts = changed ? [...project.scripts, { id: uid(), name: `${graph.name} · 自定义文本`, language: activeText.language, content: activeText.content }] : project.scripts;
    onChange({ ...project, scripts, graphs: project.graphs.map(g => g.id === graph.id ? { ...activeText.source, id: graph.id, name: graph.name } : g) }, '恢复可视结构'); setConversion(false); chooseView('traditional'); notify(changed ? '已保留自定义脚本，恢复转换前结构' : '已恢复可视结构');
  };
  const filteredDefinitions = project.definitions.filter(d => (add?.filter === 'event' ? d.kind === 'event' : add?.filter === 'statement' ? hasExecInput(d) : true) && (!add?.source || showAll || d.ports.some(p => p.direction === 'in' && (p.type === add.source!.type || p.type === 'any' || add.source!.type === 'any'))));
    return <div className={`trigger-workspace ${inspectorOpen ? 'show-inspector' : ''}`}>
    <TriggerTree project={project} activeId={graph?.id} scriptId={scriptId} onSelect={selectGraph} onSelectScript={id => { setScriptId(id); chooseView('code'); }} onChange={onChange} notify={notify} onNavigate={onNavigate} onDeclare={() => setDeclaration(true)}/>
    <main className="tw-main"><div className="tw-toolbar"><div className="tw-breadcrumb">触发器<ChevronRight size={12}/><strong>{graph.name}</strong></div><div className="tw-segment">{([['traditional', ListTree, '传统'], ['blueprint', GitBranch, '蓝图'], ['code', Code2, '代码']] as const).map(([id, Icon, name]) => <button key={id} className={view === id ? 'active' : ''} onClick={() => { chooseView(id); setParameterFocus(null); if (id !== 'code') setScriptId(''); }}><Icon size={14}/>{name}</button>)}</div><span className="tw-toolbar-spacer"/>{view !== 'code' && <><button onClick={copy} disabled={!selected.length} title="复制节点"><Copy size={14}/></button><button onClick={remove} disabled={!selected.length || !!activeText} title="删除节点"><Trash2 size={14}/></button><button className={`tw-inspector-toggle ${inspectorOpen ? 'active' : ''}`} aria-pressed={inspectorOpen} title="属性" onClick={() => setInspectorOpen(open => !open)}><Settings2 size={14}/></button><button className="tw-primary" onClick={view === 'traditional' ? addStatement : addDefault} title={view === 'traditional' ? '在所选行之后添加动作' : '添加节点'}><Plus size={14}/>添加</button></>}</div>
      {!selectedScript && <TriggerMeta key={`meta:${graph.id}`} graph={graph} onChange={changeGraph}/>}
      {activeText && view !== 'code' && <div className="tw-text-mode"><FileCode2 size={14}/>自定义文本<button onClick={() => chooseView('code')}>编辑代码<ArrowRight size={13}/></button></div>}
      {view === 'blueprint' ? <TriggerCanvas key={graph.id} graph={graph} project={project} selected={selected} setSelected={setSelected} onChange={(g, label) => { if (!activeText) changeGraph(g, label); }} onAdd={addAt} onCommit={commitParameter} onNavigate={onNavigate} parameterFocus={parameterFocus} setParameterFocus={setParameterFocus} readOnly={!!activeText} onCopy={copy} onPaste={paste} onDelete={remove} notify={notify} onShowConditions={() => chooseView('traditional')}/> : view === 'traditional' ? <TraditionalView key={graph.id} graph={graph} project={project} selected={selected} onSelect={setSelected} readOnly={!!activeText} onGraph={(next, label, created) => { if (!activeText) changeGraph(next, label, created); }} onCommit={commitParameter} onRequestAdd={requestInsert} onNavigate={onNavigate} onReveal={id => { setSelected([id]); chooseView('blueprint'); }} notify={notify}/> : <div className="tw-code-workspace"><div className="tw-code-toolbar"><FileCode2 size={15}/><span>{selectedScript?.name ?? `${graph.name}.${activeLanguage === 'Lua' ? 'lua' : 'j'}`}</span><select aria-label="代码语言" value={activeLanguage} disabled={!!activeText || !!selectedScript} onChange={e => setLanguage(e.target.value as ScriptLanguage)}>{(['JASS', 'vJass', 'Lua'] as const).map(lang => <option key={lang}>{lang}</option>)}</select><span className="tw-toolbar-spacer"/><span className="tw-code-state">{selectedScript ? '脚本源文件' : activeText ? '自定义文本' : '生成预览'}</span>{selectedScript ? <span className="tw-field-caption">无可视结构映射</span> : activeText ? <button onClick={() => setConversion(true)}><ListTree size={14}/>回转预览</button> : <button onClick={convertToText}><Code2 size={14}/>转为自定义文本</button>}</div><div className="tw-code-editor"><div className="tw-line-numbers" aria-hidden>{codeContent.split('\n').map((_, i) => <span key={i}>{i + 1}</span>)}</div><textarea aria-label="触发器代码" style={{ height: `${Math.max(25, codeContent.split('\n').length) * 21 + 45}px` }} spellCheck={false} readOnly={!activeText && !selectedScript} value={selectedScript?.content ?? activeText?.content ?? generated.content} onChange={e => { if (selectedScript) onChange({ ...project, scripts: project.scripts.map(s => s.id === selectedScript.id ? { ...s, content: e.target.value } : s) }, '编辑脚本'); else if (activeText) changeGraph({ ...graph, textDocument: { ...activeText, content: e.target.value } }, '编辑自定义文本'); }} onKeyDown={e => { if (e.key === 'Tab' && activeText) { e.preventDefault(); const element = e.currentTarget, at = element.selectionStart, end = element.selectionEnd; const content = activeText.content.slice(0, at) + '    ' + activeText.content.slice(end); changeGraph({ ...graph, textDocument: { ...activeText, content } }, '缩进代码'); requestAnimationFrame(() => { element.selectionStart = element.selectionEnd = at + 4; }); } }}/></div>{!selectedScript && (activeText?.warnings ?? generated.warnings).length > 0 && <details className="tw-code-diagnostics"><summary>{(activeText?.warnings ?? generated.warnings).length} 项生成限制</summary>{(activeText?.warnings ?? generated.warnings).map(w => <div key={w}>{w}</div>)}</details>}</div>}
      <div className="tw-status"><span><i/>{activeText ? '文本源' : '可视源'}</span><span>{graph.nodes.length} 节点</span><span>{graph.edges.length} 连线</span><span className="tw-toolbar-spacer"/><span>{project.settings.target}</span></div>
    </main>
    <aside className="tw-inspector"><header><Settings2 size={14}/>属性{selected.length > 1 && <small>{selected.length} 个节点</small>}</header>{selectedNode ? <><div className="tw-inspector-heading"><span className={`tw-type-badge kind-${selectedDefinition?.kind ?? 'action'}`}>{kindLabels[selectedDefinition?.kind ?? 'action']}</span><h3>{selectedDefinition?.title ?? selectedNode.definition}</h3><code>{selectedNode.definition}</code></div><div className="tw-inspector-section"><h4>参数</h4>{selectedDefinition?.fields.map(field => <div className="tw-property" key={field.id}><NodeParameterField node={selectedNode} field={field} graph={graph} project={project} disabled={!!activeText} onCommit={(value, binding, created) => commitParameter(selectedNode, field, value, binding, created)} onNavigate={onNavigate} open={parameterFocus?.surface === 'inspector' && parameterFocus.node === selectedNode.id && parameterFocus.field === field.id} onOpenChange={open => setParameterFocus(open ? { node: selectedNode.id, field: field.id, surface: 'inspector' } : null)}/>{field.id === 'template' && project.objects.some(o => o.id === selectedNode.values[field.id]) && <button className="tw-text-button" onClick={() => onNavigate('objects', String(selectedNode.values[field.id]))}>打开物体<ArrowRight size={12}/></button>}</div>)}</div><div className="tw-inspector-section"><h4>连接</h4>{graph.edges.filter(e => e.to === selectedNode.id || e.from === selectedNode.id).map(edge => <div className="tw-link-row" key={edge.id}><button onClick={() => setSelected([edge.to === selectedNode.id ? edge.from : edge.to])}><Link2 size={12}/>{project.definitions.find(d => d.id === graph.nodes.find(n => n.id === (edge.to === selectedNode.id ? edge.from : edge.to))?.definition)?.title ?? '节点'}</button><button title="断开连接" disabled={!!activeText} onClick={() => changeGraph({ ...graph, edges: graph.edges.filter(e => e.id !== edge.id) }, '断开连接')}><Unplug size={13}/></button></div>)}</div><div className="tw-inspector-section"><label className="tw-check"><input type="checkbox" checked={selectedNode.enabled !== false} disabled={!!activeText} onChange={e => changeGraph({ ...graph, nodes: graph.nodes.map(n => selected.includes(n.id) ? { ...n, enabled: e.target.checked } : n) }, '节点启用状态')}/>启用{selected.length > 1 ? '所选节点' : '节点'}</label><button className="tw-danger" disabled={!!activeText} onClick={remove}><Trash2 size={13}/>删除{selected.length > 1 ? ` ${selected.length} 个节点` : '节点'}</button></div></> : <><div className="tw-inspector-heading"><span className="tw-type-badge">触发器</span><h3>{graph.name}</h3></div><div className="tw-inspector-section"><label className="tw-form-field">名称<input key={graph.id + graph.name} defaultValue={graph.name} onKeyDown={e => { if (e.key === 'Enter') e.currentTarget.blur(); if (e.key === 'Escape') { e.currentTarget.value = graph.name; e.currentTarget.blur(); } }} onBlur={e => { if (e.target.value.trim() === graph.name) return; try { onChange(renameTrigger(project, graph.id, e.target.value), '重命名触发器'); } catch (error) { notify(error instanceof Error ? error.message : '名称无效'); e.target.value = graph.name; } }}/></label><label className="tw-form-field">文件夹<select value={graph.folder} onChange={e => changeGraph({ ...graph, folder: e.target.value }, '移动触发器')}>{folderList(project).map(folder => <option key={folder}>{folder}</option>)}</select></label><span className="tw-field-caption">允许、初始打开与注释在编辑区顶部；目录右键可新建、复制、删除。</span></div><div className="tw-inspector-section"><h4>目录</h4><button className="tw-text-button" onClick={() => onNavigate('variables')}><Hash size={14}/>全局变量<ArrowRight size={12}/></button><button className="tw-text-button" onClick={() => onNavigate('objects')}><Box size={14}/>物体编辑器<ArrowRight size={12}/></button></div></>}</aside>
    {add && <StatementChooser title={add.source ? '连接节点' : add.filter === 'event' ? '添加事件' : add.insert ? '添加动作' : '新建节点'} definitions={filteredDefinitions} position={{ left: Math.max(12, Math.min(add.screen.x, window.innerWidth - 612)), top: Math.max(60, Math.min(add.screen.y, window.innerHeight - 560)) }} onCreate={create} onClose={() => setAdd(null)}
      extra={add.source && <label className="tw-check"><input type="checkbox" checked={!showAll} onChange={e => setShowAll(!e.target.checked)}/>仅兼容 {add.source.type}</label>}
      footer={<footer><button disabled={!clipboard} onClick={paste}><Copy size={13}/>粘贴</button><button onClick={() => { setAdd(null); setDeclaration(true); }}><Plus size={13}/>函数声明</button></footer>}/>}
    {conversion && activeText && <div className="tw-backdrop"><section className="tw-dialog" role="dialog" aria-label="回转预览"><header><h3>回转预览</h3><button onClick={() => setConversion(false)}><X size={17}/></button></header><div className="tw-conversion"><div><ListTree size={25}/><strong>{activeText.content === activeText.generated ? '结构映射完整' : '自定义文本已修改'}</strong></div><dl><dt>原始结构</dt><dd>{activeText.source.nodes.length} 节点 · {activeText.source.edges.length} 连线</dd><dt>代码状态</dt><dd>{activeText.content === activeText.generated ? '与生成内容一致' : '修改内容保留为独立脚本'}</dd><dt>恢复结果</dt><dd>{activeText.content === activeText.generated ? '还原对应可视结构' : '恢复转换前结构，不反编译修改代码'}</dd></dl></div><footer><button onClick={() => setConversion(false)}>取消</button><button className="tw-primary" onClick={restore}>{activeText.content === activeText.generated ? '恢复可视结构' : '保留脚本并恢复原结构'}</button></footer></section></div>}
    {declaration && <div className="tw-backdrop"><section className="tw-dialog" role="dialog" aria-label="函数声明"><header><h3>函数声明</h3><button onClick={() => setDeclaration(false)}><X size={17}/></button></header><div className="tw-declaration-fields"><label className="tw-form-field">名称<input value={declarationName} onChange={e => setDeclarationName(e.target.value)}/></label><label className="tw-form-field">函数符号<input value={declarationSymbol} onChange={e => setDeclarationSymbol(e.target.value)}/></label><label className="tw-form-field">参数 · 名称:类型<input value={declarationArgs} onChange={e => setDeclarationArgs(e.target.value)} placeholder="count:number, message:text"/></label><span className="tw-field-caption">类型：number / text / boolean</span></div><footer><button onClick={() => setDeclaration(false)}>取消</button><button className="tw-primary" disabled={!declarationName.trim() || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(declarationSymbol)} onClick={() => { const fields: Definition['fields'] = declarationArgs.split(',').filter(s => s.trim()).map((argument, index) => { const [name, raw] = argument.trim().split(':'); const type = raw === 'number' || raw === 'boolean' ? raw : 'text'; return { id: name || `arg${index}`, label: name || `arg${index}`, type, default: type === 'number' ? 0 : type === 'boolean' ? false : '' }; }); if (new Set(fields.map(f => f.id)).size !== fields.length) { notify('参数名称不能重复'); return; } const definition: Definition = { id: `custom.${declarationSymbol}`, title: declarationName, namespace: 'custom', version: '1.0', kind: 'action', ports: [{ id: 'in', label: '执行', type: 'exec', direction: 'in' }, { id: 'then', label: '完成', type: 'exec', direction: 'out' }], fields, codegen: { jass: declarationSymbol, lua: declarationSymbol } }; if (project.definitions.some(d => d.id === definition.id)) { notify('该函数声明已存在'); return; } onChange({ ...project, definitions: [...project.definitions, definition] }, '添加函数声明'); setDeclaration(false); notify('函数声明已加入节点库 · 实现由目标脚本提供'); }}>添加声明</button></footer></section></div>}
  </div>;
}

/** 触发器顶部信息条：对应原版触发器面板上方的“允许 / 初始打开 / 触发器注释”，所有视图都可见。 */
function TriggerMeta({ graph, onChange }: { graph: Graph; onChange: (graph: Graph, label: string) => void }) {
  const comment = typeof graph.comment === 'string' ? graph.comment : '';
  const [open, setOpen] = useState(false);
  const off = graph.enabled === false;
  return <div className={`tw-trigger-meta ${off ? 'is-off' : ''}`}>
    <label className="tw-check" title="取消后触发器不生成代码，也不会被创建"><input type="checkbox" checked={!off} onChange={e => onChange({ ...graph, enabled: e.target.checked }, e.target.checked ? '允许触发器' : '禁用触发器')}/>允许</label>
    <label className="tw-check" title="取消后触发器在地图初始化时被关闭，需要用“开启触发器”动作打开" aria-disabled={off}><input type="checkbox" disabled={off} checked={graph.initiallyOn !== false} onChange={e => onChange({ ...graph, initiallyOn: e.target.checked }, '触发器初始状态')}/>初始打开</label>
    {off ? <span className="tw-meta-note">已禁用 · 不生成代码</span> : graph.initiallyOn === false ? <span className="tw-meta-note">初始关闭 · 生成 DisableTrigger</span> : null}
    <button className={`tw-meta-comment ${comment ? 'has-comment' : ''}`} aria-expanded={open} onClick={() => setOpen(value => !value)} title="触发器注释">{open ? <ChevronDown size={12}/> : <ChevronRight size={12}/>}<span>{comment ? comment.split(/\r?\n/)[0] : '添加注释…'}</span></button>
    {open && <textarea autoFocus aria-label="触发器注释" placeholder="说明这个触发器做什么；不会生成代码" defaultValue={comment} onKeyDown={e => { if (e.key === 'Escape') { e.currentTarget.value = comment; setOpen(false); } }} onBlur={e => { if (e.target.value !== comment) onChange({ ...graph, comment: e.target.value }, '编辑触发器注释'); }}/>}
  </div>;
}
