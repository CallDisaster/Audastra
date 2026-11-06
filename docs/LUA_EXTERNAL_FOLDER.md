# 使用外部文件夹编写 Lua

在“触发器 → Lua 脚本”中点“关联外部文件夹…”。空文件夹会先收到地图里的手写 Lua；非空文件夹会列出只在地图、只在文件夹和正文不同的文件，再由作者选择以哪一边为准。取消不会改动文件。选择以地图为准会写入、删除外部 Lua，请先检查差异。

关联后，这一页和 `lua_file_read`、`lua_file_write` 使用外部文件。VS Code 保存后会自动重新读取；编辑器中的“应用草稿”直接写盘。未应用草稿与外部修改冲突时，必须选择保留草稿、使用外部文件或取消。关联与取消关联可以随地图撤销重做；**文件夹里的写入、改名和删除不进入地图撤销栈**，文本框自身的撤销仍可使用。

取消关联后使用地图中已经嵌入的 Lua，外部文件保留。如需把最新外部内容保留在地图里，请先保存地图，再取消关联。

保存、另存和 F9 从同一个服务捕获完整快照：先检查 UTF-8、模块路径和语法，再冻结正文及 SHA，写入地图后回读核对。文件夹里消失的 Lua 会在保存前列出，只有确认后才从地图删除。捕获或发布前发现来源变化就拒绝本次保存。保存期间文件句柄可能短暂阻止外部编辑器写入；外部编辑器报告占用时，请待保存结束重试。

勾选“保存后自动热重载”后，外部文件保存也会触发发送；未发送的文件仍使热重载按钮显示强调色。删除已加载的模块需要保存并重新测试，热重载不会假装已经撤回游戏中的旧模块。

关联路径写入编辑器专用成员 `audastra/editor/lua-folder.json`，同盘使用相对地图文件的路径，跨盘才用绝对路径。另存会重新计算相对路径。地图传给另一台电脑后找不到文件夹时，继续使用嵌入地图的 Lua，保留关联；文件夹恢复后点“重新连接”或重新打开地图。来源无权限、内容不合规与路径不存在分别报告，不会把读取失败当空文件夹。

Windows 的目录通知句柄可能暂时阻止重命名正在监视的文件夹。要移动或重命名整个关联目录，请先关闭地图，再操作目录并重新打开地图。

文件范围是手写 `.lua` 模块：`a\b.lua` 对应 `require "a.b"`。跳过点开头的目录和所有符号链接、联接，不跟随链接；文件夹本身及父路径须为普通本机盘符目录。生成的主脚本与兼容层由保存链维护，不导出、不允许覆盖。单文件最多 2 MiB，最多 4096 个 Lua 文件，正文合计最多 32 MiB；扫描最多 262144 个条目、8192 个目录及 16 MiB 路径文本。预算、UTF-8、大小写冲突和模块路径错误会定位到文件，拒绝整次操作。

## 命令行

以下 `cli` 表示当前版本的 `war3-map-cli.exe`，路径包含空格时加引号。查询回执中的 SHA 必须照原值回传，不能自行生成批准值。

```text
cli lua-folder inspect map.w3x [folder]
cli lua-folder read map.w3x lessons/reload.lua
cli lua-folder link map.w3x <map-sha> linked.w3x folder <reviewed-folder-sha> folder|map|cancel
cli lua-folder unlink linked.w3x <map-sha> unlinked.w3x
cli lua-folder save linked.w3x <map-sha> saved.w3x
cli lua-folder save map.w3x <map-sha> saved.w3x --lua-folder folder
cli lua-folder write linked.w3x <map-sha> lessons/reload.lua replacement.lua <file-sha|new> output.w3x
```

`write` 在当前来源是外部文件夹时只写盘，回执为 `map_saved=false`；地图来源时写新的 `output.w3x` 副本。`save` 和 `link` 同样走地图共用保存服务，不覆盖输入地图。

`lua-folder inspect` 返回 `map_only` 和 `delete_confirmation_sha256`。确认删除这些文件后，在保存或测试命令上添加 `--confirm-lua-deletions <sha>`；来源变化导致删除集合不同，旧确认失效。`map-edit-save` 和 `map-test` 同样接受 `--lua-folder <目录>` 与该删除确认参数。覆盖只选择本次来源，不改写原关联。`map-test --reload-module-after <秒> <模块名>` 在发送时从当前来源捕获正文，模块名例如 `lessons.reload`；原有 `--reload-after` 仍可显式选择独立的 Lua 文本文件。

## MCP

从当前编辑器的 `tools/list` 查询完整 schema。`editor_state.lua_source` 报告来源、文件夹、连接状态和快照 SHA。`lua_folder_review` 返回差异和文件夹 SHA；`lua_folder_link` 必须带同次审阅的 `expected_folder_sha256` 和明确 `choice`，并遵守现有工程身份与修订检查。`lua_folder_unlink` 与 `lua_folder_reconnect` 复用同一服务。`editor_save_map`、`map_test_start` 的 `lua_delete_sha256` 接受已审阅的删除集合身份，Agent 路径不会弹出替作者确认的对话框。
