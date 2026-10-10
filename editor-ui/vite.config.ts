import { rmSync } from "fs";
import path from "path";
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// public/local-data 是本机导出的原版游戏数据，只供开发服务器读取；构建产物中删除，避免随 dist 分发。
const excludeLocalGameData = {
  name: "exclude-local-game-data",
  apply: "build" as const,
  closeBundle() {
    rmSync(path.resolve(__dirname, "dist/local-data"), { recursive: true, force: true });
  },
};

// https://vite.dev/config/
export default defineConfig({
  base: "./",
  plugins: [react(), excludeLocalGameData],
  server: {
    port: 3000,
  },
  resolve: {
    alias: {
      "@": path.resolve(__dirname, "./src"),
    },
  },
});
