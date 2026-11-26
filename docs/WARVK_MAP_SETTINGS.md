# WarVK 地图设置

“地图设置 → WarVK”保存给 WarVK 渲染器用的地图级设置：镜头限制、执行时机、天空模型、太阳方向与颜色、昼夜光照开关、云海和远景装饰物。设置存在地图内的编辑器成员 `audastra\warvk.ini`（`schema=1`），游戏本身不读它；每次重新生成脚本时，编辑器按设置写出一段受管的 JASS 初始化代码。物体编辑器中装饰物的“远景”勾选改的是同一份设置。

每次修改是地图设置撤销栈上的一条记录。有未保存的设置改动时，保存必须真正重建脚本；没有可用的脚本工具链就拒绝保存，不会悄悄保留旧脚本。Lua 地图暂不支持写出这些设置，保存时报 `WARVK_LUA_UNSUPPORTED`。

## 设置项

| 键 | 界面 | 取值 | 写出的指令 |
|---|---|---|---|
| `timing` | 执行时机 | `timer`（默认，地图初始化触发器之后的 0 秒计时器）或 `main`（main 开头） | 决定受管函数何时运行 |
| `camera.limits` | 镜头限制 | `远距上限,近裁剪`；远距 10000–400000，近裁剪 1–100 | `camera.setLimits;r:远距;r:近裁剪` |
| `doodad.far` | 远景装饰物 | 逗号分隔的四字符物编 ID，排序去重，最多 16 个 | `doodad.setFarTypes;s:ID,ID,…` |
| `sky.model` | 天空模型 | `.mdx` / `.mdl` 相对路径，反斜杠分隔，最多 259 字节 | 指令之后 `call SetSkyModel( "路径" )` |
| `lighting.celestialMotion` | 天体运行 | `0` 或 `1` | `lightingCycle.setCelestialMotionEnabled;b:0/1` |
| `lighting.timeColorGrading` | 按时间调色 | `0` 或 `1` | `lightingCycle.setTimeColorGradingEnabled;b:0/1` |
| `sun.direction` | 太阳方向 | `方位角,仰角`（度）；方位角 0–360（不含 360），仰角 -90–90 | `sun.setDirection;r:x;r:y;r:z` |
| `sun.color` | 太阳颜色、太阳强度 | `红,绿,蓝,强度`；颜色各 0–1，强度 0–10 | `sun.setColorIntensity;r:红;r:绿;r:蓝;r:强度` |
| `cloud.sea` | 云海、云海顶高、云海覆盖度 | `顶高,覆盖度`；顶高为世界 z（绝对值不超过 1000000），覆盖度 0–1 | `localFog.createBox;r:cx;r:cy;r:cz;r:sx;r:sy;r:sz;r:覆盖度;r:0.15` |

每条指令前面都带 `warvk:v1;`。省略的键不写指令，WarVK 保持自己的默认值（两个光照开关例外，见下）。越界、格式错误、未知键或重复键都整份拒绝，不截断、不改成边界值。天空模型路径按 MPQ 路径上限计算，含结尾 0 共 260 字节。

## 太阳与昼夜开关

太阳方位角与单位朝向是同一约定：0° 为东，90° 为北。写出的是光线方向，也就是从太阳射向地面的向量 (-cos仰角·cos方位, -cos仰角·sin方位, -sin仰角)，东为 +X，北为 +Y，保留 6 位小数。例如东方地平线上 6° 的太阳写成 `r:-0.994522;r:0;r:-0.104528`。

WarVK 的日夜循环会每帧覆盖地图给的太阳：天体运行开着时覆盖方向，按时间调色开着时覆盖颜色。两者在 WarVK 里默认都开，所以：

- 设了太阳方向而天体运行为“不设置”时，保存按关写出；设了太阳颜色而按时间调色为“不设置”时，同样按关写出。界面上对应开关直接显示“关”，“不设置”项不可选。设置文件里仍记为不设置，取消太阳方向或颜色后恢复原样。
- 明确选了“开”时照写，并给出提醒：`WARVK_SUN_DIRECTION_OVERRIDDEN`（方向被覆盖）或 `WARVK_SUN_COLOR_OVERRIDDEN`（颜色被覆盖）。提醒显示在页面下方、保存诊断和 `map_warvk_get` 的 `warnings` 里，不阻止保存。
- 按时间调色的开关会同时开关整个昼夜系统：选“开”等于打开日夜系统，选“关”（含设了太阳颜色时自动写出的关）等于关闭它。

## 云海

玩家打开物理天空和云层、而且地图没有打开体积光时，WarVK 把地图里最大的扁平局部雾盒画成云海：盒顶就是云海顶，雾盒密度就是覆盖度。玩家没开云层时这个雾盒不显示，也没有别的作用；不认识它的旧版 WarVK 只会忽略。编辑器不写 `volumetric.setEnabled`：体积光一打开，雾盒就归体积光，不再是云海，而是覆盖整图、密度等于覆盖度的体积雾。地图自己的自定义脚本或触发器里出现 `volumetric.setEnabled` 字面文本时，保存给出提醒 `WARVK_CLOUD_SEA_VOLUMETRIC`；拼接出来的指令串和导入的脚本库检查不到，需要作者自己留意。

雾盒在保存时按待写出的地形（完整 `war3map.w3e`）计算，不旋转：

- 中心 cx、cy 为地图中心；水平全尺寸 sx、sy 为地图宽、高的 1.5 倍，至少 4096，最多 200000。
- 盒底为地图最低地面再往下 100；sz = 顶高 − 盒底，cz = (顶高 + 盒底) / 2。
- 顶高不高于盒底（`WARVK_CLOUD_SEA_BELOW_GROUND`）、盒高超过 sx、sy 中较小者（`WARVK_CLOUD_SEA_TOO_TALL`）、中心超出 WarVK 坐标范围（`WARVK_CLOUD_SEA_RANGE`）或读不到地形（`WARVK_CLOUD_SEA_TERRAIN`）时拒绝保存并说明，不改成边界值。页面下方按当前地形提前显示这些问题，顶高的提示里给出盒底和顶高上限。
- 数值保留 3 位小数，羽化固定 0.15。

## 写出的脚本

受管函数 `AudastraWarVKInit` 先用 `GetLocalizedHotkey("warvk:v1;system.protocolVersion")` 读取协议版本，存入 `AudastraWarVKProtocol`；WarVK 接管时它非零，未接管时为 0。随后逐条 `call Preloader( "…" )`：镜头限制、远景类型、两个光照开关（实际写出值）、太阳方向、太阳颜色。设了云海时接着写 `set AudastraWarVKCloudSea = GetLocalizedHotkey( "warvk:v1;localFog.createBox;…" )`，保存雾盒 id（未接管时为 0）。每条指令和云海之后都用 `warvk:v1;system.lastErrorCode` 把结果码存入 `AudastraWarVKErrors[序号]`，序号按实际写出的顺序从 0 编起，0 表示被接受。最后调用 `SetSkyModel`。默认时机下函数由计时器调用，结束时销毁计时器。

## AI 助手与 MCP

- `map_warvk_get` 返回全部设置、两个光照开关的实际写出值（`effective_celestial_motion`、`effective_time_color_grading`）、保存时写出的指令列表、协议与结果码查询串、提醒（`warnings`），以及天空模型是否已在地图里（`sky_model_in_map`）。有已载入的地形时还返回地面范围（`map_ground`）和云海盒（`cloud_sea_box`，含查询串）；云海盒算不出时原因列在 `warnings` 里。
- `map_warvk_set` 只改请求里给出的项：
  - `camera_limits`、`sun_direction`、`sun_color`、`cloud_sea` 都是带 `enabled` 的对象，`enabled:false` 表示取消。`cloud_sea` 取 `top`（世界 z）与 `cover`（0–1）。
  - `far_doodad_types` 整表替换。
  - `sky_model` 传空串表示取消，正斜杠按反斜杠写。
  - `celestial_motion`、`time_color_grading` 取 `default`、`on` 或 `off`，`default` 表示不设置。
  - 一次调用记一条撤销。
- `map_warvk_far_doodad_set` 标记或取消单个装饰物类型；已有 16 个时，标记第 17 个会被拒绝。
