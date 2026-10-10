import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { ArrowDown, ArrowUp, ArrowUpRight, Box, Check, ChevronDown, Copy, Database, Layers, Link2, Plus, Replace, RotateCcw, Search, Settings2, Trash2, X } from 'lucide-react';
import type { EditorObject, FieldValue, ModuleProps, ObjectKind } from './types';
import { objectKinds } from './types';
import { baseObjectValue, containsReference, displayValue, fieldDefinitions, objectFields, originalLevels, readObjectValue, sameValue, writeObjectLevels, writeObjectValue } from './objectFields';
import type { ObjectField } from './objectFields';
import { ObjectPortrait } from './ObjectPortrait';
import { SharedValuePicker } from './SharedValuePicker';
import './objects.css';

function CommitInput({ value, label, type = 'text', placeholder, list, onCommit, multiline = false }: {
    value: string; label: string; type?: string; placeholder?: string; list?: string;
    onCommit: (value: string) => void; multiline?: boolean;
}) {
    const [draft, setDraft] = useState<string | null>(null);
    const text = draft ?? value;
    const commit = () => { if (draft !== null && draft !== value) onCommit(draft); setDraft(null); };
    const shared = { value: text, 'aria-label': label, placeholder, onChange: (event: React.ChangeEvent<HTMLInputElement | HTMLTextAreaElement>) => setDraft(event.target.value), onBlur: commit };
    return multiline ? <textarea {...shared} rows={3} onKeyDown={event => { if (event.key === 'Escape') { setDraft(null); event.preventDefault(); } }} /> :
        <input {...shared} type={type} step={type === 'number' ? 'any' : undefined} list={list} onKeyDown={event => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') { setDraft(null); event.preventDefault(); }
        }} />;
}

function ReferenceFieldEditor({ field, value, mixed, project, onCommit, onInvalid, onNavigate }: {
    field: ObjectField; value: FieldValue; mixed: boolean; project: ModuleProps['project']; onCommit: (value: FieldValue) => void; onInvalid: (message: string) => void; onNavigate: ModuleProps['onNavigate'];
}) {
    const anchor = useRef<HTMLDivElement>(null);
    const panel = useRef<HTMLElement>(null);
    const [open, setOpen] = useState(false);
    const [portalHost, setPortalHost] = useState<Element | null>(null);
    const [position, setPosition] = useState({ x: 0, y: 0, width: 780, maxHeight: 580 });
    const [selectedIndex, setSelectedIndex] = useState(0);
    const [action, setAction] = useState<'add' | 'replace'>('add');
    const references = field.multiple ? Array.isArray(value) ? value : typeof value === 'string' && value ? [value] : [] : typeof value === 'string' && value ? [value] : [];
    const resolve = (id: string) => project.objects.find(object => object.id === id || object.rawcode === id);
    const selected = references[selectedIndex];
    const kindName = objectKinds.find(kind => kind.id === field.objectKind)?.name ?? '对象';
    const measure = () => {
        const rect = anchor.current?.getBoundingClientRect();
        if (!rect) return;
        const width = Math.min(field.multiple ? 810 : 430, window.innerWidth - 24);
        const maxHeight = Math.min(580, window.innerHeight - 32);
        const right = rect.right + 12, left = rect.left - width - 12;
        const x = right + width <= window.innerWidth - 12 ? right : left >= 12 ? left : Math.max(12, Math.min(rect.left, window.innerWidth - width - 12));
        setPosition({ x, y: Math.max(16, Math.min(rect.top, window.innerHeight - maxHeight - 16)), width, maxHeight });
    };
    const openEditor = (index?: number) => {
        setPortalHost(anchor.current?.closest('.studio') ?? document.body);
        setSelectedIndex(index ?? 0); setAction(typeof index === 'number' || !field.multiple ? 'replace' : 'add'); measure(); setOpen(true);
    };
    useEffect(() => {
        if (!open) return;
        const closeOutside = (event: PointerEvent) => {
            if (!(event.target instanceof Node)) return;
            if (!anchor.current?.contains(event.target) && !panel.current?.contains(event.target)) setOpen(false);
        };
        const update = () => {
            const rect = anchor.current?.getBoundingClientRect();
            if (!rect) return;
            const width = Math.min(field.multiple ? 810 : 430, window.innerWidth - 24), maxHeight = Math.min(580, window.innerHeight - 32);
            const right = rect.right + 12, left = rect.left - width - 12;
            const x = right + width <= window.innerWidth - 12 ? right : left >= 12 ? left : Math.max(12, Math.min(rect.left, window.innerWidth - width - 12));
            setPosition({ x, y: Math.max(16, Math.min(rect.top, window.innerHeight - maxHeight - 16)), width, maxHeight });
        };
        const escape = (event: KeyboardEvent) => { if (event.key === 'Escape') { event.stopPropagation(); setOpen(false); } };
        document.addEventListener('pointerdown', closeOutside, true);
        document.addEventListener('keydown', escape);
        window.addEventListener('resize', update);
        document.addEventListener('scroll', update, true);
        return () => { document.removeEventListener('pointerdown', closeOutside, true); document.removeEventListener('keydown', escape); window.removeEventListener('resize', update); document.removeEventListener('scroll', update, true); };
    }, [open, field.multiple]);

    const write = (next: string[]) => onCommit(field.multiple ? next : next[0] ?? '');
    const accept = (picked: unknown) => {
        if (typeof picked !== 'string') { onInvalid('请选择一个对象'); return; }
        const object = resolve(picked);
        if (picked && !object) { onInvalid('无法解析所选对象'); return; }
        if (object && object.kind !== field.objectKind) { onInvalid(`此字段只接受${kindName}`); return; }
        if (!field.multiple) { write(picked ? [picked] : []); setOpen(false); return; }
        if (!picked) return;
        const duplicate = references.findIndex((id, index) => (id === picked || resolve(id)?.id === object?.id && !!object) && (action !== 'replace' || index !== selectedIndex));
        if (duplicate >= 0) { setSelectedIndex(duplicate); onInvalid('该对象已在列表中'); return; }
        const next = [...references];
        if (action === 'replace' && selectedIndex < next.length) next[selectedIndex] = picked;
        else { next.push(picked); setSelectedIndex(next.length - 1); }
        write(next);
    };
    const remove = () => { write(references.filter((_, index) => index !== selectedIndex)); setSelectedIndex(Math.max(0, selectedIndex - 1)); };
    const move = (direction: number) => {
        const nextIndex = selectedIndex + direction;
        if (nextIndex < 0 || nextIndex >= references.length) return;
        const next = [...references]; [next[selectedIndex], next[nextIndex]] = [next[nextIndex], next[selectedIndex]];
        write(next); setSelectedIndex(nextIndex);
    };

    return <div className="ow-reference-field" ref={anchor}>
        <div className="ow-reference-chips">{mixed ? <button className="ow-reference-chip" onClick={() => openEditor()}>多个值</button> : references.length ? references.map((id, index) => {
            const object = resolve(id); const mismatch = object && object.kind !== field.objectKind;
            return <button className={`ow-reference-chip ${!object || mismatch ? 'is-unresolved' : ''}`} key={`${index}/${id}`} aria-label={`编辑${field.name} ${object?.name ?? id}`} title={`${object?.name ?? '未解析对象'} · ${id}${mismatch ? ' · 类型不匹配' : ''}`} onClick={() => openEditor(index)}>
                {object ? <ObjectPortrait object={object} project={project} size={23} /> : <span className="ow-missing-reference">?</span>}<span>{object?.name ?? id}</span>
            </button>;
        }) : <button className="ow-reference-empty" onClick={() => openEditor()}>未设置</button>}</div>
        <button className="ow-reference-edit" aria-label={`编辑${field.name}`} title={`编辑${field.name}`} onClick={() => openEditor()}><Plus size={13} /></button>
        {open && createPortal(<section ref={panel} role="dialog" aria-modal="false" aria-label={`${field.name}编辑器`} className={`ow-reference-editor ${field.multiple ? 'is-multiple' : ''}`} style={{ left: position.x, top: position.y, width: position.width, maxHeight: position.maxHeight }} onWheel={event => event.stopPropagation()} onPointerDown={event => event.stopPropagation()} onPointerUp={event => event.stopPropagation()} onKeyDown={event => { event.stopPropagation(); if (event.key === 'Escape') setOpen(false); }} onContextMenu={event => event.stopPropagation()}>
            <header><div><strong>{field.name}</strong><span>{kindName}{field.multiple ? ` · ${references.length} 项` : ''}</span></div><button aria-label={`关闭${field.name}编辑器`} onClick={() => setOpen(false)}><X size={16} /></button></header>
            <div className="ow-reference-editor-body">
                {field.multiple && <aside className="ow-ordered-references"><div className="ow-reference-list-title">已选列表{mixed && <span>编辑将应用至所选对象</span>}</div><div className="ow-reference-actions"><button className={action === 'add' ? 'is-active' : ''} onClick={() => setAction('add')}><Plus size={13} />添加</button><button disabled={!selected} className={action === 'replace' ? 'is-active' : ''} onClick={() => setAction('replace')}><Replace size={13} />替换</button><button disabled={!selected} aria-label={`删除所选${kindName}`} title="删除" onClick={remove}><Trash2 size={13} /></button><button disabled={!selected || selectedIndex === 0} aria-label={`上移所选${kindName}`} title="上移" onClick={() => move(-1)}><ArrowUp size={13} /></button><button disabled={!selected || selectedIndex === references.length - 1} aria-label={`下移所选${kindName}`} title="下移" onClick={() => move(1)}><ArrowDown size={13} /></button></div><div className="ow-ordered-list" role="listbox" aria-label={`已选${field.name}`}>{references.map((id, index) => {
                    const object = resolve(id); const mismatch = object && object.kind !== field.objectKind;
                    return <button key={`${index}/${id}`} role="option" aria-selected={selectedIndex === index} className={`${selectedIndex === index ? 'is-selected' : ''} ${!object || mismatch ? 'is-unresolved' : ''}`} onClick={() => setSelectedIndex(index)} onDoubleClick={() => { setSelectedIndex(index); setAction('replace'); }}><span className="ow-reference-order">{index + 1}</span>{object ? <ObjectPortrait object={object} project={project} size={32} /> : <span className="ow-missing-reference">?</span>}<span className="ow-reference-object-text"><strong>{object?.name ?? id}</strong><code>{object?.rawcode ?? id}{mismatch ? ' · 类型不匹配' : !object ? ' · 未解析' : ''}</code></span></button>;
                })}{!references.length && <div className="ow-empty-small">没有已选对象</div>}</div><footer><span>{action === 'add' ? '候选对象将添加到列表末尾' : `替换第 ${selectedIndex + 1} 项`}</span></footer></aside>}
                <div className="ow-reference-candidates"><SharedValuePicker key={`${field.id}/${action}/${action === 'replace' ? selected ?? '' : ''}`} project={project} expectedType={field.objectKind ?? 'object'} objectKind={field.objectKind} multiple={false} allowedSources={['constant']} value={action === 'replace' ? selected ?? '' : ''} onSelect={accept} onNavigate={onNavigate} /></div>
            </div><footer><span>地图对象引用</span>{!field.multiple && references.length > 0 && <button onClick={() => { write([]); setOpen(false); }}>清除</button>}<button onClick={() => setOpen(false)}>完成</button></footer>
        </section>, portalHost ?? document.body)}
    </div>;
}

function FieldEditor({ field, value, mixed, project, onCommit, onInvalid, onNavigate }: {
    field: ObjectField; value: FieldValue; mixed: boolean; project: ModuleProps['project']; onCommit: (value: FieldValue) => void; onInvalid: (message: string) => void; onNavigate: ModuleProps['onNavigate'];
}) {
    if (field.objectKind) return <ReferenceFieldEditor field={field} value={value} mixed={mixed} project={project} onCommit={onCommit} onInvalid={onInvalid} onNavigate={onNavigate} />;
    if (field.type === 'boolean') return <select aria-label={field.name} value={mixed ? '' : String(value)} onChange={event => onCommit(event.target.value === 'true')}>
        {mixed && <option value="" disabled>多个值</option>}<option value="true">是</option><option value="false">否</option>
    </select>;
    const options = field.id === 'model' || field.id === 'icon' ? project.assets.map(asset => ({ value: asset.path, label: asset.name })) :
        field.id === 'race' ? ['人族', '兽族', '不死族', '暗夜精灵', '中立', '其他'].map(value => ({ value, label: value })) : [];
    const listId = `ow-pick-${field.id}`;
    return <div className="ow-value-input"><CommitInput key={field.id + ':' + displayValue(value) + ':' + mixed} value={mixed ? '' : displayValue(value)}
        label={field.name} type={field.type === 'number' ? 'number' : 'text'} placeholder={mixed ? '多个值' : field.type === 'list' ? '选择或输入，以逗号分隔' : '—'}
        list={options.length ? listId : undefined} multiline={field.id === 'tooltip'} onCommit={text => {
            if (field.type === 'number') { const number = Number(text); if (text.trim() && Number.isFinite(number)) onCommit(number); else onInvalid(`${field.name}需要有效数值`); return; }
            onCommit(field.type === 'list' ? text.split(/[,，]/).map(item => item.trim()).filter(Boolean) : text);
        }} />{field.unit && <span className="ow-unit">{field.unit}</span>}
        {!!options.length && <datalist id={listId}>{options.map(option => <option key={option.value} value={option.value}>{option.label}</option>)}</datalist>}
    </div>;
}

export function ObjectWorkspace({ project, onChange, onNavigate, notify, focusId }: ModuleProps) {
    const focused = project.objects.find(object => object.id === focusId || object.rawcode === focusId);
    const [selection, setSelection] = useState<{ focus?: string; kind: ObjectKind; ids: string[] }>({ focus: focusId, kind: focused?.kind ?? 'unit', ids: focused ? [focused.id] : [] });
    const kind = selection.focus !== focusId && focused ? focused.kind : selection.kind;
    const [query, setQuery] = useState('');
    const [fieldQuery, setFieldQuery] = useState('');
    const [sourceFilter, setSourceFilter] = useState('全部');
    const [filter, setFilter] = useState<'常用' | '已修改' | '全部'>('常用');
    const [view, setView] = useState<'字段' | '等级矩阵'>('字段');
    const [details, setDetails] = useState(true);
    const [create, setCreate] = useState<{ name: string; rawcode: string; templateId: string } | null>(null);
    const [error, setError] = useState('');
    const kindObjects = project.objects.filter(object => object.kind === kind);
    const requestedIds = selection.focus !== focusId && focused ? [focused.id] : selection.ids;
    const selected = kindObjects.filter(object => requestedIds.includes(object.id));
    const objects = selected.length ? selected : kindObjects.slice(0, 1);
    const active = objects[0];
    const visible = kindObjects.filter(object => `${object.name} ${object.rawcode} ${object.race} ${object.category}`.toLowerCase().includes(query.toLowerCase()) && (sourceFilter === '全部' || object.custom === (sourceFilter === '自定义')));
    const fields = active ? fieldDefinitions(active) : [];
    const visibleFields = fields.filter(field => `${field.name} ${field.id}`.toLowerCase().includes(fieldQuery.toLowerCase()) &&
        (filter !== '常用' || field.common) && (filter !== '已修改' || objects.some(object => !sameValue(readObjectValue(object, field), baseObjectValue(object, field)))));
    const groups = [...new Set(visibleFields.map(field => field.group))];
    const selectedIds = new Set(objects.map(object => object.id));
    const refs = active ? project.graphs.filter(graph => graph.nodes.some(node => containsReference(node.values, [active.id, active.rawcode]))) : [];
    const instances = active ? project.world.instances.filter(instance => instance.objectId === active.id || instance.objectId === active.rawcode) : [];
    const linked = active ? project.objects.filter(object => object.id !== active.id && containsReference(object.fields, [active.id, active.rawcode])) : [];
    const aiReferences = active ? project.ai.build.filter(row => [active.id, active.rawcode].includes(row.objectId)).length + project.ai.waves.filter(row => [active.id, active.rawcode].includes(row.unitId)).length + Number([active.id, active.rawcode].includes(project.ai.hero)) + project.ai.skills.filter(id => [active.id, active.rawcode].includes(id)).length : 0;
    const levelFields = fields.filter(field => field.levels || active?.levels[field.id]);
    const levelCount = active ? Math.max(1, ...Object.values(active.levels).map(values => values.length)) : 1;

    const select = (object: EditorObject, additive = false) => {
        const ids = additive ? selectedIds.has(object.id) ? objects.filter(item => item.id !== object.id).map(item => item.id) : [...objects.map(item => item.id), object.id] : [object.id];
        setSelection({ focus: focusId, kind: object.kind, ids });
        if (object.kind !== kind) { setView('字段'); setQuery(''); }
    };
    const changeSelected = (update: (object: EditorObject) => EditorObject, label: string) => {
        onChange({ ...project, objects: project.objects.map(object => selectedIds.has(object.id) ? update(object) : object) }, label);
    };
    const setValue = (field: ObjectField, value: FieldValue) => {
        if (field.id === 'name' && !String(value).trim()) { notify('名称不能为空'); return; }
        changeSelected(object => writeObjectValue(object, field, value), `${objects.length > 1 ? `批量修改 ${objects.length} 个对象 · ` : ''}${field.name}`);
    };
    const nextRawcode = () => {
        const prefix: Record<ObjectKind, string> = { unit: 'U', item: 'I', destructable: 'D', doodad: 'P', ability: 'A', buff: 'B', upgrade: 'R' };
        let index = 0;
        while (project.objects.some(object => object.rawcode === prefix[kind] + index.toString(36).toUpperCase().padStart(3, '0'))) index++;
        return prefix[kind] + index.toString(36).toUpperCase().padStart(3, '0');
    };
    const openCreate = (copy: boolean) => { setCreate({ name: copy && active ? `${active.name} 副本` : `新建${objectKinds.find(item => item.id === kind)?.name}`, rawcode: nextRawcode(), templateId: active?.id ?? '' }); setError(''); };
    const createObject = () => {
        if (!create) return;
        if (!create.name.trim()) { setError('请输入名称'); return; }
        if (!/^[\x20-\x7E]{4}$/.test(create.rawcode)) { setError('对象 ID 需要 4 个 ASCII 字符'); return; }
        if (project.objects.some(object => object.rawcode === create.rawcode)) { setError('对象 ID 已存在'); return; }
        const template = project.objects.find(object => object.id === create.templateId);
        const id = crypto.randomUUID();
        const defaultValues = Object.fromEntries(objectFields.filter(field => !field.kinds || field.kinds.includes(kind)).map(field => [field.id, field.value]));
        const object: EditorObject = template ? { ...structuredClone(template), id, name: create.name.trim(), rawcode: create.rawcode, parent: template.rawcode, custom: true } : {
            id, name: create.name.trim(), kind, rawcode: create.rawcode, parent: '', race: '其他', custom: true,
            category: '自定义', health: 100, speed: kind === 'unit' ? 270 : 0, enabled: true, fields: {}, defaults: { ...defaultValues, name: create.name.trim(), health: 100, speed: kind === 'unit' ? 270 : 0, enabled: true, category: '自定义', race: '其他' }, levels: {},
        };
        onChange({ ...project, objects: [...project.objects, object] }, `新建${objectKinds.find(item => item.id === kind)?.name} · ${object.name}`);
        setSelection({ focus: focusId, kind, ids: [id] }); setCreate(null); setQuery(''); setSourceFilter('全部');
    };
    const setLevel = (field: ObjectField, index: number, text: string) => {
        if (!text.trim() || !Number.isFinite(Number(text))) { notify('请输入有效数值'); return; }
        changeSelected(object => { const values = Array.from({ length: Math.max(levelCount, index + 1) }, (_, i) => object.levels[field.id]?.[i] ?? Number(readObjectValue(object, field))); values[index] = Number(text); return writeObjectLevels(object, { ...object.levels, [field.id]: values }); }, `${field.name} · 等级 ${index + 1}`);
    };

    return <section className="ow-workspace" aria-label="物体编辑器">
        <nav className="ow-kind-tabs" aria-label="对象类型">{objectKinds.map(item => <button key={item.id} className={kind === item.id ? 'is-active' : ''} onClick={() => { setSelection({ focus: focusId, kind: item.id, ids: [] }); setQuery(''); setView('字段'); }}><span className={`ow-kind-icon ow-kind-${item.id}`}>{item.short}</span>{item.name}<small>{project.objects.filter(object => object.kind === item.id).length}</small></button>)}</nav>
        <div className={`ow-layout ${details ? '' : 'ow-no-details'}`}>
            <aside className="ow-directory">
                <div className="ow-panel-title"><span>对象目录</span><button title="新建对象" onClick={() => openCreate(false)}><Plus size={15} /></button></div>
                <label className="ow-search"><Search size={14} /><input aria-label="搜索对象" placeholder="搜索名称 / ID" value={query} onChange={event => setQuery(event.target.value)} /></label>
                <div className="ow-source-filter">{['全部', '标准', '自定义'].map(item => <button key={item} className={sourceFilter === item ? 'is-active' : ''} onClick={() => setSourceFilter(item)}>{item}</button>)}</div>
                <div className="ow-object-list">{visible.length ? [...new Set(visible.map(object => object.race || '其他'))].map(race => <div key={race} className="ow-race-group"><div className="ow-race-label"><ChevronDown size={12} />{race}<span>{visible.filter(object => (object.race || '其他') === race).length}</span></div>{visible.filter(object => (object.race || '其他') === race).map(object => <button key={object.id} className={`ow-object-row ${selectedIds.has(object.id) ? 'is-active' : ''}`} onClick={event => select(object, event.ctrlKey || event.metaKey || event.shiftKey)} draggable onDragStart={event => { event.dataTransfer.setData('application/x-forge-object', JSON.stringify({ id: object.id, rawcode: object.rawcode, kind: object.kind })); event.dataTransfer.setData('text/plain', object.rawcode); }}><span className={`ow-object-glyph ow-kind-${kind}`}>{object.name.slice(0, 1)}</span><span><strong>{object.name}</strong><small>{object.rawcode}</small></span>{object.custom && <i title="自定义对象" />}</button>)}</div>) : <div className="ow-empty-small">没有匹配对象</div>}</div>
                <div className="ow-directory-footer"><span>{visible.length} 个对象</span><span>Ctrl 多选</span></div>
            </aside>
            <main className="ow-main">
                <div className="ow-toolbar"><button onClick={() => openCreate(false)}><Plus size={14} />新建</button><button disabled={!active} onClick={() => openCreate(true)}><Copy size={14} />复制</button><button disabled={!active} onClick={() => changeSelected(object => {
                    let result = { ...object, fields: {}, levels: structuredClone(originalLevels(object)) };
                    for (const field of fieldDefinitions(object)) if (['health', 'speed', 'enabled'].includes(field.id)) result = writeObjectValue(result, field, baseObjectValue(object, field));
                    return result;
                }, '恢复对象默认值')}><RotateCcw size={14} />恢复默认</button><span className="ow-toolbar-spacer" /><button title="显示来源与引用" className={details ? 'is-active' : ''} onClick={() => setDetails(!details)}><Settings2 size={15} />详情</button></div>
                {active ? <>
                    <header className="ow-object-heading"><span className={`ow-object-emblem ow-kind-${kind}`}><Box size={24} /></span><div><div className="ow-heading-title">{objects.length > 1 ? `${objects.length} 个对象` : active.name}<code>{objects.length > 1 ? '批量编辑' : active.rawcode}</code></div><div className="ow-heading-meta">{objectKinds.find(item => item.id === kind)?.name}<span>／</span>{objects.length > 1 ? '共同字段' : active.race}<span>／</span>{active.custom ? '自定义' : '标准对象'}</div></div></header>
                    <div className="ow-field-controls"><div className="ow-segments">{(['常用', '已修改', '全部'] as const).map(item => <button key={item} className={filter === item ? 'is-active' : ''} onClick={() => setFilter(item)}>{item}</button>)}</div><label className="ow-search ow-field-search"><Search size={13} /><input aria-label="搜索字段" placeholder="搜索字段" value={fieldQuery} onChange={event => setFieldQuery(event.target.value)} /></label>{(kind === 'ability' || kind === 'upgrade' || Object.keys(active.levels).length > 0) && <button className={`ow-matrix-switch ${view === '等级矩阵' ? 'is-active' : ''}`} onClick={() => setView(view === '字段' ? '等级矩阵' : '字段')}><Layers size={14} />等级矩阵</button>}</div>
                    <div className="ow-fields-scroll">{view === '字段' ? <><div className="ow-table-labels"><span>字段</span><span>值</span><span /></div>{groups.map(group => <section key={group} className="ow-field-group"><h3><ChevronDown size={13} />{group}<span>{visibleFields.filter(field => field.group === group).length}</span></h3>{visibleFields.filter(field => field.group === group).map(field => {
                        const value = readObjectValue(active, field); const mixed = objects.some(object => !sameValue(readObjectValue(object, field), value));
                        const modified = objects.some(object => !sameValue(readObjectValue(object, field), baseObjectValue(object, field)));
                        return <div key={field.id} className={`ow-field-row ${modified ? 'is-modified' : ''}`}><div className="ow-field-name"><span>{field.name}{modified && <i title="已修改" />}</span><code>{field.id}</code></div><FieldEditor field={field} value={value} mixed={mixed} project={project} onNavigate={onNavigate} onInvalid={notify} onCommit={value => setValue(field, value)} /><button className="ow-reset-field" disabled={!modified} title={`恢复 ${field.name} 默认值`} onClick={() => changeSelected(object => writeObjectValue(object, field, baseObjectValue(object, field)), `恢复 ${field.name}`)}><RotateCcw size={12} /></button></div>;
                    })}</section>)}{!visibleFields.length && <div className="ow-empty"><Check size={26} /><strong>{filter === '已修改' ? '没有修改的字段' : '没有匹配字段'}</strong><button onClick={() => { setFilter('全部'); setFieldQuery(''); }}>显示全部字段</button></div>}</> : <div className="ow-level-matrix"><div className="ow-matrix-toolbar"><strong>等级数据</strong><span>{levelCount} 级</span><button onClick={() => changeSelected(object => writeObjectLevels(object, { ...object.levels, ...Object.fromEntries(levelFields.map(field => { const values = Array.from({ length: levelCount }, (_, i) => object.levels[field.id]?.[i] ?? Number(readObjectValue(object, field))); return [field.id, [...values, values.at(-1) ?? 0]]; })) }), '复制末级')}><Plus size={14} />复制末级</button><button disabled={levelCount <= 1} onClick={() => changeSelected(object => writeObjectLevels(object, Object.fromEntries(Object.entries(object.levels).map(([id, values]) => [id, values.slice(0, -1)]))), '删除末级')}>删除末级</button></div><table><thead><tr><th>字段</th>{Array.from({ length: levelCount }, (_, i) => <th key={i}>等级 {i + 1}</th>)}</tr></thead><tbody>{levelFields.map(field => <tr key={field.id}><th>{field.name}</th>{Array.from({ length: levelCount }, (_, i) => <td key={i}><CommitInput value={String(active.levels[field.id]?.[i] ?? readObjectValue(active, field))} label={`${field.name} 等级 ${i + 1}`} type="number" onCommit={text => setLevel(field, i, text)} /></td>)}</tr>)}</tbody></table>{!levelFields.length && <div className="ow-empty-small">没有等级字段</div>}</div>}</div>
                    <footer className="ow-status"><span><Database size={12} />{objects.length > 1 ? `已选择 ${objects.length} 个对象` : `${visibleFields.length} 个字段`}</span><span>{refs.length + linked.length + instances.length + aiReferences} 处引用</span></footer>
                </> : <div className="ow-empty"><Box size={34} /><strong>还没有{objectKinds.find(item => item.id === kind)?.name}</strong><button className="ow-primary" onClick={() => openCreate(false)}><Plus size={14} />新建对象</button></div>}
            </main>
            {details && <aside className="ow-details"><div className="ow-panel-title"><span>来源与引用</span><button title="关闭详情" onClick={() => setDetails(false)}><X size={14} /></button></div>{active ? <><section><h3>对象信息</h3><dl><dt>对象 ID</dt><dd><code>{active.rawcode}</code></dd><dt>基础对象</dt><dd>{active.parent || '—'}</dd><dt>数据来源</dt><dd>{active.custom ? '地图' : '基础数据'}</dd><dt>对象类型</dt><dd>{objectKinds.find(item => item.id === active.kind)?.name}</dd></dl>{active.parent && project.objects.some(object => object.rawcode === active.parent && object.id !== active.id) && <button onClick={() => { const parent = project.objects.find(object => object.rawcode === active.parent && object.id !== active.id); if (parent) select(parent); }}><ArrowUpRight size={13} />打开基础对象</button>}</section><section><h3><Link2 size={13} />使用位置 <span>{refs.length + linked.length + instances.length + aiReferences}</span></h3>{refs.map(graph => <button className="ow-reference" key={graph.id} onClick={() => onNavigate('triggers', graph.id)}><span>触发器</span><strong>{graph.name}</strong><ArrowUpRight size={13} /></button>)}{linked.map(object => <button className="ow-reference" key={object.id} onClick={() => select(object)}><span>对象</span><strong>{object.name}</strong><ArrowUpRight size={13} /></button>)}{instances.map(instance => <button className="ow-reference" key={instance.id} onClick={() => onNavigate('world', instance.id)}><span>地图实例</span><strong>{instance.x}, {instance.y}</strong><ArrowUpRight size={13} /></button>)}{aiReferences > 0 && <button className="ow-reference" onClick={() => onNavigate('ai')}><span>AI</span><strong>{project.ai.name}</strong><ArrowUpRight size={13} /></button>}{!refs.length && !linked.length && !instances.length && !aiReferences && <p className="ow-muted">没有引用</p>}</section><section><h3>关联资源</h3>{['model', 'icon'].map(id => { const path = active.fields[id] ?? active.defaults[id]; const asset = project.assets.find(item => item.path === path); return path ? <button key={id} className="ow-resource-link" onClick={() => onNavigate('assets', asset?.id)}><span>{id === 'model' ? '模型' : '图标'}</span><code>{String(path).split(/[\\/]/).at(-1)}</code><ArrowUpRight size={13} /></button> : null; })}{Array.isArray(active.fields.abilities) && active.fields.abilities.map(id => { const ability = project.objects.find(object => object.rawcode === id || object.id === id); return <button className="ow-resource-link" key={id} onClick={() => ability ? select(ability) : notify(`未找到技能 ${id}`)}><span>技能</span><strong>{ability?.name ?? id}</strong><ArrowUpRight size={13} /></button>; })}<button className="ow-browse-resources" onClick={() => onNavigate('assets')}><ArrowUpRight size={13} />浏览资源</button></section></> : <div className="ow-empty-small">选择对象</div>}</aside>}
        </div>
        {create && <div className="ow-modal-backdrop" onMouseDown={event => { if (event.target === event.currentTarget) setCreate(null); }}><form className="ow-modal" onSubmit={event => { event.preventDefault(); createObject(); }}><header><strong>新建自定义{objectKinds.find(item => item.id === kind)?.name}</strong><button type="button" aria-label="关闭" onClick={() => setCreate(null)}><X size={16} /></button></header><label>基础对象<select value={create.templateId} onChange={event => setCreate({ ...create, templateId: event.target.value })}><option value="">空白对象</option>{kindObjects.map(object => <option key={object.id} value={object.id}>{object.name} · {object.rawcode}</option>)}</select></label><label>名称<input autoFocus value={create.name} onChange={event => setCreate({ ...create, name: event.target.value })} /></label><label>对象 ID<input maxLength={4} value={create.rawcode} onChange={event => setCreate({ ...create, rawcode: event.target.value })} /></label>{error && <div role="alert" className="ow-error">{error}</div>}<footer><button type="button" onClick={() => setCreate(null)}>取消</button><button className="ow-primary" type="submit"><Plus size={14} />创建</button></footer></form></div>}
    </section>;
}
