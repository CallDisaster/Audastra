// 等距地形引擎：确定性高度图 + 冰原配色 + 投影/光照工具。
// 供主视口与小地图共用，保证两处看到的"世界"一致。

export const WORLD = 40; // 世界边长（瓦片）
export const WATER_LEVEL = -0.4;

// 确定性伪随机
export function hash(x: number, y: number, seed = 7): number {
  let h = Math.sin(x * 127.1 + y * 311.7 + seed * 74.7) * 43758.5453;
  return h - Math.floor(h);
}

function smooth(t: number) {
  return t * t * (3 - 2 * t);
}

// 值噪声
function noise(x: number, y: number): number {
  const xi = Math.floor(x);
  const yi = Math.floor(y);
  const xf = smooth(x - xi);
  const yf = smooth(y - yi);
  const a = hash(xi, yi);
  const b = hash(xi + 1, yi);
  const c = hash(xi, yi + 1);
  const d = hash(xi + 1, yi + 1);
  return a + (b - a) * xf + (c - a) * yf + (a - b - c + d) * xf * yf;
}

// 高度图：山谷河流 + 山脊 + 起伏
export function heightAt(x: number, y: number): number {
  const nx = x / WORLD;
  const ny = y / WORLD;
  let h = 0;
  h += (noise(x * 0.11, y * 0.11) - 0.5) * 4.6; // 大起伏
  h += (noise(x * 0.3, y * 0.3) - 0.5) * 1.2; // 中细节
  h += (noise(x * 0.7, y * 0.7) - 0.5) * 0.28; // 微细节
  // 东北角山脊抬升
  const ridge = Math.max(0, nx + ny - 1.08) * 7;
  h += ridge;
  // 中部河道下切
  const river = Math.exp(-Math.pow((ny - 0.52 - 0.08 * Math.sin(nx * 6)) * 11, 2));
  h -= river * 3.4;
  // 边缘下沉，像一座浮岛
  const edge = Math.max(Math.abs(nx - 0.5), Math.abs(ny - 0.5)) * 2;
  h -= Math.pow(edge, 6) * 2.2;
  return h;
}

export interface RGB {
  r: number;
  g: number;
  b: number;
}

function mix(a: RGB, b: RGB, t: number): RGB {
  return { r: a.r + (b.r - a.r) * t, g: a.g + (b.g - a.g) * t, b: a.b + (b.b - a.b) * t };
}

const C = {
  deepWater: { r: 18, g: 48, b: 84 },
  shallowWater: { r: 44, g: 110, b: 158 },
  sand: { r: 176, g: 158, b: 118 },
  grassLow: { r: 74, g: 104, b: 66 },
  grassHigh: { r: 96, g: 124, b: 78 },
  rock: { r: 96, g: 98, b: 106 },
  snow: { r: 224, g: 232, b: 244 },
};

// 按高度取地形基色
export function terrainColor(h: number, x: number, y: number): RGB {
  const n = (hash(x, y, 13) - 0.5) * 0.09; // 轻微色斑
  let c: RGB;
  if (h < WATER_LEVEL - 0.9) c = C.deepWater;
  else if (h < WATER_LEVEL) c = mix(C.deepWater, C.shallowWater, (h - (WATER_LEVEL - 0.9)) / 0.9);
  else if (h < WATER_LEVEL + 0.35) c = C.sand;
  else if (h < 2.2) c = mix(C.grassLow, C.grassHigh, (h - WATER_LEVEL - 0.35) / (2.2 - WATER_LEVEL - 0.35));
  else if (h < 3.6) c = mix(C.grassHigh, C.rock, (h - 2.2) / 1.4);
  else c = mix(C.rock, C.snow, Math.min(1, (h - 3.6) / 1.8));
  return { r: c.r * (1 + n), g: c.g * (1 + n), b: c.b * (1 + n) };
}

// 坡度光照：光源来自西北
export function shadeAt(x: number, y: number): number {
  const e = 0.35;
  const dx = heightAt(x + e, y) - heightAt(x - e, y);
  const dy = heightAt(x, y + e) - heightAt(x, y - e);
  const light = -dx * 0.9 - dy * 0.55; // 西北光
  return Math.max(-0.28, Math.min(0.3, light * 0.16));
}

export function css(c: RGB, alpha = 1): string {
  const cl = (v: number) => Math.max(0, Math.min(255, Math.round(v)));
  return alpha >= 1
    ? `rgb(${cl(c.r)},${cl(c.g)},${cl(c.b)})`
    : `rgba(${cl(c.r)},${cl(c.g)},${cl(c.b)},${alpha})`;
}

// 装饰物（松树）与单位、区域的确定性布点
export interface Doodad { x: number; y: number; s: number }
export const trees: Doodad[] = [];
for (let y = 2; y < WORLD - 2; y++) {
  for (let x = 2; x < WORLD - 2; x++) {
    const h = heightAt(x, y);
    if (h > WATER_LEVEL + 0.35 && h < 1.9 && hash(x, y, 99) > 0.855) {
      trees.push({ x: x + 0.5, y: y + 0.5, s: 0.8 + hash(x, y, 31) * 0.5 });
    }
  }
}

export interface UnitToken { x: number; y: number; color: string; name: string }
export const units: UnitToken[] = [
  { x: 12.5, y: 19.5, color: "#f87171", name: "圣骑士" },
  { x: 13.8, y: 20.8, color: "#f87171", name: "步兵" },
  { x: 15.2, y: 19.8, color: "#f87171", name: "火枪手" },
  { x: 29.5, y: 13.5, color: "#60a5fa", name: "食尸鬼" },
  { x: 30.8, y: 14.8, color: "#60a5fa", name: "憎恶" },
];

export const region = { x0: 9, y0: 16, x1: 17, y1: 24, name: "出生点" };
