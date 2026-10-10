// 演示数据：面向视觉的地图编辑器内容。

export const mapInfo = {
  name: "霜语峡谷",
  desc: "1-4 人 · 战役",
  size: "192 × 192",
  tileset: "洛丹伦的夏天",
};

// 地形纹理调色板（渐变色块模拟贴图）
export const terrainTextures = [
  { id: "grass", name: "草地", colors: ["#5d7a4e", "#47623c"] },
  { id: "dirt", name: "泥土", colors: ["#8a7355", "#6b5844"] },
  { id: "rock", name: "岩石", colors: ["#8b8d96", "#5f6169"] },
  { id: "snow", name: "积雪", colors: ["#eef3fa", "#c9d6e8"] },
  { id: "sand", name: "沙滩", colors: ["#c9b585", "#a89468"] },
  { id: "water", name: "浅水", colors: ["#3f88b4", "#27567e"] },
  { id: "deep", name: "深水", colors: ["#1d3f66", "#122a48"] },
  { id: "lava", name: "岩浆", colors: ["#e2703a", "#8c2f12"] },
];

export const cliffBrushes = [
  { id: "raise", name: "抬高", icon: "▲" },
  { id: "lower", name: "降低", icon: "▼" },
  { id: "plateau", name: "平台", icon: "◼" },
  { id: "smooth", name: "平滑", icon: "◠" },
];

// 单位调色板
export interface UnitDef {
  id: string;
  name: string;
  race: string;
  icon: string; // emoji 占位图标
  tint: string;
}

export const unitPalette: UnitDef[] = [
  { id: "Hpal", name: "圣骑士", race: "人族 · 英雄", icon: "🛡️", tint: "#f0b45a" },
  { id: "Hamg", name: "大法师", race: "人族 · 英雄", icon: "🪄", tint: "#4cc2ff" },
  { id: "hfoo", name: "步兵", race: "人族", icon: "⚔️", tint: "#f87171" },
  { id: "hrif", name: "火枪手", race: "人族", icon: "🔫", tint: "#f87171" },
  { id: "hkni", name: "骑士", race: "人族", icon: "🐴", tint: "#f87171" },
  { id: "Ogra", name: "剑圣", race: "兽族 · 英雄", icon: "🗡️", tint: "#4ade80" },
  { id: "ogru", name: "兽人步兵", race: "兽族", icon: "🪓", tint: "#4ade80" },
  { id: "Ulic", name: "巫妖", race: "不死 · 英雄", icon: "💀", tint: "#a78bfa" },
  { id: "ugho", name: "食尸鬼", race: "不死", icon: "🧟", tint: "#a78bfa" },
  { id: "Ekee", name: "丛林守护者", race: "暗夜 · 英雄", icon: "🌿", tint: "#34d399" },
  { id: "earc", name: "弓箭手", race: "暗夜", icon: "🏹", tint: "#34d399" },
  { id: "nenc", name: "魔法师", race: "中立", icon: "🔮", tint: "#f472b6" },
];

export const doodadPalette = [
  { id: "pine", name: "洛丹伦松树", icon: "🌲" },
  { id: "tree", name: "橡树", icon: "🌳" },
  { id: "rockd", name: "巨岩", icon: "🪨" },
  { id: "ruin", name: "废墟", icon: "🏚️" },
  { id: "torch", name: "火把", icon: "🔥" },
  { id: "crate", name: "木箱", icon: "📦" },
  { id: "bridge", name: "石桥", icon: "🌉" },
  { id: "fount", name: "喷泉", icon: "⛲" },
];

// 触发器节点画布演示数据
export const flowNodes = [
  { id: "evt", kind: "event" as const, title: "事件", text: "时间 — 每 90 秒游戏时间", x: 20, y: 210 },
  { id: "cnd", kind: "condition" as const, title: "条件", text: "玩家 2 的存活单位 少于 40", x: 280, y: 210 },
  { id: "act1", kind: "action" as const, title: "动作", text: "创建 4 个步兵在「东侧山口」", x: 530, y: 130 },
  { id: "act2", kind: "action" as const, title: "动作", text: "命令他们攻击移动到「前线」", x: 530, y: 300 },
];

// 物体数据演示
export const objectCards = [
  { id: "Hpal", name: "圣骑士", kind: "单位", icon: "🛡️", stats: { 生命: 650, 攻击: "24-34", 护甲: 4 } },
  { id: "u000", name: "霜语者", kind: "单位", icon: "❄️", stats: { 生命: 1250, 攻击: "49-58", 护甲: 6 } },
  { id: "A000", name: "霜冻新星·改", kind: "技能", icon: "🌨️", stats: { 伤害: 150, 冷却: "8s", 耗蓝: 75 } },
  { id: "i000", name: "冰晶护符", kind: "物品", icon: "💠", stats: { 护甲: "+3", 生命: "+150" } },
  { id: "hfoo", name: "步兵", kind: "单位", icon: "⚔️", stats: { 生命: 420, 攻击: "12-13", 护甲: 2 } },
  { id: "AHhb", name: "神圣之光", kind: "技能", icon: "✨", stats: { 治疗: 200, 冷却: "5s", 耗蓝: 65 } },
];
