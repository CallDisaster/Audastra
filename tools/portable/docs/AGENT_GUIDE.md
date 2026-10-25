# Audastra 桌面与 Agent 接入指南

本包包括 GUI、CLI、MCP 和生产渲染器，并带程序自身所需的 Qt/VC 运行文件。它是当前 R08 分支 `02b866d` 起点及后续打包检查点的阶段候选，尚未审查、尚未合入 main；不称稳定 main 发布。最终提交号、文件 SHA 与实测范围以交付回执为准。

没有捆绑地图、Warcraft/Blizzard 归档、YDWE UI/编译器或它们的资源。程序可以独立启动；需要这些资源的地图编辑、脚本构建和渲染操作仍须配置合法本机安装。不能据此推断任意地图/模板都可保存，也不能把候选重开当成游戏验收。

**触发器编辑默认使用自然语言 T（传统事件/条件/动作树）。** 作者通常不读代码，新增优化必须保持可在 GUI 中阅读、修改的事件/条件/动作；未得到作者特别授权，禁止转换为 WCT 自定义 JASS/vJass/Lua。既有自定义代码原样保留，编译生成脚本只用于保存。若公开工具缺创建变量、插入子树或事务归档保存等能力，应报告缺口，不能把转码当作优化交付。历史代码实验不能作为默认推荐地图。

## 1. 入口与目录

| 入口 | 用途 |
| --- | --- |
| `Audastra.exe`（包根目录） | 原生桌面界面，等同于 `bin/war3-map-editor.exe` |
| `bin/war3-map-cli.exe <子命令> ...` | CLI 查询、生成和候选保存 |
| `bin/war3-map-mcp.exe --workspace ... --ui-data ...` | stdio MCP 服务 |

脚本入口转发参数；Agent 也可直接用绝对 exe 路径及参数数组启动子进程，避免 shell 改写参数。包含空格的路径须引用。保留整个包的相对目录结构：DLL 在 exe 旁，Qt 插件按随包 `qt.conf` 定位；当前程序从 exe 旁的 `fonts`、`translations` 读取字体和中文 Qt 翻译。不能只拷贝 exe。

GUI 可直接运行，游戏/YDWE 来源由界面配置。可用 `Audastra.exe --open-map <地图副本绝对路径>` 打开指定副本。程序使用现有用户设置机制；“便携”表示程序运行文件无需安装，不承诺把全部用户设置或外部资源收进包里。

## 2. 本机资源与保存前提

通用地图修改保存需要：

- 现存 MPQ 地图副本，及一个**不存在**的新候选路径。
- YDWE 根或可解析到其根的 UI 目录，含启用函数库 config、UI 定义、jass 包、`compiler/include`、`compiler/jass/24/common.j` 和 `blizzard.j`。
- 经典 Warcraft 数据根，能按实际来源层读取标准物编、声音等数据。
- 若地图用到内置编译链未支持的 vJass 特性，需可用的 YDWE jasshelper/pjass 回退；复杂模板/SLK 还需要现有 YDWE Lua/w3x2lni 数据链。不要通过带一个 `lua.exe` 或保留旧脚本绕过检查。
- 可写系统临时目录和候选输出目录。脚本编译使用独立临时工作目录。

包自身的位置与旧外部工具的位置是两个问题。旧 YDWE Lua 模板桥尝试 ASCII/8.3 短路径，无法表示时明确报错；不承诺所有 Unicode 外部工具路径可用。

## 3. 常用 CLI：查询到另存

下面命令签名中的 `<...>` 是待替换参数，`[...]` 是可选部分，不是可以原样执行的 shell 文本。命令根为包内的 `bin/war3-map-cli.exe`，下面简写为 `war3-map-cli`。

```text
war3-map-cli profiles
war3-map-cli map-field-catalog
war3-map-cli archive-session-inspect <map-archive>
war3-map-cli map-data-inspect <map-archive-or-directory>
war3-map-cli map-data-query <map-archive> <source-sha256> <settings|units|doodads|regions|cameras|sounds|strings> <start-index> <limit-1..128>
war3-map-cli map-data-roundtrip <map-archive>
war3-map-cli skin-layer-inspect <map-archive>
war3-map-cli skin-layer-query <map-archive> <source-sha256> <unit|ability|item|buff|upgrade|doodad|destructable> <start-index> <limit-1..128>
war3-map-cli archive-candidate-copy <map-archive> <new-candidate-path>
war3-map-cli map-edit-save --help
```

`skin-layer-inspect` / `skin-layer-query` 只读 `war3mapSkin.*` 皮肤层，不是普通物编；旧名字 `object-layer-*` 仍可用。普通七类物编用 MCP `objects_list` / `objects_get`。

`map-field-catalog` 给出字段标识、类型、目录写入能力和约束；`map-data-query` 的行字段还给出该来源布局实际是否可写。读取的 `source_index`、`row_key`、成员 SHA 和 `fields[].raw` 只对本次来源身份有效。不要从显示名称或角度文本重建 raw 前值。分页使用返回的 `next_cursor.next_index`；更换地图或 SHA 后从新快照开始。

### 通用字段保存

```text
war3-map-cli map-edit-save <source-map> <source-sha256> - <ydwe-root> <war3-root> <new-candidate.w3x> --field <kind> <row> <row-key> <member-sha256> <field-id> <expected-raw> <new-value> [--field ...] [--layer <id> ...] [--ui-root <catalog-root>]
```

最多32项字段，同类字段必须绑定同一成员 SHA；不能混入地形或摆放操作。字段目录开放情况以当前包输出为准，不在文档中永久固定可写字段名单。

原值格式：整数为十进制字符串，float/角度为 `f32:` 后8位小写十六进制，字节串为 `hex:` 后小写十六进制。直接回传查询提供的 `raw`。新值可以是符合目录约束的十进制数/UTF-8文本；角度输入为度，精确 raw 形式按原始存储解释。显示“默认”时 `value` 可能为空，仍使用其真实 `raw`，不能替换成0。

下面是**修改地图名**的 PowerShell 示例，路径均是占位示例。仅对自己的独立地图副本操作：

```powershell
$ForgeCli = 'C:/Tools/Audastra/bin/war3-map-cli.exe'
$MapCopy = 'C:/Maps/MyMap-copy.w3x'
$YdweRoot = 'C:/Games/YDWE'
$GameRoot = 'C:/Games/Warcraft III'
$Candidate = 'C:/Maps/MyMap-agent-candidate.w3x'
$SourceSha = (Get-FileHash -LiteralPath $MapCopy -Algorithm SHA256).Hash.ToLowerInvariant()
& $ForgeCli map-field-catalog
$Page = & $ForgeCli map-data-query $MapCopy $SourceSha settings 0 1 | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or !$Page.ok) { throw '地图查询失败，先处理诊断' }
$MapRow = $Page.rows | Select-Object -First 1
$MapNameField = $MapRow.fields | Where-Object id -eq 'map_name'
if (!$MapNameField -or !$MapNameField.writable) { throw '当前来源不能写地图名' }
if (Test-Path -LiteralPath $Candidate) { throw '保留已有候选，换一个新输出路径' }
& $ForgeCli map-edit-save $MapCopy $SourceSha - $YdweRoot $GameRoot $Candidate --field settings $MapRow.source_index $MapRow.row_key $Page.member.member_sha256 map_name $MapNameField.raw 'Agent candidate'
if ($LASTEXITCODE -ne 0) { throw '保存失败，检查 stderr；不要重用不明输出' }
& $ForgeCli archive-session-inspect $Candidate
if ($LASTEXITCODE -ne 0) { throw '候选重开失败' }
$CandidateSha = (Get-FileHash -LiteralPath $Candidate -Algorithm SHA256).Hash.ToLowerInvariant()
& $ForgeCli map-data-query $Candidate $CandidateSha settings 0 1
if ($LASTEXITCODE -ne 0) { throw '候选回读失败' }
if ((Get-FileHash -LiteralPath $MapCopy -Algorithm SHA256).Hash.ToLowerInvariant() -ne $SourceSha) { throw '来源 SHA 已变化' }
```

还应检查回读的目标字段确实是请求的新值。CLI 保存成功回执给出候选 SHA、被修改成员 SHA、脚本构建等信息；原版编辑器和游戏验证仍分别标为未验证。

### 其他现有候选操作

```text
war3-map-cli map-edit-save <source-map> <source-sha256> - <ydwe-root> <war3-root> <new-candidate.w3x> --rotate <units|doodads> <row> <row-key> <member-sha256> <degrees> [--layer <id> ...]
war3-map-cli map-edit-save <source-map> <source-sha256> - <ydwe-root> <war3-root> <new-candidate.w3x> --copy-source <units|doodads> <row> <row-key> <member-sha256> [--copy-source ...] --copy-offset <dx> <dy> <dz> [--layer <id> ...]
war3-map-cli terrain-snapshot <map-archive>
war3-map-cli terrain-block <map-archive> <source-sha256> <terrain-sha256> <x> <y> <width> <height>
war3-map-cli map-edit-save <source-map> <source-sha256> <terrain-sha256> <ydwe-root> <war3-root> <new-candidate.w3x> --stroke <raise|lower|smooth> <circle|square> <vertex-x> <vertex-y> <radius-0..16> <strength> [--stroke ...] [--layer <id> ...]
war3-map-cli terrain-blight-save <source-map> <source-sha256> <terrain-sha256> <ydwe-root> <war3-root> <vertex-x> <vertex-y> <radius-0..16> <set|clear> <new-candidate.w3x> [--layer <id> ...]
war3-map-cli script-build <ydwe-root> <war3-root> <map> <out.j> [--compile <work-dir> [--external|--cross-check]]
war3-map-cli archive-member-add <source-map> <source-sha256> <new-candidate.w3x> <member> <file> [<member> <file> ...]
war3-map-cli archive-member-remove <source-map> <source-sha256> <new-candidate.w3x> <member> [<member> ...] [--allow-referenced]
war3-map-cli archive-expand <source-map> <source-sha256> <new-directory> [--data-only]
```

往地图里补文件只能用 `archive-member-add`（CLI）、独立 MCP 的 `archive_members_add` 或编辑器端口的 `imports_add`。保存只写本次改动涉及的成员，不扫描工作目录，把文件拷进展开目录不会进入候选。导入清单里标志不明的旧记录，在补回它指向的唯一成员后按原记录保留；两个可能路径都有成员时仍拒绝。

删除导入文件用 `archive-member-remove`（CLI）、独立 MCP 的 `archive_members_remove` 或编辑器端口的 `imports_remove`，成员和它在 `war3map.imp` 里的记录一起删除，地图数据文件不能删。三处删除前都会在脚本、触发器、七类物编、w3i/wts、导入模型和 UI 文本里按字符串找这个路径（`.mdl`/`.mdx` 互通），找到就拒绝（CLI 和独立 MCP 为 `MEMBER_REFERENCED`，编辑器端口为 `IMPORTS_REMOVE_REFERENCED`，都列出位置）；确认要删时加 `--allow-referenced` 或 `allow_referenced=true`，回执的 `mentioned_in` 仍会列出这些位置。找不到不等于没有引用：脚本里拼接出来的路径查不出，所以回执始终是 `references_checked=false`。编辑器端口按当前内容查找，包括还没保存的触发器、物编和待导入文件；读不到的来源列在 `unread_sources`。

旋转角度范围为 `[0,360)`；复制最多4096个来源实例；笔划最多16笔，CLI 点位是地形顶点索引。raise/lower strength 为 `>0..1024`，smooth 为 `>0..1`；通行性和布局门禁仍会进一步拒绝不安全来源。地物跟随、其他未开放笔刷不因此放开。`--layer` 必须是已发现的真实函数库层，不能拿虚构层绕过目录检查。

### 只读查看触发器原始数据

```text
war3-map-cli wtg-source-inspect <wtg-file> <source-sha256> <ui-data-root> [--max-nodes N]
war3-map-cli wtg-source-query <wtg-file> <source-sha256> <ui-data-root> <catalog-sha256> <items|variables|deleted|ecas|arguments> <start> <limit-1..128> [--max-nodes N] [--trigger <items-row>]
```

先用 `items` 表找到触发器所在的行号（`item_type` 不是 1/4/64 的行），再加 `--trigger <行号>` 查 `ecas` 或 `arguments`，只返回这个触发器自己的节点，按树的顺序分页；行号仍是全表行号，可以和 `argument_indices`、`child_indices` 对照。不加 `--trigger` 时按全表分页。节点上限默认 100 万，大型旧版地图不用另外调；`--max-nodes` 只用来收紧。

`archive-candidate-copy` 是已有归档复制入口，不是脚本修改或游戏验收。

`archive-expand` 把地图包的具名成员写到一个新目录，给 MCP `--workspace` 用（见第 4 节“拿到 .w3x 时”）。目录必须尚不存在；源图只读。`--data-only` 只写 `war3map*`、`Scripts\war3map.j` 这类地图数据文件，不写导入资源，大地图几秒即可完成；写回时没展开的成员从原图保留，所以通常用它就够。路径不安全、大小写冲突或没有名字的成员不会写出，输出里逐条列出。

## 4. MCP 配置与协议

修改 [mcp.example.json](mcp.example.json) 后交给自己的 MCP 客户端配置流程。示例没有自动注册客户端。直接 exe 启动示例：

```powershell
$env:FORGE_MCP_YDWE_ROOT = 'C:/Games/YDWE'
$env:FORGE_MCP_WAR3_ROOT = 'C:/Games/Warcraft III'
& 'C:/Tools/Audastra/bin/war3-map-mcp.exe' --workspace 'C:/Maps/MyMap-Expanded' --ui-data 'C:/Games/YDWE/ui'
```

环境变量只作用当前终端及其子进程。`--workspace`/`-w` 必须指向现存展开地图目录（不能直接给 .w3x，先用 `archive-expand` 或 `archive_expand` 展开），`--ui-data`/`-u` 指向有效 UI 数据。没有 `--workspace` 时服务取当前目录，不能依靠客户端偶然的 CWD。搬家后更新 `command` 的绝对路径及必要的参数路径。

stdio 是 UTF-8 **newline JSON-RPC**：每行一条完整 JSON，响应以换行结尾，不使用 `Content-Length`。stdin 关闭会结束服务。不要在协议 stdout 写欢迎词、shell echo 或调试日志。初始化/发现消息示例，每个代码行是一条消息：

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"forge-agent-client","version":"0.1.0"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_revision","arguments":{}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_ui_catalog","arguments":{"query":"CreateUnit"}}}
```

工具结果位于 MCP `result.content` 的 text 块，文本本身是一个 JSON 对象；继续解析它的 `ok`、`diagnostics`，并检查外层 `error`/`isError`。收到 JSON-RPC 响应或进程退出0均不足以证明业务成功。

初始化失败仍可能握手、列工具。只有 `get_revision` 返回成功且 `identity_current=true`，才说明现存 workspace/UI 来源通过本次复核。展开目录需要完整 WTG/WCT 双文件（`map/war3map.wtg`+`map/war3map.wct`，或根目录同名双文件）和可解析的 Catalog。

### 编辑器端口：binding_token

连接正在运行的编辑器时，`editor_state`、触发器查询以及每次修改成功的结果里都有 `binding_token`。凡是要求 `project_identity`、`expected_revision`（和触发器修改的 `expected`）的工具，都可以只传 `binding_token`，三者由编辑器代填。

- 修改成功后结果里是新的 `binding_token`，连续修改直接用它，不必再查一次。
- 地图被你或作者改过、重新打开后，旧令牌返回 `LIVE_EDITOR_REVISION_CONFLICT`，重新读 `editor_state`。
- 令牌和那三项不能同时给（`LIVE_EDITOR_IDENTITY_AMBIGUOUS`）；两样都不给返回 `LIVE_EDITOR_IDENTITY_REQUIRED`。

### 地图数据查询身份

入口为 `get_revision` → `describe_map_data` → `query_map_data_page`，参数以 `tools/list` 返回 schema 为准：

- `expected_revision` 原样带回 `generation`、`source_sha256`、`ui_catalog_sha256`；这些 ID 和 SHA 使用字符串。
- `describe_map_data` 返回地图数据 `binding`、成员 `snapshot_token` 和 `field_catalog`。
- 页查询传入 `expected_map_data_binding`、`kind`、`expected_member_snapshot`、字符串 `start_index` 和数值 `limit`（1..128）。可设 `max_logical_bytes`（1..262144）；单次输出还受 JSON 字节预算限制。
- 使用页返回的 row identity 和 raw，未知/缺失/部分/歧义不是空表成功。目录或 Catalog 漂移时重新取身份，不能自己修改 revision 或续用旧 cursor。

这些目录数据查询仅包括已提交来源，不包括未保存 GUI 状态；活动触发器事务时拒绝。

**直接查询地图归档**（独立 MCP，无需展开目录）：`describe_map_data` 和 `query_map_data_page` 以 `source_map_path` 代替 `expected_revision`，二者只能给一个。

- `describe_map_data` 可不带 SHA，返回的 `binding.source_sha256` 即该归档的 SHA。
- 页查询必须带 `expected_source_sha256`。
- 绑定为 `archive-readonly-<SHA>`，`workspace_kind` 为 `map-archive-readonly`，与 CLI `map-data-query` 相同。
- 返回的 `source_index`、`row_key`、成员 `member_sha256` 和字段 `raw` 直接用于下文 `save_map_edit_candidate` 的 `field_edits`；保存后对候选再查一次以回读确认。
- 七类物编的 `objects_list`、`objects_get`、`objects_text`、`objects_field_options` 同样接受 `source_map_path` + `expected_source_sha256`（两者都要给），按游戏数据与地图物编合并后的结果只读返回，`project_identity` 为 `archive-readonly-<SHA>`。需要宿主配置 `FORGE_MCP_WAR3_ROOT`。物编的新增、修改、删除要先展开工作区（见下一小节）或用编辑器端口。

### 拿到 .w3x 时：展开、修改、写回

1. 不带 `--workspace` 启动独立 MCP，调用 `archive_expand`（或 CLI `archive-expand`），参数是源图、源图 SHA 和一个尚不存在的新目录，一般加 `data_only=true`。
2. 用 `--workspace <该目录>` 重新启动 MCP，`get_revision` 确认 `identity_current=true`。
3. 先 `begin_edit`，**之后**再用 `objects_list` / `objects_get` 查询并拿 `project_identity`、`revision`，然后用 `objects_set_fields` / `objects_create` / 触发器写工具修改，再 `preview_edit`、`validate_edit`。`begin_edit` 会改变修订，在它之前查到的 `revision` 拿去修改会返回 `OBJECT_REVISION_CONFLICT`。
4. `save_map_edit_candidate` 带 `source_map_path`（原 .w3x）、`expected_source_sha256`、新的 `output_path`、`transaction_id` 和当前 `expected_revision`，写出新地图包。工作区的物编基线必须与原图一致，否则拒绝。
5. 不带工作区对新候选再查一次（例如 `objects_get` + `source_map_path`）确认改动。

放进展开目录的新文件不会自动进入地图包；要加导入文件，对写出的候选再调用 `archive_members_add`。目前保存仍会重建整份脚本。

### 物编紧凑文本

看一个对象、改几项字段时，用文本比逐项 JSON 省事：

- `objects_text`（kind、id）返回一段文本，一行一个字段：`显示名 (字段[:等级[:变体]]) = 值  // 引用对象名`。默认只列与父对象不同的字段，`all=true` 列全部，`fields` 可筛选。
- 值的写法与紧凑 T 相同：数字、`"文本"`、`'A000'`、`true`/`false`、列表 `['A000', 'A001']`（顺序和重复都保留）。小数总带小数点。
- `objects_apply_text`（kind、id、text，加 `project_identity`、`expected_revision`）只写要改的行。括号里的字段键决定改哪一项；显示名、`//` 注释、空行和 `#` 行都会忽略，也可以只写 `uhpm = 300`。
- 一次 1..128 行，整批校验、整批生效，进入撤销栈，不保存。写法错误返回 `OBJECT_TEXT_INVALID`，字段值不合法时诊断也带行号；任何一行失败，整批都不改。
- 行尾写“只读”或“不透明的原始字节”的字段不能用文本写回。

### 字段候选保存

`save_map_edit_candidate` 使用**独立 MPQ 来源**，参数结构如下；占位内容必须从本次源图查询替换，不是可原样发送的请求：

```json
{
  "source_map_path": "C:/Maps/MyMap-copy.w3x",
  "output_path": "C:/Maps/MyMap-agent-candidate.w3x",
  "expected_source_sha256": "<该归档的64位小写SHA256>",
  "field_edits": [
    {
      "kind": "settings",
      "row": "<source_index>",
      "row_key": "<同一来源查询返回的row_key>",
      "expected_member_sha256": "<同一成员的64位小写SHA256>",
      "field": "map_name",
      "expected": "<该字段的raw原值>",
      "value": "Agent candidate"
    }
  ]
}
```

最多32字段；MCP当前字段 kind schema 为 settings/units/regions/cameras/sounds，字段是否可写继续由 Catalog 和来源布局决定。`value` 是字符串或有限数字；`expected` 必须是 raw 字符串。归档查询 SHA 与展开目录 aggregate SHA 不是同一种身份，不能互换；用于另存的来源/成员/row key 应来自该归档副本，例如上面的 CLI 查询。

可信宿主必须提供 `FORGE_MCP_YDWE_ROOT`、`FORGE_MCP_WAR3_ROOT` 两个绝对根；工具请求不能覆盖工具路径。可选 `ui_root` 指定实际存在的绝对 UI 目录，与 CLI 的 `--ui-root` 共用同一保存请求；省略时沿用工具根的既有 UI 来源。UI 目录和编译工具根应对应实际配套安装，不会按目录名猜测或回退另一安装。字段与地形请求互斥。MCP地形 schema 的具体预算/坐标系见工具描述，不能直接套 CLI strength 上限。现有 MCP schema 不提供 CLI 的摆放复制/旋转。

保存会经过共用脚本构建、成员校验与候选重开，失败返回诊断。它不带入活动 WTG overlay；活动 WTG 事务尚未结束时明确拒绝。成功仍需检查新候选 SHA、回读目标值并确认源 SHA 未变。

### 触发器事务

入口为 `list_triggers`、`get_trigger`、`get_ui_catalog`。修改顺序是 `get_revision` → `begin_edit` → 已注册的修改工具 → `preview_edit` → `validate_edit` → `export_map_candidate`，每步使用正确的 `transaction_id` 与完整 `expected_revision`。每次修改都会使先前 preview/validate 失效。

当前注册的写工具包括 `create_trigger`、`append_root_eca`、`append_argument_call`、`set_scalar_argument`；参数/类型必须由真实 Catalog 和现存所有者证明。

`set_scalar_argument` 的 `value_kind`：0 = 预设（TriggerParams 名，例如 `PlayerUnitEventSpellEffect`）；1 = 变量，写 `udg_` 加变量名，变量须非数组且类型与参数一致；3 = 直接值，可写整数、实数、`true`/`false`、文本、自定义代码（如注释文本的 `scriptcode`）和物编对象编号（技能、单位等写四字符编号，如 `A04W`，不加引号）。单位、点这类句柄没有直接值写法，请用变量或函数。例：“施放的技能 == 某技能”用条件 `OperatorCompareAbilityId`，`append_root_eca` 的回执给出三个参数编号，第三个参数用 `value_kind=3` 写 `A04W`。错误时用 `rollback_edit` 释放自己的事务。不要同时开两个写事务，不要把源变化当成可以跳过校验的理由。

函数槽参数（类型为 `eventcall`、`boolexpr`/`boolcall`、`code`）里放的是事件、条件、动作本身，与 WE 存法相同。例如 YDWE 的“为触发器添加事件” `AddTriggerEvent`：第二个参数是 `eventcall`，在 `edit_trigger_graph` 的 `insert_subtree` 里写 `{"call": {"function": "TriggerRegisterUnitEvent", "kind": "event", ...}}`，事件自己的参数照常填（单位可用 `YDWEGetAnyTypeLocalVariable` 等函数）。`boolexpr` 写 `"kind": "condition"`，`code` 写 `"kind": "action"`。`append_argument_call` 不带种类，会先按槽位找同名事件/条件/动作。把事件写成 `"kind": "call"` 会报 `TRIGGER_CALL_SLOT_MISMATCH` 并提示正确种类。

`export_map_candidate` 导出新的**展开目录候选**，不是 `.w3x/.w3m` 封包；后者会明确拒绝。它与 `save_map_edit_candidate` 是不同的现有出口，不能把目录事务导出描述成地图归档真保存。旧 `set_object_field/create_object/delete_object` 不再注册。

### 紧凑 T 写法（推荐的整段写入方式）

写一整段触发器逻辑时，不必逐个节点拼 JSON。`edit_trigger_graph` / `editor_edit_trigger_graph` 有两种文本操作：

- `{"op": "insert_text", "parent": <同 insert_subtree 的父列表>, "before": <可选>, "text": "..."}`：把一段文本里的全部语句按顺序插到触发器根或某个块语句分支。插到块分支时，`parent` 写 `eca_children` 加 `child_branch`，文本里不再写分支标签。
- `{"op": "replace_trigger_text", "trigger": <触发器 ref>, "text": "..."}`：用文本整体替换触发器的全部事件/条件/动作。新鲜度由批次的 `expected` 绑定保证。

读取用 `query_trigger_graph`（或 `editor_query_trigger_graph`）的 `{"table": "trigger_text", "trigger_id": "<ref.id>"}`：把整个触发器导出成同一写法。导出文本可以直接改，再用 `replace_trigger_text` 写回；导出、写回、再导出结果不变。

写法就是函数库里的函数名加参数，每个函数对应 WE 里的一个节点，写进去仍是普通 T 树，不是 JASS：

```text
events:
    TriggerRegisterUnitEvent(udg_Hero, UnitEventSpellEffect)
conditions:
    OperatorCompareAbilityId(GetSpellAbilityId(), OperatorEqualENE, 'A04W')
actions:
    SetVariable(udg_Count, 0)
    IfThenElseMultiple() {
        if:   OperatorCompareInteger(udg_Count, OperatorGreater, 3)
        then: KillUnit(GetTriggerUnit())
        else: CommentString("还没满")
    }
    ~DisplayTextToForce(GetPlayersAll(), "这一行被禁用")
```

- 触发器根用 `events:` / `conditions:` / `actions:`（或 `事件:` / `条件:` / `动作:`）分段，不写时默认是动作。
- 节点种类不用写：语句按所在段落或分支决定，参数位置按槽位类型决定。`AddTriggerEvent(udg_T, TriggerRegisterUnitEvent(...))` 这类函数槽会自动当事件/条件/动作处理。
- 参数：函数写 `Name(...)`，没有参数也要写 `()`；文本写 `"..."`（`\"`、`\\`、`\n`、`\r`、`\t` 转义）；物编编号写 `'A04W'`；数字和 `true`/`false` 直接写；预设直接写名字（如 `OperatorGreater`）；变量写 `udg_名` 或变量原名，数组写 `udg_名[下标]`；预放对象、触发器写 `gg_` 开头的名字；自定义代码类参数（如 YDWE 局部变量名）可直接写标识符；含空格等特殊字符的名字用反引号 `` `名字` ``。
- 块语句（如果/循环/选取单位/计时器等）用 `{ 分支标签: 语句... }`。分支标签写英文 key（`if`/`then`/`else`、`actions`、`params`、`events`、`conditions`），也可写中文短名（`如果`/`则`/`否则`）或 `#分支号`。只有一个分支的块（循环、选取单位等）可以不写标签。
- 语句前加 `~` 表示禁用；`//` 到行尾是注释；语句之间用换行或 `;`。
- 禁用语句不生成脚本，里面的参数不做类型检查（与 WE 一致）；启用语句照常严格检查。
- 少数原图里非块语句也带子节点，导出为 `#分支号:`；写回时子语句种类按函数库里唯一同名的事件/条件/动作确定。
- 地图对象：预放单位/物品 `gg_unit_`/`gg_item_`、脚本创建的可破坏物 `gg_dest_`、区域 `gg_rct_`、镜头 `gg_cam_`、声音 `gg_snd_`（音乐按字符串）都可直接引用；名字与类型以 `query_trigger_graph` 的 `map_objects` 表为准。
- 触发器、区域、镜头、声音参数也可以直接写显示名，如 `` AddTriggerEvent(`9-1 技能命中`, ...) ``、`` GetRectCenter(`Arena`) ``，按保存时的命名规则换成 `gg_trg_`/`gg_rct_`/`gg_cam_`/`gg_snd_` 全局。触发器名须唯一，同名多个时报错让你写 `gg_trg_` 生成名。导出（`trigger_text`、`feature_text`、预览差异）时这几类参数也写成显示名；只有一个生成名对应多个对象时保留生成名。中文名按字节换成下划线，字数相同的两个名字可能生成同一个脚本名，保存和整图检查会报 `TRIGGER_SCRIPT_NAME_DUPLICATE` / `OBJECT_SCRIPT_NAME_DUPLICATE` 并指出是哪两个，改名其中一个即可。
- 预设名写错时，报错会列出这个参数类型的前几个预设；全部预设用 `{"table": "presets", "type": "<类型>"}` 查（可分页 `start`/`limit`）。
- 先看地图有哪些类别和触发器，用 `{"table": "outline"}`：一次列出全部类别和其中触发器的名字与编号；加 `"match": "沃金"` 只列名字含该词的触发器（类别名命中时列出整个类别），加 `category_id` 只看一个类别。
- 查函数签名用编辑器端口的 `editor_get_ui_catalog`：不给 `kind` 时搜索事件、条件、动作和函数全部种类，签名前缀（`event:`、`condition:`、`action:`、`call:`）标明种类。
- 不确定某个函数怎么写，用 `{"table": "function_examples", "functions": ["函数名", ...]}` 查地图里现成的写法：每个函数最多返回 `limit` 条（默认 4，上限 16），带触发器名和导出文本行号，可直接照抄后改参数。
- `?` 是 WE 里“参数未设置”（编辑器里的红色空参数），只能留在禁用的语句里；启用的语句写 `?` 会报 `TRIGGER_GRAPH_ARGUMENT_UNSET`。
- 代码类参数（如 `CustomScriptCode`、YDWE 局部变量名所在的参数）只接受标识符（如 `loc_u`）或函数库预设；`replace_trigger_text` 可以原样保留该触发器里已有的代码和已有的不规范固定值，但不能改写、新写，也不能挪到别的触发器（报 `TRIGGER_GRAPH_CODE_UNSUPPORTED` 等）。需要新代码时先问作者。

出错时整段都不写入。名字、参数个数、分支标签这类错误一次全部列出，每条带“第几行第几列”；参数类型错误也会一次列出，并标明是哪一行的第几个参数。语法错误只报第一处。

### 改一小段：局部替换

大触发器只改几行时不必整段重写。用 `trigger_text` 导出后：

```json
{"op": "patch_trigger_text", "trigger": <触发器 ref>, "from_line": 12, "to_line": 14,
 "expected": "<导出文本第 12–14 行原文>", "text": "<替换成的几行>"}
```

- 行号就是导出文本的行号（从 1 起）。`to_line = from_line - 1` 表示不删任何行，只把 `text` 插在第 `from_line` 行前面。
- `expected` 必须与当前这几行逐字相同，否则报 `TRIGGER_TEXT_PATCH_STALE` 并给出当前内容，什么都不改。
- 替换后整段重新按紧凑 T 解析写入（同 `replace_trigger_text`），所以报错的行号是替换后整段文本的行号；块语句的 `{ }` 要配对完整。

### 一个功能一次写完：功能写法

一个功能通常要几个全局变量和几个触发器。`apply_feature_text` 一次写进一个类别：

```json
{"op": "apply_feature_text", "category": "沃金", "text": "..."}
```

```text
变量:
    VJ_Voodoo: integer = 0
    VJ_Marks: unit[]
    VJ_Model: string = "war3mapImported\\bonespirit.mdx"
触发器 沃金 Q 医疗波:
    events:
        TriggerRegisterAnyUnitEventBJ(EVENT_PLAYER_UNIT_SPELL_EFFECT)
    actions:
        SetVariable(udg_VJ_Voodoo, 0)
触发器 沃金 初始化 (init):
    actions:
        ...
```

- 顶层（不缩进）只有 `变量:`（或 `variables:`）和 `触发器 名字:`（或 `trigger 名字:`），下面的内容要缩进；触发器体就是紧凑 T 写法。
- 变量写 `名字: 类型`、`名字: 类型[]`（数组）或 `名字: 类型[长度]`，可加 `= 初值`（`"文本"`、`'A000'`、数字、预设名或 `null`）；名字可带 `udg_`。
- 触发器名后可加选项：`init`（地图初始化时运行）、`off`（初始关闭）、`disabled`（禁用）。名字含 `:`、`(`、`)` 时用双引号括起来。
- 类别不存在就新建。变量不存在就新建；已存在时类型和是否数组必须一致（否则报 `TRIGGER_FEATURE_VARIABLE_CONFLICT`），只更新初值和数组长度。触发器在该类别里同名的整体替换内容，不存在就新建；同名触发器在别的类别里会报错。文本里没提到的变量和触发器不动，也不会删除任何东西。
- 新建的触发器默认放在类别末尾；加 `"after": <触发器 ref>`（须在同一类别）时依次插在它后面。已有同名触发器原地更新，不移动。
- 报错一次列出，行号都是整段功能文本的行号；任何一处出错整批都不写入。
- 读取用 `{"table": "feature_text", "category_id": "<类别 ref.id>"}`：导出该类别的全部触发器和它们用到的变量，可直接改了再写回，导出→写回→再导出不变。

### 调整触发器或类别的位置

`{"op": "move", "target": <ref>, "before": <ref>}` 或 `"after": <ref>`（二选一）：把触发器或类别放到锚点紧前/紧后。触发器的父类别取锚点所在类别，可借此换类别。不需要列出整个兄弟顺序；需要完整顺序校验时仍可用带 `from`/`to` 的写法。动作/条件的位置用 `patch_trigger_text` 调整。

### 先预览再写入

`editor_edit_trigger_graph` 带 `"dry_run": true` 时只在私有副本上应用，不发布、不进撤销栈。返回：

- `change_summary`：新增/修改/删除了多少记录；
- `trigger_diffs`：每个有变化的触发器的紧凑 T 逐行差异（`-` 删除、`+` 新增、`@@ 第 N 行` 是写入后文本的行号，只列改动附近两行）。

确认无误后去掉 `dry_run` 用同一批次正式写入。

### 写完先做整图检查

上面的检查只保证 T 树本身合法。能不能编译要看整图脚本，用编辑器里的 `editor_check_trigger_graph`（也可在命令行用 `war3-map-cli trigger-check <ydwe根> <地图> <游戏根> [<触发器名> <紧凑T文件>]`）：

- 与保存相同的流程：生成整图脚本 → 注入函数库 → 预处理 → 模板（同保存一样在沙盒里执行）→ JASS 检查。不写地图、不执行地图代码。
- 可以带与 `editor_edit_trigger_graph` 相同的 `expected` 和 `operations`：批次只应用到私有副本上检查，不发布、不进撤销栈。通过后再用同一批次正式写入；不带批次时检查当前文档。
- `submit` 返回 `request_id`；带 `wait_ms`（最多 60000）会直接等结果。没等完就用 `{"action": "status", "request_id": ..., "wait_ms": ...}` 再取。
- 每条诊断落在 T 语句生成的代码上时带 `t`：`trigger_name`、`function`、`line`/`column`（就是 `trigger_text` 导出文本的行列）和出错的生成代码 `generated`。这类错误排在最前面。没有 `t` 的诊断来自函数库、作者代码或触发器自身的生成代码。
- 整图来源不完整时不检查，报 `TRIGGER_CHECK_CONTEXT_INCOMPLETE` 并说明原因，不能当作通过。

### 进游戏测试

编辑器端口有三个工具，用当前编辑内容（含未保存修改）启动一局单人测试，原图不变：

- `map_test_start`：启动测试，游戏窗口照常显示。回执的 `steps_supported` 说明本局能否执行测试步骤。
- `map_test_eval`：在正在运行的测试游戏里执行一段紧凑 T 动作，并读取全局变量：

```json
{"actions": "SetVariable(udg_VJ_Voodoo, 3)\nUnitAddAbility(udg_Hero, 'A04W')", "reads": ["VJ_Voodoo", "Marks[1]"], "wait_ms": 20000}
```

  - `actions` 写法同 `replace_trigger_text`，在一个临时触发器里编译，只执行动作；写错时报错带 `actions` 自身的行列，什么都不发送。
  - `reads` 是执行后要读的全局变量（可带 `udg_`，数组写下标），结果在 `reads` 里按顺序给出文本值；句柄类变量只能看到 Lua 的对象描述。
  - 游戏还在载入时会在 `wait_ms`（默认 10000，最多 60000）内等待就绪。超时且步骤尚未执行时会撤回；已经开始执行的不能撤回，回执会说明结果未知。
  - 回执的 `result` 为 `success` / `syntax_error` / `runtime_error` 等，`output` 是本次期间游戏里的其他打印。运行错误时前面的动作可能已经生效。
  - 目前只支持脚本语言为 Lua、开启了热重载的地图（自有 Lua 运行时）；JASS 地图返回 `MAP_TEST_STEPS_UNSUPPORTED`，改用下面的测试场景。
- `map_test_stop`：结束本局测试游戏（只结束这次启动的进程）。

JASS 地图用**测试场景**：启动时带一段紧凑 T 动作，只编进这次的测试副本，游戏开始后运行一次。

```json
{"scenario": "CreateNUnitsAtLoc(1, 'Oshd', Player00, GetRectCenter(GetPlayableMapRect()), 270.00)\nSetVariable(udg_Hero, GetLastCreatedUnit())\nTriggerSleepAction(2.00)\nPreload(\"通过: 英雄已创建\")",
 "reads": ["Hero"], "start_seconds": 3}
```

- 在场景里摆单位、下命令、等待，用 `Preload("文本")` 记录一行；以“通过”“失败”开头的行计为断言。
- 动作写完后，场景记下 `reads` 列出的全局变量，再写出结果文件。只能读整数、实数、文字、布尔变量；单位等句柄请在场景里用条件判断后记录“通过”或“失败”。
- 场景只用自然 T，不能写自定义代码。
- `map_test_result`：读回结果，返回记录行 `lines`、`passed` / `failed`、`reads`、`started`（游戏已载入、场景已开始）、`finished`（动作是否全部执行完）。游戏在记录期间写入的资源路径不放进 `lines`，只在 `loaded_files` 给个数。场景开始前编辑器每 2 秒给测试游戏发一次空格，越过载入后的“按任意键继续”。没写完时在 `wait_ms`（默认 30000，最多 600000）内等待；游戏退出了也会返回已有的部分。
- `map_test_capture`：截取测试游戏窗口当前画面，存成 PNG 并返回路径，用于检查特效和界面。窗口被其他窗口挡住也能截，但不能最小化；显示器关闭或锁屏时游戏不绘制画面，这时返回失败和原因，不会交回空白图。
- 地图自己在场景期间调用的预载也会出现在 `lines` 里，判断以“通过”“失败”开头的行为准。

测试步骤和场景只改游戏里的状态，不改地图；要把逻辑写进地图仍用 `edit_trigger_graph`。

## 5. 结果应如何记录

保留源/候选路径与 SHA、包内源码身份、请求的绑定/raw、新值、退出码/业务 ok、原始诊断和重开结果。候选存在时换新路径，不覆盖源图或已有证据。

分别记录程序启动、查询、修改、候选写出与重开、原版编辑器打开/另存、真实游戏运行。此次包的说明不新增后两项证据。GUI包含完整当前项目的入口与界面，不代表路线图里的模型、地形、战役/AI、新格式或其他模块都已完成；预览近似也不等同于逐帧原版一致。
