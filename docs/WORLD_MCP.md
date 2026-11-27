# 世界视口 MCP

活动编辑器的 MCP 端口与内置助手使用同一世界视口。先读取 `editor_state`，随后所有请求携带返回的 `project_identity` 和字符串形式的 `expected_revision`。关闭、切换或修改工程后重新读取身份；过期请求不会修改视口或交付旧截图。

## 视口相机

`world_viewport_camera_read` 返回 `camera` 和字符串形式的 `viewport_revision`。它表示当前请求相机；已经完成绘制的相机见截图回执。

`world_viewport_camera_set` 需要 `expected_viewport_revision`、`camera` 和明确的 `dry_run`。相机有六个有限数值字段：`target_x`、`target_y`、`target_z`、`distance`、`yaw_radians`、`pitch_radians`。角度使用弧度。参数、范围和交互繁忙检查与界面共用。`dry_run: true` 只校验；`false` 提交视角更新，成功仍需等待完成帧。这个操作需要编辑权限，只改变视口，不改地图中保存的镜头对象。

## 装饰物散布

`world_scatter_stroke` 用世界页的散布笔刷画一笔，规则、预算和撤销与界面相同：一笔一步撤销。需要编辑权限，并核对地图来源快照。

- `mode`：`paint` 散布，`erase` 擦除调色板类型的装饰物。
- `palette`：1–64 项 `{rawcode, weight?, type?}`，`type` 为 `doodad`（默认）或 `destructable`。
- `path`：1–256 个 `[x, y]` 游戏坐标；按下第一个点，再沿路径每半径一半补一笔。
- `settings`：可选，未给出的项沿用界面当前值。`radius`、`min_spacing` 用游戏单位；`density_per_100m2` 配合 `units_per_metre`（默认 40）换算；给 `count_per_dab` 时改为每笔个数。`yaw_min`/`yaw_max`、`slope_min`/`slope_max` 用度，`scale_min`/`scale_max` 为统一缩放，`random_variation` 随机变体。
- `surface`：`terrain` 是否落在地形上，`filter` 为模型路径或类型代码的通配（`*`、`?`，分号或空格分隔，不分大小写）。给了过滤时只要最上层的可见装饰物模型命中；没有可用模型表面时明确失败，不退回地形。
- `seed`：可选十进制整数串，相同参数和种子得到相同落点。
- `dry_run`：`true` 只返回落点，不改地图。

模型表面在后台读取，过滤、地形开关或调色板变化后第一次调用返回 `WORLD_SCATTER_SURFACE_BUILDING`，稍后用同样参数重试。成功结果包含 `placed`/`erased`、被拒绝的数目（无表面、坡度、间距）、前 64 个落点、表面三角形数、`map_doodads`（这一笔之后的地图装饰物数）、`palette_vertices`（调色板类型在地图里的顶点合计；`palette_vertices_complete` 为假时还有模型未读完）和新的 `document_revision`。散布出的装饰物使用绝对高度（标志 0x04）。

## 完成帧截图

1. `world_viewport_capture` 携带 `expected_viewport_revision`。可选 `max_rgba_bytes` 默认 16 MiB，最多 64 MiB。只读取本编辑器实际完成的世界帧，不截取桌面或其它窗口。读回沿用渲染插件的等待上限；PNG 编码放后台。
2. 返回 `capture_id` 和 `status: pending`。调用 `world_viewport_capture_result`，携带同一工程身份、编辑修订和 `capture_id`，查询 `pending` 或 `ready`。
3. `ready` 返回 PNG 字节数与元数据。元数据包含实际完成帧号、视口尺寸、相机、矩阵、场景/表面/视图修订、附加模型层及可核验的动画来源身份。静态场景没有证明地图编辑来源时，来源修订保留 `null`，不拿视口序号代替。命令身份另放在 `request` 中。
4. 可选 `include_png: true` 让外部 MCP 返回标准 `image` 内容块，类型为 `image/png`，图像与元数据正文分开。内置工具适配保留同一PNG结果。PNG 最多 4 MiB，元数据最多 64 KiB，超过预算明确拒绝。默认只查询元数据；活动日志不复制图像正文。

只有一个编码任务和一个最新结果；新截图会替代旧请求。PNG 保留冻结的像素和身份，后续视角变化不会改写已完成结果。工程或视口更换后不交付旧截图。

独立 `war3-map-mcp` 没有编辑器视口，同名工具返回 `WORLD_VIEWPORT_UNSUPPORTED`。世界页尚未打开、切图或重建期间返回不可用；缺少完成帧能力、过期帧或超出预算分别返回具体诊断，不生成空图。
