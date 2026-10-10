import { Fragment, useEffect, useMemo, useRef, useState } from 'react';
import type { DragEvent, KeyboardEvent, MouseEvent as ReactMouseEvent, ReactNode } from 'react';
import { ArrowDown, ArrowRight, ArrowUp, Ban, Box, Braces, ChevronDown, ChevronRight, ClipboardPaste, Copy, CornerDownRight, GitBranch, Hash, Link2, ListTree, LocateFixed, Plus, SlidersHorizontal, Trash2, TriangleAlert, Zap } from 'lucide-react';
import { uid } from '../studio/model';
import type { Definition, Graph, GraphNode } from '../studio/model';
import type { ModuleProps, VariableRecord, WorkbenchProject } from './types';
import type { ParameterBinding } from './triggerCode';
import { AnchoredPopover } from './AnchoredPopover';
import { SharedValuePicker } from './SharedValuePicker';
import { enumChoices, fieldReference, keepEditorSave, resolveFieldType } from './parameterFields';
import { bindingLabel, objectKindForType } from './valueSources';
import { buildEca, conditionTemplates, conditionTypeTitle, findCondition, graphConditions, nodeConditions, locateItem, makeCondition, mapConditions, moveItem, operandDisplay, operatorLabels, operatorsFor, parameterDisplay, removeItems, sentenceOf, setChain, subtreeIds } from './ecaModel';
import type { BlockRef, EcaBlock, EcaItem, Operand, TokenState, TriggerCondition } from './ecaModel';
import './traditional.css';

type Field = Definition['fields'][number];
export type InsertRequest = { screen: { x: number; y: number }; filter: 'event' | 'statement'; insert?: BlockRef & { index: number } };
type ChangeGraph = (graph: Graph, label: string, created?: VariableRecord[]) => void;
type RowKey = { type: 'event' | 'node' | 'condition'; id: string };
type Menu = { x: number; y: number; row: RowKey } | null;
const icons = { event: Zap, flow: GitBranch, action: Box, value: Hash };
const numeric = (type: string) => ['integer', 'real', 'number'].includes(type);

/** 语句中的一个参数：标量原位输入，复杂值在参数下方打开非模态取值面板。 */
function ValueToken({ label, text, state, expectedType, objectKind, value, binding, choices = [], disabled, onCommit, project, onNavigate, linked }: {
  label: string; text: string; state: TokenState; expectedType: string; objectKind?: ReturnType<typeof objectKindForType>;
  value: unknown; binding?: ParameterBinding; choices?: { label: string; value: string }[]; disabled?: boolean;
  onCommit: (value: unknown, binding?: ParameterBinding, created?: VariableRecord[]) => void;
  project: WorkbenchProject; onNavigate: ModuleProps['onNavigate']; linked?: string;
}) {
  const token = useRef<HTMLSpanElement>(null);
  const [open, setOpen] = useState(false);
  const [draft, setDraft] = useState<string | null>(null);
  const [invalid, setInvalid] = useState(false);
  // 键盘提交后焦点回到语句树（继续用方向键），移焦触发的 blur 不能再次提交。
  const settled = useRef(false);
  const bound = !!binding && binding.mode !== 'constant';
  const scalar = !bound && !objectKind && (numeric(expectedType) || ['string', 'text', 'code'].includes(expectedType));
  const boolean = !bound && expectedType === 'boolean';
  if (linked) return <span className="et-token is-linked" title={`由数据连线提供：${linked} · 在蓝图中断开后可编辑`}><Link2 size={10}/>{linked}</span>;
  const commitDraft = () => {
    if (draft === null || settled.current) return;
    if (numeric(expectedType) && (!draft.trim() || !Number.isFinite(Number(draft)) || (expectedType === 'integer' && !Number.isInteger(Number(draft))))) { setInvalid(true); return; }
    const next = numeric(expectedType) ? Number(draft) : draft;
    settled.current = true; setDraft(null); setInvalid(false);
    if (next !== value) onCommit(next);
  };
  const keys = (event: KeyboardEvent<HTMLInputElement>) => {
    keepEditorSave(event);
    const tree = event.currentTarget.closest<HTMLElement>('.et-tree');
    if (event.key === 'Enter') { event.preventDefault(); commitDraft(); if (settled.current) tree?.focus(); }
    if (event.key === 'Escape') { event.preventDefault(); settled.current = true; setDraft(null); setInvalid(false); tree?.focus(); }
  };
  const activate = () => {
    if (disabled) return;
    if (boolean) { onCommit(!value); return; }
    if (scalar && choices.length === 0) { settled.current = false; setDraft(String(value ?? '')); return; }
    setOpen(!open);
  };
  return <span ref={token} className={`et-token-wrap ${open ? 'is-open' : ''}`}>
    {draft !== null ? <input autoFocus className={`et-token-input ${invalid ? 'is-invalid' : ''}`} aria-label={label} aria-invalid={invalid || undefined} title={invalid ? (expectedType === 'integer' ? '请输入整数' : '请输入有效数字') : undefined} inputMode={numeric(expectedType) ? 'decimal' : undefined} spellCheck={false} value={draft} size={Math.max(3, Math.min(40, draft.length + 1))} onChange={event => { setDraft(event.target.value); setInvalid(false); }} onBlur={commitDraft} onKeyDown={keys} onClick={event => event.stopPropagation()}/>
      : <button type="button" className={`et-token is-${state} ${bound ? 'is-bound' : ''}`} aria-label={`${label}：${text}`} title={`${label} · ${bound ? binding!.mode === 'variable' ? '变量' : '函数' : scalar ? '点击输入' : boolean ? '点击切换' : '点击选择'}`} disabled={disabled} onClick={() => activate()}>{bound && (binding!.mode === 'variable' ? <Hash size={10}/> : <Braces size={10}/>)}{text}</button>}
    {!disabled && (scalar || boolean || choices.length > 0) && <button type="button" className="et-token-source" aria-label={`${label}参数来源`} title="全局变量 / 函数 / 固定值" onPointerDown={event => event.preventDefault()} onClick={() => { setDraft(null); setOpen(!open); }}><SlidersHorizontal size={10}/></button>}
    {open && !disabled && <AnchoredPopover anchor={() => token.current} placement="below" label={`${label}选择器`} title={label} onClose={() => setOpen(false)}>
      {choices.length > 0 && !bound && <div className="sv-result-list et-choices" aria-label="可选值">{choices.map(choice => <button key={choice.value} className={String(value) === choice.value ? 'is-selected' : ''} onClick={() => { onCommit(choice.value); setOpen(false); }}><span>{choice.label}<small>{choice.value}</small></span></button>)}</div>}
      <SharedValuePicker project={project} expectedType={expectedType} objectKind={objectKind} value={value} binding={binding} canCreateVariable onNavigate={onNavigate} onClose={() => setOpen(false)} onSelect={(next, nextBinding, created) => { onCommit(next, nextBinding, created); setOpen(false); }}/>
    </AnchoredPopover>}
  </span>;
}

function Sentence({ node, definition, graph, project, disabled, onCommit, onNavigate }: {
  node: GraphNode; definition?: Definition; graph: Graph; project: WorkbenchProject; disabled: boolean;
  onCommit: (field: Field, value: unknown, binding?: ParameterBinding, created?: VariableRecord[]) => void; onNavigate: ModuleProps['onNavigate'];
}) {
  const { category, parts } = sentenceOf(definition);
  const sourceTitle = (input: string) => {
    const edge = graph.edges.find(item => item.to === node.id && item.input === input);
    const source = edge && graph.nodes.find(item => item.id === edge.from);
    if (!source) return undefined;
    const sourceDefinition = project.definitions.find(item => item.id === source.definition);
    if (source.definition === 'core.number') return String(source.values.name ?? sourceDefinition?.title);
    return `${sourceDefinition?.title ?? source.definition}·${sourceDefinition?.ports.find(port => port.id === edge!.output)?.label ?? edge!.output}`;
  };
  return <span className="et-sentence">{!definition?.comment && <span className="et-category">{category} - </span>}{parts.map((part, index) => {
    if ('text' in part) return <Fragment key={index}>{part.text}</Fragment>;
    if ('port' in part) { const title = sourceTitle(part.port); return title ? <span key={index} className="et-token is-linked" title={`${part.label}：由数据连线提供`}><Link2 size={10}/>{title}</span> : <span key={index} className="et-token is-missing" title={`${part.label}：请在蓝图中连接数据`}>（{part.label}未连接）</span>; }
    const field = part.field, type = resolveFieldType(field, node, project.variables);
    const display = parameterDisplay(node, field, project);
    const reference = type.startsWith('globalvar') ? { objectKind: undefined } : fieldReference(field, node.definition);
    return <ValueToken key={field.id} label={field.label} text={display.text} state={display.state} expectedType={type} objectKind={reference.objectKind ?? objectKindForType(type)} value={node.values[field.id] ?? field.default} binding={(node.bindings as Record<string, ParameterBinding> | undefined)?.[field.id]} choices={enumChoices(field)} disabled={disabled} project={project} onNavigate={onNavigate} linked={sourceTitle(field.id)} onCommit={(value, binding, created) => onCommit(field, value, binding, created)}/>;
  })}</span>;
}

export function TraditionalView({ graph, project, selected, onSelect, readOnly, onGraph, onCommit, onRequestAdd, onNavigate, onReveal, notify }: {
  graph: Graph; project: WorkbenchProject; selected: string[]; onSelect: (ids: string[]) => void; readOnly: boolean;
  onGraph: ChangeGraph;
  onCommit: (node: GraphNode, field: Field, value: unknown, binding?: ParameterBinding, created?: VariableRecord[]) => void;
  onRequestAdd: (request: InsertRequest) => void; onNavigate: ModuleProps['onNavigate']; onReveal: (nodeId: string) => void; notify: ModuleProps['notify'];
}) {
  const definitions = project.definitions;
  const definitionMap = useMemo(() => new Map(definitions.map(item => [item.id, item])), [definitions]);
  const eca = useMemo(() => buildEca(graph, definitions), [graph, definitions]);
  const conditions = graphConditions(graph);
  const [folded, setFolded] = useState<string[]>([]);
  const [selectedCondition, setSelectedCondition] = useState<string>();
  const [menu, setMenu] = useState<Menu>(null);
  const [conditionMenu, setConditionMenu] = useState<{ parent?: string; owner: string; anchor: HTMLElement } | null>(null);
  const [drop, setDrop] = useState<{ key: string; position: 'before' | 'after' | 'inside' } | null>(null);
  const [clipboard, setClipboard] = useState<{ nodes: GraphNode[]; edges: Graph['edges']; roots: string[] } | null>(null);
  const tree = useRef<HTMLDivElement>(null);
  const dragging = useRef<string | null>(null);

  // 条件归属：'graph' 为触发条件，其余为节点 ID（If 等流程节点自带条件）。
  const conditionsOf = (owner: string) => owner === 'graph' ? conditions : nodeConditions(graph.nodes.find(node => node.id === owner) ?? { id: '', definition: '', x: 0, y: 0, values: {} });
  const ownerOf = (id: string) => findCondition(conditions, id) ? 'graph' : graph.nodes.find(node => findCondition(nodeConditions(node), id))?.id ?? 'graph';
  const setConditions = (owner: string, next: TriggerCondition[], label: string, created?: VariableRecord[]) => onGraph(owner === 'graph' ? { ...graph, conditions: next } : { ...graph, nodes: graph.nodes.map(node => node.id === owner ? { ...node, conditions: next } : node) }, label, created);
  const changeConditions = (id: string, change: (list: TriggerCondition[], index: number) => TriggerCondition[], label: string, created?: VariableRecord[]) => { const owner = ownerOf(id); setConditions(owner, mapConditions(conditionsOf(owner), id, change), label, created); };
  const guard = (action: () => void) => { if (readOnly) { notify('当前为自定义文本 · 在代码页编辑'); return; } try { action(); } catch (error) { notify(error instanceof Error ? error.message : '操作无效'); } };

  // 可见行的顺序，用于方向键导航。
  const rows: RowKey[] = [];
  eca.events.forEach(event => rows.push({ type: 'event', id: event.id }));
  const walkConditions = (list: TriggerCondition[]) => list.forEach(item => { rows.push({ type: 'condition', id: item.id }); if (item.kind !== 'compare' && !folded.includes(item.id)) walkConditions(item.children); });
  walkConditions(conditions);
  const walkBlocks = (blocks: EcaBlock[]) => blocks.forEach(block => block.items.forEach(item => { if (!item.shared) rows.push({ type: 'node', id: item.node.id }); if (!folded.includes(item.node.id)) { if (item.definition?.conditions) walkConditions(nodeConditions(item.node)); walkBlocks(item.blocks); } }));
  walkBlocks(eca.actions);
  if (eca.detached) walkBlocks([eca.detached]);
  const activeRow = selectedCondition ? { type: 'condition', id: selectedCondition } : selected.length ? { type: eca.events.some(event => event.id === selected[0]) ? 'event' : 'node', id: selected[0] } : undefined;
  const selectRow = (row: RowKey, additive = false) => {
    if (row.type === 'condition') { setSelectedCondition(row.id); onSelect([]); return; }
    setSelectedCondition(undefined);
    onSelect(additive ? selected.includes(row.id) ? selected.filter(id => id !== row.id) : [...selected, row.id] : [row.id]);
  };
  useEffect(() => { tree.current?.querySelector('.et-row.is-active')?.scrollIntoView({ block: 'nearest' }); }, [selected, selectedCondition]);

  // ---- 动作行操作 ----
  const blockItems = (block: EcaBlock) => block.items.filter(item => !item.shared).map(item => item.node.id);
  const moveBy = (nodeId: string, delta: number) => guard(() => {
    const location = locateItem(eca, nodeId);
    if (!location) return;
    const { block, index } = location;
    const target = index + delta;
    if (target < 0 || target >= block.items.length) return;
    if (block.locked) throw new Error('该分支与其他分支共享后续动作，请在蓝图中调整');
    const ids = blockItems(block); ids.splice(index, 1); ids.splice(target, 0, nodeId);
    onGraph(setChain(graph, definitions, block, ids), delta < 0 ? '上移动作' : '下移动作');
  });
  const toggleEnabled = (ids: string[]) => guard(() => { const enable = ids.some(id => graph.nodes.find(node => node.id === id)?.enabled === false); onGraph({ ...graph, nodes: graph.nodes.map(node => ids.includes(node.id) ? { ...node, enabled: enable } : node) }, enable ? '启用动作' : '禁用动作'); });
  const removeRows = (ids: string[]) => guard(() => {
    const events = ids.filter(id => eca.events.some(event => event.id === id));
    let next = removeItems(graph, definitions, ids.filter(id => !events.includes(id)));
    if (events.length) next = { ...next, nodes: next.nodes.filter(node => !events.includes(node.id)), edges: next.edges.filter(edge => !events.includes(edge.from) && !events.includes(edge.to)) };
    onGraph(next, ids.length > 1 ? `删除 ${ids.length} 行` : '删除动作'); onSelect([]);
  });
  const copyRows = (ids: string[]) => {
    const roots = rows.filter(row => row.type === 'node' && ids.includes(row.id)).map(row => row.id);
    const items = roots.map(id => locateItem(eca, id)).filter(Boolean).map(location => location!.block.items[location!.index]);
    const nodeIds = new Set(items.flatMap(subtreeIds));
    if (!nodeIds.size) return;
    setClipboard({ nodes: structuredClone(graph.nodes.filter(node => nodeIds.has(node.id))), edges: structuredClone(graph.edges.filter(edge => nodeIds.has(edge.from) && nodeIds.has(edge.to))), roots: items.map(item => item.node.id) });
    notify(`已复制 ${roots.length} 行`);
  };
  const pasteAfter = (anchorId?: string) => guard(() => {
    if (!clipboard) return;
    const location = anchorId ? locateItem(eca, anchorId) : undefined;
    const block = location?.block ?? eca.actions[0];
    if (!block || block.locked) throw new Error('无法粘贴到此位置');
    const ids = new Map(clipboard.nodes.map(node => [node.id, uid()]));
    const nodes = clipboard.nodes.map(node => ({ ...structuredClone(node), id: ids.get(node.id)!, x: node.x + 40, y: node.y + 40 }));
    // 复制行之间原有的顺序连线由块重排重建，只保留子块内部连线。
    const internal = clipboard.edges.filter(edge => !clipboard.roots.includes(edge.to)).map(edge => ({ ...edge, id: uid(), from: ids.get(edge.from)!, to: ids.get(edge.to)! }));
    const order = blockItems(block);
    const at = location ? location.index + 1 : order.length;
    order.splice(at, 0, ...clipboard.roots.map(id => ids.get(id)!));
    onGraph(setChain({ ...graph, nodes: [...graph.nodes, ...nodes], edges: [...graph.edges, ...internal] }, definitions, block, order), `粘贴 ${clipboard.roots.length} 行`);
    onSelect(clipboard.roots.map(id => ids.get(id)!));
  });
  const insertAt = (event: { clientX: number; clientY: number }, block: EcaBlock, index: number) => {
    if (readOnly) { notify('当前为自定义文本 · 在代码页编辑'); return; }
    if (!block || block.locked) { notify('该分支与其他分支共享后续动作，请在蓝图中调整'); return; }
    onRequestAdd({ screen: { x: event.clientX, y: event.clientY }, filter: 'statement', insert: { owner: block.owner, port: block.port, index } });
  };
  const insertAfterSelection = (event: { clientX: number; clientY: number }) => {
    const location = selected[0] ? locateItem(eca, selected[0]) : undefined;
    if (location) insertAt(event, location.block, location.index + 1);
    else insertAt(event, eca.actions[0], eca.actions[0]?.items.length ?? 0);
  };

  // ---- 条件操作 ----
  const updateCondition = (id: string, change: (item: TriggerCondition) => TriggerCondition, label: string, created?: VariableRecord[]) => guard(() => changeConditions(id, (list, index) => list.map((item, i) => i === index ? change(item) : item), label, created));
  const moveCondition = (id: string, delta: number) => guard(() => changeConditions(id, (list, index) => { const target = index + delta; if (target < 0 || target >= list.length) return list; const next = list.slice(); const [item] = next.splice(index, 1); next.splice(target, 0, item); return next; }, '移动条件'));
  const removeCondition = (id: string) => guard(() => { changeConditions(id, (list, index) => list.filter((_, i) => i !== index), '删除条件'); setSelectedCondition(undefined); });
  const addCondition = (templateId: string, owner: string, parent?: string) => guard(() => {
    const condition = makeCondition(templateId);
    if (parent) changeConditions(parent, (list, index) => list.map((item, i) => i === index && item.kind !== 'compare' ? { ...item, children: [...item.children, condition] } : item), '添加条件');
    else setConditions(owner, [...conditionsOf(owner), condition], '添加条件');
    if (parent) setFolded(list => list.filter(id => id !== parent));
    setSelectedCondition(condition.id); onSelect([]); setConditionMenu(null);
  });

  // ---- 键盘 ----
  const onKey = (event: KeyboardEvent<HTMLDivElement>) => {
    if ((event.target as HTMLElement).closest('input,textarea,select,.et-token-wrap.is-open')) return;
    const index = activeRow ? rows.findIndex(row => row.type === activeRow.type && row.id === activeRow.id) : -1;
    const key = event.key;
    if ((key === 'ArrowDown' || key === 'ArrowUp') && !event.altKey) { event.preventDefault(); const next = rows[Math.max(0, Math.min(rows.length - 1, index + (key === 'ArrowDown' ? 1 : -1)))]; if (next) selectRow(next, event.shiftKey); return; }
    if (!activeRow) return;
    if ((key === 'ArrowDown' || key === 'ArrowUp') && event.altKey) { event.preventDefault(); if (activeRow.type === 'condition') moveCondition(activeRow.id, key === 'ArrowUp' ? -1 : 1); else if (activeRow.type === 'node') moveBy(activeRow.id, key === 'ArrowUp' ? -1 : 1); return; }
    if (key === 'ArrowLeft' || key === 'ArrowRight') { event.preventDefault(); setFolded(list => key === 'ArrowLeft' ? [...new Set([...list, activeRow.id])] : list.filter(id => id !== activeRow.id)); return; }
    if (key === 'Delete') { event.preventDefault(); if (activeRow.type === 'condition') removeCondition(activeRow.id); else removeRows(selected); return; }
    if ((event.ctrlKey || event.metaKey) && key.toLowerCase() === 'c' && activeRow.type === 'node') { event.preventDefault(); copyRows(selected); return; }
    if ((event.ctrlKey || event.metaKey) && key.toLowerCase() === 'v') { event.preventDefault(); pasteAfter(activeRow.type === 'node' ? activeRow.id : undefined); return; }
    if (key === 'Enter') { event.preventDefault(); (tree.current?.querySelector('.et-row.is-active .et-token:not(:disabled)') as HTMLElement | null)?.click(); }
  };

  // ---- 拖放：同块重排或跨分支移动 ----
  const dropTarget = (event: DragEvent, key: string, allowInside: boolean) => {
    if (!dragging.current) return;
    event.preventDefault(); event.stopPropagation();
    const rect = (event.currentTarget as HTMLElement).getBoundingClientRect();
    const ratio = (event.clientY - rect.top) / rect.height;
    const position = allowInside && ratio > .3 && ratio < .7 ? 'inside' : ratio < .5 ? 'before' : 'after';
    if (drop?.key !== key || drop.position !== position) setDrop({ key, position });
  };
  const finishDrop = (event: DragEvent, target: BlockRef, index: number) => {
    event.preventDefault(); event.stopPropagation();
    const id = dragging.current; dragging.current = null; setDrop(null);
    if (!id) return;
    guard(() => onGraph(moveItem(graph, definitions, id, target, index), '移动动作'));
  };

  const contextMenu = (event: ReactMouseEvent, row: RowKey) => { event.preventDefault(); event.stopPropagation(); if (!(row.type !== 'condition' && selected.includes(row.id))) selectRow(row); setMenu({ x: event.clientX, y: event.clientY, row }); };
  const commitNode = (node: GraphNode) => (field: Field, value: unknown, binding?: ParameterBinding, created?: VariableRecord[]) => onCommit(node, field, value, binding, created);

  const renderItem = (item: EcaItem, block: EcaBlock, index: number, depth: number, ancestorDisabled: boolean): ReactNode => {
    const { node, definition } = item;
    if (item.shared) return <div key={`shared-${node.id}-${index}`} className="et-row is-shared" style={{ paddingLeft: 12 + depth * 20 }}><CornerDownRight size={13}/><span>接续 “{definition?.title ?? node.definition}”（与其他分支共享，在蓝图中查看）</span></div>;
    const Icon = icons[definition?.kind ?? 'action'];
    const disabled = ancestorDisabled || node.enabled === false;
    const active = activeRow?.type === 'node' && activeRow.id === node.id;
    const collapsed = folded.includes(node.id);
    const key = `node:${node.id}`;
    const canDrag = !readOnly && !block.locked;
    return <div key={node.id} className="et-item" role="none">
      <div className={`et-row ${definition?.comment ? 'is-comment' : ''} ${selected.includes(node.id) ? 'is-selected' : ''} ${active ? 'is-active' : ''} ${disabled ? 'is-disabled' : ''} ${drop?.key === key ? `drop-${drop.position}` : ''}`} role="treeitem" aria-level={depth + 1} aria-selected={selected.includes(node.id)} aria-expanded={item.blocks.length || definition?.conditions ? !collapsed : undefined} data-node-id={node.id} style={{ paddingLeft: 12 + depth * 20 }}
        draggable={canDrag} onDragStart={event => { if ((event.target as HTMLElement).closest('.et-token-wrap')) { event.preventDefault(); return; } dragging.current = node.id; event.dataTransfer.effectAllowed = 'move'; event.dataTransfer.setData('text/plain', node.id); }} onDragEnd={() => { dragging.current = null; setDrop(null); }}
        onDragOver={event => dropTarget(event, key, item.blocks.length > 0 && !collapsed)} onDragLeave={() => setDrop(current => current?.key === key ? null : current)}
        onDrop={event => { const position = drop?.key === key ? drop.position : 'after'; if (position === 'inside' && item.blocks[0]) finishDrop(event, item.blocks[0], 0); else finishDrop(event, block, position === 'before' ? index : index + 1); }}
        onClick={event => selectRow({ type: 'node', id: node.id }, event.shiftKey || event.ctrlKey)} onContextMenu={event => contextMenu(event, { type: 'node', id: node.id })} onDoubleClick={() => onReveal(node.id)}>
        <button className="et-fold" tabIndex={-1} aria-label={collapsed ? '展开' : '折叠'} onClick={event => { event.stopPropagation(); setFolded(list => collapsed ? list.filter(id => id !== node.id) : [...list, node.id]); }}>{item.blocks.length || definition?.conditions ? collapsed ? <ChevronRight size={12}/> : <ChevronDown size={12}/> : null}</button>
        <Icon size={14} className={`et-icon kind-${definition?.kind ?? 'action'}`}/>
        <Sentence node={node} definition={definition} graph={graph} project={project} disabled={readOnly} onCommit={commitNode(node)} onNavigate={onNavigate}/>
        {node.enabled === false && <span className="et-flag">已禁用</span>}
      </div>
      {!collapsed && !!definition?.conditions && <div className="et-block" role="group" aria-label="条件">
        <div className="et-block-label" style={{ paddingLeft: 30 + depth * 20 }}><ListTree size={12}/>条件{!readOnly && <button className="et-inline-add" aria-label={`添加条件到 ${definition.title}`} onClick={event => setConditionMenu({ owner: node.id, anchor: event.currentTarget })}><Plus size={12}/></button>}</div>
        {nodeConditions(node).map(condition => renderCondition(condition, depth + 1, disabled))}
        {!nodeConditions(node).length && <div className="et-empty" style={{ paddingLeft: 32 + (depth + 1) * 20 }}>无条件 · 总是执行 Then</div>}
      </div>}
      {!collapsed && item.blocks.map(child => renderBlock(child, depth + 1, disabled))}
    </div>;
  };
  const renderBlock = (block: EcaBlock, depth: number, ancestorDisabled: boolean): ReactNode => {
    const key = `block:${block.owner}:${block.port}`;
    return <div key={key} className="et-block" role="group" aria-label={block.label}>
      <div className={`et-block-label ${drop?.key === key ? 'drop-inside' : ''}`} style={{ paddingLeft: 30 + depth * 20 - 20 }} onDragOver={event => dropTarget(event, key, false)} onDragLeave={() => setDrop(current => current?.key === key ? null : current)} onDrop={event => finishDrop(event, block, 0)}>
        <ListTree size={12}/>{block.label}{block.locked && <span className="et-flag" title="与其他分支共享后续动作；重排请在蓝图中调整">共享</span>}
        {!readOnly && !block.locked && <button className="et-inline-add" aria-label={`添加到 ${block.label}`} onClick={event => insertAt(event, block, block.items.length)}><Plus size={12}/></button>}
      </div>
      {block.items.map((item, index) => renderItem(item, block, index, depth, ancestorDisabled))}
      {!block.items.length && <div className="et-empty" style={{ paddingLeft: 32 + depth * 20 }}>无动作</div>}
    </div>;
  };
  const renderCondition = (condition: TriggerCondition, depth: number, ancestorDisabled: boolean): ReactNode => {
    const active = selectedCondition === condition.id;
    const disabled = ancestorDisabled || condition.enabled === false;
    const collapsed = folded.includes(condition.id);
    const operand = (side: 'left' | 'right', item: Extract<TriggerCondition, { kind: 'compare' }>) => {
      const current: Operand = item[side];
      const display = current.binding && current.binding.mode !== 'constant' ? { text: bindingLabel(current.binding, project), state: 'set' as TokenState } : operandDisplay(current.value, item.valueType, project);
      return <ValueToken label={side === 'left' ? '左值' : '右值'} text={display.text} state={display.state} expectedType={item.valueType} objectKind={objectKindForType(item.valueType)} value={current.value} binding={current.binding} disabled={readOnly} project={project} onNavigate={onNavigate}
        onCommit={(value, binding, created) => updateCondition(condition.id, previous => previous.kind === 'compare' ? { ...previous, [side]: { value, ...(binding ? { binding } : {}) } } : previous, '修改条件', created)}/>;
    };
    return <div key={condition.id} className="et-item" role="none">
      <div className={`et-row ${active ? 'is-selected is-active' : ''} ${disabled ? 'is-disabled' : ''}`} role="treeitem" aria-level={depth + 1} aria-selected={active} data-condition-id={condition.id} style={{ paddingLeft: 12 + depth * 20 }} onClick={() => selectRow({ type: 'condition', id: condition.id })} onContextMenu={event => contextMenu(event, { type: 'condition', id: condition.id })}>
        <button className="et-fold" tabIndex={-1} aria-label={collapsed ? '展开' : '折叠'} onClick={event => { event.stopPropagation(); setFolded(list => collapsed ? list.filter(id => id !== condition.id) : [...list, condition.id]); }}>{condition.kind !== 'compare' ? collapsed ? <ChevronRight size={12}/> : <ChevronDown size={12}/> : null}</button>
        <GitBranch size={14} className="et-icon kind-condition"/>
        {condition.kind === 'compare' ? <span className="et-sentence"><span className="et-category">{conditionTypeTitle(condition.valueType)}比较 - </span>{operand('left', condition)}{' '}
          <select className="et-operator" aria-label="比较运算" value={condition.op} disabled={readOnly} onClick={event => event.stopPropagation()} onKeyDown={keepEditorSave} onChange={event => updateCondition(condition.id, previous => previous.kind === 'compare' ? { ...previous, op: event.target.value as typeof previous.op } : previous, '修改比较运算')}>{operatorsFor(condition.valueType).map(op => <option key={op} value={op}>{operatorLabels[op]}</option>)}</select>{' '}{operand('right', condition)}</span>
          : <span className="et-sentence"><span className="et-category">{condition.kind === 'and' ? 'And' : 'Or'} - </span>{condition.kind === 'and' ? '所有条件成立' : '任一条件成立'}</span>}
        {condition.enabled === false && <span className="et-flag">已禁用</span>}
      </div>
      {condition.kind !== 'compare' && !collapsed && <div className="et-block" role="group" aria-label="条件">
        <div className="et-block-label" style={{ paddingLeft: 30 + depth * 20 }}><ListTree size={12}/>条件{!readOnly && <button className="et-inline-add" aria-label="添加子条件" onClick={event => setConditionMenu({ parent: condition.id, owner: ownerOf(condition.id), anchor: event.currentTarget })}><Plus size={12}/></button>}</div>
        {condition.children.map(child => renderCondition(child, depth + 1, disabled))}
        {!condition.children.length && <div className="et-empty" style={{ paddingLeft: 32 + (depth + 1) * 20 }}>空条件组视为成立</div>}
      </div>}
    </div>;
  };

  const menuItems = (() => {
    if (!menu) return [];
    const { row } = menu;
    if (row.type === 'condition') {
      const condition = findCondition(conditionsOf(ownerOf(row.id)), row.id);
      return [
        ...(condition && condition.kind !== 'compare' ? [{ label: '添加子条件', icon: Plus, run: () => { const anchor = window.document.querySelector(`[data-condition-id="${row.id}"]`) as HTMLElement | null; if (anchor) setConditionMenu({ parent: row.id, owner: ownerOf(row.id), anchor }); } }] : []),
        { label: '上移', icon: ArrowUp, keys: 'Alt ↑', run: () => moveCondition(row.id, -1) },
        { label: '下移', icon: ArrowDown, keys: 'Alt ↓', run: () => moveCondition(row.id, 1) },
        { label: condition?.enabled === false ? '启用条件' : '禁用条件', icon: Ban, run: () => updateCondition(row.id, item => ({ ...item, enabled: item.enabled === false }), '条件启用状态') },
        { label: '删除', icon: Trash2, keys: 'Delete', danger: true, run: () => removeCondition(row.id) },
      ];
    }
    if (row.type === 'event') return [
      { label: '在蓝图中定位', icon: LocateFixed, run: () => onReveal(row.id) },
      { label: graph.nodes.find(node => node.id === row.id)?.enabled === false ? '启用事件' : '禁用事件', icon: Ban, run: () => toggleEnabled([row.id]) },
      { label: '删除事件', icon: Trash2, danger: true, run: () => removeRows([row.id]) },
    ];
    const ids = selected.includes(row.id) ? selected : [row.id];
    const location = locateItem(eca, row.id);
    const item = location?.block.items[location.index];
    return [
      { label: '在之后添加动作', icon: Plus, run: () => location && insertAt({ clientX: menu.x, clientY: menu.y }, location.block, location.index + 1) },
      ...(item?.blocks.length ? item.blocks.map(block => ({ label: `添加到 ${block.label}`, icon: CornerDownRight, run: () => insertAt({ clientX: menu.x, clientY: menu.y }, block, block.items.length) })) : []),
      { label: '上移', icon: ArrowUp, keys: 'Alt ↑', run: () => moveBy(row.id, -1) },
      { label: '下移', icon: ArrowDown, keys: 'Alt ↓', run: () => moveBy(row.id, 1) },
      { label: graph.nodes.find(node => node.id === row.id)?.enabled === false ? '启用' : '禁用', icon: Ban, run: () => toggleEnabled(ids) },
      { label: '复制', icon: Copy, keys: 'Ctrl C', run: () => copyRows(ids) },
      { label: '粘贴到之后', icon: ClipboardPaste, keys: 'Ctrl V', disabled: !clipboard, run: () => pasteAfter(row.id) },
      { label: '在蓝图中定位', icon: LocateFixed, run: () => onReveal(row.id) },
      { label: ids.length > 1 ? `删除 ${ids.length} 行` : '删除', icon: Trash2, keys: 'Delete', danger: true, run: () => removeRows(ids) },
    ];
  })();

  const firstBlock = eca.actions[0];
  return <div className="et-view" onClick={() => setMenu(null)}>
    <div className="et-heading"><ListTree size={17}/><h2>{graph.name}</h2><span className={graph.enabled === false ? 'is-off' : ''}>{graph.enabled === false ? '已禁用' : '已启用'}</span><span className={graph.initiallyOn === false ? 'is-off' : ''}>{graph.initiallyOn === false ? '初始关闭' : '初始开启'}</span>{readOnly && <span className="is-off">自定义文本</span>}</div>
    <div ref={tree} className="et-tree" role="tree" aria-label="触发器函数" tabIndex={0} onKeyDown={onKey}>
      <section className="et-section" aria-label="事件">
        <header><Zap size={14}/>事件<small>{eca.events.length}</small>{!readOnly && <button aria-label="添加事件" title="添加事件" onClick={event => onRequestAdd({ screen: { x: event.clientX, y: event.clientY }, filter: 'event' })}><Plus size={13}/></button>}</header>
        {eca.events.map(event => { const definition = definitionMap.get(event.definition), active = activeRow?.type === 'event' && activeRow.id === event.id; return <div key={event.id} className={`et-row ${selected.includes(event.id) ? 'is-selected' : ''} ${active ? 'is-active' : ''} ${event.enabled === false ? 'is-disabled' : ''}`} role="treeitem" aria-level={1} aria-selected={selected.includes(event.id)} data-node-id={event.id} style={{ paddingLeft: 12 }} onClick={e => selectRow({ type: 'event', id: event.id }, e.shiftKey || e.ctrlKey)} onContextMenu={e => contextMenu(e, { type: 'event', id: event.id })} onDoubleClick={() => onReveal(event.id)}><span className="et-fold"/><Zap size={14} className="et-icon kind-event"/><Sentence node={event} definition={definition} graph={graph} project={project} disabled={readOnly} onCommit={commitNode(event)} onNavigate={onNavigate}/>{event.enabled === false && <span className="et-flag">已禁用</span>}</div>; })}
        {!eca.events.length && <div className="et-empty">无事件 · 动作在地图初始化时执行一次</div>}
      </section>
      <section className="et-section" aria-label="条件">
        <header><GitBranch size={14}/>条件<small>{conditions.length}</small>{!readOnly && <button aria-label="添加条件" title="添加条件" onClick={event => setConditionMenu({ owner: 'graph', anchor: event.currentTarget })}><Plus size={13}/></button>}</header>
        {conditions.map(condition => renderCondition(condition, 0, false))}
        {!conditions.length && <div className="et-empty">无条件 · 事件发生时总是执行动作</div>}
      </section>
      <section className="et-section" aria-label="动作">
        <header><ArrowRight size={14}/>动作<small>{rows.filter(row => row.type === 'node').length}</small>{!readOnly && firstBlock && !firstBlock.locked && <button aria-label="添加动作" title="在所选行之后添加动作" onClick={insertAfterSelection}><Plus size={13}/></button>}</header>
        {eca.actions.map((block, index) => index === 0 ? <div key="root" className="et-block" role="group" aria-label={block.label} onDragOver={event => { if (!block.items.length) dropTarget(event, 'block:root', false); }} onDrop={event => { if (!block.items.length) finishDrop(event, block, 0); }}>{block.items.map((item, i) => renderItem(item, block, i, 0, false))}{!block.items.length && <div className={`et-empty ${drop?.key === 'block:root' ? 'drop-inside' : ''}`}>添加动作</div>}</div> : renderBlock(block, 0, false))}
        {eca.detached && <div className="et-detached"><div className="et-detached-label"><TriangleAlert size={13}/>{eca.detached.label}</div>{eca.detached.items.map((item, i) => renderItem(item, eca.detached!, i, 0, false))}</div>}
        {eca.unlinkedValues.length > 0 && <div className="et-note"><Hash size={12}/>{eca.unlinkedValues.length} 个未连接的数据节点仅在蓝图中显示</div>}
      </section>
    </div>
    {menu && <div className="et-menu" role="menu" style={{ left: Math.min(menu.x, window.innerWidth - 230), top: Math.min(menu.y, window.innerHeight - 40 - menuItems.length * 31) }} onClick={event => event.stopPropagation()} onContextMenu={event => event.preventDefault()}>
      {menuItems.map(item => <button key={item.label} role="menuitem" className={'danger' in item && item.danger ? 'is-danger' : ''} disabled={readOnly || ('disabled' in item && item.disabled)} onClick={() => { setMenu(null); item.run(); }}><item.icon size={13}/><span>{item.label}</span>{'keys' in item && <kbd>{item.keys}</kbd>}</button>)}
    </div>}
    {conditionMenu && <AnchoredPopover anchor={() => conditionMenu.anchor} placement="below" width={230} label="条件类型选择器" title={conditionMenu.parent ? '添加子条件' : '添加条件'} onClose={() => setConditionMenu(null)}>
      <div className="sv-result-list et-condition-types">{conditionTemplates.map(template => <button key={template.id} onClick={() => addCondition(template.id, conditionMenu.owner, conditionMenu.parent)}><GitBranch size={13}/><span>{template.title}{template.valueType && <small>{template.valueType}</small>}</span></button>)}</div>
    </AnchoredPopover>}
  </div>;
}
