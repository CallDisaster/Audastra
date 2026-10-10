import {
  Mountain,
  Users,
  Trees,
  SquareDashed,
  Video,
  Workflow,
  Database,
  FolderOpen,
  Undo2,
  Redo2,
  Save,
  Settings,
  Snowflake,
  Bot,
} from "lucide-react";
import { cn } from "@/lib/utils";
import { mapInfo } from "@/data/mock";

export type ToolKey =
  | "terrain"
  | "unit"
  | "doodad"
  | "region"
  | "camera"
  | "trigger"
  | "object"
  | "asset";

export const tools: { key: ToolKey; label: string; icon: typeof Mountain }[] = [
  { key: "terrain", label: "地形", icon: Mountain },
  { key: "unit", label: "单位", icon: Users },
  { key: "doodad", label: "装饰物", icon: Trees },
  { key: "region", label: "区域", icon: SquareDashed },
  { key: "camera", label: "镜头", icon: Video },
  { key: "trigger", label: "触发器", icon: Workflow },
  { key: "object", label: "物体", icon: Database },
  { key: "asset", label: "资源", icon: FolderOpen },
];

// ── 顶栏：极简，只有身份与全局动作 ─────────────────────────────
export function TopBar() {
  return (
    <header className="flex h-12 shrink-0 items-center gap-3 border-b border-[var(--line)] bg-[var(--bg-2)] px-4">
      <div className="flex items-center gap-2.5">
        <div className="flex h-7 w-7 items-center justify-center rounded-[9px] bg-gradient-to-br from-[#4cc2ff] to-[#1f6fd0] shadow-[0_2px_10px_rgba(76,194,255,0.4)]">
          <Snowflake className="h-4 w-4 text-white" strokeWidth={2.2} />
        </div>
        <div className="leading-tight">
          <div className="text-[13px] font-semibold tracking-wide">War3 Map Studio</div>
        </div>
      </div>

      <div className="mx-1 h-5 w-px bg-[var(--line-strong)]" />

      <div className="flex items-center gap-2">
        <span className="text-[13px] font-medium">{mapInfo.name}</span>
        <span className="chip">{mapInfo.desc}</span>
        <span className="chip">{mapInfo.tileset}</span>
      </div>

      <div className="flex-1" />

      <div className="flex items-center gap-1">
        <IconBtn title="撤销 (Ctrl+Z)"><Undo2 className="h-4 w-4" /></IconBtn>
        <IconBtn title="重做 (Ctrl+Y)"><Redo2 className="h-4 w-4" /></IconBtn>
        <div className="mx-1.5 h-5 w-px bg-[var(--line-strong)]" />
        <IconBtn title="AI 助手"><Bot className="h-4 w-4" /></IconBtn>
        <IconBtn title="设置"><Settings className="h-4 w-4" /></IconBtn>
        <button className="ml-1.5 flex h-8 items-center gap-1.5 rounded-[9px] bg-[var(--accent)] px-3.5 text-[12px] font-semibold text-[#06283e] shadow-[0_2px_12px_rgba(76,194,255,0.35)] transition hover:brightness-110">
          <Save className="h-3.5 w-3.5" />
          保存地图
        </button>
      </div>
    </header>
  );
}

export function IconBtn({
  children,
  title,
  active,
  onClick,
}: {
  children: React.ReactNode;
  title?: string;
  active?: boolean;
  onClick?: () => void;
}) {
  return (
    <button
      title={title}
      onClick={onClick}
      className={cn(
        "flex h-8 w-8 items-center justify-center rounded-[9px] transition-colors",
        active
          ? "bg-[var(--accent-soft)] text-[var(--accent)]"
          : "text-[var(--text-2)] hover:bg-[var(--bg-3)] hover:text-[var(--text-1)]"
      )}
    >
      {children}
    </button>
  );
}

// ── 左侧工具栏：大图标 + 文字，悬浮提示 ────────────────────────
export function ToolRail({
  tool,
  onSelect,
}: {
  tool: ToolKey;
  onSelect: (t: ToolKey) => void;
}) {
  return (
    <nav className="flex w-[72px] shrink-0 flex-col items-center gap-1.5 border-r border-[var(--line)] bg-[var(--bg-2)] py-3">
      {tools.map(({ key, label, icon: Icon }) => {
        const active = tool === key;
        return (
          <button
            key={key}
            onClick={() => onSelect(key)}
            className={cn(
              "group relative flex h-[58px] w-[56px] flex-col items-center justify-center gap-1 rounded-[11px] transition-all",
              active
                ? "bg-[var(--accent-soft)] text-[var(--accent)] shadow-[inset_0_0_0_1px_rgba(76,194,255,0.3)]"
                : "text-[var(--text-2)] hover:bg-[var(--bg-3)] hover:text-[var(--text-1)]"
            )}
          >
            <Icon className="h-[21px] w-[21px]" strokeWidth={1.8} />
            <span className="text-[10px] font-medium">{label}</span>
          </button>
        );
      })}
    </nav>
  );
}

// ── 状态栏：极简一行 ───────────────────────────────────────────
export function StatusBar() {
  return (
    <footer className="flex h-7 shrink-0 items-center gap-4 border-t border-[var(--line)] bg-[var(--bg-2)] px-4 text-[11px] text-[var(--text-3-solid)]">
      <span className="flex items-center gap-1.5">
        <span className="h-1.5 w-1.5 rounded-full bg-[var(--green)]" />
        已自动保存
      </span>
      <span>坐标 128, 96 · 高度 2.4</span>
      <div className="flex-1" />
      <span>玩家 1/4</span>
      <span>{mapInfo.size}</span>
    </footer>
  );
}
