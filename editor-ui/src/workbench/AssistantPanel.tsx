import { useEffect, useRef, useState } from 'react';
import { ArrowUpRight, Check, CornerDownLeft, Sparkles, Terminal, X } from 'lucide-react';
import type { ModuleId, WorkbenchProject } from './types';
import type { AssistantResult } from './assistantIntents';
import { assistantExamples, interpret } from './assistantIntents';
import { diffProjects } from './projectDiff';
import type { ChangeGroup } from './projectDiff';
import { ChangeList } from './ChangesPanel';
import { select, useWorld } from './worldStore';

// 助手面板：自然语言 → 编辑命令 → 提议（带差异预览）→ 作者确认后作为一次撤销步骤应用。
// 提议绑定生成时的工程修订；工程随后变化则提议过期，必须重新生成，不能套用到别的数据上。

type Message = { id: number; role: 'user' | 'assistant'; text: string; result?: AssistantResult; revision: string; diff?: ChangeGroup[]; status?: 'pending' | 'applied' | 'discarded' };

export function AssistantPanel({ project, revision, onApply, onNavigate }: { project: WorkbenchProject; revision: string; onApply: (next: WorkbenchProject, label: string, diff: ChangeGroup[]) => void; onNavigate: (module: ModuleId, id?: string) => void }) {
  const world = useWorld();
  const [messages, setMessages] = useState<Message[]>([{ id: 0, role: 'assistant', text: '我可以查询引用、批量选择、修改放置实例和地图物体字段。所有修改都会先给出差异，由你确认后才写入。', revision }]);
  const [draft, setDraft] = useState('');
  const scroll = useRef<HTMLDivElement>(null), input = useRef<HTMLTextAreaElement>(null);
  const nextId = useRef(1);
  useEffect(() => { scroll.current?.scrollTo({ top: scroll.current.scrollHeight }); }, [messages]);

  function send(text = draft) {
    const trimmed = text.trim(); if (!trimmed) return;
    const result = interpret(trimmed, { project, selection: world.selection, revision, owner: world.owner });
    if (result.select) select(result.select);
    const diff = result.proposal ? diffProjects(project, result.proposal.project) : undefined;
    setMessages(list => [...list, { id: nextId.current++, role: 'user', text: trimmed, revision }, { id: nextId.current++, role: 'assistant', text: result.reply, result, revision, diff, status: result.proposal ? 'pending' : undefined }]);
    setDraft('');
  }
  const setStatus = (id: number, status: Message['status']) => setMessages(list => list.map(m => m.id === id ? { ...m, status } : m));

  return <>
    <div className="tp-scroll as-thread" ref={scroll}>
      {messages.map(message => <div key={message.id} className={`as-msg is-${message.role}`}>
        {message.role === 'assistant' && <span className="as-avatar"><Sparkles size={12} /></span>}
        <div className="as-bubble">
          <p>{message.text}</p>
          {message.result?.results && <div className="as-results">{message.result.results.map((r, i) => <button key={i} onClick={() => r.target?.startsWith('example:') ? (setDraft(r.target.slice(8)), input.current?.focus()) : onNavigate(r.module, r.target)}><span>{r.title}</span><small>{r.detail}</small>{!r.target?.startsWith('example:') && <ArrowUpRight size={12} />}</button>)}</div>}
          {message.diff && <div className={`as-proposal is-${message.status}`}>
            <ChangeList groups={message.diff} compact />
            {message.status === 'pending' && (message.revision === revision
              ? <div className="as-actions"><button className="wb-primary" onClick={() => { onApply(message.result!.proposal!.project, message.result!.proposal!.label, message.diff!); setStatus(message.id, 'applied'); }}><Check size={13} />应用</button><button onClick={() => setStatus(message.id, 'discarded')}><X size={13} />放弃</button></div>
              : <div className="as-stale">工程在提议之后已经变化，这个提议已过期。请重新发送指令。</div>)}
            {message.status === 'applied' && <div className="as-done"><Check size={12} />已应用，可在“修改”面板核对或撤销</div>}
            {message.status === 'discarded' && <div className="as-done">已放弃</div>}
          </div>}
          {!!message.result?.calls.length && <details className="as-calls"><summary><Terminal size={11} />命令</summary>{message.result.calls.map((c, i) => <code key={i}>{c}</code>)}</details>}
        </div>
      </div>)}
    </div>
    <div className="as-suggest">{assistantExamples.slice(0, 4).map(example => <button key={example} onClick={() => send(example)}>{example}</button>)}</div>
    <footer className="as-input">
      <textarea ref={input} rows={2} value={draft} aria-label="给助手的指令" placeholder={world.selection.length ? `已选 ${world.selection.length} 个对象 · 例如：把选中的单位改为玩家 2` : '例如：在 防守区域 放置 4 个 守卫步兵'} onChange={event => setDraft(event.target.value)} onKeyDown={event => { if (event.key === 'Enter' && !event.shiftKey) { event.preventDefault(); send(); } }} />
      <button className="wb-primary" aria-label="发送" disabled={!draft.trim()} onClick={() => send()}><CornerDownLeft size={14} /></button>
    </footer>
    <p className="tp-note as-note">原型：本地句式解析，未接语言模型。原生端由 Agent 通过 MCP 调用同一批命令，提议同样要经过这里确认。</p>
  </>;
}
