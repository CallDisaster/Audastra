---
name: forge-editor-mcp
description: 通过 Audastra/War3MapEditor64 的 MCP 查询、编辑并保存 Warcraft III 地图，使用自然语言 T、明确修订身份和共用事务。适用于外部 Agent 接入、七类物编、触发器图操作和候选回读；以实际服务端工具 schema 为准。
---

# Audastra 地图工具接入

这是工具使用 Skill。连接和工具成功不等于地图游戏验证；Skill 也不替代服务端的权限、修订和保存检查。

## 发现服务

1. 先读当前项目 AGENTS.md、任务范围和来源规则；使用当前包内/宿主指定的 `war3-map-mcp`，不要混入另一包 EXE 或推测工具链安装。
2. 从 MCP initialize、tools/list 和实际工具 schema 发现可用能力。旧 README、资源占位说明、另一个版本的工具名都不能代替此次发现。
3. 区分宿主与传输：独立 `war3-map-mcp` 使用自己的展开工作区，支持客户端 command/args 的 stdio，也支持显式启动的本机 HTTP；内置助手和编辑器设置里的本机 HTTP 使用 GUI 当前地图，不能仅凭 HTTP 判定宿主。GUI 触发器与保存工具名为 `editor_*`，物编、变量、实体、地图设置与字符串工具在两种宿主共用名称和参数 schema。从实际宿主配置取得端口/令牌，不扫描任意端口或假定已监听。HTTP 为 JSON-only Streamable HTTP，没有 SSE/重连回放；开关变化后重新查询工具。

当前连接示例、工具族和身份边界见 [当前接入合同](references/current-contract.md)。只在连接/使用对应能力时读它。

## 查询、编辑与保存

- 先取得地图归档、工作区、UI 定义与图身份，分页查询所需对象和参数，保留 coverage/歧义诊断。对象 ID、修订、内容 SHA 和视口帧号是不同编号空间。
- 触发器改动默认用可编辑自然语言 T。发现实际存在的 `query_trigger_graph` / `edit_trigger_graph` 后，按其 schema 在已取得的单写事务中操作；guard 和 opaque code 审计等级不能由客户端伪造。
- 事务内预览和验证必须覆盖最新 edit generation；后续改动使旧预览失效。支持什么操作由 schema 和核心资格决定，不私下直改 WTG/WCT 或另造地图写入脚本。
- 独立工作区使用当前 `save_map_edit_candidate`；GUI 使用实际宣布的 `editor_save_map`，绑定 `project_identity` 与 `expected_revision` 并走原保存服务。候选不覆盖源图；正常作者 Save/Save As 按已授权产品行为执行，不强制改成候选模式。
- 保存后独立重开、重新查询新身份再继续；不要把旧 transaction/graph ID 套在新归档上。完整输出、脚本重建、编译、归档重开、原版和游戏各自记录。
- 失败保留诊断并释放事务；不得放宽类型检查、跳过编译或忽略来源漂移来换取成功。工具缺口作为共用服务缺口报告。

## 七类物编

`kind` 使用 `unit`、`item`、`ability`、`buff`、`destructable`、`doodad` 或 `upgrade`。对象 id 是四字节 rawcode，区分大小写，不是列表行号。来源缺失、元数据不明、未知类型或不支持的变体要保留诊断，不能猜值后写入。

| 工具 | 用途与边界 |
|---|---|
| `objects_list` | 读取标准、地图覆盖和自定义对象，返回名称、来源、父对象、稳定 id、当前身份及修订；`scope` 为 `standard/custom/all`，`page` 为 `{offset,limit}`，limit 上限 128 |
| `objects_get` | 读取真实字段键、中文显示名、类型、当前值、继承默认值和覆盖状态；可用 `fields` 筛选，字段结果同样分页 |
| `objects_field_options` | 查询字段的真实枚举或存在的引用对象；适用对象受限时提供 `id`，有序列表选项仅表示单个条目 |
| `objects_set_fields` | 将 `changes[{field,level?,variation?,value}]` 作为一批发布到共享事务/撤销栈；布尔用 true/false，列表用有序字符串数组，不能传逗号文字；原生重复字段等级从 1 开始，适用类别由元数据决定 |
| `objects_create` | 复制 `base_id` 的已有标准或自定义对象；`new_id` 可省略并由命令层分配，名称通过真实名称字段修改；不自动保存 |
| `objects_delete` | 返回确定/可能引用、位置及覆盖状态。当前共享删除审计尚不能证明脚本/动态引用完整性，因此安全拒绝删除；零静态命中和客户端传来的 Eligible 都不能放行 |

典型顺序：`objects_list(kind=unit,query=名称)` → `objects_get(kind=unit,id=…,fields=[…])` → `objects_field_options(kind=unit,id=…,field=…)` → `objects_set_fields` → 原入口的预览/保存 → 重新打开并读回。每次修改都从最新查询返回复制 `project_identity` 和 `revision`，把后者作为 `expected_revision`。这些值在 GUI 与独立工作区格式不同，只能在原宿主使用，不能互换。

编辑器入口的物编变更和原有界面编辑共享撤销/重做与修订；保存继续使用 `editor_save_map`。独立入口返回 `transaction_id`、`transaction_revision`，后续按实际 `preview_edit`、`validate_edit`、`save_map_edit_candidate` schema 使用同一事务，物编冻结快照和 WTG/WCT 在同一保存服务中发布。字段操作成功只表示已编辑，`saved=false` 不表示已写到地图。

## 变量

先从实际 `tools/list` 确认 `variables_list/get/create/update/rename/delete`。变量 `id` 是规范十进制字符串，来自当前图的 StableId，不是名称、行号或 JSON 数字；归档重解析后重新发现 ID。查询 `value` 保留类型、数组/初始化标志及当前文本，不能以显示文字代替精确原值。

创建与修改通过原 GUI 撤销栈或独立活动事务执行，不自动保存，也不自动 Begin。修改前复制最新 `project_identity` 与 opaque `revision`，作为 `expected_revision`；独立事务后续预览、验证、保存另用返回的 `transaction_id/transaction_revision`，两种修订不能互换。合法类型、数组界限与初始化字面量由实际 Catalog/Core 校验；不将表达式当字面量，不用声明未初始化来绕过后续激活时的检查。

改名需要按实际 opaque 代码块 SHA 提供明确审计；缺审计时返回 `TRIGGER_RENAME_CODE_AUDIT_REQUIRED`。删除在引用覆盖未知时返回 `TRIGGER_GRAPH_REMOVE_REFERENCE_UNKNOWN`，零命中不构成完整性证明。失败不发布部分编辑；过期修订重新查询后再决定，不能盲目重放。

## 实体字段 C1

`entities_query` 查询 `units/doodads/regions/cameras` 的当前 typed 文档，包含当前事务覆盖或作者未保存编辑。它与旧 `query_map_data_page` 的已提交来源查询范围不同。页为 `{start_index:"0",limit:64}`，上限128；保留 `member.state` 和来源诊断，损坏、冲突或不可读不能解释为空表。`source_index` 是此修订内的位置，重新打开、修改或翻页时仍须重新核对身份，不能当永久 ID。

`entities_set_fields` 回传最新 `project_identity/expected_revision/kind/expected_member_sha256` 和 `changes[{source_index,row_key,field,expected_raw,value}]`。所有索引和输入值均为字符串；`expected_raw` 必须直接取当前字段查询，浮点 raw 表示精确位值，不能用四舍五入的显示值代替。一次1–32项，同一类别，整体检查最终边界；非法末项不得发布合法前项。仅已有经典原生布局中目录标记可写的单位、区域、镜头字段开放；C4普通单位绝对XYZ范围见后文，doodads字段保持只读，没有增加或删除工具。

GUI 回到原 MapDataEditSession 与地图数据撤销栈，保留作者此前的其他编辑；普通 `editor_save_map` 仍走原完整地图保存。独立入口必须先显式 `begin_edit`，原来源累计最终字段差异最多32项，重复修改同一字段合并为相对原值的一项，恢复原值消除净差。先原 `preview_edit/validate_edit`，再 `save_map_edit_candidate`；字段候选沿用原 `fieldEdits` 保存分支，暂不混入触发器或物编变化，混合时明确拒绝。每次独立 Core 回执来自真实隔离会话，不声称其 session identity 跨工具调用不变；公开 CAS、原 transaction id/base revision和native member SHA分别核对。

## 地图设置与字符串 D

先从当前 `tools/list` 发现 `map_settings_get/set` 和 `strings_query/set`。两种宿主使用同一组工具；GUI 沿原地图数据会话与 Undo，独立宿主必须先显式 `begin_edit`。读操作返回当前事务覆盖，保留来源状态、成员 SHA、修订和诊断，不把缺失 WTS 补成空表。

- `map_settings_get` 的稳定 `setting_id` 是 `w3i`。文字字段的 `value` 是 WTS 解析后的展示值，`wire_value` 是 W3I 原文字，`raw` 是精确写入预条件。未知引用或编码返回 null/诊断；不能拿展示文字冒充原 wire 字节。玩家、队伍、科技、天气、加载画面等节仅只读，单节最多128项、整体 JSON 最多256KiB。
- `map_settings_set` 必填当前 `project_identity/expected_revision/expected_member_sha256`、`setting_id=w3i`、`write_mode=literal` 和1–32个 `changes[{field,expected_raw,value}]`。仅完整经典 v25 已有的地图名、作者、描述、建议玩家、`game_data_set` 可写。文字作为 UTF-8 字面量写入 W3I，每项最多4096字节，拒绝 `TRIGSTR_` 开头，不修改 WTS。未知版本、Partial 和未知自定义结构拒绝。
- `strings_query` 默认64、最多128行；`string_id` 是当前 WTS 中的规范十进制字符串。`value` 是正文，正文中的 `TRIGSTR` 不递归解析。保留精确 `raw`；下一页 `cursor` 绑定修订与成员 SHA，不与 `start_index` 同传。
- `strings_set` 必须明确 `write_mode=shared_string`，传当前身份、修订、成员 SHA 及1–32个 `changes[{string_id,expected_raw,value}]`。修改既有唯一 ID 的正文会影响所有引用该 ID 的字段；不改 W3I wire 引用，不创建、删除或重编号。缺失、重复 ID、歧义、非法编码和超预算整批拒绝。

上述写工具只编辑，不保存。GUI 使用原 `editor_save_map`；独立宿主先原 `preview_edit/validate_edit`，再 `save_map_edit_candidate`，沿已有字段冻结和原归档保存分支。累计净字段差异仍受32项预算，不把设置/字符串字段与触发器、物编或预放命令混合保存的拒绝绕过去。保存后重新打开并查询新身份。CTest、真实副本、内置助手、原版保存及游戏证据分别记录，工具成功不代表整组 D–H 已完成。

## 已有声音定义 E0

先从当前 `tools/list` 发现 `sounds_list/get/update`。两种宿主沿同一声音字段服务，读取当前事务或GUI未保存覆盖。`sound_id`来自原名字字节的规范hex稳定键，不接受行号或损失编码的显示名；重新打开后重新发现身份。list默认64、最多128行，cursor绑定文档、修订、源/Sounds成员SHA和query；每页最多检查4096条定义，大表总数保持未知且空页仍可能有续页。损坏或冲突来源不伪造空表，扩展布局保留只读诊断。

`sounds_update`传当前 `project_identity/expected_revision/expected_member_sha256`、顶层 `sound_id` 和1–32个 `changes[{field,expected_raw,value}]`。仅已有完整classic W3S v1的volume、fade_in、fade_out可写，value必须是真JSON int32整数；不接受字符串、bool、小数或null。volume为-1默认或0..127，fade为-1默认或非负int32原生速率，不能解释为毫秒。重复原名字、生成的gg_snd符号冲突、Partial/v2/v3、来源或raw漂移整批拒绝。声音增删、声音文件、导入和其它属性没有因此开放。

GUI使用同一Prepared字段检查和原receipt Undo，保留作者此前其它合法UI编辑；不把独立fieldEdits的保存范围强加到GUI整张地图。独立宿主先显式begin_edit，原来源六成员净差累计最多32项，沿原preview/validate/字段Save门禁；混合触发器、物编或预放命令的保存拒绝保持。工具编辑成功不表示已保存，真实副本、原版真保存和游戏各自核验。

## 导入元数据 E1

仅在实际 `tools/list` 宣布后使用 `imports_list/get`。list将IMP原记录、命名物理成员和GUI未冻结队列分别列出；不按路径合并丢弃重复IMP记录。原flags、raw_path_hex保留，非UTF-8的raw_path为null；歧义保留双候选身份，不替作者选择来源。payload始终opaque，不返回正文，不按扩展名解析，覆盖低层资源时仍报告unknown。

list默认64、最多128行，每页最多4096次谓词检查；大表matched_total为null，空页仍可能有next_cursor。下一页重复query并传完整cursor；它绑定文档/来源/IMP/资产覆盖，不能只传索引。get必须使用同次list的import_id和expected_inventory_sha256。可选project_identity/expected_revision须成对提供。成员bytes/SHA只描述原source_path，GUI移动/删除单列overlay_state；queued-unfrozen仅有reported_bytes，未读取队列磁盘文件或冻结内容。独立MCP查询原目录来源，GUI查询原来源加当前路径覆盖。元数据工具没有导入、改名、删除、read_text或保存入口。

本单元源码接入尚待统一构建和实际工具验收，不能以此文档视为已发布支持。

## 预放命令 C2 有限范围

从最新 `entities_query` 取得来源修订与成员SHA。`entities_rotate` 传 `source{source_index,row_key}` 和数值 `facing_radians`，只设一个单位或普通地物的绝对朝向，有限 `[0,2π)` 弧度、XYZ不变；C1 `facing` 的普通数值文本按度解析，`f32:` raw 则保留原生弧度位值。`entities_copy` 传1–32个同类 `sources` 和数值 `offset{x,y,z}`，完整复制源记录及嵌套数据，再分配新创建编号。仅支持classic/no SkinId；特殊地物拒绝，`sloc`不能复制，朝向修改不联动W3I。发布前64MiB affected native wire预检不是总内存限制。

GUI命令继续用原会话/Undo并保留作者已有编辑，可重新查询复制后的当前行；独立入口暂只允许一次原来源copy，或同一原行重复绝对朝向并冻结最后请求float。在已有预放净差时，第二copy、copy与rotation混合及其它行rotation明确拒绝；C1净字段变化及触发器/物编混合保存也拒绝。朝向或字段全部恢复原native字节后，可清除净差并开始新的受支持命令；保存允许原成员SHA精确匹配的净零map-data覆盖，其它额外变化仍拒绝。回执 `before.rows/after.rows` 为对应源行与新行；定位只在当前快照有效，实体导航没有真实能力时不显示。独立入口先显式原事务preview/validate，再原 `save_map_edit_candidate`，保存后重开新身份；GUI继续使用原 `editor_save_map`；不得调用新脚本绕过拒绝。真实单位复制与旋转各自通过原Save/CLI整native/新HTTP服务回读；地物真实保存、HWND及游戏仍未验收，证据见夜间报告。

## 单位查询 C3 有限筛选

原 `entities_query` 的 `kind=units` 可传 `filter:{type_id?,player?}`，包括空对象；其它kind不能传filter。type_id按实际四字节匹配、区分大小写，可传四个UTF-8字节或 `hex:xxxxxxxx`，非UTF-8与零字节用hex。player为int32整数，查询允许原生未知/负值，不套用写入0–15限制。筛选包含整个units.doo表，不擅自排除物品或sloc。

下一页重复相同过滤语义并传返回的完整 `page.cursor`，不能同时传start_index。cursor绑定完整身份、修订、成员SHA、canonical过滤条件与原索引；修改后用刷新外层CAS携带旧cursor仍拒绝，不是永久locator或加密令牌。每页最多128返回行、4096次原记录检查。总表≤4096行时每页完整扫描并报告精确全局matched_total；更大表返回null与matched_total_complete=false。零行且有next_cursor表示本批未命中，应继续分页；不得断言全表无匹配。保留来源状态、原source_index和原字段格式，不改旧query_map_data_page合同，不触发编辑或保存。

## 普通单位绝对坐标 C4

同一 `entities_set_fields` 可改已有Ready classic8/subversion11普通non-sloc行的字符串x/y/z，一组三轴是一个原Undo命令。数值为绝对native世界坐标，不自动采样地形或改其它轴。十进制先按double验证有限±1000000再转float；f32精确保留位型，由值门禁拒NaN/Inf/超界。sloc全部XYZ、Skin/Partial/非classic来源只读；出生点须走W3I关联路径，不能绕过字段拒绝。查询writable与实际发布、修改记录逆向使用同一行级门禁。

GUI保留作者此前命令，XYZ Undo回到作者基线；独立使用原fieldEdits、原32项累计限制和原Save，没有新placement recipe。当前真实原保存样例仅普通单位三轴，完整native12字节窗口oracle和新服务回读通过；与C2预放/触发器/物编混合净差的旧拒绝保持。整个C、Native窗口和游戏仍待验收。

地图设置、声音、导入资源、地形写入、视图控制与通用诊断工具，只有实际 `tools/list` 已宣布才可调用；本 Skill 的领域说明不将未完成能力视为已支持。完整能力及未实现名称见仓库 `docs/MCP_CAPABILITIES_2026-10-04.md`，计划工具名不能代替发现结果。逐项权限/原Undo/事务/保存证据另见 `docs/MCP_TOOL_ACCEPTANCE_2026-10-04.md`。

## 展示与资源边界

对作者展示中文触发器、分类、变量和动作参数的实际前后变化、改动文件、保存结果及剩余项；不只交巨型 JSON 或生成脚本。

AI 载荷和未知可透传成员保持原字节；不读取 `tiara.ai` 正文，不按扩展名检查导入内容。来源缺失/冲突不能静默降级到其他安装。模型截图只使用实际完成帧和明确设备/视口身份，后台启动不能代替可见验收。
