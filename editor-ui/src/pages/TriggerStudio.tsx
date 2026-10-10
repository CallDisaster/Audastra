import { useState } from "react";
import { Plus, Zap, GitBranch, Play, Search, ChevronRight, FolderClosed } from "lucide-react";
import { cn } from "@/lib/utils";
import { flowNodes } from "@/data/mock";

const kindStyle = {
  event: { color: "#f0b45a", icon: Zap, label: "事件" },
  condition: { color: "#a78bfa", icon: GitBranch, label: "条件" },
  action: { color: "#4cc2ff", icon: Play, label: "动作" },
} as const;

// ── 触发器：可视化逻辑节点画布 ─────────────────────────────────
export function TriggerStudio() {
  const [selected, setSelected] = useState("act1");

  return (
    <div className="flex h-full min-h-0 fade-in">
      {/* 触发器列表 */}
      <aside className="flex w-[240px] shrink-0 flex-col border-r border-[var(--line)] bg-[var(--bg-2)]">
        <div className="flex items-center gap-2 border-b border-[var(--line)] p-3">
          <div className="flex flex-1 items-center gap-2 rounded-[9px] bg-[var(--bg-1)] px-2.5 py-2">
            <Search className="h-3.5 w-3.5 text-[var(--text-3-solid)]" />
            <input
              placeholder="搜索触发器…"
              className="w-full bg-transparent text-[12px] outline-none placeholder:text-[var(--text-3-solid)]"
            />
          </div>
          <button className="flex h-8 w-8 items-center justify-center rounded-[9px] bg-[var(--accent)] text-[#06283e] transition hover:brightness-110">
            <Plus className="h-4 w-4" />
          </button>
        </div>
        <div className="flex-1 overflow-y-auto p-2">
          <Group name="初始化" items={["地图设置", "变量绑定"]} active={null} onPick={() => {}} />
          <Group name="战斗系统" items={["援军波次", "英雄阵亡", "暴击特效"]} active="援军波次" onPick={() => {}} />
          <Group name="任务" items={["护送车队", "霜语者的秘密"]} active={null} onPick={() => {}} />
          <Group name="电影分镜" items={["开场", "结局"]} active={null} onPick={() => {}} />
        </div>
      </aside>

      {/* 节点画布 */}
      <div className="relative min-w-0 flex-1 overflow-hidden bg-[var(--bg-0)]">
        {/* 点阵背景 */}
        <div
          className="absolute inset-0"
          style={{
            backgroundImage: "radial-gradient(rgba(255,255,255,0.07) 1px, transparent 1px)",
            backgroundSize: "24px 24px",
          }}
        />

        {/* 连线（SVG 贝塞尔） */}
        <svg className="absolute inset-0 h-full w-full">
          <Wire from={[232, 250]} to={[280, 250]} color="#f0b45a" />
          <Wire from={[492, 250]} to={[530, 180]} color="#a78bfa" label="是" />
          <Wire from={[492, 250]} to={[530, 350]} color="#a78bfa" label="否" />
        </svg>

        {/* 节点 */}
        {flowNodes.map((n) => {
          const st = kindStyle[n.kind];
          const Icon = st.icon;
          const active = selected === n.id;
          return (
            <button
              key={n.id}
              onClick={() => setSelected(n.id)}
              style={{ left: n.x, top: n.y, borderColor: active ? st.color : "var(--line)" }}
              className={cn(
                "absolute w-[212px] rounded-[12px] border bg-[var(--bg-2)] p-0 text-left shadow-[var(--shadow-chip)] transition-all hover:-translate-y-0.5",
                active && "shadow-[0_0_0_3px_rgba(76,194,255,0.15),var(--shadow-chip)]"
              )}
            >
              <div
                className="flex items-center gap-2 rounded-t-[12px] px-3 py-2"
                style={{ background: `${st.color}1f` }}
              >
                <Icon className="h-3.5 w-3.5" style={{ color: st.color }} />
                <span className="text-[10.5px] font-semibold uppercase tracking-wider" style={{ color: st.color }}>
                  {st.label}
                </span>
              </div>
              <div className="px-3 py-2.5 text-[12px] leading-5 text-[var(--text-1)]">{n.text}</div>
            </button>
          );
        })}

        {/* 顶部标题 */}
        <div className="pointer-events-none absolute left-1/2 top-4 -translate-x-1/2">
          <div className="glass pointer-events-auto flex items-center gap-3 px-4 py-2">
            <span className="text-[18px]">⚡</span>
            <div className="leading-tight">
              <div className="text-[12px] font-semibold">战斗 · 援军波次</div>
              <div className="text-[10.5px] text-[var(--text-3-solid)]">点击节点编辑 · 拖拽空白平移画布</div>
            </div>
          </div>
        </div>

        {/* 底部：添加节点 */}
        <div className="absolute bottom-4 left-1/2 -translate-x-1/2">
          <div className="glass flex items-center gap-1 p-1.5">
            {(["event", "condition", "action"] as const).map((k) => {
              const st = kindStyle[k];
              const Icon = st.icon;
              return (
                <button
                  key={k}
                  className="flex h-8 items-center gap-1.5 rounded-[8px] px-3 text-[11.5px] font-medium transition hover:bg-[var(--bg-3)]"
                  style={{ color: st.color }}
                >
                  <Plus className="h-3.5 w-3.5" />
                  <Icon className="h-3.5 w-3.5" />
                  {st.label}
                </button>
              );
            })}
          </div>
        </div>
      </div>

      {/* 节点属性 */}
      <aside className="flex w-[264px] shrink-0 flex-col border-l border-[var(--line)] bg-[var(--bg-2)]">
        <div className="border-b border-[var(--line)] px-4 py-3">
          <div className="overline">节点属性</div>
        </div>
        <div className="flex-1 overflow-y-auto p-4">
          <div className="mb-4 rounded-[12px] border border-[var(--line)] bg-[var(--bg-3)] p-3.5">
            <div className="text-[12px] font-medium leading-5">创建 4 个步兵在「东侧山口」</div>
            <div className="mt-1 text-[10.5px] text-[var(--text-3-solid)]">单位 — 创建单位（面向角度）</div>
          </div>
          {[
            ["单位类型", "步兵", "#f87171"],
            ["数量", "4", "#4cc2ff"],
            ["所属玩家", "玩家 2（蓝色）", "#60a5fa"],
            ["位置", "东侧山口 · 中心", "#4ade80"],
            ["面向角度", "270.00°", "#f0b45a"],
          ].map(([k, v, c]) => (
            <button
              key={k}
              className="mb-1.5 flex w-full items-center justify-between rounded-[9px] px-3 py-2.5 text-left transition hover:bg-[var(--bg-3)]"
            >
              <span className="text-[11.5px] text-[var(--text-2)]">{k}</span>
              <span className="rounded-full px-2.5 py-0.5 text-[11px] font-medium" style={{ background: `${c}1a`, color: c }}>
                {v}
              </span>
            </button>
          ))}
          <div className="mt-5 rounded-[10px] bg-[var(--bg-1)] p-3 text-[11px] leading-5 text-[var(--text-3-solid)]">
            💡 点击彩色参数可直接修改；数值、单位、区域都有各自的专属编辑器。
          </div>
        </div>
      </aside>
    </div>
  );
}

function Group({
  name,
  items,
  active,
  onPick,
}: {
  name: string;
  items: string[];
  active: string | null;
  onPick: (n: string) => void;
}) {
  const [open, setOpen] = useState(true);
  return (
    <div className="mb-1">
      <button
        onClick={() => setOpen((v) => !v)}
        className="flex w-full items-center gap-1.5 rounded-[8px] px-2.5 py-2 text-[11.5px] font-medium text-[var(--text-2)] transition hover:bg-[var(--bg-3)]"
      >
        <ChevronRight className={cn("h-3 w-3 transition-transform", open && "rotate-90")} />
        <FolderClosed className="h-3.5 w-3.5 text-[var(--gold)]" />
        {name}
        <span className="ml-auto text-[10px] text-[var(--text-3-solid)]">{items.length}</span>
      </button>
      {open &&
        items.map((it) => (
          <button
            key={it}
            onClick={() => onPick(it)}
            className={cn(
              "ml-4 flex w-[calc(100%-16px)] items-center gap-2 rounded-[8px] px-3 py-2 text-left text-[12px] transition",
              active === it
                ? "bg-[var(--accent-soft)] text-[var(--accent)]"
                : "text-[var(--text-2)] hover:bg-[var(--bg-3)] hover:text-[var(--text-1)]"
            )}
          >
            <Zap className="h-3 w-3 shrink-0 opacity-60" />
            {it}
          </button>
        ))}
    </div>
  );
}

function Wire({
  from,
  to,
  color,
  label,
}: {
  from: [number, number];
  to: [number, number];
  color: string;
  label?: string;
}) {
  const [x1, y1] = [from[0], from[1]];
  const [x2, y2] = [to[0], to[1]];
  const mx = (x1 + x2) / 2;
  return (
    <g>
      <path
        d={`M ${x1} ${y1} C ${mx} ${y1}, ${mx} ${y2}, ${x2} ${y2}`}
        fill="none"
        stroke={color}
        strokeWidth="2"
        strokeOpacity="0.7"
        strokeLinecap="round"
      />
      <circle cx={x1} cy={y1} r="3.5" fill={color} />
      <circle cx={x2} cy={y2} r="3.5" fill={color} />
      {label && (
        <text
          x={mx}
          y={(y1 + y2) / 2 - 6}
          fill={color}
          fontSize="10"
          fontWeight="600"
          textAnchor="middle"
        >
          {label}
        </text>
      )}
    </g>
  );
}
