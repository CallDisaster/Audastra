import { useMemo, useState } from 'react';
import { ArrowDown, ArrowUp, Check, RotateCcw, Search, X } from 'lucide-react';
import type { ObjectKind, WorkbenchProject } from './types';
import type { War3Field, War3Index, War3Value } from './war3Data';
import { iconUrl } from './war3Data';
import { displayValue, isFlagType, listObjects, objectLabel, referenceKindsOf, sameValue, splitValues, wc3Segments } from './objectModel';
import { keepEditorSave } from './parameterFields';

// 物编复杂字段的右侧抽屉：非模态，表格仍可滚动与切换；有序引用列表“已选 + 候选”同面板，不再层层弹窗。
export type DrawerTarget = { field: War3Field; level?: number };
const kindNames: Record<ObjectKind, string> = { unit: '单位', item: '物品', ability: '技能', buff: '魔法效果', upgrade: '科技', destructable: '可破坏物', doodad: '装饰物' };

export function Wc3Text({ value }: { value: string }) {
  return <>{wc3Segments(value).map((segment, index) => segment.newline ? <br key={index}/> : <span key={index} style={segment.color ? { color: segment.color } : undefined}>{segment.text}</span>)}</>;
}
function Portrait({ icon, size = 20 }: { icon: string; size?: number }) {
  return icon ? <img className="oe-icon" src={iconUrl(icon)} alt="" width={size} height={size} loading="lazy" draggable={false}/> : <span className="oe-icon oe-icon-empty" style={{ width: size, height: size }}/>;
}

function ReferenceEditor({ project, index, field, value, onCommit }: { project: WorkbenchProject; index: War3Index; field: War3Field; value: string; onCommit: (value: string) => void }) {
  const kinds = referenceKindsOf(field.type)!;
  const single = /Code$/.test(field.type);
  const [kind, setKind] = useState<ObjectKind>(kinds[0]);
  const [query, setQuery] = useState('');
  const [group, setGroup] = useState('');
  const selected = splitValues(value);
  // 英雄技能栏只接受英雄技能（与原版一致）；其他列表不限制分组。
  const heroOnly = field.type === 'heroAbilityList';
  const candidates = useMemo(() => listObjects(project, index, kind).filter(row => (!row.hidden || row.custom) && (!heroOnly || row.group.includes('英雄'))), [project, index, kind, heroOnly]);
  const groups = [...new Set(candidates.map(row => row.group[0]))];
  const words = query.trim().toLowerCase().split(/\s+/).filter(Boolean);
  const matches = candidates.filter(row => (!group || row.group[0] === group) && words.every(word => `${row.name} ${row.suffix} ${row.rawcode} ${row.group.join(' ')}`.toLowerCase().includes(word)));
  const shown = matches.slice(0, 250);
  const write = (list: string[]) => onCommit(list.join(','));
  const toggle = (code: string) => single ? onCommit(selected[0] === code ? '' : code) : write(selected.includes(code) ? selected.filter(item => item !== code) : [...selected, code]);
  const move = (at: number, delta: number) => { const next = selected.slice(); const [item] = next.splice(at, 1); next.splice(at + delta, 0, item); write(next); };
  return <div className="oe-ref">
    <section className="oe-ref-selected" aria-label="已选">
      <header>已选<small>{selected.length}{single ? ' / 1' : ''}</small></header>
      {selected.map((code, at) => { const label = objectLabel(project, index, kinds, code); return <div key={`${code}-${at}`} className={`oe-ref-item ${label.known ? '' : 'is-unknown'}`}>
        <Portrait icon={label.icon}/><span title={label.known ? label.name : '原版和地图中都没有这个 ID，保留原值'}>{label.name}</span><code>{code}</code>
        {!single && <><button aria-label={`上移 ${label.name}`} disabled={at === 0} onClick={() => move(at, -1)}><ArrowUp size={12}/></button><button aria-label={`下移 ${label.name}`} disabled={at === selected.length - 1} onClick={() => move(at, 1)}><ArrowDown size={12}/></button></>}
        <button aria-label={`移除 ${label.name}`} onClick={() => write(selected.filter((_, i) => i !== at))}><X size={12}/></button>
      </div>; })}
      {!selected.length && <div className="oe-empty">未选择</div>}
    </section>
    <section className="oe-ref-candidates" aria-label="候选">
      <div className="oe-ref-filters">
        {kinds.length > 1 && <div className="oe-segment">{kinds.map(item => <button key={item} className={kind === item ? 'is-active' : ''} onClick={() => { setKind(item); setGroup(''); }}>{kindNames[item]}</button>)}</div>}
        <label className="oe-search"><Search size={13}/><input autoFocus aria-label="搜索候选" placeholder="名称、rawcode 或分组" value={query} onChange={event => setQuery(event.target.value)} onKeyDown={event => { keepEditorSave(event); if (event.key === 'Enter' && shown[0]) toggle(shown[0].rawcode); }}/></label>
        <select aria-label="分组筛选" value={group} onChange={event => setGroup(event.target.value)}><option value="">全部分组</option>{groups.map(item => <option key={item}>{item}</option>)}</select>
      </div>
      <div className="oe-ref-list" role="listbox" aria-label={`候选${kindNames[kind]}`} aria-multiselectable={!single}>
        {shown.map(row => { const on = selected.includes(row.rawcode); return <button key={row.key} role="option" aria-selected={on} className={on ? 'is-selected' : ''} onClick={() => toggle(row.rawcode)} title={row.group.join(' / ')}>
          <Portrait icon={row.icon}/><span>{row.name}{row.suffix && <em>{row.suffix}</em>}</span><code>{row.rawcode}</code>{on && <Check size={12}/>}
        </button>; })}
        {matches.length > shown.length && <div className="oe-empty">还有 {matches.length - shown.length} 项，请输入更多关键字</div>}
        {!matches.length && <div className="oe-empty">没有匹配的{kindNames[kind]}</div>}
      </div>
    </section>
  </div>;
}

function FlagEditor({ index, field, value, onCommit }: { index: War3Index; field: War3Field; value: string; onCommit: (value: string) => void }) {
  const choices = index.enums[field.type] ?? [];
  const selected = splitValues(value).map(item => item.toLowerCase());
  const write = (next: string[]) => onCommit(choices.filter(choice => next.includes(choice.value.toLowerCase())).map(choice => choice.value).join(','));
  return <div className="oe-flags" role="group" aria-label={field.name}>{choices.map(choice => { const on = selected.includes(choice.value.toLowerCase()); return <label key={choice.value} className={on ? 'is-on' : ''}><input type="checkbox" checked={on} onChange={() => {
    if (choice.value === '*') { write(on ? [] : ['*']); return; }
    write(on ? selected.filter(item => item !== choice.value.toLowerCase()) : [...selected.filter(item => item !== '*'), choice.value.toLowerCase()]);
  }}/><span>{choice.label}</span><code>{choice.value}</code></label>; })}</div>;
}

function TextEditor({ label, value, onCommit, resolve }: { label: string; value: string; onCommit: (value: string) => void; resolve?: (text: string) => string }) {
  const [draft, setDraft] = useState(value);
  const dirty = draft !== value;
  return <div className="oe-text">
    <textarea aria-label={label} spellCheck={false} value={draft} onChange={event => setDraft(event.target.value)} onKeyDown={event => { keepEditorSave(event); if (event.key === 'Enter' && (event.ctrlKey || event.metaKey)) { event.preventDefault(); onCommit(draft); } if (event.key === 'Escape' && dirty) { event.preventDefault(); event.stopPropagation(); setDraft(value); } }}/>
    <div className="oe-text-actions"><small>Ctrl+Enter 应用 · Esc 放弃修改</small><button disabled={!dirty} onClick={() => setDraft(value)}>放弃</button><button className="oe-primary" disabled={!dirty} onClick={() => onCommit(draft)}>应用</button></div>
    <div className="oe-preview" aria-label="显示预览"><Wc3Text value={resolve ? resolve(draft) : draft}/></div>{resolve && /<[A-Za-z0-9]{4},/.test(draft) && <small className="oe-preview-note">预览中的 &lt;ID,数据&gt; 已替换为对应等级的数值</small>}
  </div>;
}

export function ObjectDrawer({ project, index, target, value, baseValue, levels, modified, onCommit, onReset, onClose, resolveText }: {
  project: WorkbenchProject; index: War3Index; target: DrawerTarget; value: War3Value | undefined; baseValue: War3Value | undefined; levels: number;
  modified: boolean; onCommit: (value: War3Value) => void; onReset: () => void; onClose: () => void; resolveText?: (text: string) => string;
}) {
  const { field } = target;
  const perLevel = field.repeat > 0;
  const [level, setLevel] = useState(Math.min(target.level ?? 0, levels - 1));
  const list = perLevel ? Array.from({ length: levels }, (_, i) => (Array.isArray(value) ? value[Math.min(i, value.length - 1)] : value) ?? '') : [];
  const current = perLevel ? list[level] : value;
  const write = (next: string) => onCommit(perLevel ? list.map((item, i) => i === level ? next : item) : next);
  const body = referenceKindsOf(field.type) ? <ReferenceEditor key={`${field.id}-${level}`} project={project} index={index} field={field} value={String(current ?? '')} onCommit={write}/>
    : isFlagType(index, field.type) || field.type === 'tilesetList' ? <FlagEditor index={index} field={field} value={String(current ?? '')} onCommit={write}/>
    : <TextEditor key={`${field.id}-${level}`} label={field.name} value={String(current ?? '')} onCommit={write} resolve={resolveText}/>;
  return <aside className="oe-drawer" aria-label={`${field.name} 编辑`} onKeyDown={event => { if (event.key === 'Escape' && !event.defaultPrevented) { event.preventDefault(); onClose(); } }}>
    <header><div><strong>{index.categories[field.category] ? `${index.categories[field.category]} - ` : ''}{field.name}</strong><code>{field.id} · {field.type}</code></div><button aria-label="关闭字段编辑" onClick={onClose}><X size={15}/></button></header>
    {perLevel && levels > 1 && <div className="oe-levels" role="tablist" aria-label="等级">{list.map((item, i) => <button key={i} role="tab" aria-selected={i === level} className={`${i === level ? 'is-active' : ''} ${!sameValue(item, Array.isArray(baseValue) ? baseValue[i] : baseValue) ? 'is-modified' : ''}`} onClick={() => setLevel(i)}>等级 {i + 1}</button>)}</div>}
    <div className="oe-drawer-body">{body}</div>
    <footer><span title="基础物体的值">基础值：{displayValue(project, index, field, Array.isArray(baseValue) ? baseValue[perLevel ? level : 0] : baseValue) || '（空）'}</span>{modified && <button onClick={onReset}><RotateCcw size={12}/>恢复基础值</button>}</footer>
  </aside>;
}
