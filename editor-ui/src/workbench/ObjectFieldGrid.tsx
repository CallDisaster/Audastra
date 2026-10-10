import { useRef, useState } from 'react';
import type { KeyboardEvent, ReactNode } from 'react';
import { ChevronDown, ChevronRight, RotateCcw } from 'lucide-react';
import type { WorkbenchProject } from './types';
import type { War3Field, War3Index, War3Value } from './war3Data';
import { iconUrl } from './war3Data';
import { displayValue, fieldCategoryOrder, isFlagType, isNumberType, needsDrawer, referenceKindsOf, sameValue } from './objectModel';
import type { DrawerTarget } from './ObjectDrawer';
import { Wc3Text } from './ObjectDrawer';
import { keepEditorSave } from './parameterFields';

// 高密度字段表：一行一个字段；标量就地编辑，等级字段同一行按等级并排，复杂值交给右侧抽屉。
const maxInlineLevels = 6;
const iconName = (path: string) => path ? path.replaceAll('/', '\\').toLowerCase().replace(/\.[a-z]+$/, '').replace(/[^a-z0-9]+/g, '_') + '.png' : '';
function validate(field: War3Field, draft: string): string | undefined {
  if (!isNumberType(field.type)) return field.type === 'char' && draft.length > 1 ? '只能输入一个字符' : undefined;
  if (!draft.trim() || !Number.isFinite(Number(draft))) return '请输入数字';
  const number = Number(draft);
  if (field.type === 'int' && !Number.isInteger(number)) return '请输入整数';
  if (field.type === 'unreal' && number < 0) return '不能小于 0';
  if (typeof field.min === 'number' && number < field.min) return `不能小于 ${field.min}`;
  if (typeof field.max === 'number' && number > field.max) return `不能大于 ${field.max}`;
  return undefined;
}
function ScalarCell({ field, value, display, modified, label, onCommit }: { field: War3Field; value: string | number; display: string; modified: boolean; label: string; onCommit: (value: string | number) => void }) {
  const [draft, setDraft] = useState<string | null>(null);
  // 键盘提交后焦点移回表格会触发 blur，用标记避免同一次编辑提交两次。
  const settled = useRef(false);
  const error = draft === null ? undefined : validate(field, draft);
  const finish = () => { if (draft === null || error || settled.current) return; settled.current = true; setDraft(null); const next = isNumberType(field.type) ? Number(draft) : draft; if (String(next) !== String(value)) onCommit(next); };
  // 提交或放弃后把焦点还给表格，方向键可以直接继续到下一行。
  const keys = (event: KeyboardEvent<HTMLInputElement>) => {
    keepEditorSave(event);
    const grid = event.currentTarget.closest<HTMLElement>('.oe-grid');
    if (event.key === 'Enter') { event.preventDefault(); if (error) return; finish(); grid?.focus(); }
    if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); settled.current = true; setDraft(null); grid?.focus(); }
  };
  if (draft !== null) return <input autoFocus className={`oe-input ${error ? 'is-invalid' : ''} ${isNumberType(field.type) ? 'is-number' : ''}`} aria-label={label} aria-invalid={!!error || undefined} title={error} value={draft} spellCheck={false} onChange={event => setDraft(event.target.value)} onBlur={() => { if (error) setDraft(null); else finish(); }} onKeyDown={keys}/>;
  return <button type="button" className={`oe-cell ${modified ? 'is-modified' : ''} ${isNumberType(field.type) ? 'is-number' : ''}`} aria-label={`${label}：${display || '空'}`} data-edit onClick={() => { settled.current = false; setDraft(String(value ?? '')); }}>{display || <span className="oe-blank">—</span>}</button>;
}

type RowProps = { project: WorkbenchProject; index: War3Index; field: War3Field; value: War3Value | undefined; base: War3Value | undefined; modified: boolean; levels: number; raw: boolean; active: boolean; drawer: boolean; onSet: (value: War3Value) => void; onReset: () => void; onDrawer: (target: DrawerTarget) => void; onActivate: () => void };
function FieldRow({ project, index, field, value, base, modified, levels, raw, active, drawer, onSet, onReset, onDrawer, onActivate }: RowProps) {
  const perLevel = field.repeat > 0;
  const list = perLevel ? Array.from({ length: levels }, (_, i) => (Array.isArray(value) ? value[Math.min(i, value.length - 1)] : value) ?? '') : [];
  const baseList = perLevel ? Array.from({ length: levels }, (_, i) => (Array.isArray(base) ? base[Math.min(i, base.length - 1)] : base) ?? '') : [];
  const show = (item: War3Value | undefined) => raw ? String(item ?? '') : displayValue(project, index, field, item);
  const complex = needsDrawer(index, field, value);
  const choices = index.enums[field.type];
  const single = (item: string | number, commit: (next: string | number) => void, label: string, cellModified: boolean): ReactNode => {
    if (field.type === 'bool') return <label className={`oe-check ${cellModified ? 'is-modified' : ''}`}><input type="checkbox" data-edit aria-label={label} checked={Number(item) === 1} onChange={event => commit(event.target.checked ? 1 : 0)}/><span>{Number(item) === 1 ? '是' : '否'}</span></label>;
    if (choices && !isFlagType(index, field.type)) return <select className={`oe-select ${cellModified ? 'is-modified' : ''}`} data-edit aria-label={label} value={choices.some(choice => choice.value === String(item)) ? String(item) : ''} onChange={event => { const grid = event.currentTarget.closest<HTMLElement>('.oe-grid'); commit(event.target.value); grid?.focus(); }} onKeyDown={event => { keepEditorSave(event); if (event.key === 'Escape') { event.preventDefault(); event.currentTarget.closest<HTMLElement>('.oe-grid')?.focus(); } }}>{!choices.some(choice => choice.value === String(item)) && <option value="">{String(item) || '（空）'}</option>}{choices.map(choice => <option key={choice.value} value={choice.value}>{raw ? choice.value : choice.label}</option>)}</select>;
    return <ScalarCell field={field} value={item} display={show(item)} modified={cellModified} label={label} onCommit={commit}/>;
  };
  let content: ReactNode;
  if (complex && perLevel) content = <div className="oe-levels-inline">{list.slice(0, maxInlineLevels).map((item, i) => <button key={i} className={`oe-cell oe-level-cell ${!sameValue(item, baseList[i]) ? 'is-modified' : ''}`} data-edit={i === 0 || undefined} aria-label={`${field.name} 等级 ${i + 1}`} onClick={() => onDrawer({ field, level: i })}><i>{i + 1}</i>{field.type === 'string' && !raw ? <Wc3Text value={String(item)}/> : show(item) || <span className="oe-blank">—</span>}</button>)}{levels > maxInlineLevels && <button className="oe-more" onClick={() => onDrawer({ field, level: maxInlineLevels })}>+{levels - maxInlineLevels}</button>}</div>;
  else if (complex) {
    const kinds = referenceKindsOf(field.type);
    const icons = kinds && !raw ? String(value ?? '').split(',').filter(Boolean).slice(0, 6).map(code => code.trim()) : [];
    content = <button className={`oe-cell oe-complex ${modified ? 'is-modified' : ''}`} data-edit aria-label={`${field.name}：${show(value) || '空'}，打开编辑`} onClick={() => onDrawer({ field })}>
      {icons.length > 0 && <span className="oe-mini-icons">{icons.map(code => { const standard = kinds!.map(kind => index.kinds[kind].objects.find(object => object.id === code)).find(Boolean); return standard?.icon ? <img key={code} src={iconUrl(standard.icon)} alt="" loading="lazy"/> : null; })}</span>}
      <span className="oe-complex-text">{field.type === 'string' && !raw ? <Wc3Text value={String(value ?? '')}/> : show(value) || <span className="oe-blank">—</span>}</span><ChevronRight size={12}/>
    </button>;
  } else if (perLevel) content = <div className="oe-levels-inline">{list.slice(0, maxInlineLevels).map((item, i) => <div key={i} className="oe-level-slot"><i>{i + 1}</i>{single(item, next => onSet(list.map((old, j) => j === i ? next : old)), `${field.name} 等级 ${i + 1}`, !sameValue(item, baseList[i]))}</div>)}{levels > maxInlineLevels && <button className="oe-more" onClick={() => onDrawer({ field, level: maxInlineLevels })}>+{levels - maxInlineLevels}</button>}</div>;
  else content = <div className="oe-value-line">{field.type === 'icon' && typeof value === 'string' && value && <img className="oe-inline-icon" src={iconUrl(iconName(value))} alt="" onError={event => { event.currentTarget.style.display = 'none'; }}/>}{single(value as string | number ?? '', next => onSet(next), field.name, modified)}</div>;
  return <div className={`oe-row ${modified ? 'is-modified' : ''} ${active ? 'is-active' : ''} ${drawer ? 'is-drawer' : ''}`} role="row" data-field-id={field.id} onPointerDown={onActivate}>
    {raw && <code className="oe-raw" role="cell">{field.id}</code>}
    <span className="oe-name" role="rowheader" title={`${field.name}（${field.id} · ${field.type}）`}>{field.name}{perLevel && <small>按等级</small>}</span>
    <div className="oe-value" role="cell">{content}</div>
    <span className="oe-actions" role="cell">{modified && <button aria-label={`恢复 ${field.name} 的基础值`} title="恢复基础值" onClick={onReset}><RotateCcw size={12}/></button>}</span>
  </div>;
}

export function ObjectFieldGrid({ project, index, fields, effective, overrides, base, levels, raw, drawerField, onSet, onReset, onDrawer, emptyText }: {
  project: WorkbenchProject; index: War3Index; fields: War3Field[]; effective: Record<string, War3Value>; overrides: Record<string, War3Value>; base: Record<string, War3Value>;
  levels: number; raw: boolean; drawerField?: string; onSet: (field: War3Field, value: War3Value) => void; onReset: (field: War3Field) => void; onDrawer: (target: DrawerTarget) => void; emptyText: string;
}) {
  const [collapsed, setCollapsed] = useState<string[]>([]);
  const [active, setActive] = useState<string>();
  const grid = useRef<HTMLDivElement>(null);
  const categories = [...new Set(fields.map(field => field.category))].sort((a, b) => (fieldCategoryOrder.indexOf(a) + 99) % 99 - (fieldCategoryOrder.indexOf(b) + 99) % 99);
  const visible = categories.flatMap(category => collapsed.includes(category) ? [] : fields.filter(field => field.category === category));
  const focusRow = (id: string) => { setActive(id); requestAnimationFrame(() => grid.current?.querySelector(`[data-field-id="${id}"]`)?.scrollIntoView({ block: 'nearest' })); };
  const onKey = (event: KeyboardEvent<HTMLDivElement>) => {
    // 勾选框聚焦时仍允许方向键移动行；输入框、下拉框、文本框内的按键留给控件本身。
    const target = event.target as HTMLElement;
    if (target.closest('select,textarea') || (target.closest('input') && (target as HTMLInputElement).type !== 'checkbox')) return;
    const at = visible.findIndex(field => field.id === active);
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); const next = visible[Math.max(0, Math.min(visible.length - 1, at + (event.key === 'ArrowDown' ? 1 : -1)))]; if (next) focusRow(next.id); if (target !== grid.current) grid.current?.focus(); }
    if (event.key === 'Enter' && active) { event.preventDefault(); grid.current?.querySelector<HTMLElement>(`[data-field-id="${active}"] [data-edit]`)?.click(); grid.current?.querySelector<HTMLElement>(`[data-field-id="${active}"] select[data-edit]`)?.focus(); }
    if ((event.key === 'Delete' || event.key === 'Backspace') && active && active in overrides) { event.preventDefault(); const field = fields.find(item => item.id === active); if (field) onReset(field); }
  };
  if (!fields.length) return <div className="oe-grid-empty">{emptyText}</div>;
  return <div ref={grid} className={`oe-grid ${raw ? 'show-raw' : ''}`} role="grid" aria-label="字段" tabIndex={0} onKeyDown={onKey}>
    {categories.map(category => { const list = fields.filter(field => field.category === category); const closed = collapsed.includes(category); const changed = list.filter(field => field.id in overrides).length; return <section key={category} role="rowgroup" aria-label={index.categories[category] ?? category}>
      <button className="oe-category" aria-expanded={!closed} onClick={() => setCollapsed(items => closed ? items.filter(item => item !== category) : [...items, category])}>{closed ? <ChevronRight size={13}/> : <ChevronDown size={13}/>}<strong>{index.categories[category] ?? category}</strong><small>{list.length}</small>{changed > 0 && <em>{changed} 项已修改</em>}</button>
      {!closed && list.map(field => <FieldRow key={field.id} project={project} index={index} field={field} value={effective[field.id]} base={base[field.id]} modified={field.id in overrides} levels={levels} raw={raw} active={active === field.id} drawer={drawerField === field.id} onActivate={() => setActive(field.id)} onSet={value => onSet(field, value)} onReset={() => onReset(field)} onDrawer={onDrawer}/>)}
    </section>; })}
  </div>;
}
