# 编辑小地图图标

在世界页的小地图“图层”菜单中选择“编辑图标”。从金矿、中立建筑、开始位置里选类型，点击“颜色”打开取色器，再点“放置”并点击地图。拖动已有图标只改位置，保留原类型和颜色。可在地图空白处框选，或按 Ctrl/Shift 增减选择后一次删除。需要精确坐标时再展开“高级坐标”。每次修改进入工程的资源撤销栈；选择“图标点”也可在世界页小地图查看未保存的改动。

普通保存将手动图标写入标准 `war3map.mmp`，另写与 MMP 正文 SHA-256 绑定的归属成员 `audastra/minimap-markers.bin`。有归属记录时，正常的自动生成继续更新自动图标，再合入作者的手动图标。移动或删除自动图标只屏蔽当时那条完整记录；如果单位移动后生成了一条坐标不同的记录，它是一条新的自动图标。归属缺失的旧图不会被自动改写，保存报告 `TERRAIN_MMP_PRESERVED_LEGACY`。归属与正文不一致时报告并保留原图标，不按类型、坐标或颜色猜测来源。

手动图标保留由 Audastra 保存链保证。用原版编辑器重存会按原版规则重建图标。这些图标用于游戏选图预览，对局中的 HUD 小地图另行绘制。

没有图标、也没有手动编辑的地图可以按旧规则自动生成图标。如果首次写归属会触及无法确认的静态通行依赖，普通保存仍生成原版 MMP，暂不写归属，并报告 `TERRAIN_MMP_PROVENANCE_DEFERRED`。这不适用于已有归属或手动编辑的图标。

命令行先查询，再提供实际返回的源 SHA 与图标修订，普通保存到新候选：

```text
war3-map-cli minimap-marker-list map.w3x
war3-map-cli minimap-marker-edit map.w3x <源SHA> <图标修订> <工具链根> <游戏根> candidate.w3x add 1 96 128 ffffffff
war3-map-cli minimap-marker-edit map.w3x <源SHA> <图标修订> <工具链根> <游戏根> candidate.w3x move 0 110 128
war3-map-cli minimap-marker-edit map.w3x <源SHA> <图标修订> <工具链根> <游戏根> candidate.w3x remove 0
```

活动编辑器 MCP 工具 `editor_query_minimap_markers` 分页返回图标、行号、来源与修订。调用 `editor_edit_minimap_marker` 时使用当前工程身份、工程修订和图标修订，另传 `operation=add|move|remove`；移动和删除指定 `row`，添加指定 `type` 和四个整数的 `bgra`，添加与移动指定 `x/y`。行号从 0 开始，跟随查询当时的顺序。修改后应重新查询。
