import { useState } from 'react';
import { AlertTriangle, Ban, Check, Circle, Loader2, Play } from 'lucide-react';
import type { ModuleId, WorkbenchProject } from './types';
import { validateProject } from '../studio/model';
import { withCatalog } from './triggerCatalog';
import { generateTrigger } from './triggerCode';
import type { ScriptLanguage } from './triggerCode';

// 一键测试：检查 → 生成脚本 → 另存测试副本 → 启动游戏。任何一步失败就停下并说明原因，不跳过、不假报成功。
// 原型只能真实执行前两步；写出与启动需要原生端的 MapSaveService 与游戏启动器，这里如实显示“未连接”。

type StepState = 'idle' | 'running' | 'ok' | 'warn' | 'fail' | 'blocked';
type Step = { id: string; title: string; state: StepState; detail?: string; items?: { text: string; module?: ModuleId; target?: string }[] };
const settingsKey = 'forge-testrun-r08';
const initialSteps = (): Step[] => [
  { id: 'check', title: '检查工程', state: 'idle' }, { id: 'script', title: '生成地图脚本', state: 'idle' },
  { id: 'write', title: '另存测试副本', state: 'idle' }, { id: 'launch', title: '启动游戏并进入地图', state: 'idle' },
];
function readSettings() { try { return { client: 'classic', windowed: true, keep: true, player: 1, ...JSON.parse(localStorage.getItem(settingsKey) ?? '{}') }; } catch { return { client: 'classic', windowed: true, keep: true, player: 1 }; } }

export function TestRunPanel({ project, onNavigate }: { project: WorkbenchProject; onNavigate: (module: ModuleId, id?: string) => void }) {
  const [settings, setSettings] = useState(readSettings);
  const [steps, setSteps] = useState<Step[]>(initialSteps);
  const [runs, setRuns] = useState<{ at: string; result: string }[]>([]);
  const running = steps.some(s => s.state === 'running');
  const update = (patch: Partial<typeof settings>) => { const next = { ...settings, ...patch }; setSettings(next); try { localStorage.setItem(settingsKey, JSON.stringify(next)); } catch { /* 界面偏好 */ } };

  async function run() {
    const list = initialSteps(); setSteps(list);
    const set = (id: string, patch: Partial<Step>) => { const i = list.findIndex(s => s.id === id); list[i] = { ...list[i], ...patch }; setSteps([...list]); };
    const pause = () => new Promise(resolve => setTimeout(resolve, 260));
    set('check', { state: 'running' }); await pause();
    const issues = validateProject(withCatalog(project));
    const errors = issues.filter(i => i.level === 'error');
    set('check', { state: errors.length ? 'fail' : issues.length ? 'warn' : 'ok', detail: issues.length ? `${errors.length} 个错误，${issues.length - errors.length} 个提示` : '结构检查通过', items: issues.slice(0, 8).map(i => ({ text: i.message, module: 'triggers', target: i.graph })) });
    if (errors.length) { setRuns(r => [{ at: new Date().toLocaleTimeString(), result: '检查未通过' }, ...r].slice(0, 5)); return; }
    set('script', { state: 'running' }); await pause();
    try {
      const language = (['JASS', 'vJass', 'Lua'].includes(project.settings.language) ? project.settings.language : 'JASS') as ScriptLanguage;
      const results = project.graphs.filter(g => g.enabled !== false).map(g => ({ graph: g, ...generateTrigger(g, project, language) }));
      const lines = results.reduce((sum, r) => sum + r.content.split('\n').length, 0), warnings = results.flatMap(r => r.warnings.map(w => ({ text: `${r.graph.name}：${w}`, module: 'triggers' as ModuleId, target: r.graph.id })));
      set('script', { state: warnings.length ? 'warn' : 'ok', detail: `${language} · ${results.length} 个触发器 · ${lines} 行${warnings.length ? ` · ${warnings.length} 条警告` : ''}`, items: warnings.slice(0, 6) });
    } catch (error) { set('script', { state: 'fail', detail: error instanceof Error ? error.message : '脚本生成失败' }); setRuns(r => [{ at: new Date().toLocaleTimeString(), result: '脚本生成失败' }, ...r].slice(0, 5)); return; }
    set('write', { state: 'blocked', detail: '原型未连接原生后端。原生端经 MapSaveService 写出到测试目录（不覆盖原图），重开校验后才进入下一步。' });
    set('launch', { state: 'blocked', detail: settings.client === 'classic' ? '将以 war3.exe -loadfile <测试副本>' + (settings.windowed ? ' -window' : '') + ' 启动；退出后回到编辑器。' : '将通过 YDWE 的测试入口启动；需要本机 YDWE 目录。' });
    setRuns(r => [{ at: new Date().toLocaleTimeString(), result: '停在“另存测试副本”：未连接后端' }, ...r].slice(0, 5));
  }

  const icon = (state: StepState) => state === 'running' ? <Loader2 size={14} className="tr-spin" /> : state === 'ok' ? <Check size={14} /> : state === 'warn' ? <AlertTriangle size={14} /> : state === 'fail' ? <AlertTriangle size={14} /> : state === 'blocked' ? <Ban size={14} /> : <Circle size={14} />;
  return <>
    <div className="tp-scroll">
      <section className="tr-settings">
        <label>游戏<select value={settings.client} onChange={event => update({ client: event.target.value })}><option value="classic">经典 Warcraft III 1.27a</option><option value="ydwe">YDWE 测试入口</option></select></label>
        <label>测试玩家<select value={settings.player} onChange={event => update({ player: Number(event.target.value) })}>{Array.from({ length: 12 }, (_, i) => <option key={i} value={i + 1}>玩家 {i + 1}</option>)}</select></label>
        <label className="pw-check"><input type="checkbox" checked={settings.windowed} onChange={event => update({ windowed: event.target.checked })} />窗口模式</label>
        <label className="pw-check"><input type="checkbox" checked={settings.keep} onChange={event => update({ keep: event.target.checked })} />保留测试副本</label>
        <p className="tp-note">测试副本写到工程的测试目录，原地图保持不变。</p>
      </section>
      <ol className="tr-steps">{steps.map(step => <li key={step.id} className={`is-${step.state}`}>
        <span className="tr-icon">{icon(step.state)}</span><div><strong>{step.title}</strong>{step.detail && <p>{step.detail}</p>}
          {step.items?.map((item, i) => <button key={i} className="tr-item" onClick={() => item.module && onNavigate(item.module, item.target)}>{item.text}</button>)}</div>
      </li>)}</ol>
      {runs.length > 0 && <section className="tr-history"><h4>最近运行</h4>{runs.map((r, i) => <div key={i}><span>{r.at}</span>{r.result}</div>)}</section>}
    </div>
    <footer className="tp-foot"><span className="tp-note">F5 · 测试地图</span><div className="wb-spacer" /><button className="wb-primary" disabled={running} onClick={run}><Play size={14} />开始测试</button></footer>
  </>;
}
