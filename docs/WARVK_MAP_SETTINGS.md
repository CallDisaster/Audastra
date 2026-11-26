# WarVK 地图设置

“地图设置 → WarVK”保存给 WarVK 渲染器用的地图级设置：镜头限制、执行时机、天空模型、太阳方向与颜色、昼夜光照开关和远景装饰物。设置存在地图内的编辑器成员 `audastra\warvk.ini`（`schema=1`），游戏本身不读它；每次重新生成脚本时，编辑器按设置写出一段受管的 JASS 初始化代码。物体编辑器中装饰物的“远景”勾选改的是同一份设置。

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

每条指令前面都带 `warvk:v1;`。省略的键不写指令，WarVK 保持自己的默认值。越界、格式错误、未知键或重复键都整份拒绝，不截断、不改成边界值。天空模型路径按 MPQ 路径上限计算，含结尾 0 共 260 字节。

太阳方位角与单位朝向是同一约定：0° 为东，90° 为北。写出的是光线方向，也就是从太阳射向地面的向量 (-cos仰角·cos方位, -cos仰角·sin方位, -sin仰角)，东为 +X，北为 +Y，保留 6 位小数。例如东方地平线上 6° 的太阳写成 `r:-0.994522;r:0;r:-0.104528`。要让太阳固定不动，需要把“天体运行”和“按时间调色”都设为关。

## 写出的脚本

受管函数 `AudastraWarVKInit` 先用 `GetLocalizedHotkey("warvk:v1;system.protocolVersion")` 读取协议版本，存入 `AudastraWarVKProtocol`；WarVK 接管时它非零，未接管时为 0。随后按上表顺序逐条 `call Preloader( "…" )`：镜头限制、远景类型、两个光照开关、太阳方向、太阳颜色。每条之后用 `warvk:v1;system.lastErrorCode` 把结果码存入 `AudastraWarVKErrors[序号]`，0 表示被接受。最后调用 `SetSkyModel`。默认时机下函数由计时器调用，结束时销毁计时器。

## AI 助手与 MCP

- `map_warvk_get` 返回全部设置、保存时写出的指令列表、协议与结果码查询串，以及天空模型是否已在地图里（`sky_model_in_map`）。
- `map_warvk_set` 只改请求里给出的项：
  - `camera_limits`、`sun_direction`、`sun_color` 都是带 `enabled` 的对象，`enabled:false` 表示取消。
  - `far_doodad_types` 整表替换。
  - `sky_model` 传空串表示取消，正斜杠按反斜杠写。
  - `celestial_motion`、`time_color_grading` 取 `default`、`on` 或 `off`，`default` 表示不设置。
  - 一次调用记一条撤销。
- `map_warvk_far_doodad_set` 标记或取消单个装饰物类型；已有 16 个时，标记第 17 个会被拒绝。
