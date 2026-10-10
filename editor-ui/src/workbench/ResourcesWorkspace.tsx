import { useEffect, useRef, useState } from 'react';
import { ArrowDownToLine, ArrowUpRight, AudioLines, Box, Check, ChevronRight, File, FileCode2, Folder, FolderOpen, Globe2, Image, LayoutGrid, List, Search, Trash2, Upload, X } from 'lucide-react';
import type { EditorAsset, ModuleProps } from './types';
import { assetKind, assetKinds, assetPathKey, assetReferences, embeddedSize, MAX_EMBEDDED_BYTES, normalizeAssetPath, pathError, readableSize, readAssetFile } from './resourceHelpers';
import type { AssetKind } from './resourceHelpers';
import './resources.css';

function AssetIcon({ asset, size = 18 }: { asset: EditorAsset; size?: number }) {
    const kind = assetKind(asset);
    const Icon = kind === 'image' ? Image : kind === 'audio' ? AudioLines : kind === 'model' ? Box : kind === 'text' ? FileCode2 : File;
    return <Icon size={size} />;
}

function AssetPreview({ asset }: { asset: EditorAsset }) {
    const [text, setText] = useState('');
    const [failedImage, setFailedImage] = useState<string>();
    const audioRef = useRef<HTMLAudioElement>(null);
    const kind = assetKind(asset);
    useEffect(() => {
        const audio = audioRef.current;
        return () => { audio?.pause(); };
    }, []);
    useEffect(() => {
        if (kind !== 'text' || !asset.dataUrl) return;
        let alive = true;
        fetch(asset.dataUrl).then(response => response.text()).then(value => { if (alive) setText(value.slice(0, 60000)); }).catch(() => { if (alive) setText('无法读取文本'); });
        return () => { alive = false; };
    }, [asset.dataUrl, kind]);
    if (!asset.dataUrl) return <div className="rw-preview-empty"><AssetIcon asset={asset} size={42} /><span>未嵌入文件内容</span><small>{asset.name}</small></div>;
    if (kind === 'image' && failedImage !== asset.dataUrl) return <div className="rw-image-preview"><img alt={asset.name} src={asset.dataUrl} onError={() => setFailedImage(asset.dataUrl)} /></div>;
    if (kind === 'audio') return <div className="rw-audio-preview"><AudioLines size={42} /><audio ref={audioRef} controls preload="metadata" src={asset.dataUrl} /></div>;
    if (kind === 'text') return <pre className="rw-text-preview">{text || '正在读取…'}</pre>;
    return <div className="rw-preview-empty"><AssetIcon asset={asset} size={42} /><span>{kind === 'model' ? '模型预览未接入' : '此格式暂不支持预览'}</span><small>{asset.name}</small></div>;
}

export function ResourcesWorkspace({ project, onChange, notify, onNavigate, focusId }: ModuleProps) {
    const [selection, setSelection] = useState({ focusId, id: focusId || project.assets[0]?.id || '' });
    const [query, setQuery] = useState('');
    const [scope, setScope] = useState('全部');
    const [kind, setKind] = useState<AssetKind | 'all'>('all');
    const [view, setView] = useState<'list' | 'grid'>('grid');
    const [pathDraft, setPathDraft] = useState<{ id: string; value: string } | null>(null);
    const [deleteId, setDeleteId] = useState<string | null>(null);
    const [busy, setBusy] = useState(false);
    const upload = useRef<HTMLInputElement>(null);
    const replace = useRef<HTMLInputElement>(null);
    const latest = useRef(project);
    useEffect(() => { latest.current = project; }, [project]);
    const selectedId = selection.focusId === focusId ? selection.id : focusId || '';
    const selected = project.assets.find(asset => asset.id === selectedId);
    const refs = selected ? assetReferences(project, selected) : [];
    const visible = project.assets.filter(asset => (scope === '全部' || asset.scope === scope) && (kind === 'all' || assetKind(asset) === kind) && `${asset.name} ${asset.path} ${asset.type}`.toLowerCase().includes(query.toLowerCase()));
    const deleteAsset = project.assets.find(asset => asset.id === deleteId);
    const deleteRefs = deleteAsset ? assetReferences(project, deleteAsset) : [];
    const select = (id: string) => { setSelection({ focusId, id }); setPathDraft(null); };

    async function importFiles(files: FileList | File[], replacingId?: string) {
        if (!files.length || busy) return;
        setBusy(true);
        const added: EditorAsset[] = [];
        let lastId = '';
        try {
            for (const file of Array.from(files)) {
                const dataUrl = await readAssetFile(file);
                const current = latest.current;
                const original = replacingId ? current.assets.find(asset => asset.id === replacingId) : undefined;
                const existing = current.assets.filter(asset => asset.id !== replacingId);
                if (embeddedSize([...existing, ...added]) + dataUrl.length > MAX_EMBEDDED_BYTES) throw new Error('工程内嵌资源超过 3 MB，请减少文件大小');
                const targetScope = original?.scope === '游戏' ? '地图' : original?.scope || (scope === '战役' ? '战役' : '地图');
                let path = original?.path || `war3mapImported\\${file.name}`;
                if (!original) {
                    let suffix = 2;
                    const stem = file.name.replace(/\.[^.]+$/, '');
                    const ext = file.name.includes('.') ? `.${file.name.split('.').pop()}` : '';
                    while ([...existing, ...added].some(asset => asset.scope === targetScope && assetPathKey(asset.path) === assetPathKey(path))) path = `war3mapImported\\${stem}_${suffix++}${ext}`;
                }
                const asset: EditorAsset = { id: original?.id || crypto.randomUUID(), name: original?.name || file.name, type: file.type || file.name.split('.').pop()?.toUpperCase() || 'file', size: file.size, path, scope: targetScope, dataUrl, source: file.name };
                added.push(asset); lastId = asset.id;
            }
            const current = latest.current;
            onChange({ ...current, assets: [...current.assets.filter(asset => asset.id !== replacingId), ...added] }, replacingId ? '替换资源文件' : `导入 ${added.length} 个资源`);
            setScope('全部'); setKind('all'); setQuery(''); select(lastId);
            notify(replacingId ? '文件已替换，包内路径和引用已保留' : `已导入 ${added.length} 个文件`);
        } catch (error) { notify(error instanceof Error ? error.message : '导入失败'); }
        finally { setBusy(false); if (upload.current) upload.current.value = ''; if (replace.current) replace.current.value = ''; }
    }

    function savePath() {
        if (!selected || !pathDraft) return;
        const path = normalizeAssetPath(pathDraft.value);
        const error = pathError(path);
        if (error) { notify(error); return; }
        if (project.assets.some(asset => asset.id !== selected.id && asset.scope === selected.scope && assetPathKey(asset.path) === assetPathKey(path))) { notify('此来源已存在同路径资源'); return; }
        // 精确的路径引用同步更新，动态构造的代码字符串保持原样。
        const replaceValue = (value: unknown): unknown => {
            if (typeof value === 'string') return assetPathKey(value) === assetPathKey(selected.path) ? path : value;
            if (Array.isArray(value)) return value.map(replaceValue);
            if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, replaceValue(item)]));
            return value;
        };
        onChange({ ...project, assets: project.assets.map(asset => asset.id === selected.id ? { ...asset, path, name: path.split('\\').pop() || asset.name } : asset), objects: project.objects.map(object => ({ ...object, fields: replaceValue(object.fields) as typeof object.fields })), graphs: project.graphs.map(graph => ({ ...graph, nodes: graph.nodes.map(node => ({ ...node, values: replaceValue(node.values) as typeof node.values })) })) }, '修改资源路径');
        setPathDraft(null); notify('路径及精确引用已更新');
    }

    return <div className="rw-workspace" onDragOver={event => { if (event.dataTransfer.types.includes('Files')) event.preventDefault(); }} onDrop={event => { if (event.dataTransfer.files.length) { event.preventDefault(); void importFiles(event.dataTransfer.files); } }}>
        <input hidden type="file" multiple ref={upload} onChange={event => { if (event.target.files) void importFiles(event.target.files); }} />
        <input hidden type="file" ref={replace} onChange={event => { if (event.target.files && selected) void importFiles(event.target.files, selected.id); }} />
        <aside className="rw-sidebar"><div className="rw-section-title">资源库</div>
            {(['全部', '地图', '战役', '游戏'] as const).map(item => <button className={`rw-nav ${scope === item ? 'active' : ''}`} key={item} onClick={() => setScope(item)}>{item === '游戏' ? <Globe2 size={16} /> : <FolderOpen size={16} />}<span>{item === '全部' ? '全部资源' : `${item}资源`}</span><small>{project.assets.filter(asset => item === '全部' || asset.scope === item).length}</small></button>)}
            <div className="rw-section-title rw-gap">类型</div>
            <button className={`rw-nav ${kind === 'all' ? 'active' : ''}`} onClick={() => setKind('all')}><Folder size={15} /><span>全部类型</span></button>
            {assetKinds.map(item => <button className={`rw-nav ${kind === item.id ? 'active' : ''}`} key={item.id} onClick={() => setKind(item.id)}><span className={`rw-kind-dot ${item.id}`} /><span>{item.label}</span><small>{project.assets.filter(asset => assetKind(asset) === item.id).length}</small></button>)}
            <div className="rw-sidebar-bottom"><span>{project.assets.length} 个资源</span><span>{readableSize(project.assets.reduce((sum, asset) => sum + asset.size, 0))}</span></div>
        </aside>
        <main className="rw-main"><header className="rw-toolbar"><div className="rw-breadcrumb">资源 <ChevronRight size={13} /><strong>{scope === '全部' ? '全部资源' : `${scope}资源`}</strong></div><div className="rw-toolbar-actions"><button className="rw-primary" disabled={busy} onClick={() => upload.current?.click()}><Upload size={15} />{busy ? '正在读取' : '导入文件'}</button></div></header>
            <div className="rw-filterbar"><label className="rw-search"><Search size={15} /><input aria-label="搜索资源" value={query} onChange={event => setQuery(event.target.value)} placeholder="搜索名称、类型或路径" /></label><span className="rw-result-count">{visible.length} 项</span><div className="rw-segment"><button aria-label="资源网格" className={view === 'grid' ? 'active' : ''} onClick={() => setView('grid')}><LayoutGrid size={16} /></button><button aria-label="资源列表" className={view === 'list' ? 'active' : ''} onClick={() => setView('list')}><List size={17} /></button></div></div>
            <div className={`rw-assets rw-${view}`}>
                {view === 'list' && <div className="rw-list-head"><span>名称</span><span>包内路径</span><span>来源</span><span>大小</span></div>}
                {visible.map(asset => <button key={asset.id} className={`rw-asset ${selected?.id === asset.id ? 'selected' : ''}`} onClick={() => select(asset.id)} draggable onDragStart={event => { event.dataTransfer.setData('application/x-forge-asset', JSON.stringify({ id: asset.id, path: asset.path, type: asset.type })); event.dataTransfer.setData('text/plain', asset.path); }}><span className={`rw-asset-art ${assetKind(asset)}`}>{assetKind(asset) === 'image' && asset.dataUrl && /\.(png|jpe?g|webp|gif|svg|bmp)$/i.test(asset.name) ? <img src={asset.dataUrl} alt="" /> : <AssetIcon asset={asset} size={view === 'grid' ? 36 : 18} />}</span><span className="rw-asset-name">{asset.name}<small>{asset.type}</small></span><span className="rw-asset-path">{asset.path}</span><span className="rw-asset-scope">{asset.scope}</span><span className="rw-asset-size">{readableSize(asset.size)}</span></button>)}
                {!visible.length && <div className="rw-empty"><FolderOpen size={36} /><strong>{query || kind !== 'all' ? '没有匹配的资源' : '暂无资源'}</strong><button onClick={() => upload.current?.click()}><Upload size={15} />导入文件</button></div>}
            </div><footer className="rw-main-footer"><span>可拖入文件导入</span><span>内嵌 {readableSize(embeddedSize(project.assets))} / 3 MB</span></footer>
        </main>
        <aside className="rw-inspector"><div className="rw-inspector-heading"><span>资源详情</span>{selected && <span className="rw-tag">{selected.scope}</span>}</div>{selected ? <><div className="rw-preview"><AssetPreview key={`${selected.id}-${selected.dataUrl?.length}`} asset={selected} /></div><div className="rw-detail-body"><h3>{selected.name}</h3><dl className="rw-meta"><dt>类型</dt><dd>{selected.type}</dd><dt>大小</dt><dd>{readableSize(selected.size)}</dd><dt>源文件</dt><dd>{selected.source || '—'}</dd></dl><label className="rw-field"><span>包内路径</span><textarea aria-label="资源包内路径" rows={3} value={pathDraft?.id === selected.id ? pathDraft.value : selected.path} onChange={event => setPathDraft({ id: selected.id, value: event.target.value })} /></label>{pathDraft?.id === selected.id && <div className="rw-inline-actions"><button className="rw-primary" onClick={savePath}><Check size={14} />应用路径</button><button onClick={() => setPathDraft(null)}>取消</button></div>}<div className="rw-resource-actions"><button disabled={busy} onClick={() => replace.current?.click()}><Upload size={14} />替换文件</button><button disabled={!selected.dataUrl} onClick={() => { const link = document.createElement('a'); link.href = selected.dataUrl!; link.download = selected.name; link.click(); }}><ArrowDownToLine size={14} />导出</button><button aria-label="删除资源" className="rw-danger" onClick={() => setDeleteId(selected.id)}><Trash2 size={15} /></button></div><div className="rw-subheading"><span>引用</span><span>{refs.length}</span></div>{refs.map(ref => <button key={ref.id} className="rw-reference" onClick={() => onNavigate(ref.module, ref.target)}><span><strong>{ref.label}</strong><small>{ref.detail}</small></span><ArrowUpRight size={15} /></button>)}{!refs.length && <div className="rw-muted-line">未发现精确引用</div>}<div className="rw-footnote">动态路径引用不在此列表中</div></div></> : <div className="rw-empty"><File size={30} /><span>选择资源</span></div>}</aside>
        {deleteAsset && <div className="rw-modal-backdrop"><section className="rw-modal" role="dialog" aria-modal="true" aria-label="删除资源"><header><h3>删除资源</h3><button aria-label="关闭删除确认" onClick={() => setDeleteId(null)}><X size={17} /></button></header><p>{deleteAsset.name}</p>{deleteRefs.length ? <><div className="rw-warning">{deleteRefs.length} 处引用将失效</div>{deleteRefs.map(ref => <div key={ref.id} className="rw-delete-ref">{ref.label}<small>{ref.detail}</small></div>)}</> : <div className="rw-muted-line">未发现精确引用，动态路径仍可能使用此文件。</div>}<footer><button onClick={() => setDeleteId(null)}>取消</button><button className="rw-danger-solid" onClick={() => { onChange({ ...project, assets: project.assets.filter(asset => asset.id !== deleteAsset.id) }, '删除资源'); setDeleteId(null); notify('资源已删除，可撤销恢复'); }}>删除文件</button></footer></section></div>}
    </div>;
}
