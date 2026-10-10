import { useMemo } from 'react';
import { ArrowUpRight, Camera, Copy, Grid2x2Check, Link2, MousePointer2, SquareDashed, Trash2 } from 'lucide-react';
import type { ModuleId, WorkbenchProject, WorldInstance } from './types';
import type { WorldEntity } from './worldModel';
import { duplicateEntities, entityKindLabels, entityName, patchEntities, playerOf, players, removeEntities, snap, worldEntities } from './worldModel';
import { select, useWorld } from './worldStore';
import { referencesTo } from './references';
import { ObjectPortrait } from './ObjectPortrait';
import { EmptyState } from './ui';

// 属性区：单选显示完整属性，多选时相同的值直接显示、不同的值显示“多个值”，改动一次写入全部所选（一次撤销）。
// 实例属性与物体类型分开：这里只改放置实例；类型字段通过“打开物体定义”跳到物编。

type Mixed<T> = { value: T | undefined; mixed: boolean };
function common<T>(values: T[]): Mixed<T> { const first = values[0]; return values.every(v => v === first) ? { value: first, mixed: false } : { value: undefined, mixed: true }; }

function NumberField({ label, state, onCommit, min, max, step, suffix }: { label: string; state: Mixed<number>; onCommit: (value: number) => void; min?: number; max?: number; step?: number; suffix?: string }) {
  const key = `${label}:${state.mixed ? 'mixed' : state.value}`;
  return <label className="wi-field"><span>{label}</span><span className="wi-input">
    <input key={key} type="number" aria-label={label} defaultValue={state.mixed ? '' : state.value} placeholder={state.mixed ? '多个值' : ''} min={min} max={max} step={step ?? 'any'}
      onBlur={event => { const raw = event.currentTarget.value.trim(); if (raw === '') { event.currentTarget.value = state.mixed ? '' : String(state.value); return; } const n = Number(raw); if (!Number.isFinite(n) || (min !== undefined && n < min) || (max !== undefined && n > max)) { event.currentTarget.setAttribute('aria-invalid', 'true'); return; } event.currentTarget.removeAttribute('aria-invalid'); if (state.mixed || n !== state.value) onCommit(n); }}
      onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur(); if (event.key === 'Escape') { event.currentTarget.value = state.mixed ? '' : String(state.value); event.currentTarget.blur(); } }} />{suffix && <small>{suffix}</small>}</span></label>;
}

export function WorldInspector({ project, onChange, onNavigate }: { project: WorkbenchProject; onChange: (next: WorkbenchProject, label: string) => void; onNavigate: (module: ModuleId, id?: string) => void }) {
  const world = useWorld();
  const entities = useMemo(() => worldEntities(project).filter(e => world.selection.includes(e.id)), [project, world.selection]);
  const instances = entities.filter((e): e is Extract<WorldEntity, { instance: WorldInstance }> => e.kind !== 'region' && e.kind !== 'camera');
  const ids = instances.map(e => e.id);
  const setInstances = (patch: Partial<WorldInstance>, label: string) => onChange(patchEntities(project, ids, { instance: patch }), instances.length > 1 ? `${label}（${instances.length} 个）` : label);
  if (!entities.length) return <aside className="wi-root" aria-label="属性"><div className="wb-panel-title">属性</div><EmptyState icon={<MousePointer2 size={24} />} title="在视口或大纲中选择对象" /><div className="wi-tips"><p><kbd>Shift</kbd> 加选 · <kbd>Ctrl</kbd> 取消 · 拖空白处框选</p><p><kbd>Ctrl</kbd> 拖动已选对象：朝向指向鼠标</p><p><kbd>Ctrl C</kbd>/<kbd>Ctrl V</kbd> 复制到光标处 · <kbd>Ctrl D</kbd> 副本</p></div></aside>;

  const single = entities.length === 1 ? entities[0] : undefined;
  const refs = single ? referencesTo(project, single.id).slice(0, 8) : [];
  const kinds = [...new Set(entities.map(e => e.kind))];
  const units = instances.filter(e => e.kind === 'unit');
  const heroes = units.filter(e => /^[A-Z]/.test(e.object?.rawcode ?? e.instance.objectId));
  const owner = common(instances.map(e => e.instance.owner));

  return <aside className="wi-root" aria-label="属性">
    <div className="wb-panel-title">属性<span>{entities.length > 1 ? `${entities.length} 个` : entityKindLabels[entities[0].kind]}</span></div>
    {single && single.kind !== 'region' && single.kind !== 'camera' ? <div className="wi-identity">
      {single.object ? <ObjectPortrait object={single.object} project={project} size={44} /> : <span className="wi-missing">?</span>}
      <div><strong>{entityName(single)}</strong><code>{single.object?.rawcode ?? single.instance.objectId}</code><small>{single.object ? (single.object.custom ? '地图物体' : '原版物体') : '物体不存在，保存前需处理'}</small></div>
    </div> : single ? <div className="wi-identity">{single.kind === 'region' ? <SquareDashed size={30} /> : <Camera size={30} />}<div><strong>{entityName(single)}</strong><code>{single.id}</code></div></div>
      : <div className="wi-identity is-multi"><div><strong>已选择 {entities.length} 个对象</strong><div className="wi-kinds">{kinds.map(kind => <span key={kind}>{entityKindLabels[kind]} {entities.filter(e => e.kind === kind).length}</span>)}</div></div></div>}

    {instances.length > 0 && <section className="wi-section"><h4>放置实例{instances.length !== entities.length && <small>{instances.length} 个</small>}</h4>
      <label className="wi-field"><span>所属玩家</span><span className="wi-input"><i className="wi-player" style={{ background: owner.mixed ? 'transparent' : playerOf(owner.value!).color }} /><select aria-label="所属玩家" value={owner.mixed ? '' : owner.value} onChange={event => setInstances({ owner: Number(event.target.value) }, '修改所属玩家')}>{owner.mixed && <option value="">多个值</option>}{players.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}</select></span></label>
      <div className="wi-pair">
        <NumberField label="X" state={common(instances.map(e => e.instance.x))} onCommit={x => setInstances({ x }, '修改位置')} />
        <NumberField label="Y" state={common(instances.map(e => e.instance.y))} onCommit={y => setInstances({ y }, '修改位置')} />
      </div>
      <div className="wi-pair">
        <NumberField label="朝向" suffix="°" min={0} max={360} state={common(instances.map(e => e.instance.rotation))} onCommit={rotation => setInstances({ rotation: rotation % 360 }, '修改朝向')} />
        <NumberField label="缩放" min={0.1} max={10} step={0.05} state={common(instances.map(e => e.instance.scale ?? 1))} onCommit={scale => setInstances({ scale }, '修改缩放')} />
      </div>
      <label className="wi-field"><span>高度</span><span className="wi-input wi-readonly" title="原版编辑器按地形重新计算放置高度，复制高度规则仍在取证（H1），暂不开放手工修改">随地形<small>只读</small></span></label>
    </section>}
    {units.length > 0 && <section className="wi-section"><h4>单位状态{units.length !== instances.length && <small>{units.length} 个单位</small>}</h4>
      <div className="wi-pair">
        <NumberField label="生命" suffix="%" min={1} max={100} state={common(units.map(e => e.instance.life ?? 100))} onCommit={life => setInstances({ life }, '修改生命百分比')} />
        <NumberField label="魔法" suffix="%" min={0} max={100} state={common(units.map(e => e.instance.mana ?? 100))} onCommit={mana => setInstances({ mana }, '修改魔法百分比')} />
      </div>
      {heroes.length === units.length && <NumberField label="英雄等级" min={1} max={10} step={1} state={common(heroes.map(e => e.instance.level ?? 1))} onCommit={level => setInstances({ level: Math.round(level) }, '修改英雄等级')} />}
    </section>}
    {single?.kind === 'region' && <section className="wi-section"><h4>区域</h4>
      <label className="wi-field"><span>名称</span><span className="wi-input"><input key={single.region.name} aria-label="区域名称" defaultValue={single.region.name} onBlur={event => { const name = event.currentTarget.value.trim(); if (!name) { event.currentTarget.value = single.region.name; return; } if (name !== single.region.name) onChange(patchEntities(project, [single.id], { region: { name } }), '重命名区域'); }} onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur(); }} /></span></label>
      <div className="wi-pair">{(['x', 'y'] as const).map(key => <NumberField key={key} label={key === 'x' ? '左' : '下'} state={{ value: single.region[key], mixed: false }} onCommit={v => onChange(patchEntities(project, [single.id], { region: { [key]: v } }), '移动区域')} />)}</div>
      <div className="wi-pair">{(['width', 'height'] as const).map(key => <NumberField key={key} label={key === 'width' ? '宽' : '高'} min={16} state={{ value: single.region[key], mixed: false }} onCommit={v => onChange(patchEntities(project, [single.id], { region: { [key]: v } }), '调整区域大小')} />)}</div>
      <label className="wi-field"><span>颜色</span><span className="wi-input wi-colors">{['#8ad6b0', '#7ebff1', '#cfb67c', '#e58c8c', '#b5a0d5'].map(color => <button key={color} aria-label={`区域颜色 ${color}`} className={(single.region.color ?? '#8ad6b0') === color ? 'is-active' : ''} style={{ background: color }} onClick={() => onChange(patchEntities(project, [single.id], { region: { color } }), '修改区域颜色')} />)}</span></label>
    </section>}
    {single?.kind === 'camera' && <section className="wi-section"><h4>镜头</h4>
      <div className="wi-pair"><NumberField label="X" state={{ value: single.camera.x, mixed: false }} onCommit={x => onChange(patchEntities(project, [single.id], { camera: { x } }), '移动镜头')} /><NumberField label="Y" state={{ value: single.camera.y, mixed: false }} onCommit={y => onChange(patchEntities(project, [single.id], { camera: { y } }), '移动镜头')} /></div>
      <div className="wi-pair"><NumberField label="距离" min={100} state={{ value: single.camera.distance, mixed: false }} onCommit={distance => onChange(patchEntities(project, [single.id], { camera: { distance } }), '修改镜头距离')} /><NumberField label="视野" suffix="°" min={20} max={120} state={{ value: single.camera.fov, mixed: false }} onCommit={fov => onChange(patchEntities(project, [single.id], { camera: { fov } }), '修改镜头视野')} /></div>
      <div className="wi-pair"><NumberField label="旋转" suffix="°" state={{ value: single.camera.rotation, mixed: false }} onCommit={rotation => onChange(patchEntities(project, [single.id], { camera: { rotation } }), '修改镜头旋转')} /><NumberField label="俯仰" suffix="°" state={{ value: single.camera.aoa, mixed: false }} onCommit={aoa => onChange(patchEntities(project, [single.id], { camera: { aoa } }), '修改镜头俯仰')} /></div>
    </section>}

    <section className="wi-section wi-actions">
      {single && single.kind !== 'region' && single.kind !== 'camera' && single.object && <button onClick={() => onNavigate('objects', single.object!.custom ? single.object!.id : `${single.object!.kind}:${single.object!.rawcode}`)}><ArrowUpRight size={14} />打开物体定义</button>}
      <button onClick={() => { let next = project; for (const e of entities) next = e.kind === 'region' ? patchEntities(next, [e.id], { region: { x: snap(e.region.x, world.step), y: snap(e.region.y, world.step) } }) : e.kind === 'camera' ? patchEntities(next, [e.id], { camera: { x: snap(e.camera.x, world.step), y: snap(e.camera.y, world.step) } }) : patchEntities(next, [e.id], { instance: { x: snap(e.instance.x, world.step), y: snap(e.instance.y, world.step) } }); onChange(next, '对齐到网格'); }}><Grid2x2Check size={14} />对齐到网格（{world.step}）</button>
      <button onClick={() => { const result = duplicateEntities(project, world.selection, world.step * 2, -world.step * 2); onChange(result.project, `创建 ${result.ids.length} 个副本`); select(result.ids); }}><Copy size={14} />创建副本<kbd>Ctrl D</kbd></button>
      <button className="wb-danger" onClick={() => { onChange(removeEntities(project, world.selection), `删除 ${world.selection.length} 个对象`); select([]); }}><Trash2 size={14} />删除<kbd>Del</kbd></button>
    </section>
    {single && <section className="wi-section"><h4><Link2 size={12} />被引用<small>{refs.length ? `${refs.length} 处` : '静态未检出'}</small></h4>
      {refs.map((ref, i) => <button key={i} className="wi-ref" onClick={() => onNavigate(ref.module, ref.target)}><span>{ref.label}</span><small>{ref.detail}</small><ArrowUpRight size={12} /></button>)}
      {!refs.length && <p className="wi-note">只统计工程数据中的直接引用；自定义脚本中的动态引用不在此列，不能据此判断可以安全删除。</p>}
    </section>}
  </aside>;
}
