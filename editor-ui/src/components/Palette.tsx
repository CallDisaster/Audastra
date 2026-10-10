import { useState } from "react";
import { Search } from "lucide-react";
import { cn } from "@/lib/utils";
import type { ToolKey } from "./chrome";
import { terrainTextures, cliffBrushes, unitPalette, doodadPalette } from "@/data/mock";

// ── 左侧调色板：随工具切换内容 ─────────────────────────────────
export function Palette({ tool }: { tool: ToolKey }) {
  const [tex, setTex] = useState("grass");
  const [brush, setBrush] = useState(3);
  const [unitSel, setUnitSel] = useState("Hpal");
  const [q, setQ] = useState("");

  return (
    <aside className="flex w-[240px] shrink-0 flex-col border-r border-[var(--line)] bg-[var(--bg-2)]">
      {tool === "terrain" && (
        <>
          <Section title="笔刷">
            <div className="mb-3 flex gap-1.5">
              {cliffBrushes.map((b) => (
                <button
                  key={b.id}
                  className="flex h-9 flex-1 flex-col items-center justify-center rounded-[8px] bg-[var(--bg-3)] text-[13px] text-[var(--text-2)] transition hover:text-[var(--text-1)]"
                  title={b.name}
                >
                  <span>{b.icon}</span>
                  <span className="text-[9px]">{b.name}</span>
                </button>
              ))}
            </div>
            <SliderRow label="大小" value={brush} min={1} max={8} onChange={setBrush} suffix={`${brush}`} />
            <SliderRow label="强度" value={60} min={0} max={100} onChange={() => {}} suffix="60%" />
          </Section>
          <Section title="纹理" grow>
            <div className="grid grid-cols-4 gap-2">
              {terrainTextures.map((t) => (
                <button
                  key={t.id}
                  onClick={() => setTex(t.id)}
                  title={t.name}
                  className={cn(
                    "group flex flex-col items-center gap-1",
                  )}
                >
                  <span
                    className={cn(
                      "h-10 w-full rounded-[8px] transition-all",
                      tex === t.id
                        ? "ring-2 ring-[var(--accent)] ring-offset-2 ring-offset-[var(--bg-2)]"
                        : "opacity-80 group-hover:opacity-100"
                    )}
                    style={{ background: `linear-gradient(135deg, ${t.colors[0]}, ${t.colors[1]})` }}
                  />
                  <span className="text-[9.5px] text-[var(--text-3-solid)]">{t.name}</span>
                </button>
              ))}
            </div>
          </Section>
        </>
      )}

      {(tool === "unit" || tool === "doodad") && (
        <>
          <div className="border-b border-[var(--line)] p-3">
            <div className="flex items-center gap-2 rounded-[9px] bg-[var(--bg-1)] px-2.5 py-2">
              <Search className="h-3.5 w-3.5 text-[var(--text-3-solid)]" />
              <input
                value={q}
                onChange={(e) => setQ(e.target.value)}
                placeholder={tool === "unit" ? "搜索单位…" : "搜索装饰物…"}
                className="w-full bg-transparent text-[12px] outline-none placeholder:text-[var(--text-3-solid)]"
              />
            </div>
          </div>
          <div className="flex-1 overflow-y-auto p-3">
            {tool === "unit" ? (
              <div className="grid grid-cols-3 gap-2">
                {unitPalette
                  .filter((u) => !q || u.name.includes(q))
                  .map((u) => (
                    <button
                      key={u.id}
                      onClick={() => setUnitSel(u.id)}
                      className={cn(
                        "flex flex-col items-center gap-1.5 rounded-[10px] p-2.5 transition-all",
                        unitSel === u.id
                          ? "bg-[var(--accent-soft)] shadow-[inset_0_0_0_1px_rgba(76,194,255,0.35)]"
                          : "bg-[var(--bg-3)] hover:bg-[var(--bg-4)]"
                      )}
                    >
                      <span className="text-[22px] leading-none">{u.icon}</span>
                      <span className="text-[10px] font-medium">{u.name}</span>
                      <span className="text-[8.5px] text-[var(--text-3-solid)]">{u.race}</span>
                    </button>
                  ))}
              </div>
            ) : (
              <div className="grid grid-cols-4 gap-2">
                {doodadPalette
                  .filter((d) => !q || d.name.includes(q))
                  .map((d) => (
                    <button
                      key={d.id}
                      title={d.name}
                      className="flex flex-col items-center gap-1 rounded-[10px] bg-[var(--bg-3)] p-2.5 transition hover:bg-[var(--bg-4)]"
                    >
                      <span className="text-[20px] leading-none">{d.icon}</span>
                      <span className="max-w-full truncate text-[9px] text-[var(--text-3-solid)]">{d.name}</span>
                    </button>
                  ))}
              </div>
            )}
          </div>
        </>
      )}

      {(tool === "region" || tool === "camera") && (
        <Section title={tool === "region" ? "区域列表" : "镜头列表"} grow>
          <div className="space-y-1.5">
            {(tool === "region" ? ["出生点", "东侧山口", "前线", "车队起点"] : ["开场镜头", "峡谷俯瞰", "北门特写"]).map(
              (name, i) => (
                <button
                  key={name}
                  className={cn(
                    "flex w-full items-center gap-2.5 rounded-[9px] px-3 py-2.5 text-left text-[12px] transition",
                    i === 0
                      ? "bg-[var(--accent-soft)] text-[var(--accent)]"
                      : "bg-[var(--bg-3)] text-[var(--text-2)] hover:text-[var(--text-1)]"
                  )}
                >
                  <span
                    className="h-2.5 w-2.5 rounded-[3px]"
                    style={{ background: ["#4cc2ff", "#f0b45a", "#4ade80", "#a78bfa"][i] }}
                  />
                  {name}
                </button>
              )
            )}
          </div>
        </Section>
      )}
    </aside>
  );
}

function Section({
  title,
  children,
  grow,
}: {
  title: string;
  children: React.ReactNode;
  grow?: boolean;
}) {
  return (
    <div className={cn("border-b border-[var(--line)] p-3.5", grow && "flex-1 overflow-y-auto")}>
      <div className="overline mb-3">{title}</div>
      {children}
    </div>
  );
}

function SliderRow({
  label,
  value,
  min,
  max,
  onChange,
  suffix,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  onChange: (v: number) => void;
  suffix: string;
}) {
  return (
    <div className="mb-3 flex items-center gap-2.5">
      <span className="w-8 text-[11px] text-[var(--text-2)]">{label}</span>
      <input
        type="range"
        min={min}
        max={max}
        value={value}
        onChange={(e) => onChange(Number(e.target.value))}
        className="flex-1"
      />
      <span className="w-8 text-right text-[11px] font-medium text-[var(--accent)]">{suffix}</span>
    </div>
  );
}
