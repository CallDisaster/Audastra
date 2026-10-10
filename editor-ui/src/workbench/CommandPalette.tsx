import { useEffect, useMemo, useRef, useState } from 'react';
import type { LucideIcon } from 'lucide-react';
import { ChevronRight, CornerDownLeft, Search } from 'lucide-react';
import { rank } from './commandRank';

// 命令面板：一个输入框找到并执行一切。默认同时搜命令与工程内容；以 “>” 开头只搜命令。
// 命令来自与菜单/快捷键/Agent 相同的命令表（原生端 CommandRegistry），这里只是它的一个入口。

export interface PaletteCommand { id: string; title: string; group: string; shortcut?: string; icon?: LucideIcon; keywords?: string; disabled?: boolean; run: () => void }
export interface PaletteEntry { id: string; title: string; detail: string; icon?: LucideIcon; keywords?: string; run: () => void }
const recentKey = 'forge-palette-recent';
function readRecent(): string[] { try { const v = JSON.parse(localStorage.getItem(recentKey) ?? '[]'); return Array.isArray(v) ? v.filter(x => typeof x === 'string').slice(0, 12) : []; } catch { return []; } }

export function CommandPalette({ initial, commands, entries, onClose }: { initial: string; commands: PaletteCommand[]; entries: (query: string) => PaletteEntry[]; onClose: () => void }) {
  const dialog = useRef<HTMLDialogElement>(null), list = useRef<HTMLDivElement>(null);
  const [query, setQuery] = useState(initial), [active, setActive] = useState(0);
  const [recent] = useState(readRecent);
  useEffect(() => { dialog.current?.showModal(); }, []);
  const commandOnly = query.startsWith('>'), q = commandOnly ? query.slice(1) : query;
  const rows = useMemo(() => {
    const boost = (c: PaletteCommand) => { const i = recent.indexOf(c.id); return i < 0 ? 0 : 60 - i * 4; };
    const cmds = rank(commands.filter(c => !c.disabled), q, c => `${c.title} ${c.group} ${c.keywords ?? ''}`, boost, commandOnly ? 60 : q.trim() ? 8 : 6)
      .map(c => ({ key: `c:${c.id}`, section: '命令', title: c.title, detail: c.group, shortcut: c.shortcut, icon: c.icon, run: () => { try { localStorage.setItem(recentKey, JSON.stringify([c.id, ...recent.filter(x => x !== c.id)].slice(0, 12))); } catch { /* 界面偏好 */ } c.run(); } }));
    const found = commandOnly ? [] : rank(entries(q), q, e => `${e.title} ${e.detail} ${e.keywords ?? ''}`, () => 0, 40).map(e => ({ key: `e:${e.id}`, section: '工程内容', title: e.title, detail: e.detail, shortcut: undefined, icon: e.icon, run: e.run }));
    return [...cmds, ...found];
  }, [commands, entries, q, commandOnly, recent]);
  const index = Math.min(active, Math.max(0, rows.length - 1));
  useEffect(() => { list.current?.querySelector(`[data-index="${index}"]`)?.scrollIntoView({ block: 'nearest' }); }, [index]);
  const run = (i: number) => { const row = rows[i]; if (!row) return; onClose(); setTimeout(row.run, 0); };

  return <dialog ref={dialog} className="studio-dialog cp-dialog" aria-label="命令面板" onCancel={event => { event.preventDefault(); onClose(); }} onClick={event => { if (event.target === event.currentTarget) onClose(); }}>
    <div className="cp-input"><Search size={16} /><input autoFocus value={query} aria-label="搜索命令或工程内容" placeholder="搜索触发器、物体、变量、地图对象…  输入 > 只搜命令"
      onChange={event => { setQuery(event.target.value); setActive(0); }}
      onKeyDown={event => {
        if (event.key === 'ArrowDown') { event.preventDefault(); setActive(Math.min(rows.length - 1, index + 1)); }
        if (event.key === 'ArrowUp') { event.preventDefault(); setActive(Math.max(0, index - 1)); }
        if (event.key === 'Enter') { event.preventDefault(); run(index); }
      }} /><kbd>Esc</kbd></div>
    <div className="cp-list" ref={list} role="listbox" aria-label="结果">
      {rows.map((row, i) => <div key={row.key}>
        {(i === 0 || rows[i - 1].section !== row.section) && <div className="cp-section">{row.section}{row.section === '命令' && !q.trim() && !commandOnly && <small>最近使用</small>}</div>}
        <button data-index={i} role="option" aria-selected={i === index} className={`cp-row ${i === index ? 'is-active' : ''}`} onMouseMove={() => { if (i !== index) setActive(i); }} onClick={() => run(i)}>
          {row.icon ? <row.icon size={15} /> : <ChevronRight size={15} />}<span className="cp-title">{row.title}</span><small>{row.detail}</small>{row.shortcut && <kbd>{row.shortcut}</kbd>}{i === index && <CornerDownLeft size={13} className="cp-enter" />}
        </button>
      </div>)}
      {!rows.length && <div className="cp-empty">没有匹配结果</div>}
    </div>
    <footer className="cp-foot"><span><kbd>↑</kbd><kbd>↓</kbd> 选择</span><span><kbd>Enter</kbd> 执行</span><span><kbd>&gt;</kbd> 只搜命令</span><div className="wb-spacer" /><span>Ctrl K · Ctrl Shift P</span></footer>
  </dialog>;
}
