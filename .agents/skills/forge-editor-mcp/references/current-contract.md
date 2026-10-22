# 当前接入合同

核查日期：2026-10-04。本轮夜间树起于 `ecf68922b0db9c9cd4ed5c4aa1f984b605524c06`；实际候选提交和 EXE 身份见随包 manifest 与回执，旧便携包不会自动获得新能力。运行中的 initialize/tools/list/schema 始终优先。

## 已核实的 stdio 入口

便携包 `Start-MCP.cmd` 调用自身 `bin/war3-map-mcp.exe` 并透传参数。可直接使用该 EXE 避免客户端对 cmd 文件的不同处理。例如通用客户端配置：

```json
{
  "mcpServers": {
    "forge": {
      "command": "C:/FORGE/bin/war3-map-mcp.exe",
      "args": [
        "--workspace", "D:/Maps/EditableWorkspace",
        "--ui-data", "D:/Tools/YDWE/UI"
      ]
    }
  }
}
```

这些是示例位置，不是本机已存在路径；使用客户端对应的配置语法。`--workspace` 指向受支持的展开工程，不能仅把 .w3x 当成这个目录；独立归档工具另有明确的路径/来源 SHA 参数。`--ui-data` 必须绑定真实有效 UI 来源，不能凭安装名推断。

工具根/游戏根由可信宿主的 `FORGE_MCP_YDWE_ROOT`、`FORGE_MCP_WAR3_ROOT` 配置提供；并非每个请求可任意选择编译器。完整 Lua 使用宿主绑定的 `--lua-map-target` 档案，实际存在时按当前 schema 选择 `configured_lua`，不是给任意经典/MemHack 地图强制改输出语言。

初始化按当前实现协商协议版本；工具的 `listChanged` 取决于宿主是否安装通知通道，资源列表不推送变化。GUI 的 JSON-only HTTP 没有通知流，修改权限后重新查询工具目录。没有开放 prompts；Skill 通过 resources 或随包文件发现。工具失败或空工作区不能因为 initialize 成功而当作已加载地图。

## 工具族

| 需求 | 已见工具 | 使用要点 |
|---|---|---|
| 工作区身份/事务 | get_revision、begin_edit、preview_edit、validate_edit、rollback_edit | 阅读实际 revision、来源复核、transaction 和 edit generation 合同 |
| 地图只读/归档 | describe_map_data、query_map_data_page、query_map_archive_member_identities | 分页、覆盖和真实成员身份；成员哈希不授权读不透明正文。前两者可用 source_map_path 直接只读查询 .w3x（与 expected_revision 二选一），结果直接用于 save_map_edit_candidate 的 field_edits |
| 七类物编 | objects_list、objects_get、objects_field_options、objects_set_fields、objects_create、objects_delete | GUI/独立入口共用 schema；rawcode区分大小写，未知引用覆盖禁止删除。独立服务的三个查询工具可用 source_map_path + expected_source_sha256 直接只读查 .w3x |
| 变量 | variables_list、variables_get、variables_create、variables_update、variables_rename、variables_delete | 当前StableId与opaque修订；改名需要实际代码块SHA审计，未知引用删除拒绝 |
| 实体字段 C1 | entities_query、entities_set_fields | 当前typed覆盖查询；source_index、row_key、完整成员SHA、精确raw与CAS成组绑定；doodads只读 |
| 预放命令 C2 有限范围 | entities_rotate、entities_copy | classic普通units/doodads；绝对朝向弧度与共同XYZ偏移copy，原Undo/有限原来源保存；真实单位两类保存与回读通过，地物真实保存及整个实体CRUD尚未验收 |
| 设置与字符串 D | map_settings_get/set、strings_query/set | 稳定w3i/string_id、当前成员SHA/raw/CAS；v25 literal与已有WTS shared_string显式区分；原Undo/字段保存，不创建WTS或整组D–H |
| 已有声音 E0 | sounds_list/get/update | 原名字字节sound_id、当前成员SHA/raw/CAS；只改完整classic v1的volume/fade_in/fade_out整数，原Undo/字段保存；扩展、歧义及其它属性拒绝，真实/原版/游戏验收另记 |
| 导入元数据 E1 | imports_list/get | 区分原IMP记录、物理成员与GUI待导入队列，绑定source/inventory及当前修订的cursor；默认64/最多128行、每页最多4096行匹配检查。保留原flags/path hex/歧义与覆盖未知；pending只reported_bytes，不读磁盘、不造SHA。只读，不提供正文。新增/删除走编辑器端口的 imports_add / imports_remove（进资源撤销栈；删除前按当前内容查路径，找到拒绝 IMPORTS_REMOVE_REFERENCED，allow_referenced=true 才删，回执 mentioned_in / unread_sources），独立服务的 archive_members_add / archive_members_remove，或 CLI archive-member-add / archive-member-remove |
| 单位查询 C3 | 原 entities_query | 可选精确type_id/player组合筛选，绑定同身份/成员SHA/过滤的原索引cursor；每页128行、4096原记录检查，大表总数未知；不新增工具或改变保存 |
| 普通单位坐标 C4 | 原 entities_set_fields | Ready classic8/11 non-sloc行绝对native x/y/z；有限±1000000先double校验，原Undo/fieldEdits保存；sloc/Skin/Partial拒绝，三轴实图原保存回读通过 |
| 自然 T 图 | query_trigger_graph、edit_trigger_graph | categories/variables/triggers/ecas/arguments/map_objects/trigger_text/feature_text；真实 ID、类型和 guard；insert_text / replace_trigger_text / patch_trigger_text 紧凑写法，apply_feature_text 功能写法；编辑器内 dry_run 预览差异 |
| T 整图检查 | editor_check_trigger_graph（CLI trigger-check） | 同保存链生成整图并检查；可带批次只在私有副本上检查；诊断带 t（紧凑 T 行列） |
| 简单 T 编辑 | create_trigger、append_root_eca、append_argument_call、set_scalar_argument | 老式窄操作不代表完整图操作资格；直接值支持整数/实数/布尔/文本/自定义代码/物编对象编号（`A04W`），句柄类型须用变量或函数 |
| 归档保存 | save_map_edit_candidate | 完整来源/工具/UI/overlay 输入，同一地图保存服务；保存后新身份 |
| 展开目录导出 | export_map_candidate | 不要与归档保存工具混淆 |
| 归档展开与导入成员 | archive_expand、archive_members_add、archive_members_remove | 独立服务，Save 权限。archive_expand 把 .w3x 具名成员写到新目录供 --workspace 使用，data_only 不写导入资源；成员增删写新候选，只改请求成员和 war3map.imp，不重建脚本；删除前按字符串查找路径出现处，找到即拒绝（MEMBER_REFERENCED），allow_referenced=true 才删，查不到不等于无引用 |
| 地形/模型 | get_terrain_snapshot、query_terrain_block、capture_model_animation | 读取快照/区块；完成帧身份和设备证据另核 |

这张表用于独立宿主的 stdio/本机 HTTP 工作区，不替代 tools/list 的真实结果；关闭或缺失的工具不能调用。`forge://skills/manifest` 提供此次服务捕获的 Skill 版本、逐文件 SHA 与可读 URI；资源是不可变捕获字节，文件更新后须由宿主重新注册。

C1查询包括活动事务覆盖，旧基础成员查询仍保持已提交来源合同。字段值输入使用字符串，索引使用规范十进制字符串，单命令同类1–32项。GUI命令沿用原MapDataEditSession及撤销栈，保留已有作者编辑；独立事务累计原来源最终差异最多32项，只通过原fieldEdits保存分支发布，不混合触发器/物编净改动。独立Core session回执每次来自实际隔离候选，不能据此宣称跨工具会话身份连续；核对公开修订、原transaction envelope和成员SHA。C4仅普通单位绝对native XYZ，不采样地形；sloc/Skin/Partial拒绝。实体导航、其余完整移动、新增和删除尚未开放。

C2使用相同查询定位；旋转传数值 `facing_radians`，复制传同类1–32项 `sources` 与世界数值 `offset{x,y,z}`。C1 facing普通数值文本按度解析、显示为度；`f32:` raw保留原生弧度位值。特殊地物与sloc复制不支持；sloc朝向保持XYZ/W3I。GUI使用当前原会话，保留作者此前编辑；独立保存只支持一批原来源复制，或同一原行连续绝对朝向，保存冻结最后请求float及完整原来源native重放。已有预放净差时拒绝第二复制、预放混合及其它原行旋转；字段净差和触发器/物编混合保存也拒绝。全部恢复原native字节后可清除净差，开始新的受支持命令；保存只允许原成员SHA精确匹配的净零map-data覆盖，不接受其它额外变化。64MiB复制前wire预算不是RSS。C1地物字段仍只读，C2不能被称为任意新建/删除/移动或整个C已完成，实际验收见夜间报告。

## GUI 活动地图与内置助手

设置启用 HTTP 后，GUI 在 `http://127.0.0.1:<实际端口>/mcp` 提供同一个 live 工具入口。访问令牌由当前 Windows 用户加密保存；复制设置里的客户端配置，不把令牌提交到仓库或地图。服务启动失败不会假报监听成功。

- `editor_state` 返回当前 GUI 地图会话身份、文档 generation 和编辑 revision，各编号空间分开。
- `editor_query_trigger_graph` 和 `editor_edit_trigger_graph` 复用 Core 图 DTO/类型/guard；写操作核验实际 schema 的身份字段，整批事务进入 GUI 的一个撤销命令。它们不操作独立 stdio 的另一份地图状态。
- `editor_save_map` 经 GUI 原保存服务写回或另存；写回仍自动备份。能力、路径和身份以当次 schema 为准。
- 主窗口设置控制查询/编辑/保存/外部操作及逐工具开关。列工具和执行均检查权限；旧客户端缓存的工具名不能绕过关闭。JSON-only HTTP 不提供 listChanged 通知流，修改设置后重新拉列表。
- 没有开放的文件/命令执行能力不得用额外 shell 或直接改归档绕开。自然语言 T 仍是默认逻辑形式。

## 图与保存边界

- `query_trigger_graph` 可分页，`arguments` 定点查询最多16个 ID，并可请求参数原位替换 guard；参数及限制以 schema 为准。
- `edit_trigger_graph` 在目录单写事务中完成核心支持的创建/字段/精确改名/移动/删除/类型子树及受 guard 的替换。CallerAsserted 的 opaque code 审计仅是调用方声明，不是服务端自动证明“代码无引用”。
- 引用一律用查询结果各行 `ref` 里的 `{kind, id}`。分类行另有 `category_id`（WTG 分类号），不能当引用编号；误用时报 `TRIGGER_GRAPH_RECORD_MISSING` 并给出正确的 `ref.id`。
- `move` 的 `from`/`to` 写 `{"kind":"category_triggers","owner":<分类行 ref>,"ordered_children":[该分类当前全部触发器 ref，按移动前顺序]}`；同分类内调整顺序时两者相同。`before` 是要排到其前面的触发器 ref，省略则放到末尾。例如把末尾的 A 移到 `.7` 之后：`before` 写原本紧跟 `.7` 的那个触发器。
- 函数槽参数（`eventcall` / `boolexpr`、`boolcall` / `code`）的 `call` 分别写 `kind` 为 `event` / `condition` / `action`，例如 `AddTriggerEvent` 第二个参数填 `TriggerRegisterUnitEvent`（event）；也可填返回该类型的普通 call。其余参数只能是 call。
- 整段写入优先用紧凑 T 写法：`insert_text`（插到触发器根或块分支）、`replace_trigger_text`（整体替换触发器内容），读取用 `trigger_text` 表加 `trigger_id` 导出同一写法；规则见随包 AGENT_GUIDE 的“紧凑 T 写法”。仍是普通 T 树，走同一类型检查、撤销与保存。
- 一个功能（变量 + 多个触发器）用 `apply_feature_text` 一次写进一个类别，读取用 `feature_text` 表；只改几行用 `patch_trigger_text`（导出行号 + 原文 guard）；编辑器内 `dry_run: true` 只预览 `trigger_diffs`，不发布。
- 写入前后用 `editor_check_trigger_graph` 做整图检查（`wait_ms` 直接等结果）；带同一批次时只在私有副本上检查。整图来源不完整报 `TRIGGER_CHECK_CONTEXT_INCOMPLETE`，不算通过。
- 大批重构分批查询、编辑和保存。保存从冻结输入构建脚本和归档；候选生成、独立重开及新图查询各自取得新身份，不在已失效 overlay 上继续使用旧 ID。
- 编码、include、工具身份、未知语言 API 和类型门禁仍由共用保存服务检查；来源变化或失败不回落到旁边另一套安装。
- 默认自然 T；`script_target` 是编译输出目标，不是把作者触发器转换成自定义代码的授权。
