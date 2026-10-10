import { useEffect, useRef } from "react";
import { WORLD, WATER_LEVEL, heightAt, terrainColor, css, units, region } from "@/lib/iso";

// 小地图：顶视角渲染同一个世界
export function Minimap() {
  const ref = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    const S = 148;
    canvas.width = S * dpr;
    canvas.height = S * dpr;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.scale(dpr, dpr);

    const cell = S / WORLD;
    for (let y = 0; y < WORLD; y++) {
      for (let x = 0; x < WORLD; x++) {
        const h = heightAt(x + 0.5, y + 0.5);
        ctx.fillStyle = css(terrainColor(h, x, y));
        ctx.fillRect(x * cell, y * cell, cell + 0.5, cell + 0.5);
      }
    }
    // 水域压暗一点增加对比
    for (let y = 0; y < WORLD; y++) {
      for (let x = 0; x < WORLD; x++) {
        if (heightAt(x + 0.5, y + 0.5) < WATER_LEVEL) {
          ctx.fillStyle = "rgba(10,30,60,0.25)";
          ctx.fillRect(x * cell, y * cell, cell + 0.5, cell + 0.5);
        }
      }
    }
    // 区域
    ctx.strokeStyle = "#4cc2ff";
    ctx.lineWidth = 1.2;
    ctx.strokeRect(region.x0 * cell, region.y0 * cell, (region.x1 - region.x0) * cell, (region.y1 - region.y0) * cell);
    // 单位点
    for (const u of units) {
      ctx.beginPath();
      ctx.arc(u.x * cell, u.y * cell, 2.2, 0, Math.PI * 2);
      ctx.fillStyle = u.color;
      ctx.fill();
    }
    // 视野框（演示固定）
    ctx.strokeStyle = "rgba(255,255,255,0.85)";
    ctx.lineWidth = 1;
    ctx.strokeRect(S * 0.28, S * 0.26, S * 0.42, S * 0.4);
  }, []);

  return (
    <div className="glass overflow-hidden p-1.5">
      <canvas ref={ref} style={{ width: 148, height: 148 }} className="rounded-[8px]" />
    </div>
  );
}
