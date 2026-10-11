# 世界放置工具

编辑器内置助手和独立 MCP 服务共用 `entities_create`、`entities_delete`、`entities_copy` 和
`entities_rotate`。这些工具修改地图中的实例，不新建物编类型。先用 `objects_list` 或
`objects_get` 确认物体 ID，再用 `entities_query` 获取当前地图的修订和成员身份。

这些工具都需要编辑权限。内置助手使用当前工程的地图会话和撤销栈；独立 MCP 需要通过
`begin_edit` 开启事务。调用工具后仍须经过正常预览、验证和保存，回执中的 `saved` 为
`false`，不表示已经写入地图。

## 新建实例

`entities_create` 每批接受 1–32 项。`kind` 指定存储成员：`units` 可同时包含单位和物品，
`doodads` 可同时包含装饰物和可破坏物。不同成员分批操作。

| 参数 | 含义 |
|---|---|
| `object_kind` | `unit`、`item`、`doodad` 或 `destructable` |
| `object_id` | 原版或自定义物编 ID，区分大小写 |
| `x`、`y` | 游戏世界坐标，必须在当前地形边界内 |
| `height_offset` | 相对当前采样地面的放置高度，默认 0 |
| `facing_radians` | `[0, 2π)` 弧度，默认 `3π/2` |
| `scale` | 同时提供 `x`、`y`、`z`，每轴大于 0 且不超过 1000，默认均为 1 |
| `variation` | 非负变体编号，默认 0 |
| `owner` | 单位玩家编号 0–15，默认 0；物品固定为 15；地物不接受此项 |
| `hero_level` | 单位英雄等级，至少 1，默认 1 |
| `life_percent` | 单位或地物的生命百分比 0–100；单位另可用 -1 表示默认值 |
| `mana` | 沿现有字段接口使用整数值，-1 表示默认值；仅限单位 |
| `item_sets` | 单位或可破坏物的内联掉落组，物品 ID 必须存在 |

物体类型和自定义父链、地形边界、玩家以及字段范围采用与世界页摆放相同的校验。
类型来源缺失、父链损坏或地形不可读时会拒绝，不会猜测。独立服务从宿主配置的
`FORGE_MCP_WAR3_ROOT` 读取标准目录；工具参数不能指定另一个游戏目录。

每个掉落组为 `{"items":[{"item_id":"I001","chance_percent":100}]}`。
最多 32 组、每组 1–32 项、每个实例合计最多 128 项；每组概率合计不超过 100。
暂不接受 W3I 随机掉落表编号。

本轮没有实现 `mana_percent`，传入该参数会明确拒绝。放置高度写入原生记录，实际游戏
初始化是否沿用该高度还需要对应地图的初始化脚本与原版游戏确认。

请求还必须带 `project_identity`、`expected_revision` 和当前成员的
`expected_member_sha256`。成功后的 `new_ids` 包含 `kind`、`source_index`、`row_key`、
`creation_id` 和 `member_sha256`；按请求顺序返回。行定位绑定当前快照，后续编辑后应重新
查询，不能只凭旧行号继续操作。

## 删除、移动与旋转

`entities_delete` 的 `sources` 每批接受 1–32 组 `source_index` 与 `row_key`，并核对同一
成员 SHA。重复来源、过期行或不支持的记录会拒绝整批。`changes.items` 中删除项的
`after` 为 `null`，创建项的 `before` 为 `null`；查询格式的 `before.rows` 或 `after.rows`
在对应侧没有记录时为空数组。

普通创建和删除不支持玩家开始位置 `sloc` 或特殊地物尾部。开始位置需要与玩家设置
同步，不能用普通单位命令代替。

修改已有实例坐标使用 `entities_set_fields` 的现有坐标字段。复制使用 `entities_copy`
的世界 XYZ 偏移；绝对旋转使用 `entities_rotate` 的弧度参数。注意原字段编辑中的
`facing` 使用度，两种入口的单位不同。

## 预览、撤销与错误

上述四个放置工具都接受 `dry_run: true`。预览给出候选记录和变化，不提交界面草稿，
不申请事务、不消耗命令编号、不改变修订或撤销重做。创建和复制预览的身份放在
`candidate_ids`，`new_ids` 为空；候选身份不代表已经创建实例。

预览只反映当时已提交到工程的内容，不包含编辑控件里尚未提交的草稿，也不预留后续
写入权限。实际应用仍会重新核对修订、来源、事务组合和完整回执预算。

内置助手可连续操作当前行，使用编辑器原有撤销重做。独立 MCP 的一个原来源事务暂时
只允许一批同类创建、删除或复制，或者反复修改同一原行的朝向；不能混入其它放置配方
或任意字段变化。批量中任意一项失败时，不发布前面的合法项目。

常见错误包括：

- `ENTITY_REVISION_CONFLICT` 或 `LIVE_EDITOR_REVISION_CONFLICT`：工程已经变化，重新查询。
- `MAP_PLACEMENT_MEMBER_STALE`、`MAP_PLACEMENT_ROW_STALE`：成员或行身份过期，重新取得定位。
- `ENTITY_CREATE_HOST_UNAVAILABLE`：创建所需的物编或地形宿主未接通。
- `PLACEMENT_OWNER_INVALID`、`PLACEMENT_DROPS_INVALID`：玩家或掉落字段不适用于该类型，或数值无效。
- `MCP_PLACEMENT_COMBINATION_UNSUPPORTED`：独立事务中已有不兼容的操作，应结束当前事务后重新打开候选。
- `ENTITY_RESPONSE_BUDGET`：完整回执超出预算，未发布；缩小批次后重试。

本程序写出和重开原生记录，与原版编辑器打开、地图初始化脚本生成、实际进游戏是不同
的验证步骤。涉及运行时效果时仍应另行确认。
