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
| 七类物编 | objects_list、objects_get、objects_text、objects_field_options、objects_set_fields、objects_apply_text、objects_create、objects_delete | GUI/独立入口共用 schema；rawcode区分大小写，未知引用覆盖禁止删除。独立服务的四个查询工具可用 source_map_path + expected_source_sha256 直接只读查 .w3x |
| 变量 | variables_list、variables_get、variables_create、variables_update、variables_rename、variables_delete | 当前StableId与opaque修订；改名需要实际代码块SHA审计，未知引用删除拒绝 |
| 实体字段 C1 | entities_query、entities_set_fields | 当前typed覆盖查询；source_index、row_key、完整成员SHA、精确raw与CAS成组绑定；doodads只读 |
| 预放命令 C2 | entities_create、entities_delete、entities_copy、entities_rotate、entities_transform | 单位/物品写 `units`，装饰物/可破坏物写 `doodads`，单批 1–32。create 按物编 ID 新建，`height_offset` 相对当前地面；transform 改一个实例的绝对位置、朝向或缩放（各自可选、至少一项；position 给 x/y 加 z 或 height_offset 之一，须在地图内；scale 只用于地物，三轴 (0,1000]）；sloc 只能改朝向，特殊地物拒绝。dry_run 只预览；GUI 走原 Undo，独立事务只接受一批同类新建/删除/复制，或同一原行连续变换（后一次没给的分量沿用前一次）。用 MCP 摆的地物真实保存后进游戏尚未核对 |
| 设置与字符串 D | map_settings_get/set、strings_query/set | 稳定w3i/string_id、当前成员SHA/raw/CAS；v25 literal与已有WTS shared_string显式区分；原Undo/字段保存，不创建WTS或整组D–H |
| 已有声音 E0 | sounds_list/get/update | 原名字字节sound_id、当前成员SHA/raw/CAS；只改完整classic v1的volume/fade_in/fade_out整数，原Undo/字段保存；扩展、歧义及其它属性拒绝，真实/原版/游戏验收另记 |
| 导入元数据 E1 | imports_list/get | 区分原IMP记录、物理成员与GUI待导入队列，绑定source/inventory及当前修订的cursor；默认64/最多128行、每页最多4096行匹配检查。保留原flags/path hex/歧义与覆盖未知；pending只reported_bytes，不读磁盘、不造SHA。只读，不提供正文。新增/删除走编辑器端口的 imports_add / imports_remove（进资源撤销栈；删除前按当前内容查路径，找到拒绝 IMPORTS_REMOVE_REFERENCED，allow_referenced=true 才删，回执 mentioned_in / unread_sources），独立服务的 archive_members_add / archive_members_remove，或 CLI archive-member-add / archive-member-remove |
| 单位查询 C3 | 原 entities_query | 可选精确type_id/player组合筛选，绑定同身份/成员SHA/过滤的原索引cursor；每页128行、4096原记录检查，大表总数未知；不新增工具或改变保存 |
| 普通单位坐标 C4 | 原 entities_set_fields | Ready classic8/11 non-sloc行绝对native x/y/z；有限±1000000先double校验，原Undo/fieldEdits保存；sloc/Skin/Partial拒绝，三轴实图原保存回读通过 |
| 自然 T 图 | query_trigger_graph、edit_trigger_graph | categories/variables/triggers/ecas/arguments/map_objects/trigger_text/feature_text/presets/function_examples；真实 ID、类型和 guard；insert_text / replace_trigger_text / patch_trigger_text 紧凑写法，apply_feature_text 功能写法；编辑器内 dry_run 预览差异 |
| T 整图检查 | editor_check_trigger_graph（CLI trigger-check） | 同保存链生成整图并检查；可带批次只在私有副本上检查；诊断带 t（紧凑 T 行列） |
| 进游戏测试 | map_test_start、map_test_eval、map_test_result、map_test_capture、map_test_stop（编辑器端口） | start 默认看不见（1.27a 隐身窗口照常绘制、可截图；其他版本开在不可见的独立桌面、截不到画面；都不弹出、不抢焦点、不收键鼠，1.27a 不出声），visible: true 才显示在屏幕上；eval 用紧凑 T 动作在运行中的测试游戏里执行并读回全局变量（reads），仅 Lua 地图且开热重载，JASS 地图返回 MAP_TEST_STEPS_UNSUPPORTED；JASS 地图用 start 的 scenario（紧凑 T，只编进测试副本，开局后运行一次，Preload 记录行，“通过/失败”开头计断言），result 读回记录、断言数、reads 与 started/finished（游戏写入的资源路径只给 loaded_files 个数，场景开始前自动发空格越过“按任意键继续”）；capture 存 PNG：1.27a 由游戏自己截图（看不见的测试也能截），其他版本用系统图形捕获截窗口（只对 visible 测试有效，窗口被遮挡也可）；游戏没画出新画面时失败并给原因；stop 只结束本次启动的进程。不改地图 |
| 简单 T 编辑 | create_trigger、append_root_eca、append_argument_call、set_scalar_argument | 老式窄操作不代表完整图操作资格；直接值支持整数/实数/布尔/文本/自定义代码/物编对象编号（`A04W`），句柄类型须用变量或函数 |
| 归档保存 | save_map_edit_candidate | 完整来源/工具/UI/overlay 输入，同一地图保存服务；保存后新身份 |
| 展开目录导出 | export_map_candidate | 不要与归档保存工具混淆 |
| 归档展开与导入成员 | archive_expand、archive_members_add、archive_members_remove | 独立服务，Save 权限。archive_expand 把 .w3x 具名成员写到新目录供 --workspace 使用，data_only 不写导入资源；成员增删写新候选，只改请求成员和 war3map.imp，不重建脚本；删除前按字符串查找路径出现处，找到即拒绝（MEMBER_REFERENCED），allow_referenced=true 才删，查不到不等于无引用 |
| 地形读写 | get_terrain_edit_state、query_terrain_points、edit_terrain_stroke、edit_terrain_texture_slot、edit_terrain_bounds、edit_terrain_resize、edit_terrain_block | GUI 与独立入口同名同 schema、同一 Core 命令。先 get_terrain_edit_state 取 terrain_binding、贴图表和保存资格；笔划和贴图槽必须显式 dry_run；笔划 kind 含高度、平滑、噪声、贴图、荒芜、悬崖层级/贴图、直坡/转角坡、水面、边界、通行限制，坡道单点坐标与水位规则以工具描述为准。提交沿原 Undo，不自行保存：编辑器端口用 editor_save_map，独立目录用 save_map_edit_candidate |
| 地形/模型只读 | get_terrain_snapshot、query_terrain_block、capture_model_animation | 独立目录会话读取快照/区块（编辑器端口没有这两个，用 query_terrain_points）；完成帧身份和设备证据另核 |
| 世界视口 | world_viewport_camera_read/set、world_viewport_capture、world_viewport_capture_result | 只有编辑器端口的活动世界视口可用，独立 stdio 明确不支持；设置相机不改地图镜头对象；截图取实际完成帧，按请求 ID 取回，PNG 默认不带、最多 4MiB |
| 区域/镜头结构 | auxiliary_create、auxiliary_update、auxiliary_copy、auxiliary_delete | 读取用 entities_query；字段名按 schema，角度为度。create 给完整已知字段，copy 给新名字（区域另给新 creation_number），未提供的原字段逐位保留 |
| 地图自定义设置 | map_custom_settings_get、map_custom_settings_edit | 升级、科技限制、随机单位/物品表；先 summary 取 settings_binding，逐条 CAS，失败不发布前缀 |
| 物编资源 | object_resource_state、object_resource_query、object_resource_inspect | 先取资源身份再查候选；inspect 读实际来源的图片尺寸或模型动作，不返回正文；写字段仍用 objects_set_fields |
| 界面工作区 | game_ui_view_read/set、game_ui_description_create/read/edit/triggers、game_ui_nodes_query、game_ui_preview_set、game_ui_render、game_ui_simulate、game_ui_trigger_simulation | 描述是地图里的 `AudastraUI/<编号>` 导入文件；edit 整组检查；triggers 生成自然 T；render 出 PNG；trigger_simulation 在编辑器里跑作者自己的触发器 |
| Lua 文件与热重载 | lua_files_list、lua_file_read、lua_file_write、lua_test_hot_reload、lua_test_reload_result | 只有编辑器端口可用；写入按 sha256 整体替换并进撤销栈；热重载只发给本编辑器启动的单人测试，不保存 |
| 战役 | open_campaign、create_campaign、query_campaign、query_campaign_objects、edit_campaign、undo_campaign、redo_campaign、save_campaign、export_campaign_member | 经典 .w3n；另存到不存在的路径，in_place 走原子保存加备份；导出不覆盖原文件 |
| 运行库 | map_runtime_query、map_runtime_set | 查询本图运行库设置和可信包身份；启用须明确批准固定包，不自动选包，不保存不启动 |
| 代码检查与搜索 | code.check、project_search、editor_get_ui_catalog | code.check 异步检查 JASS/vJass/Lua5.3（submit/status/cancel）；project_search 只读全局搜索；ui catalog 读取当前触发器定义目录 |
| 反馈与更新 | feedback.draft、feedback.list、feedback.status、update.status | 只在本机起草/读取清理后的程序问题报告，不发送；update.status 只读版本与发布信任，不联网 |

这张表用于独立宿主的 stdio/本机 HTTP 工作区，不替代 tools/list 的真实结果；关闭或缺失的工具不能调用。`forge://skills/manifest` 提供此次服务捕获的 Skill 版本、逐文件 SHA 与可读 URI；资源是不可变捕获字节，文件更新后须由宿主重新注册。

C1查询包括活动事务覆盖，旧基础成员查询仍保持已提交来源合同。字段值输入使用字符串，索引使用规范十进制字符串，单命令同类1–32项。GUI命令沿用原MapDataEditSession及撤销栈，保留已有作者编辑；独立事务累计原来源最终差异最多32项，只通过原fieldEdits保存分支发布，不混合触发器/物编净改动。独立Core session回执每次来自实际隔离候选，不能据此宣称跨工具会话身份连续；核对公开修订、原transaction envelope和成员SHA。C4仅普通单位绝对native XYZ，不采样地形；sloc/Skin/Partial拒绝。实体导航、其余完整移动、新增和删除尚未开放。

C2使用相同查询定位；旋转传数值 `facing_radians`，复制传同类1–32项 `sources` 与世界数值 `offset{x,y,z}`。C1 facing普通数值文本按度解析、显示为度；`f32:` raw保留原生弧度位值。特殊地物与sloc复制不支持；sloc朝向保持XYZ/W3I。GUI使用当前原会话，保留作者此前编辑；独立保存只支持一批原来源复制，或同一原行连续绝对朝向，保存冻结最后请求float及完整原来源native重放。已有预放净差时拒绝第二复制、预放混合及其它原行旋转；字段净差和触发器/物编混合保存也拒绝。全部恢复原native字节后可清除净差，开始新的受支持命令；保存只允许原成员SHA精确匹配的净零map-data覆盖，不接受其它额外变化。64MiB复制前wire预算不是RSS。C1地物字段仍只读，C2不能被称为任意新建/删除/移动或整个C已完成，实际验收见夜间报告。

## GUI 活动地图与内置助手

设置启用 HTTP 后，GUI 在 `http://127.0.0.1:<实际端口>/mcp` 提供同一个 live 工具入口。访问令牌由当前 Windows 用户加密保存；复制设置里的客户端配置，不把令牌提交到仓库或地图。服务启动失败不会假报监听成功。

- `editor_state` 返回当前 GUI 地图会话身份、文档 generation 和编辑 revision，各编号空间分开。GUI 结果另带 `binding_token`，可代替 `project_identity`、`expected_revision` 和触发器批次的 `expected`；修改成功后返回新令牌，过期令牌按修订冲突拒绝。
- `editor_query_trigger_graph` 和 `editor_edit_trigger_graph` 复用 Core 图 DTO/类型/guard；写操作核验实际 schema 的身份字段，整批事务进入 GUI 的一个撤销命令。它们不操作独立 stdio 的另一份地图状态。
- `editor_save_map` 经 GUI 原保存服务写回或另存；写回仍自动备份。能力、路径和身份以当次 schema 为准。只因小地图无法按当前内容生成而失败时返回 `MINIMAP_RISK_SAVE_AVAILABLE`；先告诉作者，作者同意后再传 `keep_original_minimap: true` 保留原小地图继续保存，不要自行默认打开。
- 主窗口设置控制查询/编辑/保存/外部操作及逐工具开关。列工具和执行均检查权限；旧客户端缓存的工具名不能绕过关闭。JSON-only HTTP 不提供 listChanged 通知流，修改设置后重新拉列表。
- 没有开放的文件/命令执行能力不得用额外 shell 或直接改归档绕开。自然语言 T 仍是默认逻辑形式。

## 图与保存边界

- `query_trigger_graph` 可分页，`arguments` 定点查询最多16个 ID，并可请求参数原位替换 guard；参数及限制以 schema 为准。
- `edit_trigger_graph` 在目录单写事务中完成核心支持的创建/字段/精确改名/移动/删除/类型子树及受 guard 的替换。CallerAsserted 的 opaque code 审计仅是调用方声明，不是服务端自动证明“代码无引用”。
- 引用一律用查询结果各行 `ref` 里的 `{kind, id}`。分类行另有 `category_id`（WTG 分类号），不能当引用编号；误用时报 `TRIGGER_GRAPH_RECORD_MISSING` 并给出正确的 `ref.id`。
- `move` 的 `from`/`to` 写 `{"kind":"category_triggers","owner":<分类行 ref>,"ordered_children":[该分类当前全部触发器 ref，按移动前顺序]}`；同分类内调整顺序时两者相同。`before` 是要排到其前面的触发器 ref，省略则放到末尾。例如把末尾的 A 移到 `.7` 之后：`before` 写原本紧跟 `.7` 的那个触发器。只调位置时可省略 `from`/`to`，写 `{"op":"move","target":A,"after":.7}`（或 `before`）：触发器/类别放到锚点旁，触发器可借此换类别。
- 函数槽参数（`eventcall` / `boolexpr`、`boolcall` / `code`）的 `call` 分别写 `kind` 为 `event` / `condition` / `action`，例如 `AddTriggerEvent` 第二个参数填 `TriggerRegisterUnitEvent`（event）；也可填返回该类型的普通 call。其余参数只能是 call。
- 整段写入优先用紧凑 T 写法：`insert_text`（插到触发器根或块分支）、`replace_trigger_text`（整体替换触发器内容），读取用 `trigger_text` 表加 `trigger_id` 导出同一写法；规则见随包 AGENT_GUIDE 的“紧凑 T 写法”。仍是普通 T 树，走同一类型检查、撤销与保存。
- 一个功能（变量 + 多个触发器）用 `apply_feature_text` 一次写进一个类别（`after` 指定新触发器插在哪个触发器之后），读取用 `feature_text` 表；触发器/区域/镜头/声音参数可写显示名；预设查 `presets` 表，现成写法查 `function_examples` 表；只改几行用 `patch_trigger_text`（导出行号 + 原文 guard）；编辑器内 `dry_run: true` 只预览 `trigger_diffs`，不发布。
- 写入前后用 `editor_check_trigger_graph` 做整图检查（`wait_ms` 直接等结果）；带同一批次时只在私有副本上检查。整图来源不完整报 `TRIGGER_CHECK_CONTEXT_INCOMPLETE`，不算通过。
- 大批重构分批查询、编辑和保存。保存从冻结输入构建脚本和归档；候选生成、独立重开及新图查询各自取得新身份，不在已失效 overlay 上继续使用旧 ID。
- 编码、include、工具身份、未知语言 API 和类型门禁仍由共用保存服务检查；来源变化或失败不回落到旁边另一套安装。
- 默认自然 T；`script_target` 是编译输出目标，不是把作者触发器转换成自定义代码的授权。
