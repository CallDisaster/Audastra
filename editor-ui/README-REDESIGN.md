# Forge Studio · 通用编辑器前端

> 当前默认入口已切换到 R02 九模块工作台，见 [R02 交付说明](README-R02.md)。本文件记录 R01；使用 `?r01` 打开，旧版图纸入口为 `?r01#design`。

## 运行

```powershell
npm run dev -- --host 127.0.0.1 --port 5178 --strictPort
```

打开 `http://127.0.0.1:5178`；界面图纸入口位于右上角，也可以打开 `/#design`。生产构建使用 `npm run build`，构建目录为 `dist`。

## 当前功能

- 蓝图：节点拖动与8px吸附、缩放和平移、小地图定位、类型端口连线、删除连线、节点复制/删除、参数编辑、大纲、结构诊断、JSON 文档编辑。
- 工程：新建、导入/导出 `.forge.json`、本地保存、跨模块撤销/重做、命令搜索、多蓝图与多脚本。
- 数据：对象搜索、新建、属性编辑；资源模块维护文件路径、类型和大小索引，不保存文件二进制。
- 扩展：从 JSON 导入/修改定义，根据定义自动生成参数和端口。不按函数编号、命名空间或地图名称限制编辑。定义数组的示例可在界面“导入定义”中查看。
- 外观：石墨、深海、浅色主题，紧凑密度，工程树/属性面板切换，390–1600px窗口验证。

蓝图是持久化的工程文档。当前没有脚本执行器，也未连接原生 C++ Core、WTG/WCT 或 MPQ；“检查”验证文档结构，不表示地图编译或游戏运行通过。后端迁移设计见 `../docs/EDITOR_REDESIGN_2026-09-22.md`。

`Ctrl+S` 保存到当前浏览器源的本地存储。更换端口/浏览器不会自动共享该存储，跨环境使用“导出工程 / 打开 JSON 工程”。缺失定义及未知字段随工程完整保存。

## 验证

```powershell
node tools/check-model.mjs
npx eslint src/studio src/App.tsx
npm run build
npx --yes --package @playwright/cli playwright-cli -s=forge-check open http://127.0.0.1:5178
npx --yes --package @playwright/cli playwright-cli -s=forge-check resize 1600 1000
npx --yes --package @playwright/cli playwright-cli -s=forge-check run-code --filename tools/browser-regression.js
npx --yes --package @playwright/cli playwright-cli -s=forge-check run-code --filename tools/browser-interactions.js
```

浏览器回归脚本在隔离的测试浏览器中操作示例工程，请不要对保存了作者工程的浏览器运行。文件选择器由 CLI 的 `upload` 独立处理。测试与截图输出到 `output/`，不进入 Git。

旧 React 组件保留在原目录中，但新入口不再引用。此前已有的 Qt/MCP 工作区修改保留，本次未覆盖。
