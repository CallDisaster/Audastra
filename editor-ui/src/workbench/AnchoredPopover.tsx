import { useLayoutEffect, useRef } from 'react';
import type { CSSProperties, ReactNode } from 'react';
import { createPortal } from 'react-dom';
import { X } from 'lucide-react';
import { keepEditorSave } from './parameterFields';

// 非模态取值浮层：蓝图节点贴右侧，传统语句贴参数下方；两者只在锚点与方向上不同。
export function AnchoredPopover({ anchor, placement, label, title, onClose, children, width = 328 }: {
  anchor: () => HTMLElement | null;
  placement: 'right' | 'below';
  label: string;
  title: ReactNode;
  onClose: () => void;
  children: ReactNode;
  width?: number;
}) {
  const panel = useRef<HTMLElement>(null);
  const closeRef = useRef(onClose);
  const anchorRef = useRef(anchor);
  useLayoutEffect(() => { closeRef.current = onClose; anchorRef.current = anchor; });
  const host = document.querySelector('.studio') ?? document.body;
  useLayoutEffect(() => {
    let frame = 0;
    // 首次在布局阶段同步定位（避免一帧错位，也保证打开后立即可聚焦），之后逐帧跟随平移/缩放。
    const place = () => {
      const owner = anchorRef.current(), element = panel.current;
      if (!owner || !element) return;
      const rect = owner.getBoundingClientRect();
      const viewport = window.visualViewport;
      const viewportX = viewport?.offsetLeft ?? 0, viewportY = viewport?.offsetTop ?? 0;
      const viewWidth = viewport?.width ?? window.innerWidth, viewHeight = viewport?.height ?? window.innerHeight;
      const margin = 8, gap = placement === 'right' ? 8 : 4;
      element.style.width = `${Math.min(width, viewWidth - margin * 2)}px`;
      element.style.maxHeight = `${Math.min(540, viewHeight - margin * 2)}px`;
      const panelWidth = element.offsetWidth, panelHeight = element.offsetHeight;
      let left: number, top: number, side: string;
      if (placement === 'right') {
        const right = rect.right + gap;
        const flip = right + panelWidth > viewportX + viewWidth - margin && rect.left - panelWidth - gap >= viewportX + margin;
        left = flip ? rect.left - panelWidth - gap : right; top = rect.top; side = flip ? 'left' : 'right';
      } else {
        const below = rect.bottom + gap;
        const flip = below + panelHeight > viewportY + viewHeight - margin && rect.top - panelHeight - gap >= viewportY + margin;
        left = rect.left; top = flip ? rect.top - panelHeight - gap : below; side = flip ? 'above' : 'below';
      }
      left = Math.max(viewportX + margin, Math.min(left, viewportX + viewWidth - panelWidth - margin));
      top = Math.max(viewportY + margin, Math.min(top, viewportY + viewHeight - panelHeight - margin));
      element.style.left = `${Math.round(left)}px`; element.style.top = `${Math.round(top)}px`;
      element.style.visibility = 'visible'; element.dataset.side = side;
      element.style.setProperty('--np-arrow-top', `${Math.max(20, Math.min(panelHeight - 20, rect.top + 25 - top))}px`);
      element.style.setProperty('--np-arrow-left', `${Math.max(14, Math.min(panelWidth - 14, rect.left + Math.min(rect.width / 2, 18) - left))}px`);
    };
    const follow = () => { place(); frame = requestAnimationFrame(follow); };
    place();
    // 子组件的 autoFocus 发生在面板可见之前，这里补一次：打开后键盘直接进入搜索/输入框。
    const element = panel.current;
    if (element && !element.contains(document.activeElement)) element.querySelector<HTMLElement>('input:not([type=checkbox]):not([disabled]),textarea')?.focus();
    frame = requestAnimationFrame(follow);
    const dismiss = (event: globalThis.PointerEvent) => {
      if (!(event.target instanceof Node)) return;
      if (panel.current?.contains(event.target) || anchorRef.current()?.contains(event.target)) return;
      closeRef.current();
    };
    const escape = (event: globalThis.KeyboardEvent) => { if (event.key === 'Escape') closeRef.current(); };
    document.addEventListener('pointerdown', dismiss, true); document.addEventListener('keydown', escape, true);
    return () => { cancelAnimationFrame(frame); document.removeEventListener('pointerdown', dismiss, true); document.removeEventListener('keydown', escape, true); };
  }, [placement, width]);
  const stop = (event: { stopPropagation: () => void }) => event.stopPropagation();
  return createPortal(<section ref={panel} className="np-popover" role="dialog" aria-label={label} aria-modal={false} style={{ visibility: 'hidden' } as CSSProperties} onPointerDown={stop} onPointerUp={stop} onPointerMove={stop} onClick={stop} onDoubleClick={stop} onContextMenu={stop} onWheel={stop} onDragStart={stop} onKeyDown={event => { keepEditorSave(event); if (event.key === 'Escape') { event.preventDefault(); onClose(); } }}><header><strong>{title}</strong><button aria-label="关闭参数选择器" onClick={onClose}><X size={14}/></button></header>{children}</section>, host);
}
