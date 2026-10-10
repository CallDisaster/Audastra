# MCP 安全事务适配器

`war3-map-mcp` 是实验性 stdio MCP 适配器。它复用 `EditorSession`、
`WtgWctCodec` 与 `TriggerDocumentEditor`，不会直接写源工作区。

## 写入合同

- `get_revision` 返回完整 generation、源目录 SHA-256 和 UI Catalog SHA-256，
  并通过 `identity_checked`/`identity_current` 明示本次来源复核结果。
- `begin_edit` 获取单写租约；之后每个工具都必须携带十进制字符串
  `transaction_id` 和同一份 `expected_revision`。工具参数严格拒绝缺失字段、
  未声明字段、错误 JSON 类型与非小写十六进制 SHA-256。
- `create_trigger`、`append_root_eca`、`append_argument_call` 和
  `set_scalar_argument` 每次成功后都会重新生成密封的 WTG/WCT 双文件 patch。
- `append_root_eca` 固定在根列表尾部追加；MCP 不提供插入、删除、移动或重排。
  嵌套 call 与标量槽类型只能从可达的唯一 owner 和当前 Catalog 签名推导，
  不接受客户端自报类型；无法证明的 preset、变量或直接值一律拒绝。
- 函数库不按编号特判（9527 与其它库相同）；未知 Catalog 定义、陈旧 revision、
  不可证明的参数所有者、来源目录变化和 Catalog 变化全部 fail-closed。
- 每次成功 mutation 都会使旧 preview/validate 失效。只有当前 edit generation
  已显式 `preview_edit` 且通过 `validate_edit` 才能调用 `export_map_candidate`；
  发布 rename 前还会再次核验源与 Catalog。候选路径必须不存在，且不能位于
  源工作区内。
- `rollback_edit` 仍校验 transaction 与 revision 身份，但不受来源/Catalog 漂移
  阻塞，确保失败事务始终可以释放单写租约并丢弃 overlay。

旧版 `set_object_field`、`create_object`、`delete_object` 与松散
`export_candidate` 不再注册。尤其禁止用自动签发的删除审计令牌代替真实引用审计。

## 当前明确边界

`export_map_candidate` 仍只原子输出完整的**展开目录候选**，同时保留未修改成员
并写入 SHA-256 manifest。传入 `.w3x` 或 `.w3m` 仍返回
`MCP_ARCHIVE_EXPORT_UNAVAILABLE`；它不会把活动 WTG 事务直接封包。

另有独立的 `save_terrain_blight_candidate`：对已有 MPQ 地图执行圆形荒芜笔刷，
经地图数据事务、W3E/WPM 联动、脚本重建和 `MapSaveService` 写全新 `.w3x` 候选。
调用方必须提供绝对来源/候选路径、来源归档与 W3E 的小写 SHA-256、顶点坐标、
半径 0..16 和 set/clear；活动 WTG 事务期间拒绝运行，以免误以为候选含该 overlay。
YDWE 工具根与游戏根仅由启动宿主设置的 `FORGE_MCP_YDWE_ROOT`、
`FORGE_MCP_WAR3_ROOT` 环境变量提供，工具参数不能覆盖可执行工具路径。
这个窄工具不代表通用 `.forge`/MCP 地图保存，也不自动宣称原版或游戏验收。
生产候选仍须分别核验 World Editor/YDWE 真打开与另存、重开及隔离运行。
