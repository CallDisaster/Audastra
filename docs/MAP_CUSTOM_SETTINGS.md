# 地图科技与随机表的 Agent 接口

CLI、活动编辑器与独立 MCP 共用无 Qt 的自定义设置服务和正常地图保存链。升级可用性和科技限制按行修改；随机单位、随机物品表按整表修改。随机初始化支持经典 v25 的 JASS 全局表；其它模式与目标仍须通过相应资格，不能通过保留旧脚本绕过。

当前修改要求唯一、完整的原生 W3I v25。v18/28/31/32/33、LNI、未知尾、损坏或冲突来源只查询；FF 省略表示没有提供自定义表，不会被初始化为空集合。查询中的读取状态、`custom_present`、`write_blockers` 和 `random_initialization_blocked` 分别表达这些情况。格式读取、正常保存、原版编辑器重开和游戏运行是不同资格。

## 查询

活动工程先调用 `map_custom_settings_get`，传 `project_identity`、字符串 `expected_revision` 和 `section:"summary"`。后续页精确回传 summary 的 `settings_binding`，其中包含文档身份、代次、修订、源地图 SHA 和 W3I SHA。过期页拒绝返回，不能混用另一张图的行令牌。

独立 MPQ 使用 `query_map_custom_settings`，传 `source_map_path`、`expected_source_sha256` 和相同查询字段。CLI 等价入口：

```text
war3-map-cli map-settings-custom-query <map> <source-sha256> [query-json-file]
```

省略查询文件时返回 summary。UTF-8 查询文件最多 1 MiB，例如把 summary 的 binding 填入：

```json
{"section":"upgrades","settings_binding":{"document":{"document_id":"archive","document_generation":"1","revision":"0","source_sha256":"<源图SHA>"},"member_sha256":"<W3I SHA>"},"offset":0,"limit":64}
```

`section` 为 `upgrades`、`tech_restrictions`、`random_units` 或 `random_items`。offset 从 0 起，limit 为 1..256，响应最多 1 MiB，`next_offset:null` 表示当前集合结束。科技页直接返回行及 `row_key`。随机表默认 `part:"table_headers"`，返回 number、名称原字节 `raw_hex`、显示解析结果、计数和 `table_key`；子表查询明确提供 number：

- 单位表：`part:"columns"` 或 `"rows"`。
- 物品表：`part:"sets"`；`part:"items"` 还需 `set_index`。

number 是原文件标识，不是数组下标。重复编号不能任意选取一个子表。超出令牌预算的大表仍可分页读取，表头令牌为 null，不授权编辑。名称显示不用于反向编码，原字节始终由 raw_hex 保留。

## 命令与 CAS

科技操作支持 append/update/delete：append 带 `expected_count`；update/delete 带零基 index 和完整 `expected_row_key`。CAS 针对执行到每条操作时的私有候选，因此连续 append 的期望数量依次增加。任意后缀失败不发布前缀。

```json
{"section":"upgrades","op":"append","expected_count":0,"value":{"player_mask":1,"id_hex":"52303031","level":1,"availability":2}}
```

`id_hex` 是文件顺序的四字节小写十六进制，示例为原创占位升级 R001；必须替换为实际目录存在的对象。升级 level 为零基，实际等级上限来自同一物编层序；availability 为 0 禁用、1 默认可用、2 已研究。初始化已研究等级为 level+1。同一玩家同一升级同时禁用/已研究会拒绝。科技限制 value 只有 player_mask、id_hex，ID 必须唯一解析到单位、物品或技能。新增空 mask、地图玩家集合外的位、未知对象、重复新行或不可证明的资源来源均拒绝。

随机表 append/replace/delete 用 number 定位，append 比对 expected_count；replace/delete 比对 expected_table_key，number 不自动重编。值为整张有界类型化表。名称仅接受 `{"utf8":"字面量"}` 或更新时 `{"keep":true}` 保留原编码，不隐式创建 TRIGSTR 引用。随机 ID 为 `{"kind":"object","id_hex":"..."}` 或明确的 `{"kind":"none"}`。类别/等级伪 ID 只保留旧来源，不开放新写。新增表须核对脚本符号，删除与列结构变动还核对原生摆放、全局掉落和触发器引用。缺少完整 T/WCT、自定义代码、动态函数分发或不透明脚本覆盖时会明确拒绝受影响的操作；只按 AI 成员名字记录不透明来源，始终不读取内容。同列替换不改变符号身份，仍核实际对象和概率。已有引用的列不能删除或变换类别；没有引用的尾列可在完整覆盖下移除。

单位随机行 chance 为 0..100，表总和至少 1；总和不等于100仅提示按比例抽取。物品组每项0..100、组总和不大于100，余量不掉落。一次最多128条操作/1MiB；每个新单位表最多64列、4096行、16384单元，物品表最多4096集合/16384项。

## 预览、活动撤销与独立保存

活动 `map_custom_settings_edit` 需要工程身份、修订、完整 settings_binding、operations 和必填 dry_run。true 只准备验证，false 在来源/资源再核对后沿原地图数据会话发布，一个批次一个撤销记录。同值不增加记录。回执包含 has_changes、can_commit、before/after_member_sha256、step_keys、diagnostics 和 save_blockers；`map_saved:false` 表示仍需编辑器正常保存。工程尚有未进入物编文档的原生物编导入/删除时先拒绝验证，避免用旧文档给新资源授权。

独立目录事务暂不接此领域写入，返回 `SETTINGS_DIRECTORY_WRITE_UNSUPPORTED`；独立 MPQ 用现有候选保存入口：

```text
war3-map-cli map-edit-save <source> <source-sha256> - <tool-root> <game-root> <new-candidate> --settings-custom-edit-file <commands.json>
```

commands.json 是严格 UTF-8 对象，包含 `expected_member_sha256` 和 `operations` 数组。独立 MCP `save_map_edit_candidate` 对应参数为 `expected_settings_sha256` 和结构化 `settings_custom_edits` 数组，不读取客户端命令文件路径。这一独立入口一次只接受一个领域；其它编辑通过活动工程的共用会话累积后正常保存。

两者都绑定源图 SHA，经现有脚本生成、检查、最终派生资源检查和独立归档回读；不允许传 W3I/脚本字节、任意成员路径或自报物编验证标记。保存前会重新验证实际物编来源，失败、源漂移或 no-op 不产生新候选；源图保持不变。

## 随机初始化范围

JASS 在创建预放物体之前调用 `InitRandomGroups`，每张单位表只抽一次行，各列来自同一行，保持相关选择。`none` 不创建对象；全局单位掉落表与可破坏物掉落表保留每组独立抽取及余量不掉落。全局随机放置要求精确的 `uDNR/bDNR/iDNR` 标记、表编号和列类型一致；对象类别、颜色、技能与可破坏物来源必须可证明，未知对象不能按普通单位默认处理。

可破坏物全局掉落限原生 flags 为 0/4 的脚本创建组、生命值为100%的已证范围；归档自行加载组含掉落时继续拒绝，不自动改标志。0权重物品记录在 W3I 中保留，生成分布时不占用 BJ 数组槽；余量仍按原概率计算。

Any/Custom 随机放置模式、未证明的建筑伪 ID、非 v25 随机初始化和 Lua 随机初始化仍明确拒绝。旧单位/物品类别等级伪 ID 只在已知编码范围保留，不授权新写。随机占位也不授予固定生成名的 T 引用资格，可用原版枚举取得实际创建的句柄。`can_commit` 表示当前设置事务可发布；正常保存仍会核对最终脚本目标、对象与所有受影响成员，不是全图运行保证。

此范围已用原创地图通过正常保存、独立回读及经典 1.27a 隐藏实例的两行分支、相关列、none、100%/0%单位掉落和可破坏物掉落对照。原版可见编辑器重开、其它版本、Lua、概率统计分布和完整游戏生命周期单独验收。

宿主的只读原因使用 `MapCustomSettingsEditing::RandomWriteQualification(document, selectedLuaTarget)`。第二参为实际选择的 `LuaMapTargetRequest` 指针，未选 Lua 时为空；经典 Lua 的 W3I 仍可能为 language0，不能只凭文档猜目标。成功表示基础布局和目标资格满足，失败诊断可直接显示；对象、引用与最终脚本仍由准备和保存继续核对。`random_commit_supported` 表示构建提供随机表能力，不能替代针对实际目标的这个判定。活动工程及独立保存均由可信宿主冻结目标，协议不接受回调或自报验证标记。
