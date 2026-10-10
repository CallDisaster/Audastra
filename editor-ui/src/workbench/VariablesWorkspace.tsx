import { useState } from 'react';
import { ArrowUpRight, Braces, ChevronRight, Code2, Copy, Link2, Plus, Search, Trash2, X } from 'lucide-react';
import type { ModuleProps, VariableRecord, WorkbenchProject } from './types';
import type { GraphNode } from '../studio/model';
import type { TriggerCondition } from './ecaModel';
import type { ParameterBinding } from './triggerCode';
import { containsReference } from './objectFields';
import './objects.css';

const variableTypes = [
    ['integer', '整数'], ['real', '实数'], ['boolean', '布尔'], ['string', '字符串'],
    ['unit', '单位实例'], ['unitcode', '单位类型'], ['item', '物品实例'], ['itemcode', '物品类型'],
    ['player', '玩家'], ['group', '单位组'], ['force', '玩家组'], ['location', '点'], ['rect', '矩形'],
    ['region', '区域'], ['timer', '计时器'], ['trigger', '触发器'], ['sound', '声音'], ['camerasetup', '镜头'],
    ['effect', '特效'], ['ability', '技能实例'], ['abilcode', '技能类型'], ['destructable', '可破坏物'],
    ['dialog', '对话框'], ['button', '对话框按钮'], ['quest', '任务'], ['questitem', '任务要求'],
    ['leaderboard', '排行榜'], ['multiboard', '多面板'], ['texttag', '漂浮文字'], ['hashtable', '哈希表'],
    ['gamecache', '游戏缓存'], ['unitpool', '单位池'], ['itempool', '物品池'],
];
const defaults: Record<string, string> = { integer: '0', real: '0.0', boolean: 'false', string: '', unitcode: '', itemcode: '', abilcode: '', group: 'CreateGroup()', force: 'CreateForce()', timer: 'CreateTimer()' };
const reservedWords = new Set(['if', 'then', 'else', 'elseif', 'endif', 'loop', 'endloop', 'function', 'endfunction', 'local', 'set', 'call', 'return', 'returns', 'takes', 'nothing', 'null', 'true', 'false', 'globals', 'endglobals', 'constant', 'native', 'type', 'extends', 'and', 'or', 'not', 'do', 'end', 'repeat', 'until', 'while', 'for', 'in', 'break', 'library', 'scope', 'struct']);

function validInitial(type: string, value: string): boolean {
    if (type === 'integer') return /^[-+]?\d+$/.test(value.trim()) && Number.isSafeInteger(Number(value));
    if (type === 'real') return !!value.trim() && Number.isFinite(Number(value));
    if (type === 'boolean') return value === 'true' || value === 'false';
    return true;
}

function initialForType(before: VariableRecord, type: string): string {
    if (type === before.type || ['integer', 'real'].includes(type) && ['integer', 'real'].includes(before.type) && validInitial(type, before.initial)) return before.initial;
    return defaults[type] ?? 'null';
}

function scriptMentionsVariable(content: string, name: string): boolean {
    const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    return !!name && new RegExp(`(^|[^A-Za-z0-9_])(?:udg_)?${escaped}([^A-Za-z0-9_]|$)`).test(content);
}

function VariableInput({ value, label, onCommit, type = 'text', list }: { value: string; label: string; onCommit: (text: string) => void; type?: string; list?: string }) {
    const [draft, setDraft] = useState<string | null>(null);
    return <input aria-label={label} type={type} list={list} value={draft ?? value} onChange={event => setDraft(event.target.value)} onBlur={() => { if (draft !== null && draft !== value) onCommit(draft); setDraft(null); }} onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur(); if (event.key === 'Escape') { setDraft(null); event.preventDefault(); } }} />;
}

function variableKeys(variable: VariableRecord): string[] { return [variable.id, variable.name, `udg_${variable.name}`, `$${variable.name}`]; }

function bindingsOf(node: GraphNode): Record<string, ParameterBinding> {
    if (!node.bindings || typeof node.bindings !== 'object' || Array.isArray(node.bindings)) return {};
    return Object.fromEntries(Object.entries(node.bindings).filter((entry): entry is [string, ParameterBinding] => {
        const value: unknown = entry[1];
        return !!value && typeof value === 'object' && 'mode' in value && ['constant', 'variable', 'expression', 'function'].includes(String(value.mode)) && 'value' in value && typeof value.value === 'string';
    }));
}

function directValues(node: GraphNode): Record<string, unknown> {
    const bindings = bindingsOf(node);
    return Object.fromEntries(Object.entries(node.values).filter(([field]) => !bindings[field] && !/expression|condition|formula|script|code/i.test(field)));
}

function nodeReferencesVariable(node: GraphNode, variable: VariableRecord): boolean {
    const references = (binding: ParameterBinding): boolean => binding.mode === 'variable' ? variableKeys(variable).includes(binding.value) : binding.mode === 'function' && Object.values(binding.args ?? {}).some(argument => !!argument.binding && references(argument.binding));
    return containsReference(directValues(node), variableKeys(variable)) || Object.values(bindingsOf(node)).some(references);
}

/** 条件树（触发条件或 If 节点条件）中是否引用变量：比较两侧的绑定，含函数参数中的嵌套绑定。 */
function conditionsReferenceVariable(list: unknown, variable: VariableRecord): boolean {
    const references = (binding: ParameterBinding | undefined): boolean => !!binding && (binding.mode === 'variable' ? variableKeys(variable).includes(binding.value) : binding.mode === 'function' && Object.values(binding.args ?? {}).some(argument => references(argument.binding)));
    return Array.isArray(list) && (list as TriggerCondition[]).some(item => item.kind === 'compare' ? references(item.left.binding) || references(item.right.binding) : conditionsReferenceVariable(item.children, variable));
}
function replaceConditionBindings(list: unknown, replace: (binding: ParameterBinding) => ParameterBinding): unknown {
    if (!Array.isArray(list)) return list;
    return (list as TriggerCondition[]).map(item => item.kind === 'compare'
        ? { ...item, left: item.left.binding ? { ...item.left, binding: replace(item.left.binding) } : item.left, right: item.right.binding ? { ...item.right, binding: replace(item.right.binding) } : item.right }
        : { ...item, children: replaceConditionBindings(item.children, replace) as TriggerCondition[] });
}

function expressionReferences(project: WorkbenchProject, variable: VariableRecord) {
    return project.graphs.flatMap(graph => graph.nodes.flatMap(node => {
        const bindings = bindingsOf(node);
        const candidates: { field: string; text: string; kind: string }[] = [];
        const inspect = (field: string, binding: ParameterBinding) => {
            if (binding.mode === 'expression' && scriptMentionsVariable(binding.value, variable.name)) candidates.push({ field, text: binding.value, kind: '表达式' });
            if (binding.mode === 'variable' && typeof binding.index === 'string' && scriptMentionsVariable(binding.index, variable.name)) candidates.push({ field: `${field}[下标]`, text: binding.index, kind: '数组下标' });
            if (binding.mode === 'function') for (const [argument, value] of Object.entries(binding.args ?? {})) if (value.binding) inspect(`${field}.${argument}`, value.binding);
        };
        for (const [field, binding] of Object.entries(bindings)) inspect(field, binding);
        for (const [field, value] of Object.entries(node.values)) {
            if (bindings[field] || typeof value !== 'string' || !scriptMentionsVariable(value, variable.name)) continue;
            const expression = /expression|condition|formula|script|code/i.test(field);
            if (expression || !variableKeys(variable).includes(value)) candidates.push({ field, text: value, kind: expression ? '表达式' : '参数文本' });
        }
        return candidates.map(candidate => ({ graph, node, ...candidate }));
    }));
}

function replaceReference(value: unknown, before: VariableRecord, after: VariableRecord): unknown {
    if (typeof value === 'string') {
        if (value === before.id) return after.id;
        if (value === before.name) return after.name;
        if (value === `udg_${before.name}`) return `udg_${after.name}`;
        if (value === `$${before.name}`) return `$${after.name}`;
        return value;
    }
    if (Array.isArray(value)) return value.map(item => replaceReference(item, before, after));
    if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, replaceReference(item, before, after)]));
    return value;
}

function renameGraphReferences(project: WorkbenchProject, before: VariableRecord, after: VariableRecord): WorkbenchProject['graphs'] {
    const replaceBinding = (binding: ParameterBinding): ParameterBinding => {
        if (binding.mode === 'variable') return { ...binding, value: replaceReference(binding.value, before, after) as string };
        if (binding.mode === 'function') return { ...binding, args: Object.fromEntries(Object.entries(binding.args ?? {}).map(([field, argument]) => [field, { ...argument, ...(argument.binding ? { binding: replaceBinding(argument.binding) } : {}) }])) };
        return binding;
    };
    return project.graphs.map(graph => ({ ...graph, ...(graph.conditions ? { conditions: replaceConditionBindings(graph.conditions, replaceBinding) } : {}), nodes: graph.nodes.map(node => {
        const variableValues = directValues(node);
        const bindings = node.bindings && typeof node.bindings === 'object' && !Array.isArray(node.bindings) ? node.bindings : {};
        const updatedBindings = Object.fromEntries(Object.entries(bindings).map(([field, raw]) => {
            const binding = bindingsOf(node)[field];
            return [field, binding ? replaceBinding(binding) : raw];
        }));
        return { ...node, values: { ...node.values, ...replaceReference(variableValues, before, after) as Record<string, unknown> }, ...(node.bindings ? { bindings: updatedBindings } : {}), ...(node.conditions ? { conditions: replaceConditionBindings(node.conditions, replaceBinding) } : {}) };
    }) }));
}

export function VariablesWorkspace({ project, onChange, notify, onNavigate, focusId }: ModuleProps) {
    const [query, setQuery] = useState('');
    const [typeFilter, setTypeFilter] = useState('全部类型');
    const [folder, setFolder] = useState('全部变量');
    const [selection, setSelection] = useState<{ focus?: string; id?: string }>({ focus: focusId, id: focusId });
    const [newVariable, setNewVariable] = useState<VariableRecord | null>(null);
    const [pending, setPending] = useState<{ before: VariableRecord; after?: VariableRecord; delete?: boolean; replacement: string } | null>(null);
    const [error, setError] = useState('');
    const activeId = selection.focus === focusId ? selection.id : focusId;
    const active = project.variables.find(variable => variable.id === activeId || variable.name === activeId) ?? project.variables[0];
    const folders = [...new Set(project.variables.map(variable => variable.folder || '未分类'))];
    const visible = project.variables.filter(variable => `${variable.name} ${variable.type}`.toLowerCase().includes(query.toLowerCase()) && (folder === '全部变量' || (variable.folder || '未分类') === folder) && (typeFilter === '全部类型' || variable.type === typeFilter));
    const references = (variable: VariableRecord) => project.graphs.flatMap(graph => [
        ...(conditionsReferenceVariable(graph.conditions, variable) ? [{ graph, node: undefined, where: '触发条件' }] : []),
        ...graph.nodes.filter(node => nodeReferencesVariable(node, variable) || conditionsReferenceVariable(node.conditions, variable)).map(node => ({ graph, node, where: nodeReferencesVariable(node, variable) ? '触发器' : '条件' })),
    ]);
    const activeRefs = active ? references(active) : [];
    const activeExpressions = active ? expressionReferences(project, active) : [];
    const pendingExpressions = pending ? expressionReferences(project, pending.before) : [];
    const scriptRefs = active ? project.scripts.filter(script => scriptMentionsVariable(script.content, active.name)) : [];
    const select = (variable: VariableRecord) => setSelection({ focus: focusId, id: variable.id });

    const validateName = (name: string, id?: string): string => {
        if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) return '名称须以字母或下划线开头，仅含字母、数字和下划线';
        if (reservedWords.has(name.toLowerCase())) return '名称与语言保留字冲突';
        if (project.variables.some(variable => variable.id !== id && variable.name.toLowerCase() === name.toLowerCase())) return '变量名称已存在';
        return '';
    };
    const saveVariable = (before: VariableRecord, after: VariableRecord, label: string) => {
        const issue = validateName(after.name, after.id);
        if (issue) { notify(issue); return; }
        if (!validInitial(after.type, after.initial)) { notify('初始值与变量类型不匹配'); return; }
        if (!Number.isSafeInteger(after.size) || after.size < 1 || after.size > 1000000) { notify('原型支持的初始化范围为 1–1000000'); return; }
        const structural = before.name !== after.name || before.type !== after.type || before.array !== after.array;
        if (structural && (references(before).length > 0 || expressionReferences(project, before).length > 0 || project.scripts.some(script => scriptMentionsVariable(script.content, before.name)))) { setPending({ before, after, replacement: '' }); setError(''); return; }
        onChange({ ...project, variables: project.variables.map(variable => variable.id === before.id ? after : variable) }, label);
    };
    const newRecord = (copy = false) => {
        let index = 1; const prefix = copy && active ? `${active.name}_copy` : 'Variable';
        let name = `${prefix}${String(index).padStart(2, '0')}`;
        while (project.variables.some(variable => variable.name === name)) { index++; name = `${prefix}${String(index).padStart(2, '0')}`; }
        setNewVariable(copy && active ? { ...active, id: crypto.randomUUID(), name } : { id: crypto.randomUUID(), name, type: 'integer', array: false, size: 1, initial: '0', folder: folder === '全部变量' ? '全局变量' : folder }); setError('');
    };
    const addVariable = () => {
        if (!newVariable) return;
        const issue = validateName(newVariable.name);
        if (issue) { setError(issue); return; }
        if (!validInitial(newVariable.type, newVariable.initial)) { setError('初始值与变量类型不匹配'); return; }
        if (!Number.isSafeInteger(newVariable.size) || newVariable.size < 1 || newVariable.size > 1000000) { setError('原型支持的初始化范围为 1–1000000'); return; }
        onChange({ ...project, variables: [...project.variables, newVariable] }, `新建变量 ${newVariable.name}`);
        select(newVariable); setNewVariable(null); setQuery(''); setTypeFilter('全部类型'); setFolder('全部变量');
    };
    const applyPending = () => {
        if (!pending) return;
        const replacement = project.variables.find(variable => variable.id === pending.replacement);
        if (pending.delete && references(pending.before).length && !replacement) { setError('请选择替换变量'); return; }
        const after = pending.after ?? replacement;
        onChange({ ...project, variables: pending.delete ? project.variables.filter(variable => variable.id !== pending.before.id) : project.variables.map(variable => variable.id === pending.before.id ? pending.after! : variable),
            graphs: after ? renameGraphReferences(project, pending.before, after) : project.graphs }, pending.delete ? `删除变量 ${pending.before.name}` : `修改变量 ${pending.before.name}`);
        setPending(null);
    };

    return <section className="vw-workspace" aria-label="全局变量">
        <aside className="vw-sidebar"><div className="ow-panel-title"><span>变量</span><Braces size={15} /></div><button className={`vw-folder ${folder === '全部变量' ? 'is-active' : ''}`} onClick={() => setFolder('全部变量')}><Braces size={15} /><span>全部变量</span><small>{project.variables.length}</small></button><div className="vw-section-label">分类</div>{folders.map(item => <button className={`vw-folder ${folder === item ? 'is-active' : ''}`} key={item} onClick={() => setFolder(item)}><ChevronRight size={12} /><span>{item}</span><small>{project.variables.filter(variable => (variable.folder || '未分类') === item).length}</small></button>)}<div className="vw-sidebar-bottom"><Code2 size={13} />{project.settings.language} · 全局作用域</div></aside>
        <main className="vw-main"><div className="ow-toolbar"><button onClick={() => newRecord()}><Plus size={14} />新建变量</button><button disabled={!active} onClick={() => newRecord(true)}><Copy size={14} />复制</button><button disabled={!active} onClick={() => { if (active) { setPending({ before: active, delete: true, replacement: '' }); setError(''); } }}><Trash2 size={14} />删除</button><span className="ow-toolbar-spacer" /><span className="vw-scope-badge">全局</span></div>
            <header className="vw-heading"><h2>全局变量 <span>{project.variables.length}</span></h2><div><label className="ow-search"><Search size={14} /><input aria-label="搜索变量" placeholder="搜索名称 / 类型" value={query} onChange={event => setQuery(event.target.value)} /></label><select aria-label="筛选变量类型" value={typeFilter} onChange={event => setTypeFilter(event.target.value)}><option>全部类型</option>{variableTypes.filter(([id]) => project.variables.some(variable => variable.type === id)).map(([id, name]) => <option key={id} value={id}>{name}</option>)}</select></div></header>
            <div className="vw-table-scroll"><table className="vw-table"><thead><tr><th>名称</th><th>类型</th><th>数组</th><th>初始值</th><th>引用</th></tr></thead><tbody>{visible.map(variable => <tr key={variable.id} className={active?.id === variable.id ? 'is-active' : ''} onClick={() => select(variable)} draggable onDragStart={event => { event.dataTransfer.setData('application/x-forge-variable', JSON.stringify({ id: variable.id, name: variable.name, type: variable.type })); event.dataTransfer.setData('text/plain', variable.name); }}><td><span className={`vw-type-dot vw-type-${variable.type}`} /><VariableInput value={variable.name} label={`变量名称 ${variable.name}`} onCommit={name => saveVariable(variable, { ...variable, name: name.trim() }, '变量改名')} /></td><td><select aria-label={`变量类型 ${variable.name}`} value={variable.type} onChange={event => { const type = event.target.value; saveVariable(variable, { ...variable, type, initial: initialForType(variable, type) }, '修改变量类型'); }}>{!variableTypes.some(([id]) => id === variable.type) && <option value={variable.type}>{variable.type}</option>}{variableTypes.map(([id, name]) => <option key={id} value={id}>{name}</option>)}</select></td><td><input type="checkbox" aria-label={`数组 ${variable.name}`} checked={variable.array} onChange={event => saveVariable(variable, { ...variable, array: event.target.checked }, '修改数组形式')} />{variable.array && <span className="vw-array-size">[{variable.size}]</span>}</td><td>{variable.type === 'boolean' ? <select aria-label={`初始值 ${variable.name}`} value={variable.initial} onChange={event => saveVariable(variable, { ...variable, initial: event.target.value }, '修改初始值')}><option value="false">假</option><option value="true">真</option></select> : <VariableInput value={variable.initial} label={`初始值 ${variable.name}`} list={['unitcode', 'itemcode', 'abilcode'].includes(variable.type) ? `vw-initial-${variable.type}` : undefined} onCommit={initial => saveVariable(variable, { ...variable, initial }, '修改初始值')} />}</td><td><button className="vw-ref-count" title="查看引用" onClick={() => select(variable)}><Link2 size={12} />{references(variable).length}{expressionReferences(project, variable).length > 0 && <span className="vw-review-count">+{expressionReferences(project, variable).length} 待核对</span>}</button></td></tr>)}</tbody></table>{!visible.length && <div className="ow-empty"><Braces size={30} /><strong>{project.variables.length ? '没有匹配的变量' : '还没有全局变量'}</strong><button className="ow-primary" onClick={() => newRecord()}><Plus size={14} />新建变量</button></div>}</div><footer className="ow-status"><span>{visible.length} 个变量</span><span>双击值或直接输入 · Enter 确认</span></footer>
        </main>
        <aside className="vw-details"><div className="ow-panel-title"><span>变量详情</span><SettingsIcon /></div>{active ? <><section><h3><span className={`vw-type-dot vw-type-${active.type}`} />{active.name}</h3><dl><dt>代码名称</dt><dd><code>{project.settings.language.toLowerCase() === 'lua' ? active.name : `udg_${active.name}`}</code></dd><dt>脚本类型</dt><dd><code>{['unitcode', 'itemcode', 'abilcode'].includes(active.type) ? 'integer' : active.type}</code></dd><dt>作用域</dt><dd>全局</dd></dl><label className="vw-detail-field">分类<VariableInput value={active.folder} label="变量分类" onCommit={folder => saveVariable(active, { ...active, folder }, '修改变量分类')} /></label>{active.array && <label className="vw-detail-field">初始化范围<VariableInput type="number" value={String(active.size)} label="初始化范围" onCommit={size => saveVariable(active, { ...active, size: Number(size) }, '修改初始化范围')} /><small>索引 0 – {active.size - 1}</small></label>}</section><section><h3><Link2 size={13} />图内绑定 <span>{activeRefs.length}</span></h3>{activeRefs.map(({ graph, node, where }) => <button key={`${graph.id}/${node?.id ?? 'conditions'}`} className="ow-reference" onClick={() => onNavigate('triggers', node?.id ?? graph.id)}><span>{where}</span><strong>{graph.name}</strong><ArrowUpRight size={13} /></button>)}{!activeRefs.length && <div className="ow-muted">没有图内绑定</div>}</section>{activeExpressions.length > 0 && <section><h3>表达式待核对 <span>{activeExpressions.length}</span></h3>{activeExpressions.map(({ graph, node, field, text, kind }) => <button key={`${graph.id}/${node.id}/${field}`} className="vw-expression-reference" onClick={() => onNavigate('triggers', graph.id)}><span>{kind} · {graph.name}</span><strong>{field}</strong><code>{text}</code><ArrowUpRight size={12} /></button>)}</section>}{!!scriptRefs.length && <section><h3>脚本文本匹配 <span>{scriptRefs.length}</span></h3>{scriptRefs.map(script => <button key={script.id} className="ow-reference" onClick={() => onNavigate('triggers', script.id)}><span>待核对</span><strong>{script.name}</strong><ArrowUpRight size={13} /></button>)}</section>}</> : <div className="ow-empty-small">选择变量</div>}</aside>
        {['unitcode', 'itemcode', 'abilcode'].map(type => <datalist key={type} id={`vw-initial-${type}`}>{project.objects.filter(object => object.kind === ({ unitcode: 'unit', itemcode: 'item', abilcode: 'ability' }[type])).map(object => <option key={object.id} value={object.rawcode}>{object.name}</option>)}</datalist>)}
        {newVariable && <div className="ow-modal-backdrop"><form className="ow-modal" onSubmit={event => { event.preventDefault(); addVariable(); }}><header><strong>新建变量</strong><button type="button" aria-label="关闭" onClick={() => setNewVariable(null)}><X size={16} /></button></header><label>名称<input autoFocus value={newVariable.name} onChange={event => setNewVariable({ ...newVariable, name: event.target.value })} /></label><label>类型<select value={newVariable.type} onChange={event => setNewVariable({ ...newVariable, type: event.target.value, initial: defaults[event.target.value] ?? 'null' })}>{variableTypes.map(([id, name]) => <option value={id} key={id}>{name} · {id}</option>)}</select></label><label className="vw-checkbox-label"><input type="checkbox" checked={newVariable.array} onChange={event => setNewVariable({ ...newVariable, array: event.target.checked })} />数组</label>{newVariable.array && <label>初始化范围<input type="number" min={1} value={newVariable.size} onChange={event => setNewVariable({ ...newVariable, size: Number(event.target.value) })} /></label>}<label>初始值<input value={newVariable.initial} onChange={event => setNewVariable({ ...newVariable, initial: event.target.value })} /></label><label>分类<input value={newVariable.folder} onChange={event => setNewVariable({ ...newVariable, folder: event.target.value })} /></label>{error && <div className="ow-error" role="alert">{error}</div>}<footer><button type="button" onClick={() => setNewVariable(null)}>取消</button><button type="submit" className="ow-primary"><Plus size={14} />创建</button></footer></form></div>}
        {pending && <div className="ow-modal-backdrop"><div className="ow-modal"><header><strong>{pending.delete ? '删除变量' : '更新变量引用'}</strong><button aria-label="关闭" onClick={() => setPending(null)}><X size={16} /></button></header><div className="vw-change-summary"><code>{pending.before.name}</code>{pending.after && <><ChevronRight size={14} /><code>{pending.after.name}</code></>}</div><dl className="vw-impact-summary"><dt>图内绑定</dt><dd>{references(pending.before).length}</dd><dt>待核对表达式</dt><dd>{pendingExpressions.length}</dd><dt>更新方式</dt><dd>{pending.delete ? '替换后删除' : '同步名称与 ID 绑定'}</dd></dl>{pending.after && pending.before.type !== pending.after.type && <div className="vw-change-summary"><code>{pending.before.type}</code><ChevronRight size={14} /><code>{pending.after.type}</code></div>}{pending.delete && references(pending.before).length > 0 && <label>替换变量<select value={pending.replacement} onChange={event => setPending({ ...pending, replacement: event.target.value })}><option value="">选择同类型变量</option>{project.variables.filter(variable => variable.id !== pending.before.id && variable.type === pending.before.type && variable.array === pending.before.array).map(variable => <option key={variable.id} value={variable.id}>{variable.name}</option>)}</select></label>}{pendingExpressions.length > 0 && <div className="vw-unresolved"><strong>表达式原文保留，需手动核对</strong><div className="vw-expression-impact">{pendingExpressions.map(({ graph, node, field, text }) => <div key={`${graph.id}/${node.id}/${field}`}><span>{graph.name} · {field}</span><code>{text}</code></div>)}</div></div>}{project.scripts.some(script => scriptMentionsVariable(script.content, pending.before.name)) && <div className="vw-unresolved">脚本文本匹配需手动核对</div>}{error && <div className="ow-error" role="alert">{error}</div>}<footer><button onClick={() => setPending(null)}>取消</button><button className="ow-primary" onClick={applyPending}>{pending.delete ? '删除' : '应用修改'}</button></footer></div></div>}
    </section>;
}

function SettingsIcon() { return <Braces size={14} />; }
