import { useState } from "react";
import { Grid3X3, Maximize2, Minus, Plus, Rotate3d, Sun } from "lucide-react";
import { IsoViewport } from "@/components/IsoViewport";
import { Minimap } from "@/components/Minimap";
import { Palette } from "@/components/Palette";
import { Inspector } from "@/components/Inspector";
import type { ToolKey } from "@/components/chrome";
import { cn } from "@/lib/utils";

// ── 地图工作室：视口为中心，其余皆为悬浮/边栏 ──────────────────
export function MapStudio({ tool }: { tool: ToolKey }) {
  const [yaw, setYaw] = useState(45);
  const [zoom, setZoom] = useState(1.4);
  const [showGrid, setShowGrid] = useState(false);
  const [layers, setLayers] = useState({ units: true, doodads: true, regions: true });
  const [selectedUnit, setSelectedUnit] = useState<string | null>(null);

  const showPalette = ["terrain", "unit", "doodad", "region", "camera"].includes(tool);

  return (
    <div className="flex h-full min-h-0 fade-in">
      {showPalette && <Palette tool={tool} />}

      {/* 视口区 */}
      <div className="relative min-w-0 flex-1 bg-[var(--bg-0)]">
        <IsoViewport
          yaw={yaw}
          zoom={zoom}
          showGrid={showGrid}
          layers={layers}
          selectedUnit={selectedUnit}
          onSelectUnit={setSelectedUnit}
        />

        {/* 顶部中央：当前工具提示 */}
        <div className="pointer-events-none absolute left-1/2 top-4 -translate-x-1/2">
          <div className="glass pointer-events-auto flex items-center gap-3 px-4 py-2">
            <ToolHint tool={tool} />
          </div>
        </div>

        {/* 右上：视图控制 */}
        <div className="absolute right-4 top-4 flex flex-col gap-2">
          <div className="glass flex items-center gap-1 p-1.5">
            <ViewBtn active={showGrid} onClick={() => setShowGrid((v) => !v)} title="网格">
              <Grid3X3 className="h-4 w-4" />
            </ViewBtn>
            <ViewBtn title="光照">
              <Sun className="h-4 w-4" />
            </ViewBtn>
            <ViewBtn title="最大化视口">
              <Maximize2 className="h-4 w-4" />
            </ViewBtn>
          </div>
          <div className="glass flex items-center gap-2.5 px-3 py-2.5">
            <Rotate3d className="h-4 w-4 shrink-0 text-[var(--text-3-solid)]" />
            <input
              type="range"
              min={0}
              max={360}
              value={yaw}
              onChange={(e) => setYaw(Number(e.target.value))}
              className="w-24"
            />
            <span className="w-8 text-right text-[11px] font-medium tabular-nums text-[var(--text-2)]">
              {yaw}°
            </span>
          </div>
        </div>

        {/* 左下：小地图 */}
        <div className="absolute bottom-4 left-4">
          <Minimap />
        </div>

        {/* 右下：缩放 */}
        <div className="absolute bottom-4 right-4">
          <div className="glass flex items-center gap-1 p-1.5">
            <ViewBtn onClick={() => setZoom((z) => Math.max(0.5, z - 0.15))} title="缩小">
              <Minus className="h-4 w-4" />
            </ViewBtn>
            <span className="w-11 text-center text-[11px] font-medium tabular-nums text-[var(--text-2)]">
              {Math.round(zoom * 100)}%
            </span>
            <ViewBtn onClick={() => setZoom((z) => Math.min(2.2, z + 0.15))} title="放大">
              <Plus className="h-4 w-4" />
            </ViewBtn>
          </div>
        </div>

        {/* 底部中央：操作提示 */}
        <div className="pointer-events-none absolute bottom-4 left-1/2 -translate-x-1/2">
          <div className="chip !bg-[rgba(13,15,20,0.7)] backdrop-blur-md">
            拖拽平移 · 滚轮缩放 · 点击单位选中
          </div>
        </div>
      </div>

      <Inspector
        selectedUnit={selectedUnit}
        layers={layers}
        onToggleLayer={(k) => setLayers((l) => ({ ...l, [k]: !l[k] }))}
      />
    </div>
  );
}

function ToolHint({ tool }: { tool: ToolKey }) {
  const hints: Record<string, { icon: string; title: string; desc: string }> = {
    terrain: { icon: "⛰️", title: "地形编辑", desc: "左键涂刷纹理 · 右键抬高地面" },
    unit: { icon: "🛡️", title: "放置单位", desc: "选择单位后点击视口放置" },
    doodad: { icon: "🌲", title: "放置装饰物", desc: "树木、岩石与环境装饰" },
    region: { icon: "▭", title: "区域", desc: "拖拽框选创建触发器区域" },
    camera: { icon: "🎬", title: "镜头", desc: "放置过场动画相机书签" },
  };
  const h = hints[tool] ?? { icon: "🗺️", title: "浏览", desc: "自由查看地图" };
  return (
    <>
      <span className="text-[18px]">{h.icon}</span>
      <div className="leading-tight">
        <div className="text-[12px] font-semibold">{h.title}</div>
        <div className="text-[10.5px] text-[var(--text-3-solid)]">{h.desc}</div>
      </div>
    </>
  );
}

function ViewBtn({
  children,
  active,
  onClick,
  title,
}: {
  children: React.ReactNode;
  active?: boolean;
  onClick?: () => void;
  title?: string;
}) {
  return (
    <button
      title={title}
      onClick={onClick}
      className={cn(
        "flex h-7 w-7 items-center justify-center rounded-[7px] transition",
        active
          ? "bg-[var(--accent-soft)] text-[var(--accent)]"
          : "text-[var(--text-2)] hover:bg-[var(--bg-3)] hover:text-[var(--text-1)]"
      )}
    >
      {children}
    </button>
  );
}
