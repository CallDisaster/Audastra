# Forge Studio R02 · 前端交互原型

> 节点内编辑和通用选取已更新为 [R03](README-R03.md)，共享 R02 工程存储。

2026-09-22。入口：<http://127.0.0.1:5178/>。本轮以用户确认的模块规划为基础，由触发器、物编/变量、声音/资源三个并行分工与主工作台集成完成。

## 查看与运行

```powershell
npm run dev -- --host 127.0.0.1 --port 5178 --strictPort
```

当前默认显示 R02，`?r01` 保留上一版工作台。旧版本按需加载。R02 保存使用独立的 `forge-workbench-r02` 浏览器存储，不覆盖 R01 存储。

`Ctrl+S` 保存，`Ctrl+K` 搜索，`Ctrl+Z` 撤销，`Alt+1` 至 `Alt+9` 切换模块。通过工程菜单导入、导出 JSON；跨浏览器、端口使用时需要导出文件。交互测试只在独立测试浏览器中重置示例。

## 已接入的工作区

| 工作区 | 本轮可操作内容 |
| --- | --- |
| 触发器 | 传统事件/条件/动作树、蓝图、代码视图；直接参数编辑；常量/变量/表达式/物体选择；端口连线；右键搜索函数；多选、移动、复制、删除、撤销；定义导入 |
| 物体 | 单位、物品、可破坏物、装饰物、技能、魔法效果、科技；搜索、筛选、新建、复制、批量字段编辑、恢复默认、等级矩阵、引用跳转 |
| 变量 | 类型、数组、初始值；新建、复制、删除；重命名影响预览；结构化绑定引用与待核对表达式区分 |
| 声音 | 声音定义、真实音频试听、波形、播放进度、循环及空间参数；复制、删除、资源关联 |
| 资源/输入 | 真实文件导入、替换、导出；路径编辑与冲突检查；列表/网格、分类、引用；文本、图片、音频预览 |
| 物体管理 | 跨模块目录、搜索、静态引用与定位 |
| 战役 | 地图列表与章节菜单分别维护；标题、顺序、可见性、地图关联、菜单预览、共享内容入口 |
| AI | 常规策略、英雄技能顺序、建造顺序、采集、进攻波次、对象选择及静态引用检查 |
| 世界 | 俯视布局预览、对象放置与移动、区域、实例属性、物编跳转；为后续地形工作区留位 |

工程、撤销/重做、保存、查找和对象身份由各模块共享。字段、函数定义和对象选择遵循数据定义；没有针对函数编号 9527/9857 的内置拒绝规则。

## 原型边界

- 当前使用示例工程和前端 JSON 文档，尚未连接 C++ Core、原生 WTG/WCT、MPQ 或地图构建。结构检查不代表游戏验证。
- JASS、vJass、Lua 均可编辑；代码预览覆盖当前支持的节点及提供生成符号的定义，不是完整语言编译器。
- 可视结构生成文本时保留来源映射。恢复时使用已有结构；改动后的文本可先保留为独立脚本。没有实现任意手写代码的无损反编译。
- 变量表达式和代码文本中的潜在引用标记为待核对，不把字符串搜索伪装成语义解析。
- 物编字段库是可扩展的示例集合，尚未导入各 Warcraft 版本的完整字段元数据。
- 资源原型内嵌文件上限为单文件 2 MiB、总量 3 MiB；模型三维预览未接通。实际产品应交由 C++ 资源服务维护文件。
- 声音试听通过浏览器音频完成；空间距离和淡入淡出为可保存参数，不构成 Warcraft 空间音频运行验证。
- AI 运行测试未接通；世界是二维布局预览，坐标表示布局比例，不等同游戏世界坐标，尚未实现地形绘制与三维渲染。

## 验证记录

- `npm run build`：通过，TypeScript 与 Vite 生产构建成功。主脚本 453.23 kB，R01 按需脚本 49.96 kB；无大块警告。
- `npx eslint src/workbench src/App.tsx`：通过。
- `node tools/check-workbench.mjs`：8 项通过，结果在 `output/check-workbench/results.json`。
- 浏览器交互断言共 50 项通过：`r02-layout-check.js` 13 项，`r02-workflow-check.js` 7 项，`r02-trigger-check.js` 13 项，`r02-final-check.js` 17 项。
- 检查内容包含九模块布局、字段编辑与撤销、真实音频播放、文件导入、保存刷新、三视图参数同步、三种代码语言、多节点操作、函数搜索、变量改名影响、等级表恢复、主题切换，以及 1440×900、1280×800、1024×768 桌面尺寸。
- 浏览器检查期间控制台 0 errors、0 warnings；React 开发工具提示为 info。

验证脚本位于 `tools/`，截图位于 `output/playwright/`。它们操作示例数据，不得在有未导出作者数据的浏览器会话中执行。

## 视觉审查记录

R02，1600×1000。审查过蓝图、物编、声音、技能等级矩阵和 AI 建造视图。已修正物编根 section 未铺满可用宽度的问题，并把示例蓝图调整为三列布局与 100% 初始阅读比例。等级表与 AI 表格在最终审查中没有文字遮挡或横向溢出。

- `output/playwright/workbench-r02-triggers-final.png`：最终蓝图，无临时提示遮挡；节点参数、树、属性和工具条清晰。
- `output/playwright/workbench-r02-levels.png`：技能等级矩阵，所有等级与引用面板可见。
- `output/playwright/workbench-r02-ai.png`：建造顺序、对象选择、条件及策略概览可见。
- 其他已留存视图：`workbench-r02-objects.png`、`workbench-r02-sound.png`、`workbench-r02-campaign.png`、`workbench-r02-traditional.png`。

## 后续 C++ 迁移入口

| 文件 | 职责 |
| --- | --- |
| `src/workbench/types.ts` | 共享文档类型与模块回调契约 |
| `src/workbench/sample.ts` | 示例工程、空工程与导入验证 |
| `src/workbench/Workbench.tsx` | 导航、工程存储、历史、搜索、设置 |
| `src/workbench/TriggerWorkspace.tsx`、`triggerCode.ts` | 触发器交互与有限代码生成 |
| `src/workbench/ObjectWorkspace.tsx`、`objectFields.ts` | 物编交互与字段元数据 |
| `src/workbench/VariablesWorkspace.tsx` | 变量与影响分析 |
| `src/workbench/SoundWorkspace.tsx`、`ResourcesWorkspace.tsx` | 声音与资源工作流 |
| `src/workbench/ProjectWorkspaces.tsx` | 战役、AI、管理器、世界布局 |
| `src/workbench/references.ts` | 跨模块静态引用索引 |
| `src/workbench/ui.tsx`、各 CSS | 共享控件、视觉尺寸与状态 |

后续复刻应先确认这版交互，再把共享文档、操作历史、资源引用和诊断接入 C++ 服务。原生地图往返、编译及游戏测试需要分别验证。本轮没有修改现有 C++、地图或游戏运行链。
