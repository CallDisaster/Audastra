import { useState } from "react";
import { Search, Plus } from "lucide-react";
import { cn } from "@/lib/utils";
import { objectCards } from "@/data/mock";

const kinds = ["全部", "单位", "技能", "物品", "升级", "装饰物"];

// ── 物体数据：卡片画廊 + 详情 ──────────────────────────────────
export function ObjectStudio() {
  const [kind, setKind] = useState("全部");
  const [sel, setSel] = useState("u000");
  const [q, setQ] = useState("");

  const list = objectCards.filter(
    (c) => (kind === "全部" || c.kind === kind) && (!q || c.name.includes(q))
  );
  const current = objectCards.find((c) => c.id === sel);

  return (
    <div className="flex h-full min-h-0 flex-col fade-in">
      {/* 顶部过滤 */}
      <div className="flex h-14 shrink-0 items-center gap-2 border-b border-[var(--line)] bg-[var(--bg-2)] px-4">
        {kinds.map((k) => (
          <button
            key={k}
            onClick={() => setKind(k)}
            className={cn(
              "h-8 rounded-full px-3.5 text-[12px] font-medium transition",
              kind === k
                ? "bg-[var(--accent)] text-[#06283e]"
                : "bg-[var(--bg-3)] text-[var(--text-2)] hover:text-[var(--text-1)]"
            )}
          >
            {k}
          </button>
        ))}
        <div className="flex-1" />
        <div className="flex w-52 items-center gap-2 rounded-[9px] bg-[var(--bg-1)] px-2.5 py-2">
          <Search className="h-3.5 w-3.5 text-[var(--text-3-solid)]" />
          <input
            value={q}
            onChange={(e) => setQ(e.target.value)}
            placeholder="搜索名称…"
            className="w-full bg-transparent text-[12px] outline-none placeholder:text-[var(--text-3-solid)]"
          />
        </div>
        <button className="flex h-8 items-center gap-1.5 rounded-[9px] bg-[var(--accent)] px-3 text-[12px] font-semibold text-[#06283e] transition hover:brightness-110">
          <Plus className="h-3.5 w-3.5" /> 新建
        </button>
      </div>

      <div className="flex min-h-0 flex-1">
        {/* 卡片网格 */}
        <div className="flex-1 overflow-y-auto p-5">
          <div className="grid grid-cols-[repeat(auto-fill,minmax(168px,1fr))] gap-3.5">
            {list.map((c) => (
              <button
                key={c.id}
                onClick={() => setSel(c.id)}
                className={cn(
                  "rounded-[14px] border bg-[var(--bg-2)] p-4 text-left transition-all hover:-translate-y-0.5 hover:shadow-[var(--shadow-chip)]",
                  sel === c.id
                    ? "border-[var(--accent)] shadow-[0_0_0_3px_rgba(76,194,255,0.15)]"
                    : "border-[var(--line)]"
                )}
              >
                <div className="flex h-16 items-center justify-center rounded-[10px] bg-gradient-to-b from-[var(--bg-3)] to-[var(--bg-1)] text-[34px]">
                  {c.icon}
                </div>
                <div className="mt-3 text-[12.5px] font-semibold">{c.name}</div>
                <div className="mt-0.5 flex items-center gap-1.5">
                  <span className="chip !px-2 !py-0.5 !text-[9.5px]">{c.kind}</span>
                  <code className="text-[9.5px] text-[var(--text-3-solid)]">{c.id}</code>
                </div>
              </button>
            ))}
          </div>
        </div>

        {/* 详情 */}
        <aside className="flex w-[264px] shrink-0 flex-col border-l border-[var(--line)] bg-[var(--bg-2)]">
          <div className="border-b border-[var(--line)] px-4 py-3">
            <div className="overline">详情</div>
          </div>
          <div className="flex-1 overflow-y-auto p-4">
            {current && (
              <div className="rise-in">
                <div className="mb-4 rounded-[12px] border border-[var(--line)] bg-gradient-to-b from-[var(--bg-3)] to-[var(--bg-1)] p-5 text-center">
                  <div className="text-[42px] leading-none">{current.icon}</div>
                  <div className="mt-2.5 text-[14px] font-semibold">{current.name}</div>
                  <div className="mt-0.5 text-[11px] text-[var(--text-3-solid)]">
                    {current.kind} · <code>{current.id}</code>
                  </div>
                </div>
                <div className="overline mb-2">关键数值</div>
                {Object.entries(current.stats).map(([k, v]) => (
                  <div
                    key={k}
                    className="mb-1 flex items-center justify-between rounded-[8px] px-2.5 py-2 transition hover:bg-[var(--bg-3)]"
                  >
                    <span className="text-[11.5px] text-[var(--text-2)]">{k}</span>
                    <span className="text-[12px] font-medium tabular-nums">{v}</span>
                  </div>
                ))}
                <button className="mt-4 h-9 w-full rounded-[9px] bg-[var(--bg-3)] text-[12px] font-medium transition hover:bg-[var(--bg-4)]">
                  编辑全部字段
                </button>
              </div>
            )}
          </div>
        </aside>
      </div>
    </div>
  );
}
