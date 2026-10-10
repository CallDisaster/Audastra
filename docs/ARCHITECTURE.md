# 架构设计

## 进程与模块边界

```text
Qt 桌面端 ─┐
CLI ────────┼─> EditorSession ─> War3MapCore codec/validator
MCP（后续）─┘          │
                      └─> revision 校验的 overlay 事务
                                      │
                                      └─> 独立候选工作区

War3MapCore ─> 不可变 SceneSnapshot ─> 版本化 C renderer plugin（可选）
```

`War3MapCore` 不依赖 Qt。GUI、CLI 与后续 MCP 都只是同一套项目 revision、事务、diff 和验证服务的适配器。

## 工作区策略

- `DirectoryWorkspace` 记录每个源成员的大小、时间戳和 SHA-256 快照。
- session 生命周期内所有源成员保持只读。
- 使用具名 Windows semaphore，确保同一已解析工作区在跨 session、跨进程场景下最多只有一个活动写入者。
- 触发器与物体 patch 只能由对应 codec 生成，是绑定源成员 SHA-256 身份的 move-only 对象；调用方不能自行构造原始替换声明。
- patch 使用规范化相对路径，并受所属格式的成员域约束。
- 候选路径检查使用 Windows handle 的最终路径和不区分大小写的组件边界，并解析 reparse point。
- 候选先在同级临时目录构建；复制和序列化出的每个成员都会重新计算 SHA，随后再次验证源工作区，最后再将目录重命名到目标位置。
- 转义后的 JSON manifest 使用同级 sidecar，避免被误当成 Warcraft 地图成员。
- 已存在的候选不会被覆盖。
- `MpqWorkspace` 当前仍是明确的只读、fail-closed 空实现；只有能够证明抽取身份后才会启用。

## Core codec 契约

每个格式 codec 统一实现 `Parse`、`Validate`、`Serialize`、`Diff` 与 `Inspect`。能安全确定边界的未知字节可以连同 `OriginSpan` 保留；版本或结构未知时禁止编辑。

Core/UI/MCP 边界只传递稳定 ID，不传递格式内部裸指针或 Qt value。

## 兼容配置

- `classic-1.27a-zhCN`：Warcraft III 1.27a World Editor。

游戏数据根和 UI 根由用户配置，不进入仓库。`profiles/` 中的 JSON 文件是内置配置的事实来源；CMake 只嵌入其原始字节，不做语义重写。配置文件的精确字节 SHA 与 UI Catalog SHA 都属于项目 revision。

UI Catalog 不包含编译进程序的第三方函数清单。`ui/config` 中的层按顺序动态发现：已安装的层参与定义覆盖和签名计算，未安装的可选层只产生诊断，不会被虚构或自动补齐。WTG 节点始终以文件内的原始函数名作为持久身份；主编辑树则必须使用当前生效 UI TXT 的 `description` 位置模板生成自然语言。原始函数名属于检查器、诊断和悬浮提示，不能代替面向作者的低代码界面。

自然语言展示由无 Qt 依赖的 Trigger Presentation 服务生成，因此 Qt、CLI 和后续 MCP 共享完全相同的模板解释规则。`${...}` 标签只负责可读说明，按出现顺序与非 `nothing` 参数配对；嵌套 call 和数组索引递归展开。预设显示值来自当前 Catalog 的 `TriggerParams`，若数据根没有提供 `WESTRING_*` 翻译，则动态降级到同一条定义的脚本值，不在程序中硬编码第三方名称。

Trigger Editing 同样位于无 Qt Core。新节点的类型、参数、默认值、预设和默认 call 均来自当前 Catalog；call 必须提供与目标参数完全一致的返回类型。每次新增只生成一份包含尾部 ECA/参数节点和附着点的收据，Qt `QUndoStack` 以严格逆序调用 Core 的 undo/redo，不复制完整 `TriggerDocument`。重做会先验证附着点和尾部 generation，再改变图，防止失败后留下半写节点。生成 ID 使用独立稳定域，对外仍只暴露稳定 ID。

GUI 参数对话框只编辑普通、已识别的节点：标量、预设和变量直接形成参数 patch；嵌套 call 通过返回类型过滤的函数选择器创建或进入。数组索引、9527、未知定义和不能证明边界安全的复杂替换继续保持只读。关闭、切换项目或重载 UI Catalog 前如存在未导出修改，桌面端必须显式确认。

WTG v7 本身不保存 ECA 参数数量。因此，当 WTG 引用当前 Catalog 中不存在的函数时，Core 会报告原始函数名并停止结构解析，禁止猜测参数边界。作者可以据此补回库，或在具备该库的环境中删除对应节点。

## 首版明确不做的内容

- 不实现 3D renderer、MDX 预览或地形编辑。
- 不承诺完整 WTG→JASS 生成。
- 不把任何通用 LNI/MPQ 输出描述为可直接运行。
- 不提供直接覆盖源地图的 API。
