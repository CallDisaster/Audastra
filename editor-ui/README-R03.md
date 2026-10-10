# R03 · 节点内编辑与通用取值

预览：<http://127.0.0.1:5178/>。沿用 R02 工程和存储，不修改 C++ 或原生地图。

## 两种选取方式

- 蓝图：数字、文本和布尔直接在节点内编辑。引用类型在整个节点右侧展开非模态小面板，覆盖其他节点；右侧空间不足才翻转。选择固定值立即生效，Escape/外部点击关闭。
- 触发器取值：全局变量 → 函数 → 固定值。按参数类型过滤变量和返回值函数，支持数组下标、函数嵌套入参和自定义表达式。传统 ECA 参数窗口也复用这套内容。
- 物编引用：属性行显示头像和名称；已选列表支持添加、替换、删除、上移、下移。候选目录只允许对应类型的固定对象，保留未知旧 ID，不接收运行时变量或函数。
- 共用目录：名称/rawcode 搜索、种族/分类过滤、图标预览、悬停/键盘聚焦详情。详情可停留滚动；变量、函数、区域等目录也支持搜索。
- 已连接的数据参数显示连接状态，避免把默认值编辑误当成修改连线结果。修改可撤销，并纳入工程保存。

## 结构

- `NodeParameterField.tsx`：行内输入、参数按钮、节点旁定位和事件隔离。
- `SharedValuePicker.tsx`：三来源内容、类型过滤、函数参数与对象目录。
- `valueSources.ts`：类型契约和返回值函数目录；导入函数定义按同一契约参与选择，不按名称或编号限制。
- `ObjectPortrait.tsx`、`public/portraits/`：优先显示导入图标；示例使用 6 个原创职业头像和 3 个原创技能图标。游戏 BLP/MDX 路径不会被当作网页 URL 请求。
- `ObjectWorkspace.tsx`：有序对象引用列表，复用固定值候选内容。
- `triggerCode.ts`：保存和生成结构化函数绑定；全局变量、数组下标与嵌套函数同步到 JASS/vJass/Lua 预览。
- `VariablesWorkspace.tsx`：追踪嵌套函数内的变量引用；表达式文本保留待核对提示。

字段使用 `valueType`、`objectKind` 元数据；函数定义使用 `returnType` 与语言映射。旧 core 字段有兼容映射，扩展中的普通 `target` 等字段名不会被强制当成区域。

## 验证

- `r03-parameter-check.js`：62 项浏览器断言通过，覆盖行内编辑/取消/撤销、键盘与拖动隔离、选择器层级/定位/滚轮/窄视口，以及物编有序列表操作与持久化。
- `r03-function-check.js`：13 项通过，覆盖三来源、严格返回类型、嵌套函数、数组参数、三种语言预览、保存恢复以及切回固定值时移除旧绑定。
- `check-workbench.mjs`：13 项数据检查通过，包括导入自定义函数、未知定义诊断与结构化绑定往返。
- `r03-final-views.js`：验证行内输入 Ctrl+S 保存后刷新、右侧优先、方形头像；最终流程没有浏览器错误或警告。
- TypeScript/Vite 构建与 `eslint src/workbench src/App.tsx` 通过。

所有浏览器验证运行在独立测试会话，操作示例工程，不覆盖作者浏览器数据。联调时遇到过组件尚未落盘产生的临时 HMR 导入错误，补齐后已恢复；最终流程错误收集为空。

## 视觉审查

R03，1600×1000。已审查蓝图固定值选择与物编技能列表视图。修复了右侧仅差几像素便翻转、头像被通用 span 样式拉伸、悬停详情挤压候选名称的问题。最终头像尺寸为 48×48（候选）和 34×34（详情）。

- `output/playwright/workbench-r03-blueprint-selector.png`
- `output/playwright/workbench-r03-object-selector.png`
- `output/playwright/portrait-contact-r03.png`

函数目录仍是前端原型的可扩展样例集，并非完整 Warcraft 函数数据库。代码生成验证是结构与文本层检查，不代表 JASS 编译、原生地图或游戏运行验收。物编完整版本元数据、游戏资源解码和 C++ 接入继续沿用 R02 的后续范围。

交互参考：[Epic Blueprint Basic User Guide](https://dev.epicgames.com/documentation/unreal-engine/blueprint-basic-user-guide-in-unreal-engine)、[Epic Blueprint Nodes](https://dev.epicgames.com/documentation/unreal-engine/nodes-in-unreal-engine)。具体的右侧头像面板与物编有序列表依据用户提供的两种工作流。
