import { useEffect, useRef, useState } from 'react';
import type { ReactNode } from 'react';
import { Search, X } from 'lucide-react';

type CommitInputProps = { value: string | number; onCommit: (value: string) => void; label: string; type?: string; min?: number; max?: number; multiline?: boolean };
export function CommitInput(props: CommitInputProps) { return <CommitInputDraft key={`${props.label}:${props.value}`} {...props} />; }
function CommitInputDraft({ value, onCommit, label, type = 'text', min, max, multiline = false }: CommitInputProps) {
  const [draft, setDraft] = useState(String(value));
  const [invalid, setInvalid] = useState(false);
  const cancelled = useRef(false);
  function commit() {
    if (cancelled.current) { cancelled.current = false; return; }
    if (type === 'number' && (draft.trim() === '' || !Number.isFinite(Number(draft)) || (min !== undefined && Number(draft) < min) || (max !== undefined && Number(draft) > max))) { setInvalid(true); return; }
    setInvalid(false); if (draft !== String(value)) onCommit(draft);
  }
  const props = { value: draft, 'aria-label': label, 'aria-invalid': invalid, onChange: (event: React.ChangeEvent<HTMLInputElement | HTMLTextAreaElement>) => setDraft(event.target.value), onBlur: commit, onKeyDown: (event: React.KeyboardEvent<HTMLInputElement | HTMLTextAreaElement>) => { if (event.key === 'Escape') { cancelled.current = true; setDraft(String(value)); setInvalid(false); event.currentTarget.blur(); } if (event.key === 'Enter' && !multiline) event.currentTarget.blur(); } };
  return multiline ? <textarea {...props} rows={4} /> : <input {...props} type={type} min={min} max={max} step={type === 'number' ? 'any' : undefined} />;
}
export function SearchBox({ value, onChange, placeholder = '搜索', label = placeholder }: { value: string; onChange: (value: string) => void; placeholder?: string; label?: string }) {
  return <label className="wb-search"><Search size={14} /><input value={value} onChange={e => onChange(e.target.value)} placeholder={placeholder} aria-label={label} />{value && <button type="button" onClick={() => onChange('')} aria-label="清除搜索"><X size={12} /></button>}</label>;
}
export function Modal({ title, close, children, wide = false }: { title: string; close: () => void; children: ReactNode; wide?: boolean }) {
  const ref = useRef<HTMLDialogElement>(null);
  useEffect(() => { ref.current?.showModal(); }, []);
  return <dialog ref={ref} className={`studio-dialog wb-dialog ${wide ? 'wide' : ''}`} onCancel={e => { e.preventDefault(); close(); }} onClick={e => { if (e.target === e.currentTarget) close(); }}><div className="wb-dialog-heading"><strong>{title}</strong><button onClick={close} aria-label="关闭窗口"><X size={17} /></button></div>{children}</dialog>;
}
export function EmptyState({ icon, title, action }: { icon: ReactNode; title: string; action?: ReactNode }) { return <div className="wb-empty">{icon}<span>{title}</span>{action}</div>; }
