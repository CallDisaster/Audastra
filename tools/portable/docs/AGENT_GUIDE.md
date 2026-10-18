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

删除导入文件用 `archive-member-remove`（CLI）、独立 MCP 的 `archive_members_remove` 或编辑器端口的 `imports_remove`，成员和它在 `war3map.imp` 里的记录一起删除，地图数据文件不能删。CLI 和独立 MCP 删除前会在脚本、七类物编、w3i/wts、导入模型和 UI 文本里按字符串找这个路径（`.mdl`/`.mdx` 互通），找到就拒绝（`MEMBER_REFERENCED`，列出位置）；确认要删时加 `--allow-referenced` 或 `allow_referenced=true`，回执的 `mentioned_in` 仍会列出这些位置。找不到不等于没有引用：脚本里拼接出来的路径查不出，所以回执始终是 `references_checked=false`。编辑器端口的 `imports_remove` 目前不做这项查找。

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
- 七类物编的 `objects_list`、`objects_get`、`objects_field_options` 同样接受 `source_map_path` + `expected_source_sha256`（两者都要给），按游戏数据与地图物编合并后的结果只读返回，`project_identity` 为 `archive-readonly-<SHA>`。需要宿主配置 `FORGE_MCP_WAR3_ROOT`。物编的新增、修改、删除要先展开工作区（见下一小节）或用编辑器端口。

### 拿到 .w3x 时：展开、修改、写回

1. 不带 `--workspace` 启动独立 MCP，调用 `archive_expand`（或 CLI `archive-expand`），参数是源图、源图 SHA 和一个尚不存在的新目录，一般加 `data_only=true`。
2. 用 `--workspace <该目录>` 重新启动 MCP，`get_revision` 确认 `identity_current=true`。
3. 先 `begin_edit`，**之后**再用 `objects_list` / `objects_get` 查询并拿 `project_identity`、`revision`，然后用 `objects_set_fields` / `objects_create` / 触发器写工具修改，再 `preview_edit`、`validate_edit`。`begin_edit` 会改变修订，在它之前查到的 `revision` 拿去修改会返回 `OBJECT_REVISION_CONFLICT`。
4. `save_map_edit_candidate` 带 `source_map_path`（原 .w3x）、`expected_source_sha256`、新的 `output_path`、`transaction_id` 和当前 `expected_revision`，写出新地图包。工作区的物编基线必须与原图一致，否则拒绝。
5. 不带工作区对新候选再查一次（例如 `objects_get` + `source_map_path`）确认改动。

放进展开目录的新文件不会自动进入地图包；要加导入文件，对写出的候选再调用 `archive_members_add`。目前保存仍会重建整份脚本。

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

当前注册的写工具包括 `create_trigger`、`append_root_eca`、`append_argument_call`、`set_scalar_argument`；参数/类型必须由真实 Catalog 和现存所有者证明。错误时用 `rollback_edit` 释放自己的事务。不要同时开两个写事务，不要把源变化当成可以跳过校验的理由。

`export_map_candidate` 导出新的**展开目录候选**，不是 `.w3x/.w3m` 封包；后者会明确拒绝。它与 `save_map_edit_candidate` 是不同的现有出口，不能把目录事务导出描述成地图归档真保存。旧 `set_object_field/create_object/delete_object` 不再注册。

## 5. 结果应如何记录

保留源/候选路径与 SHA、包内源码身份、请求的绑定/raw、新值、退出码/业务 ok、原始诊断和重开结果。候选存在时换新路径，不覆盖源图或已有证据。

分别记录程序启动、查询、修改、候选写出与重开、原版编辑器打开/另存、真实游戏运行。此次包的说明不新增后两项证据。GUI包含完整当前项目的入口与界面，不代表路线图里的模型、地形、战役/AI、新格式或其他模块都已完成；预览近似也不等同于逐帧原版一致。
