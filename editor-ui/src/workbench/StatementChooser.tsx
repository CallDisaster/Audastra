import { useMemo, useRef, useState } from 'react';
import type { ReactNode } from 'react';
import { Box, GitBranch, Hash, MessageSquareText, Search, X, Zap } from 'lucide-react';
import type { Definition } from '../studio/model';
import { categoryOf, sentenceOf } from './ecaModel';
import { keepEditorSave } from './parameterFields';

// 对应原版“设置动作/事件”窗口：类别筛选 + 搜索 + 语句预览与说明；键盘 ↑↓ 选择、Enter 添加、Esc 关闭。
const icons = { event: Zap, flow: GitBranch, action: Box, value: Hash };
const kindLabels = { event: '事件', flow: '流程', action: '动作', value: '数据' };
export function SentencePreview({ definition }: { definition: Definition }) {
  const { category, parts } = sentenceOf(definition);
  return <>{!definition.comment && <span className="sc-category">{category} - </span>}{parts.map((part, index) => 'text' in part ? <span key={index}>{part.text}</span> : <u key={index}>{'field' in part ? part.field.label : part.label}</u>)}</>;
}
export function StatementChooser({ title, definitions, position, onCreate, onClose, extra, footer }: {
  title: string; definitions: Definition[]; position: { left: number; top: number };
  onCreate: (definition: Definition) => void; onClose: () => void; extra?: ReactNode; footer?: ReactNode;
}) {
  const [query, setQuery] = useState('');
  const [category, setCategory] = useState('');
  const [highlight, setHighlight] = useState(0);
  const list = useRef<HTMLDivElement>(null);
  const categories = useMemo(() => {
    const counts = new Map<string, number>();
    definitions.forEach(definition => counts.set(categoryOf(definition), (counts.get(categoryOf(definition)) ?? 0) + 1));
    return [...counts.entries()];
  }, [definitions]);
  const words = query.trim().toLowerCase().split(/\s+/).filter(Boolean);
  const matches = definitions.filter(definition => {
    if (category && categoryOf(definition) !== category) return false;
    const haystack = `${definition.title} ${categoryOf(definition)} ${definition.id} ${typeof definition.sentence === 'string' ? definition.sentence : ''} ${definition.fields.map(field => field.label).join(' ')}`.toLowerCase();
    return words.every(word => haystack.includes(word));
  });
  const current = matches[Math.min(highlight, matches.length - 1)];
  const move = (delta: number) => {
    const next = Math.max(0, Math.min(matches.length - 1, highlight + delta));
    setHighlight(next);
    requestAnimationFrame(() => list.current?.querySelector<HTMLElement>(`[data-index="${next}"]`)?.scrollIntoView({ block: 'nearest' }));
  };
  return <><div className="tw-menu-dismiss" onPointerDown={onClose}/>
    <section className="tw-node-menu sc-chooser" role="dialog" aria-label={title} style={position} onKeyDown={keepEditorSave}>
      <header><span>{title}</span><button onClick={onClose} aria-label="关闭节点搜索"><X size={14}/></button></header>
      <label className="tw-search"><Search size={14}/><input autoFocus aria-label="搜索函数" placeholder="搜索名称、语句或类别" value={query} onChange={event => { setQuery(event.target.value); setHighlight(0); }} onKeyDown={event => {
        if (event.key === 'ArrowDown') { event.preventDefault(); move(1); }
        if (event.key === 'ArrowUp') { event.preventDefault(); move(-1); }
        if (event.key === 'Enter' && current) { event.preventDefault(); onCreate(current); }
        if (event.key === 'Escape') { event.preventDefault(); onClose(); }
      }}/><kbd>↵</kbd></label>
      {extra}
      <div className="sc-body">
        <nav className="sc-categories" aria-label="类别">
          <button className={!category ? 'is-active' : ''} onClick={() => { setCategory(''); setHighlight(0); }}><span>全部</span><small>{definitions.length}</small></button>
          {categories.map(([name, count]) => <button key={name} className={category === name ? 'is-active' : ''} onClick={() => { setCategory(name); setHighlight(0); }}><span>{name}</span><small>{count}</small></button>)}
        </nav>
        <div ref={list} className="sc-results" role="listbox" aria-label="可添加的函数">
          {matches.map((definition, index) => { const Icon = definition.comment ? MessageSquareText : icons[definition.kind]; return <button key={definition.id} data-index={index} role="option" aria-selected={definition === current} aria-label={definition.title} className={definition === current ? 'is-highlighted' : ''} onMouseEnter={() => setHighlight(index)} onClick={() => onCreate(definition)}>
            <Icon size={15} className={`sc-icon kind-${definition.kind}`}/><span><strong>{definition.title}</strong><small><SentencePreview definition={definition}/></small></span><em>{kindLabels[definition.kind]}</em>
          </button>; })}
          {!matches.length && <div className="tw-menu-empty">未找到函数</div>}
        </div>
      </div>
      {current && <div className="sc-preview" aria-live="polite"><p><SentencePreview definition={current}/></p>{typeof current.description === 'string' && <small>{current.description}</small>}<code>{current.id}</code></div>}
      {footer}
    </section>
  </>;
}
