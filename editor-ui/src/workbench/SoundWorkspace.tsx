import { useEffect, useRef, useState } from 'react';
import { ArrowUpRight, AudioLines, Check, ChevronRight, CircleDot, Copy, FileAudio, FolderOpen, ListMusic, Music2, Pause, Play, Plus, Search, Square, Trash2, Upload, Volume2, X } from 'lucide-react';
import type { EditorAsset, ModuleProps, SoundRecord } from './types';
import { assetKind, assetPathKey, embeddedSize, MAX_EMBEDDED_BYTES, readAssetFile } from './resourceHelpers';
import './resources.css';

const channels = ['通用', '单位选择', '单位回应', '单位移动', '单位就绪', '战斗', '错误', '音乐', '界面', '环境', '动画', '建造', '出生', '火焰'];
const makeSound = (assetId: string, name: string): SoundRecord => ({ id: crypto.randomUUID(), name, assetId, music: false, loop: false, spatial: false, volume: 100, pitch: 1, minDistance: 600, maxDistance: 10000, fadeIn: 0, fadeOut: 0, channel: '通用' });
const timeLabel = (seconds: number) => Number.isFinite(seconds) ? `${Math.floor(seconds / 60)}:${Math.floor(seconds % 60).toString().padStart(2, '0')}.${Math.floor(seconds * 10) % 10}` : '0:00.0';

function SoundPlayer({ sound, asset, notify }: { sound: SoundRecord; asset?: EditorAsset; notify: ModuleProps['notify'] }) {
    const audioRef = useRef<HTMLAudioElement>(null);
    const [playing, setPlaying] = useState(false);
    const [position, setPosition] = useState(0);
    const [duration, setDuration] = useState(0);
    const [previewVolume, setPreviewVolume] = useState(70);
    const [waveform, setWaveform] = useState<number[]>([]);
    const [failedSource, setFailedSource] = useState<string>();
    const src = asset?.dataUrl;
    const failed = !!src && src === failedSource;
    useEffect(() => {
        const audio = audioRef.current;
        return () => { audio?.pause(); };
    }, []);
    useEffect(() => {
        const audio = audioRef.current;
        if (!audio) return;
        audio.volume = Math.max(0, Math.min(1, sound.volume / 127 * previewVolume / 100));
        audio.playbackRate = Math.max(0.25, Math.min(4, sound.pitch));
        audio.preservesPitch = false;
    }, [previewVolume, sound.volume, sound.pitch]);
    useEffect(() => {
        if (!src) return;
        let active = true;
        const context = new AudioContext();
        fetch(src).then(response => response.arrayBuffer()).then(buffer => context.decodeAudioData(buffer)).then(buffer => {
            if (!active) return;
            const samples = buffer.getChannelData(0);
            const count = 100;
            const step = Math.max(1, Math.floor(samples.length / count));
            const values = Array.from({ length: count }, (_, index) => {
                let peak = 0;
                for (let i = index * step; i < Math.min(samples.length, (index + 1) * step); i++) peak = Math.max(peak, Math.abs(samples[i]));
                return peak;
            });
            const maximum = Math.max(...values, 0.001);
            setWaveform(values.map(value => value / maximum));
        }).catch(() => { /* 浏览器不支持解码时仍保留原生音频播放能力。 */ }).finally(() => { void context.close(); });
        return () => { active = false; };
    }, [src]);
    const toggle = async () => {
        const audio = audioRef.current;
        if (!audio || !src) return;
        if (!audio.paused) audio.pause();
        else {
            try { await audio.play(); }
            catch { notify('无法播放此文件，请检查音频格式'); setFailedSource(src); }
        }
    };
    const stop = () => { const audio = audioRef.current; if (audio) { audio.pause(); audio.currentTime = 0; setPosition(0); } };
    return <section className="sw-player"><div className="sw-player-title"><span>文件试听</span><span>{asset?.name || '未绑定资源'}</span></div>
        {src && <audio ref={audioRef} src={src} preload="metadata" loop={sound.loop} onPlay={() => setPlaying(true)} onPause={() => setPlaying(false)} onEnded={() => setPlaying(false)} onLoadedMetadata={event => setDuration(event.currentTarget.duration)} onTimeUpdate={event => setPosition(event.currentTarget.currentTime)} onError={() => setFailedSource(src)} />}
        <div className={`sw-waveform ${!src || failed ? 'unavailable' : ''}`}>
            {waveform.length ? <div className="sw-wave-bars" aria-label="音频波形">{waveform.map((peak, index) => <i key={index} className={duration && index / waveform.length <= position / duration ? 'played' : ''} style={{ height: `${Math.max(2, peak * 88)}%` }} />)}</div> : <div className="sw-no-wave"><AudioLines size={34} /><span>{!src ? '未嵌入音频文件' : failed ? '此音频无法播放' : '读取音频波形'}</span></div>}
            {src && !failed && <input className="sw-seek" aria-label="试听进度" type="range" min="0" max={duration || 1} step="0.01" value={position} onChange={event => { if (audioRef.current) { audioRef.current.currentTime = Number(event.target.value); setPosition(Number(event.target.value)); } }} />}
        </div><div className="sw-player-controls"><button className="sw-play" aria-label={playing ? '暂停试听' : '播放试听'} disabled={!src || failed} onClick={() => void toggle()}>{playing ? <Pause size={17} /> : <Play size={17} fill="currentColor" />}</button><button aria-label="停止试听" disabled={!src} onClick={stop}><Square size={14} /></button><span className="sw-time">{timeLabel(position)} <em>/ {timeLabel(duration)}</em></span><label className="sw-preview-volume"><Volume2 size={16} /><input aria-label="试听音量" type="range" min="0" max="100" value={previewVolume} onChange={event => setPreviewVolume(Number(event.target.value))} /><span>{previewVolume}%</span></label></div>
    </section>;
}

export function SoundWorkspace({ project, onChange, notify, onNavigate, focusId }: ModuleProps) {
    const [selection, setSelection] = useState({ focusId, id: focusId || project.sounds[0]?.id || '' });
    const [query, setQuery] = useState('');
    const [filter, setFilter] = useState('全部');
    const [tab, setTab] = useState<'basic' | 'spatial' | 'refs'>('basic');
    const [creating, setCreating] = useState(false);
    const [name, setName] = useState('');
    const [assetId, setAssetId] = useState('');
    const [deleteId, setDeleteId] = useState<string | null>(null);
    const [busy, setBusy] = useState(false);
    const upload = useRef<HTMLInputElement>(null);
    const latest = useRef(project);
    useEffect(() => { latest.current = project; }, [project]);
    const selectedId = selection.focusId === focusId ? selection.id : focusId || '';
    const selected = project.sounds.find(sound => sound.id === selectedId);
    const audioAssets = project.assets.filter(asset => assetKind(asset) === 'audio');
    const asset = project.assets.find(item => item.id === selected?.assetId);
    const visible = project.sounds.filter(sound => (filter === '全部' || filter === '音乐' && sound.music || filter === '声音' && !sound.music || filter === '空间声音' && sound.spatial) && `${sound.name} ${project.assets.find(item => item.id === sound.assetId)?.name || ''}`.toLowerCase().includes(query.toLowerCase()));
    const select = (id: string) => setSelection({ focusId, id });
    const refs = selected ? project.graphs.flatMap(graph => graph.nodes.filter(node => Object.values(node.values).some(value => value === selected.id || value === selected.name)).map(node => ({ id: `${graph.id}.${node.id}`, graph: graph.id, name: graph.name, node: project.definitions.find(def => def.id === node.definition)?.title || node.definition }))) : [];

    function patch(values: Partial<SoundRecord>) {
        if (!selected) return;
        const changedName = values.name?.trim();
        if (changedName !== undefined && (!changedName || project.sounds.some(sound => sound.id !== selected.id && sound.name === changedName))) { notify('声音名称不能为空或重复'); return; }
        const changed = { ...selected, ...values };
        if (changed.maxDistance < changed.minDistance) { notify('最大距离不能小于最小距离'); return; }
        const graphs = values.name ? project.graphs.map(graph => ({ ...graph, nodes: graph.nodes.map(node => ({ ...node, values: Object.fromEntries(Object.entries(node.values).map(([key, value]) => [key, value === selected.name ? values.name : value])) })) })) : project.graphs;
        onChange({ ...project, graphs, sounds: project.sounds.map(sound => sound.id === selected.id ? changed : sound) }, '编辑声音定义');
    }
    function create() {
        const value = name.trim();
        if (!value || project.sounds.some(sound => sound.name === value)) { notify('请输入不重复的声音名称'); return; }
        if (!audioAssets.some(item => item.id === assetId)) { notify('请选择音频资源'); return; }
        const sound = makeSound(assetId, value);
        onChange({ ...project, sounds: [...project.sounds, sound] }, '新建声音定义'); select(sound.id); setCreating(false); setFilter('全部'); setQuery('');
    }
    async function importAudio(files: FileList) {
        if (!files.length || busy) return;
        setBusy(true);
        try {
            const assets: EditorAsset[] = [];
            const sounds: SoundRecord[] = [];
            for (const file of Array.from(files)) {
                if (assetKind({ name: file.name, type: file.type }) !== 'audio') throw new Error(`${file.name} 不是音频文件`);
                const dataUrl = await readAssetFile(file);
                const current = latest.current;
                if (embeddedSize([...current.assets, ...assets]) + dataUrl.length > MAX_EMBEDDED_BYTES) throw new Error('工程内嵌资源超过 3 MB');
                const stem = file.name.replace(/\.[^.]+$/, '');
                const ext = file.name.split('.').pop();
                let path = `Audio\\${file.name}`;
                let index = 2;
                while ([...current.assets, ...assets].some(item => item.scope === '地图' && assetPathKey(item.path) === assetPathKey(path))) path = `Audio\\${stem}_${index++}.${ext}`;
                const imported: EditorAsset = { id: crypto.randomUUID(), name: file.name, path, type: file.type || 'audio', size: file.size, scope: '地图', source: file.name, dataUrl };
                let name = stem; index = 2;
                while ([...current.sounds, ...sounds].some(sound => sound.name === name)) name = `${stem}_${index++}`;
                assets.push(imported); sounds.push(makeSound(imported.id, name));
            }
            const current = latest.current;
            onChange({ ...current, assets: [...current.assets, ...assets], sounds: [...current.sounds, ...sounds] }, '导入声音');
            select(sounds[0].id); setFilter('全部'); setQuery(''); notify(`已导入 ${sounds.length} 个声音`);
        } catch (error) { notify(error instanceof Error ? error.message : '音频导入失败'); }
        finally { setBusy(false); if (upload.current) upload.current.value = ''; }
    }
    const numberField = (label: string, key: 'volume' | 'pitch' | 'minDistance' | 'maxDistance' | 'fadeIn' | 'fadeOut', min = 0, max = 1000000, step = 1, unit = '') => selected && <label className="sw-property"><span>{label}</span><div className="sw-number"><input key={`${selected.id}-${key}-${selected[key]}`} aria-label={label} type="number" defaultValue={selected[key]} min={min} max={max} step={step} onBlur={event => { const value = Number(event.target.value); if (!Number.isFinite(value) || value < min || value > max || event.target.value === '') { event.target.value = String(selected[key]); notify(`${label}超出有效范围`); } else if (value !== selected[key]) { patch({ [key]: value }); event.target.value = String(selected[key]); } }} onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur(); }} />{unit && <small>{unit}</small>}</div></label>;

    return <div className="rw-workspace sw-workspace"><input hidden type="file" accept="audio/*,.wav,.mp3,.ogg,.flac" multiple ref={upload} onChange={event => { if (event.target.files) void importAudio(event.target.files); }} />
        <aside className="rw-sidebar"><div className="rw-section-title">声音库</div>{['全部', '声音', '音乐', '空间声音'].map(item => <button key={item} className={`rw-nav ${filter === item ? 'active' : ''}`} onClick={() => setFilter(item)}>{item === '音乐' ? <Music2 size={16} /> : item === '空间声音' ? <CircleDot size={16} /> : <AudioLines size={16} />}<span>{item === '全部' ? '全部声音' : item}</span><small>{project.sounds.filter(sound => item === '全部' || item === '音乐' && sound.music || item === '声音' && !sound.music || item === '空间声音' && sound.spatial).length}</small></button>)}<div className="rw-section-title rw-gap">资源</div><button className="rw-nav" onClick={() => onNavigate('assets')}><FolderOpen size={16} /><span>音频文件</span><small>{audioAssets.length}</small></button><div className="rw-sidebar-bottom"><span>{project.sounds.length} 个声音定义</span></div></aside>
        <main className="rw-main"><header className="rw-toolbar"><div className="rw-breadcrumb">声音 <ChevronRight size={13} /><strong>{filter === '全部' ? '全部声音' : filter}</strong></div><div className="rw-toolbar-actions"><button disabled={busy} onClick={() => upload.current?.click()}><Upload size={15} />{busy ? '正在读取' : '导入音频'}</button><button className="rw-primary" onClick={() => { setName(''); setAssetId(audioAssets[0]?.id || ''); setCreating(true); }}><Plus size={16} />新建声音</button></div></header>
            <div className="rw-filterbar"><label className="rw-search"><Search size={15} /><input aria-label="搜索声音" value={query} onChange={event => setQuery(event.target.value)} placeholder="搜索声音名称或文件" /></label><span className="rw-result-count">{visible.length} 项</span></div>
            <div className="sw-sound-list"><div className="sw-list-head"><span>声音</span><span>资源文件</span><span>模式</span><span>通道</span></div>{visible.map(sound => <button className={`sw-sound-row ${selected?.id === sound.id ? 'selected' : ''}`} key={sound.id} onClick={() => select(sound.id)}><span className="sw-sound-name"><span className="sw-sound-icon">{sound.music ? <Music2 size={19} /> : <AudioLines size={19} />}</span><strong>{sound.name}</strong></span><span className={!project.assets.some(item => item.id === sound.assetId) ? 'sw-missing' : ''}>{project.assets.find(item => item.id === sound.assetId)?.name || '资源已丢失'}</span><span className="sw-mode">{sound.spatial ? '3D' : '2D'}{sound.loop && <small>循环</small>}</span><span>{sound.channel}</span></button>)}{!visible.length && <div className="rw-empty"><ListMusic size={38} /><strong>没有匹配的声音</strong><button onClick={() => upload.current?.click()}><Upload size={15} />导入音频</button></div>}</div>
            {selected ? <div className="sw-detail"><div className="sw-detail-heading"><div><span className="sw-eyebrow">声音定义</span><h2>{selected.name}</h2></div><div className="sw-detail-badges"><span>{selected.music ? '音乐' : '音效'}</span><span>{selected.spatial ? '3D' : '2D'}</span></div></div><SoundPlayer key={`${selected.id}-${selected.assetId}-${asset?.dataUrl?.length}`} sound={selected} asset={asset} notify={notify} /><div className="sw-file-link"><FileAudio size={16} /><span>{asset?.path || '未找到资源'}</span><button disabled={!asset} onClick={() => onNavigate('assets', asset?.id)}>定位资源<ArrowUpRight size={14} /></button></div>{selected.spatial && <div className="sw-spatial-summary"><div className="sw-spatial-orbit"><i /><i /><CircleDot size={20} /></div><div><strong>空间声音</strong><span>最小距离 {selected.minDistance} · 最大距离 {selected.maxDistance}</span></div><button onClick={() => setTab('spatial')}>编辑距离<ChevronRight size={14} /></button></div>}</div> : <div className="rw-empty"><AudioLines size={42} /><span>选择声音</span></div>}
            <footer className="rw-main-footer"><span>{selected ? `声音 ID · ${selected.id}` : '声音编辑器'}</span><span>文件试听</span></footer>
        </main>
        <aside className="rw-inspector"><div className="rw-inspector-heading"><span>声音属性</span>{selected && <button aria-label="复制声音定义" onClick={() => { let name = `${selected.name}_copy`; let index = 2; while (project.sounds.some(sound => sound.name === name)) name = `${selected.name}_copy${index++}`; const copy = { ...selected, id: crypto.randomUUID(), name }; onChange({ ...project, sounds: [...project.sounds, copy] }, '复制声音定义'); select(copy.id); }}><Copy size={14} /></button>}</div>{selected ? <><div className="sw-tabs">{([{ id: 'basic', label: '基本' }, { id: 'spatial', label: '空间' }, { id: 'refs', label: '引用' }] as const).map(item => <button className={tab === item.id ? 'active' : ''} key={item.id} onClick={() => setTab(item.id)}>{item.label}{item.id === 'refs' && <small>{refs.length}</small>}</button>)}</div><div className="rw-detail-body">
                {tab === 'basic' && <><label className="rw-field"><span>变量名</span><input key={`${selected.id}-${selected.name}`} aria-label="声音变量名" defaultValue={selected.name} onBlur={event => { if (event.target.value.trim() !== selected.name) { patch({ name: event.target.value.trim() }); event.target.value = selected.name; } }} onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur(); }} /></label><label className="rw-field"><span>音频资源</span><select aria-label="声音音频资源" value={selected.assetId} onChange={event => patch({ assetId: event.target.value })}>{!asset && <option value={selected.assetId}>资源已丢失</option>}{audioAssets.map(item => <option value={item.id} key={item.id}>{item.name}</option>)}</select></label><div className="sw-property-group"><label className="sw-toggle"><span>作为音乐</span><input type="checkbox" checked={selected.music} onChange={event => patch({ music: event.target.checked })} /></label><label className="sw-toggle"><span>循环播放</span><input type="checkbox" checked={selected.loop} onChange={event => patch({ loop: event.target.checked })} /></label><label className="sw-toggle"><span>3D 声音</span><input type="checkbox" checked={selected.spatial} onChange={event => patch({ spatial: event.target.checked })} /></label></div><div className="rw-subheading">播放</div>{numberField('音量', 'volume', 0, 127)}{numberField('音调', 'pitch', 0.25, 4, 0.05, '×')}<label className="sw-property"><span>通道</span><select aria-label="声音通道" value={selected.channel} onChange={event => patch({ channel: event.target.value })}>{!channels.includes(selected.channel) && <option>{selected.channel}</option>}{channels.map(channel => <option key={channel}>{channel}</option>)}</select></label><div className="rw-subheading">渐变</div>{numberField('淡入速率', 'fadeIn')}{numberField('淡出速率', 'fadeOut')}</>}
                {tab === 'spatial' && <><label className="sw-toggle"><span>3D 声音</span><input type="checkbox" checked={selected.spatial} onChange={event => patch({ spatial: event.target.checked })} /></label><fieldset className="sw-spatial-fields" disabled={!selected.spatial}><div className="rw-subheading">距离</div>{numberField('最小距离', 'minDistance')}{numberField('最大距离', 'maxDistance')}<svg className="sw-distance-chart" viewBox="0 0 240 140" role="img" aria-label="声音距离参数"><path d="M24 16V112H222" fill="none" stroke="var(--line)" /><path d={`M24 34H${24 + 160 * Math.min(0.8, selected.minDistance / Math.max(1, selected.maxDistance))}L218 109`} fill="none" stroke="var(--accent)" strokeWidth="2" /><text x="24" y="133">0</text><text x="173" y="133">{selected.maxDistance}</text><text x="29" y="24">距离示意</text></svg></fieldset></>}
                {tab === 'refs' && <><div className="rw-subheading">触发器引用 <span>{refs.length}</span></div>{refs.map(ref => <button className="rw-reference" key={ref.id} onClick={() => onNavigate('triggers', ref.graph)}><span><strong>{ref.name}</strong><small>{ref.node}</small></span><ArrowUpRight size={15} /></button>)}{!refs.length && <div className="rw-muted-line">未发现精确引用</div>}<div className="rw-footnote">动态代码引用不在此列表中</div></>}
                <div className="sw-delete"><button className="rw-danger" onClick={() => setDeleteId(selected.id)}><Trash2 size={14} />删除声音定义</button></div></div></> : <div className="rw-empty"><AudioLines size={30} /><span>选择声音</span></div>}</aside>
        {creating && <div className="rw-modal-backdrop"><section className="rw-modal" role="dialog" aria-modal="true" aria-label="新建声音"><header><h3>新建声音</h3><button aria-label="关闭新建声音" onClick={() => setCreating(false)}><X size={17} /></button></header><label className="rw-field"><span>变量名</span><input autoFocus aria-label="新声音名称" placeholder="NewSound" value={name} onChange={event => setName(event.target.value)} /></label><label className="rw-field"><span>音频资源</span><select aria-label="新声音资源" value={assetId} onChange={event => setAssetId(event.target.value)}><option value="">选择音频文件</option>{audioAssets.map(item => <option key={item.id} value={item.id}>{item.name}</option>)}</select></label>{!audioAssets.length && <button onClick={() => { setCreating(false); upload.current?.click(); }}><Upload size={15} />导入音频</button>}<footer><button onClick={() => setCreating(false)}>取消</button><button className="rw-primary" onClick={create}><Check size={15} />创建</button></footer></section></div>}
        {deleteId && <div className="rw-modal-backdrop"><section className="rw-modal" role="dialog" aria-modal="true" aria-label="删除声音"><header><h3>删除声音定义</h3><button aria-label="关闭删除声音" onClick={() => setDeleteId(null)}><X size={17} /></button></header><p>{project.sounds.find(sound => sound.id === deleteId)?.name}</p>{refs.length > 0 && <div className="rw-warning">{refs.length} 处触发器引用将失效</div>}<div className="rw-muted-line">保留音频资源文件。</div><footer><button onClick={() => setDeleteId(null)}>取消</button><button className="rw-danger-solid" onClick={() => { onChange({ ...project, sounds: project.sounds.filter(sound => sound.id !== deleteId) }, '删除声音定义'); setDeleteId(null); notify('声音定义已删除，源文件已保留'); }}>删除定义</button></footer></section></div>}
    </div>;
}
