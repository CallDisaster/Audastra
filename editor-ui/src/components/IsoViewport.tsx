import { useEffect, useRef, useState, useCallback } from "react";
import {
  WORLD,
  WATER_LEVEL,
  heightAt,
  terrainColor,
  shadeAt,
  css,
  trees,
  units,
  region,
} from "@/lib/iso";

// 等距 3D 视口：拖拽平移 / 滚轮缩放 / 水面微光动画 / 西北光照
export function IsoViewport({
  yaw,
  zoom,
  showGrid,
  layers,
  selectedUnit,
  onSelectUnit,
}: {
  yaw: number;
  zoom: number;
  showGrid: boolean;
  layers: { units: boolean; doodads: boolean; regions: boolean };
  selectedUnit: string | null;
  onSelectUnit: (name: string | null) => void;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const wrapRef = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState({ w: 0, h: 0 });
  const panRef = useRef({ x: 0, y: 0 });
  const dragRef = useRef<{ x: number; y: number } | null>(null);
  const unitScreenRef = useRef<{ name: string; x: number; y: number; r: number }[]>([]);

  // 尺寸跟踪
  useEffect(() => {
    const el = wrapRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => {
      const r = el.getBoundingClientRect();
      setSize({ w: Math.round(r.width), h: Math.round(r.height) });
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const draw = useCallback(
    (time: number) => {
      const canvas = canvasRef.current;
      if (!canvas || size.w < 4 || size.h < 4) return;
      const dpr = window.devicePixelRatio || 1;
      canvas.width = size.w * dpr;
      canvas.height = size.h * dpr;
      const ctx = canvas.getContext("2d");
      if (!ctx) return;
      ctx.scale(dpr, dpr);
      const W = size.w;
      const H = size.h;

      // ── 天空背景
      const sky = ctx.createLinearGradient(0, 0, 0, H);
      sky.addColorStop(0, "#0a1626");
      sky.addColorStop(0.55, "#0e1c30");
      sky.addColorStop(1, "#0d0f14");
      ctx.fillStyle = sky;
      ctx.fillRect(0, 0, W, H);

      // 远景光晕
      const halo = ctx.createRadialGradient(W * 0.5, H * 0.32, 20, W * 0.5, H * 0.32, W * 0.5);
      halo.addColorStop(0, "rgba(90,150,210,0.14)");
      halo.addColorStop(1, "rgba(90,150,210,0)");
      ctx.fillStyle = halo;
      ctx.fillRect(0, 0, W, H);

      const rad = (yaw * Math.PI) / 180;
      const tile = Math.min(W, H) / (WORLD * 1.62) * zoom;
      const cx = W / 2 + panRef.current.x;
      const cy = H * 0.3 + panRef.current.y;

      const proj = (x: number, y: number, h: number) => {
        const rx = (x - WORLD / 2) * Math.cos(rad) - (y - WORLD / 2) * Math.sin(rad);
        const ry = (x - WORLD / 2) * Math.sin(rad) + (y - WORLD / 2) * Math.cos(rad);
        return { x: cx + rx * tile, y: cy + ry * tile * 0.5 - h * tile * 0.6 };
      };
      const depth = (x: number, y: number) =>
        (x - WORLD / 2) * Math.sin(rad) + (y - WORLD / 2) * Math.cos(rad);

      // ── 地面阴影（岛形投影）
      ctx.save();
      ctx.translate(cx, cy + WORLD * tile * 0.26);
      ctx.scale(1, 0.42);
      const island = ctx.createRadialGradient(0, 0, 10, 0, 0, WORLD * tile * 0.75);
      island.addColorStop(0, "rgba(0,0,0,0.5)");
      island.addColorStop(1, "rgba(0,0,0,0)");
      ctx.fillStyle = island;
      ctx.fillRect(-WORLD * tile, -WORLD * tile, WORLD * tile * 2, WORLD * tile * 2);
      ctx.restore();

      // ── 地形瓦片（画家算法）
      const order: { x: number; y: number }[] = [];
      for (let y = 0; y < WORLD; y++) for (let x = 0; x < WORLD; x++) order.push({ x, y });
      order.sort((a, b) => depth(a.x, a.y) - depth(b.x, b.y));

      for (const c of order) {
        const h00 = heightAt(c.x, c.y);
        const h10 = heightAt(c.x + 1, c.y);
        const h11 = heightAt(c.x + 1, c.y + 1);
        const h01 = heightAt(c.x, c.y + 1);
        const hAvg = (h00 + h10 + h11 + h01) / 4;
        // 水下角点压平到水面，形成连续水面
        const flat = (h: number) => Math.max(h, WATER_LEVEL);
        const p1 = proj(c.x, c.y, flat(h00));
        const p2 = proj(c.x + 1, c.y, flat(h10));
        const p3 = proj(c.x + 1, c.y + 1, flat(h11));
        const p4 = proj(c.x, c.y + 1, flat(h01));

        let col = terrainColor(hAvg, c.x, c.y);
        const sh = shadeAt(c.x + 0.5, c.y + 0.5);
        col = { r: col.r * (1 + sh), g: col.g * (1 + sh), b: col.b * (1 + sh * 0.9) };

        // 水面微光
        let alpha = 1;
        if (hAvg < WATER_LEVEL) {
          alpha = 0.94;
          const shimmer = Math.sin(time / 900 + c.x * 0.7 + c.y * 0.5) * 0.5 + 0.5;
          col = {
            r: col.r + shimmer * 14,
            g: col.g + shimmer * 20,
            b: col.b + shimmer * 26,
          };
        }

        ctx.beginPath();
        ctx.moveTo(p1.x, p1.y);
        ctx.lineTo(p2.x, p2.y);
        ctx.lineTo(p3.x, p3.y);
        ctx.lineTo(p4.x, p4.y);
        ctx.closePath();
        ctx.fillStyle = css(col, alpha);
        ctx.fill();
        // 同色描边消除瓦片接缝
        ctx.strokeStyle = css(col, alpha);
        ctx.lineWidth = 0.8;
        ctx.stroke();

        if (showGrid) {
          ctx.strokeStyle = "rgba(255,255,255,0.05)";
          ctx.lineWidth = 0.5;
          ctx.stroke();
        }

        // 悬崖侧面：高度突变处画暗面
        const dropS = h00 + h10 - h11 - h01; // 南缘落差
        if (dropS > 1.2) {
          ctx.beginPath();
          ctx.moveTo(p4.x, p4.y);
          ctx.lineTo(p3.x, p3.y);
          ctx.lineTo(p3.x, p3.y + dropS * tile * 0.28);
          ctx.lineTo(p4.x, p4.y + dropS * tile * 0.28);
          ctx.closePath();
          ctx.fillStyle = "rgba(40,34,30,0.55)";
          ctx.fill();
        }
      }

      // ── 区域覆盖
      if (layers.regions) {
        const corners = [
          proj(region.x0, region.y0, heightAt(region.x0, region.y0)),
          proj(region.x1, region.y0, heightAt(region.x1, region.y0)),
          proj(region.x1, region.y1, heightAt(region.x1, region.y1)),
          proj(region.x0, region.y1, heightAt(region.x0, region.y1)),
        ];
        ctx.beginPath();
        corners.forEach((p, i) => (i === 0 ? ctx.moveTo(p.x, p.y) : ctx.lineTo(p.x, p.y)));
        ctx.closePath();
        ctx.fillStyle = "rgba(76,194,255,0.1)";
        ctx.fill();
        ctx.strokeStyle = "rgba(76,194,255,0.85)";
        ctx.lineWidth = 1.2;
        ctx.setLineDash([6, 4]);
        ctx.stroke();
        ctx.setLineDash([]);
        // 标签
        const lp = proj((region.x0 + region.x1) / 2, region.y0, heightAt((region.x0 + region.x1) / 2, region.y0));
        ctx.font = "600 11px Inter, sans-serif";
        const tw = ctx.measureText(region.name).width;
        ctx.fillStyle = "rgba(13,17,23,0.85)";
        ctx.beginPath();
        ctx.roundRect(lp.x - tw / 2 - 8, lp.y - 30, tw + 16, 20, 6);
        ctx.fill();
        ctx.strokeStyle = "rgba(76,194,255,0.4)";
        ctx.stroke();
        ctx.fillStyle = "#4cc2ff";
        ctx.textAlign = "center";
        ctx.fillText(region.name, lp.x, lp.y - 16);
      }

      // ── 松树与单位（按深度排序混入）
      type Sprite = { d: number; draw: () => void };
      const sprites: Sprite[] = [];

      if (layers.doodads) {
        for (const t of trees) {
          const h = heightAt(t.x, t.y);
          const p = proj(t.x, t.y, h);
          sprites.push({
            d: depth(t.x, t.y),
            draw: () => drawPine(ctx, p.x, p.y, tile * t.s),
          });
        }
      }
      if (layers.units) {
        unitScreenRef.current = [];
        for (const u of units) {
          const h = heightAt(u.x, u.y);
          const p = proj(u.x, u.y, h);
          sprites.push({
            d: depth(u.x, u.y),
            draw: () => {
              const sel = selectedUnit === u.name;
              drawUnit(ctx, p.x, p.y, tile, u.color, sel, time);
              unitScreenRef.current.push({ name: u.name, x: p.x, y: p.y - tile * 0.5, r: tile * 0.42 });
              if (sel) {
                ctx.font = "600 11px Inter, sans-serif";
                const tw = ctx.measureText(u.name).width;
                ctx.fillStyle = "rgba(13,17,23,0.85)";
                ctx.beginPath();
                ctx.roundRect(p.x - tw / 2 - 8, p.y - tile * 1.7, tw + 16, 20, 6);
                ctx.fill();
                ctx.strokeStyle = "rgba(240,180,90,0.5)";
                ctx.stroke();
                ctx.fillStyle = "#f0b45a";
                ctx.textAlign = "center";
                ctx.fillText(u.name, p.x, p.y - tile * 1.7 + 14);
              }
            },
          });
        }
      }
      sprites.sort((a, b) => a.d - b.d);
      sprites.forEach((s) => s.draw());

      // ── 暗角
      const vig = ctx.createRadialGradient(W / 2, H / 2, H * 0.42, W / 2, H / 2, H);
      vig.addColorStop(0, "rgba(0,0,0,0)");
      vig.addColorStop(1, "rgba(0,0,0,0.38)");
      ctx.fillStyle = vig;
      ctx.fillRect(0, 0, W, H);
    },
    [size, yaw, zoom, showGrid, layers, selectedUnit]
  );

  // RAF 循环（水面微光 + 选中光圈）
  useEffect(() => {
    let raf = 0;
    const loop = (t: number) => {
      draw(t);
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, [draw]);

  // 交互：拖拽平移 / 滚轮缩放 / 点选单位
  const onPointerDown = (e: React.PointerEvent) => {
    dragRef.current = { x: e.clientX - panRef.current.x, y: e.clientY - panRef.current.y };
    (e.target as HTMLElement).setPointerCapture(e.pointerId);
  };
  const onPointerMove = (e: React.PointerEvent) => {
    if (!dragRef.current) return;
    panRef.current = { x: e.clientX - dragRef.current.x, y: e.clientY - dragRef.current.y };
  };
  const onPointerUp = (e: React.PointerEvent) => {
    const moved = dragRef.current
      ? Math.abs(e.clientX - dragRef.current.x - panRef.current.x) +
        Math.abs(e.clientY - dragRef.current.y - panRef.current.y)
      : 0;
    dragRef.current = null;
    if (moved > 0) return; // 是拖拽不是点击
    // 点选单位
    const rect = canvasRef.current?.getBoundingClientRect();
    if (!rect) return;
    const mx = e.clientX - rect.left;
    const my = e.clientY - rect.top;
    const hit = unitScreenRef.current.find(
      (u) => Math.hypot(u.x - mx, u.y - my) < Math.max(u.r, 14)
    );
    onSelectUnit(hit ? hit.name : null);
  };

  return (
    <div ref={wrapRef} className="absolute inset-0 cursor-grab active:cursor-grabbing">
      <canvas
        ref={canvasRef}
        className="h-full w-full"
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
      />
    </div>
  );
}

// 风格化松树
function drawPine(ctx: CanvasRenderingContext2D, x: number, y: number, s: number) {
  // 地面阴影
  ctx.beginPath();
  ctx.ellipse(x, y + s * 0.1, s * 0.5, s * 0.18, 0, 0, Math.PI * 2);
  ctx.fillStyle = "rgba(0,0,0,0.3)";
  ctx.fill();
  // 树干
  ctx.fillStyle = "#4a3626";
  ctx.fillRect(x - s * 0.06, y - s * 0.5, s * 0.12, s * 0.55);
  // 三层树冠
  const layers: [number, number, number][] = [
    [0.95, 0.42, 0], // 宽, 高, 底偏移
    [0.72, 0.38, 0.34],
    [0.48, 0.34, 0.64],
  ];
  layers.forEach(([w, h, off], i) => {
    const base = y - s * (0.1 + off);
    const grad = ctx.createLinearGradient(x, base - s * h, x, base);
    grad.addColorStop(0, i === 2 ? "#3d6b52" : "#2c5540");
    grad.addColorStop(1, "#1d3a2c");
    ctx.beginPath();
    ctx.moveTo(x, base - s * h - s * 0.12);
    ctx.lineTo(x + (s * w) / 2, base);
    ctx.lineTo(x - (s * w) / 2, base);
    ctx.closePath();
    ctx.fillStyle = grad;
    ctx.fill();
  });
  // 雪顶
  ctx.beginPath();
  ctx.moveTo(x, y - s * 1.16);
  ctx.lineTo(x + s * 0.14, y - s * 0.98);
  ctx.lineTo(x - s * 0.14, y - s * 0.98);
  ctx.closePath();
  ctx.fillStyle = "rgba(230,238,248,0.9)";
  ctx.fill();
}

// 单位标记：发光圆点 + 旗帜杆
function drawUnit(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  tile: number,
  color: string,
  selected: boolean,
  time: number
) {
  // 地面光圈
  ctx.beginPath();
  ctx.ellipse(x, y, tile * 0.34, tile * 0.14, 0, 0, Math.PI * 2);
  ctx.fillStyle = selected ? "rgba(240,180,90,0.35)" : "rgba(0,0,0,0.35)";
  ctx.fill();

  if (selected) {
    const pulse = (Math.sin(time / 300) + 1) / 2;
    ctx.beginPath();
    ctx.ellipse(x, y, tile * (0.4 + pulse * 0.1), tile * (0.17 + pulse * 0.04), 0, 0, Math.PI * 2);
    ctx.strokeStyle = `rgba(240,180,90,${0.5 + pulse * 0.5})`;
    ctx.lineWidth = 1.6;
    ctx.stroke();
  }

  // 身体
  const r = tile * 0.2;
  const grad = ctx.createRadialGradient(x - r * 0.4, y - tile * 0.5 - r * 0.4, r * 0.2, x, y - tile * 0.5, r * 1.4);
  grad.addColorStop(0, "#ffffff");
  grad.addColorStop(0.25, color);
  grad.addColorStop(1, "rgba(0,0,0,0.6)");
  ctx.beginPath();
  ctx.arc(x, y - tile * 0.5, r, 0, Math.PI * 2);
  ctx.fillStyle = grad;
  ctx.fill();
  ctx.strokeStyle = "rgba(255,255,255,0.25)";
  ctx.lineWidth = 1;
  ctx.stroke();
}
