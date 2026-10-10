import { useId, useRef, useState } from 'react';
import type { KeyboardEvent, PointerEvent } from 'react';
import { Braces, Check, ChevronDown, Hash, Link2, SlidersHorizontal } from 'lucide-react';
import type { Definition, Graph, GraphNode } from '../studio/model';
import type { ModuleProps, VariableRecord, WorkbenchProject } from './types';
import { AnchoredPopover } from './AnchoredPopover';
import { enumChoices, fieldReference, keepEditorSave } from './parameterFields';
import { bindingOf } from './triggerCode';
import type { ParameterBinding } from './triggerCode';
import { ObjectPortrait } from './ObjectPortrait';
import { findObject } from './objectCatalog';
import { SharedValuePicker } from './SharedValuePicker';
import { bindingLabel, expectedFieldType } from './valueSources';
import './nodeParameter.css';

type Field = Definition['fields'][number];

export interface NodeParameterFieldProps {
  node: GraphNode;
  field: Field;
  graph: Graph;
  project: WorkbenchProject;
  onCommit: (value: unknown, binding?: ParameterBinding, created?: VariableRecord[]) => void;
  onNavigate: ModuleProps['onNavigate'];
  open: boolean;
  onOpenChange: (open: boolean) => void;
  disabled?: boolean;
}

function constantValue(node: GraphNode, field: Field, project: WorkbenchProject): unknown {
  const variable = node.definition === 'core.number' && field.id === 'value' && project.variables.find(v => v.name === node.values.name || v.id === node.values.name);
  if (variable) return Number.isFinite(Number(variable.initial)) ? Number(variable.initial) : variable.initial;
  return node.values[field.id] ?? field.default;
}
function InlineInput({ value, type, label, disabled, onCommit, choices = [] }: {
  value: unknown; type: 'text' | 'number'; label: string; disabled?: boolean;
  onCommit: (value: unknown) => void; choices?: { label: string; value: string }[];
}) {
  const input = useRef<HTMLInputElement>(null);
  const editing = useRef(false);
  const [isEditing, setIsEditing] = useState(false);
  const [draft, setDraft] = useState(String(value ?? ''));
  const [invalid, setInvalid] = useState(false);
  const listId = useId();
  const finish = () => {
    if (!editing.current) return;
    if (type === 'number' && (!draft.trim() || !Number.isFinite(Number(draft)))) { setInvalid(true); return; }
    editing.current = false;
    setIsEditing(false);
    setInvalid(false);
    const next = type === 'number' ? Number(draft) : draft;
    if (next !== value) onCommit(next);
  };
  const keys = (event: KeyboardEvent<HTMLInputElement>) => {
    keepEditorSave(event);
    if (event.key === 'Enter') { event.preventDefault(); finish(); if (type !== 'number' || (draft.trim() && Number.isFinite(Number(draft)))) input.current?.blur(); }
    if (event.key === 'Escape') { event.preventDefault(); editing.current = false; setIsEditing(false); setInvalid(false); input.current?.blur(); }
  };
  return <><input ref={input} className={`np-inline-input ${type === 'number' ? 'np-number' : ''} ${invalid ? 'np-invalid' : ''}`} type="text" inputMode={type === 'number' ? 'decimal' : undefined} aria-label={label} aria-invalid={invalid || undefined} title={invalid ? '请输入有效数字' : String(value ?? '')} disabled={disabled} spellCheck={false} value={isEditing ? draft : String(value ?? '')} list={choices.length ? listId : undefined} onFocus={() => { editing.current = true; setIsEditing(true); setDraft(String(value ?? '')); setInvalid(false); }} onChange={event => { setDraft(event.target.value); setInvalid(false); }} onBlur={finish} onKeyDown={keys}/>{choices.length > 0 && <datalist id={listId}>{choices.map(choice => <option value={choice.value} key={choice.value}>{choice.label}</option>)}</datalist>}</>;
}

function ParameterPicker({ node, field, project, anchor, onCommit, onClose, onNavigate }: {
  node: GraphNode; field: Field; project: WorkbenchProject;
  anchor: React.RefObject<HTMLDivElement | null>;
  onCommit: NodeParameterFieldProps['onCommit']; onClose: () => void; onNavigate: ModuleProps['onNavigate'];
}) {
  const reference = fieldReference(field, node.definition);
  return <AnchoredPopover anchor={() => (anchor.current?.closest('.tw-node') as HTMLElement | null) ?? anchor.current} placement="right" label={`${field.label}选择器`} title={field.label} onClose={onClose}><SharedValuePicker project={project} expectedType={expectedFieldType(field, node.definition)} objectKind={reference.objectKind} value={constantValue(node, field, project)} binding={bindingOf(node, field.id)} canCreateVariable onSelect={(next, nextBinding, created) => { onCommit(next, nextBinding, created); onClose(); }} onNavigate={onNavigate} onClose={onClose}/></AnchoredPopover>;
}
export function NodeParameterField({ node, field, graph, project, onCommit, onNavigate, open, onOpenChange, disabled = false }: NodeParameterFieldProps) {
  const anchor = useRef<HTMLDivElement>(null);
  const binding = bindingOf(node, field.id);
  const reference = fieldReference(field, node.definition);
  const value = constantValue(node, field, project);
  const edge = graph.edges.find(item => item.to === node.id && item.input === field.id);
  const sourceNode = edge && graph.nodes.find(item => item.id === edge.from);
  const sourceName = project.definitions.find(item => item.id === sourceNode?.definition)?.title ?? sourceNode?.definition ?? '';
  const selectedObject = reference.kind === 'object' ? findObject(project, value, reference.objectKind) : undefined;
  const display = selectedObject?.name ?? project.world.regions.find(region => region.id === value)?.name ?? project.sounds.find(sound => sound.id === value)?.name ?? String(value ?? '');
  const boundDisplay = bindingLabel(binding, project);
  const openPicker = () => { if (disabled || edge) return; onOpenChange(!open); };
  const stopPointer = (event: PointerEvent<HTMLDivElement>) => event.stopPropagation();
  return <div ref={anchor} className={`np-field ${edge ? 'is-connected' : ''} ${disabled ? 'is-readonly' : ''}`} data-node-parameter={field.id} onPointerDown={stopPointer} onPointerUp={stopPointer} onClick={event => event.stopPropagation()} onDoubleClick={event => event.stopPropagation()} onKeyDown={keepEditorSave} onWheel={event => event.stopPropagation()}>
    <span className="np-label" title={field.label}>{field.label}</span>
    {edge ? <span className="np-connected" title={`已连接：${sourceName}`}><Link2 size={11}/>已连接</span> : binding && binding.mode !== 'constant' ? <button className="np-value np-binding" aria-label={`选择${field.label}`} disabled={disabled} onClick={() => openPicker()} title={boundDisplay}>{binding.mode === 'variable' ? <Hash size={11}/> : <Braces size={11}/>}<span>{boundDisplay}</span></button> : reference.kind ? <button className="np-value np-reference" aria-label={`选择${field.label}`} disabled={disabled} onClick={() => openPicker()} title={display}>{selectedObject && <ObjectPortrait object={selectedObject} project={project} size={20}/>}<span>{display}</span><ChevronDown size={10}/></button> : field.type === 'boolean' ? <button className={`np-boolean ${value ? 'is-checked' : ''}`} role="switch" aria-checked={!!value} aria-label={field.label} disabled={disabled} onClick={() => onCommit(!value)}><i>{!!value && <Check size={10}/>}</i><span>{value ? '真' : '假'}</span></button> : <InlineInput value={value} type={field.type} label={field.label} disabled={disabled} onCommit={next => onCommit(next)} choices={enumChoices(field)}/>}
    <button className={`np-source-button ${open ? 'is-active' : ''}`} aria-label={`${field.label}参数来源`} title="全局变量 / 函数 / 固定值" disabled={disabled || !!edge} aria-expanded={open && !edge && !disabled} onClick={() => openPicker()}><SlidersHorizontal size={11}/></button>
    {open && !edge && !disabled && <ParameterPicker node={node} field={field} project={project} anchor={anchor} onCommit={onCommit} onClose={() => onOpenChange(false)} onNavigate={onNavigate}/>}
  </div>;
}
