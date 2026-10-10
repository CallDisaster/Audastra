import { useRef, useState } from 'react';
import type { ReactNode } from 'react';
import { GitCompareArrows, Play, Sparkles, X } from 'lucide-react';
import './toolPanels.css';

// 右侧工具栏位：修改 / 测试 / 助手。与编辑器模块不同，它们是全局工具，不进顶栏模块切换器；
// 打开时占用主区右侧一列（推开内容而非遮挡），宽度可拖动。原生端对应右侧停靠组中的三个固定工具页。

export type ToolId = 'changes' | 'test' | 'assistant';
const toolMeta: { id: ToolId; label: string; icon: typeof Play }[] = [
  { id: 'changes', label: '修改', icon: GitCompareArrows }, { id: 'test', label: '测试', icon: Play }, { id: 'assistant', label: '助手', icon: Sparkles },
];
export function ToolPanel({ tool, onTool, onClose, badges, children }: { tool: ToolId; onTool: (tool: ToolId) => void; onClose: () => void; badges: Partial<Record<ToolId, number>>; children: ReactNode }) {
  const [width, setWidth] = useState(() => { try { return Math.max(320, Math.min(640, Number(localStorage.getItem('forge-toolpanel-width')) || 380)); } catch { return 380; } });
  const drag = useRef<{ x: number; w: number } | null>(null);
  return <aside className="tp-root" style={{ width }} aria-label="工具面板">
    <div className="tp-resize" role="separator" aria-orientation="vertical" aria-label="调整工具面板宽度" onPointerDown={event => { drag.current = { x: event.clientX, w: width }; event.currentTarget.setPointerCapture(event.pointerId); }} onPointerMove={event => { if (drag.current) setWidth(Math.max(320, Math.min(640, drag.current.w - (event.clientX - drag.current.x)))); }} onPointerUp={() => { drag.current = null; try { localStorage.setItem('forge-toolpanel-width', String(width)); } catch { /* 界面偏好 */ } }} />
    <header className="tp-head" role="tablist">{toolMeta.map(t => <button key={t.id} role="tab" aria-selected={tool === t.id} className={tool === t.id ? 'is-active' : ''} onClick={() => onTool(t.id)}><t.icon size={13} />{t.label}{!!badges[t.id] && <b>{badges[t.id]}</b>}</button>)}<div className="wb-spacer" /><button aria-label="关闭工具面板" title="关闭 · Esc" onClick={onClose}><X size={14} /></button></header>
    <div className="tp-body">{children}</div>
  </aside>;
}
