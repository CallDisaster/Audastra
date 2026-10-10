import { useState } from "react";
import { Eye, EyeOff, Layers } from "lucide-react";
import { cn } from "@/lib/utils";

// ── 右侧检查器：选中对象属性 + 图层 ────────────────────────────
export function Inspector({
  selectedUnit,
  layers,
  onToggleLayer,
}: {
  selectedUnit: string | null;
  layers: { units: boolean; doodads: boolean; regions: boolean };
  onToggleLayer: (k: "units" | "doodads" | "regions") => void;
}) {
  const [tab, setTab] = useState<"prop" | "layers">("prop");

  return (
    <aside className="flex w-[264px] shrink-0 flex-col border-l border-[var(--line)] bg-[var(--bg-2)]">
      {/* 页签 */}
      <div className="flex gap-1 border-b border-[var(--line)] p-2">
        {(["prop", "layers"] as const).map((t) => (
          <button
            key={t}
            onClick={() => setTab(t)}
            className={cn(
              "flex h-8 flex-1 items-center justify-center gap-1.5 rounded-[8px] text-[12px] font-medium transition",
              tab === t
                ? "bg-[var(--bg-4)] text-[var(--text-1)]"
                : "text-[var(--text-3-solid)] hover:text-[var(--text-2)]"
            )}
          >
            {t === "layers" && <Layers className="h-3.5 w-3.5" />}
            {t === "prop" ? "属性" : "图层"}
          </button>
        ))}
      </div>

      {tab === "prop" ? (
        <div className="flex-1 overflow-y-auto p-4">
          {selectedUnit ? (
            <UnitProps name={selectedUnit} />
          ) : (
            <div className="flex h-full flex-col items-center justify-center gap-3 text-center">
              <div className="flex h-14 w-14 items-center justify-center rounded-[14px] bg-[var(--bg-3)] text-[24px]">
                🗺️
              </div>
              <div>
                <div className="text-[12.5px] font-medium text-[var(--text-2)]">未选中对象</div>
                <div className="mt-1 text-[11px] leading-5 text-[var(--text-3-solid)]">
                  在视口中点击单位查看属性
                  <br />
                  或从左侧调色板放置新对象
                </div>
              </div>
            </div>
          )}
        </div>
      ) : (
        <div className="flex-1 overflow-y-auto p-3">
          <div className="space-y-1">
            <LayerRow name="单位" count={1204} visible={layers.units} onToggle={() => onToggleLayer("units")} color="#f87171" />
            <LayerRow name="装饰物" count={3871} visible={layers.doodads} onToggle={() => onToggleLayer("doodads")} color="#4ade80" />
            <LayerRow name="区域" count={26} visible={layers.regions} onToggle={() => onToggleLayer("regions")} color="#4cc2ff" />
            <LayerRow name="镜头" count={8} visible={true} onToggle={() => {}} color="#a78bfa" />
            <LayerRow name="地形高度" count={0} visible={true} onToggle={() => {}} color="#f0b45a" />
          </div>
        </div>
      )}
    </aside>
  );
}

function LayerRow({
  name,
  count,
  visible,
  onToggle,
  color,
}: {
  name: string;
  count: number;
  visible: boolean;
  onToggle: () => void;
  color: string;
}) {
  return (
    <div className="flex items-center gap-2.5 rounded-[9px] px-2.5 py-2 transition hover:bg-[var(--bg-3)]">
      <span className="h-3 w-3 rounded-[4px]" style={{ background: color }} />
      <span className="flex-1 text-[12px] font-medium">{name}</span>
      {count > 0 && <span className="text-[10.5px] text-[var(--text-3-solid)]">{count.toLocaleString()}</span>}
      <button onClick={onToggle} className="text-[var(--text-3-solid)] transition hover:text-[var(--text-1)]">
        {visible ? <Eye className="h-3.5 w-3.5" /> : <EyeOff className="h-3.5 w-3.5" />}
      </button>
    </div>
  );
}

function UnitProps({ name }: { name: string }) {
  return (
    <div className="rise-in">
      {/* 预览卡 */}
      <div className="mb-4 overflow-hidden rounded-[12px] border border-[var(--line)] bg-gradient-to-b from-[var(--bg-3)] to-[var(--bg-1)] p-5 text-center">
        <div className="text-[44px] leading-none drop-shadow-[0_4px_12px_rgba(0,0,0,0.5)]">🛡️</div>
        <div className="mt-3 text-[14px] font-semibold">{name}</div>
        <div className="mt-0.5 text-[11px] text-[var(--text-3-solid)]">人族 · 英雄 · 等级 5</div>
      </div>

      <div className="overline mb-2">基础</div>
      <PropRow label="生命值" value="650 / 650" bar={1} barColor="#4ade80" />
      <PropRow label="魔法值" value="255 / 300" bar={0.85} barColor="#4cc2ff" />
      <PropRow label="攻击" value="24 - 34" />
      <PropRow label="护甲" value="4（英雄）" />
      <PropRow label="移动速度" value="270" />

      <div className="overline mb-2 mt-5">位置</div>
      <PropRow label="坐标" value="128.5, 96.0" />
      <PropRow label="面向" value="270°" />
      <PropRow label="所属" value="玩家 1（红色）" />

      <div className="mt-5 grid grid-cols-2 gap-2">
        <button className="h-9 rounded-[9px] bg-[var(--bg-3)] text-[12px] font-medium text-[var(--text-1)] transition hover:bg-[var(--bg-4)]">
          编辑技能
        </button>
        <button className="h-9 rounded-[9px] bg-[var(--bg-3)] text-[12px] font-medium text-[var(--text-1)] transition hover:bg-[var(--bg-4)]">
          物品栏
        </button>
      </div>
    </div>
  );
}

function PropRow({
  label,
  value,
  bar,
  barColor,
}: {
  label: string;
  value: string;
  bar?: number;
  barColor?: string;
}) {
  return (
    <div className="mb-1 rounded-[8px] px-2.5 py-2 transition hover:bg-[var(--bg-3)]">
      <div className="flex items-center justify-between">
        <span className="text-[11.5px] text-[var(--text-2)]">{label}</span>
        <span className="text-[12px] font-medium tabular-nums">{value}</span>
      </div>
      {bar !== undefined && (
        <div className="mt-1.5 h-1 overflow-hidden rounded-full bg-[var(--bg-1)]">
          <div
            className="h-full rounded-full"
            style={{ width: `${bar * 100}%`, background: barColor }}
          />
        </div>
      )}
    </div>
  );
}
