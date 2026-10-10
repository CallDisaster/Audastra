import { useMemo, useState } from 'react';
import { ArrowUpRight, ChevronDown, ChevronRight, Minus, Pencil, Plus, RotateCcw, Save, Sparkles } from 'lucide-react';
import type { ModuleId, WorkbenchProject } from './types';
import type { ChangeGroup, ChangeItem } from './projectDiff';
import { changeCount, revertChange } from './projectDiff';

// 本次修改：保存前逐项核对。每一项可以跳到原位置或单独撤销（撤销本身也进入撤销历史）。
// 助手应用的改动带“助手”标记，便于作者核对自动修改。

const kindMeta = { added: { label: '新增', icon: Plus }, removed: { label: '删除', icon: Minus }, modified: { label: '修改', icon: Pencil } };

export function ChangeList({ groups, onOpen, onRevert, attribution, compact = false }: { groups: ChangeGroup[]; onOpen?: (item: ChangeItem) => void; onRevert?: (item: ChangeItem) => void; attribution?: Record<string, string>; compact?: boolean }) {
  const [closed, setClosed] = useState<Set<string>>(new Set());
  return <div className={`cg-list ${compact ? 'is-compact' : ''}`}>{groups.map(group => <section key={group.id} className="cg-group">
    <button className="cg-group-head" aria-expanded={!closed.has(group.id)} onClick={() => setClosed(set => { const next = new Set(set); if (next.has(group.id)) next.delete(group.id); else next.add(group.id); return next; })}>{closed.has(group.id) ? <ChevronRight size={13} /> : <ChevronDown size={13} />}<span>{group.label}</span><small>{group.items.length}</small></button>
    {!closed.has(group.id) && group.items.map(item => { const Icon = kindMeta[item.kind].icon; return <article key={item.key} className={`cg-item is-${item.kind}`}>
      <header><span className="cg-kind" title={kindMeta[item.kind].label}><Icon size={11} /></span><strong>{item.title}</strong>{attribution?.[item.key] && <span className="cg-by"><Sparkles size={10} />{attribution[item.key]}</span>}
        <div className="wb-spacer" />
        {onOpen && item.kind !== 'removed' && item.target !== undefined && <button title="转到" aria-label={`转到 ${item.title}`} onClick={() => onOpen(item)}><ArrowUpRight size={13} /></button>}
        {onRevert && <button title="撤销这一项" aria-label={`撤销 ${item.title}`} onClick={() => onRevert(item)}><RotateCcw size={13} /></button>}
      </header>
      {item.summary && <p className="cg-summary">{item.summary}</p>}
      {item.fields.length > 0 && <dl>{item.fields.slice(0, compact ? 4 : 12).map(field => <div key={field.path}><dt title={field.path}>{field.label}</dt><dd><del>{field.before}</del><span>→</span><ins>{field.after}</ins></dd></div>)}{item.fields.length > (compact ? 4 : 12) && <div className="cg-more">另有 {item.fields.length - (compact ? 4 : 12)} 个字段</div>}</dl>}
    </article>; })}
  </section>)}</div>;
}

export function ChangesPanel({ project, baseline, groups, attribution, onChange, onNavigate, onSave, onRevertAll }: { project: WorkbenchProject; baseline: WorkbenchProject | undefined; groups: ChangeGroup[]; attribution: Record<string, string>; onChange: (next: WorkbenchProject, label: string) => void; onNavigate: (module: ModuleId, id?: string) => void; onSave: () => void; onRevertAll: () => void }) {
  const total = changeCount(groups);
  const stats = useMemo(() => groups.flatMap(g => g.items).reduce((acc, item) => ({ ...acc, [item.kind]: acc[item.kind] + 1 }), { added: 0, removed: 0, modified: 0 }), [groups]);
  if (!baseline) return <div className="tp-empty">无法读取已保存的基线，暂时不能列出修改。<button className="wb-primary" onClick={onSave}><Save size={14} />保存</button></div>;
  return <>
    <div className="tp-summary">{total ? <><strong>{total} 项未保存修改</strong><span className="cg-stat is-added">新增 {stats.added}</span><span className="cg-stat is-modified">修改 {stats.modified}</span><span className="cg-stat is-removed">删除 {stats.removed}</span></> : <strong>没有未保存的修改</strong>}</div>
    <div className="tp-scroll">{total ? <ChangeList groups={groups} attribution={attribution} onOpen={item => onNavigate(item.module, item.target)} onRevert={item => onChange(revertChange(project, baseline, item), `撤销修改：${item.title}`)} /> : <div className="tp-empty">当前工程与上次保存一致。</div>}</div>
    <footer className="tp-foot"><button disabled={!total} onClick={onRevertAll}><RotateCcw size={14} />全部撤销</button><div className="wb-spacer" /><button className="wb-primary" disabled={!total} onClick={onSave}><Save size={14} />保存 {total || ''} 项</button></footer>
    <p className="tp-note">原生端在这里按地图成员列出将写出的文件（w3u、wtg、w3r…）与脚本重建结果；门禁拒绝的项目会标出原因，不会静默跳过。</p>
  </>;
}
