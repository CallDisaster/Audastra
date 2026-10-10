import { useEffect, useState } from 'react';
import type { ModuleProps } from './types';
import { findObject } from './objectCatalog';
import { WorldOutliner } from './WorldOutliner';
import { WorldViewport } from './WorldViewport';
import { WorldInspector } from './WorldInspector';
import { AssetBrowser } from './AssetBrowser';
import { frameSelection, select, useWorld } from './worldStore';
import './world.css';

// 世界工作区：场景大纲 | 视口 + 资源浏览器 | 属性。三处共用同一选择集（worldStore）。
export function WorldWorkspace({ project, onChange, notify, onNavigate, focusId }: ModuleProps) {
  const world = useWorld();
  // 从引用、搜索、修改审阅跳来时选中并对准目标。
  useEffect(() => { if (focusId) { select([focusId]); frameSelection(); } }, [focusId]);
  const template = world.template ? findObject(project, world.template) : undefined;
  // 大纲/属性可收起，窄屏或打开右侧工具面板时把空间让给视口；偏好只存本机。
  const [panels, setPanels] = useState<{ outliner: boolean; inspector: boolean }>(() => { try { return { outliner: true, inspector: true, ...JSON.parse(localStorage.getItem('forge-world-panels') ?? '{}') }; } catch { return { outliner: true, inspector: true }; } });
  const toggle = (key: 'outliner' | 'inspector') => setPanels(current => { const next = { ...current, [key]: !current[key] }; try { localStorage.setItem('forge-world-panels', JSON.stringify(next)); } catch { /* 界面偏好 */ } return next; });
  return <div className="ww-root">
    {panels.outliner && <WorldOutliner project={project} />}
    <div className="ww-center">
      <WorldViewport project={project} onChange={onChange} notify={notify} templateObject={template} panels={panels} onTogglePanel={toggle} />
      <AssetBrowser project={project} onNavigate={onNavigate} />
    </div>
    {panels.inspector && <WorldInspector project={project} onChange={onChange} onNavigate={onNavigate} />}
  </div>;
}
