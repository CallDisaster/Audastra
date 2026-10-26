# 地图自带 runtime DLL

本文说明 Audastra 自有 DLL 的构建、地图接入和故障定位。不描述或分析 AI 引导器源码；引导器使用契约以提供方随版本交付的 `AGENT_USAGE.md` 等说明为准。仓库不包含引导文件或提供方交付目录；其许可与公开分发尚待项目作者确认，构建本项目不等于取得该交付物。`.ai` / `.AI` 只允许整体导入和大小／SHA-256 校验，不得读取、解析、格式化或编辑源码。

**2026-10-26 开发状态：作者仅批准现有raw黑盒用于内部原创测试图/验收副本，不随编辑器或地图公开分发。** 共用包/保存策略、最终JASS启动、运行库身份握手与进程级生命周期正在集成；新的源码构建不继承下述旧冻结DLL的实机资格。目前没有由正常编辑器保存流程新产生并完成游戏验收的交付地图，共存适配也未完成。

### 安装侧可信包目录（开发接口）

编辑器只从安装根下的 `runtime/catalog.json` 读取本机包目录，缺席表示没有安装包，不自动下载或使用地图内路径。清单仅由安装者维护，最多16项、32KiB，结构如下（摘要占位符不是可用包）：

```json
{
  "schema": 1,
  "packages": [
    {
      "directory": "validation-1",
      "manifestSha256": "<64位小写SHA-256>",
      "publicDistributionApproved": false
    }
  ]
}
```

包位于 `runtime/packages/validation-1/`；其中 `manifest.json` 必须与安装者锁定的摘要相符，包内文件仍须逐项核验。目录名只接受受限小写标识，不接受绝对路径、穿越、设备名、重解析路径、重复项或未知字段。损坏配置返回 `RUNTIME_CATALOG_INVALID`，不当成“没有依赖”。安装目录配置是宿主信任输入，不是签名或沙箱；地图、工程和Agent参数不能增加包来源或分发许可。现有raw内部包不能因为该布尔字段设为true就变成公开包。

`LoadInstalledMapRuntimeSources` 和 `QueryMapRuntimePackages` 为共用查询；后者只返回包身份、版本、DLL身份、内部范围、可读取状态及诊断，不返回目录或黑盒内容。“可读取”不是游戏可运行或公开分发的证明。Qt工程的 `QueryMapRuntimeState` 另附当前generation/revision、地图设置与本工程明确批准状态；地图自带包身份不会自动使批准为true。查询不修改工程；启用仍须使用同一图/资源修订事务，保存仍有独立用途与发布前核验。CLI和独立保存入口已接同一准备服务，真实包与验收候选按版本分别登记；实机共存仍待完成。

### 测试副本与活动地图工具（开发接口）

普通“保存／另存为”保持 `Normal` 用途，不因为地图内声明、已批准包或测试副本标志而取得内部包的公开分发资格。“测试地图”在准备脚本之前通过 `PrepareMapTestCopy` 冻结 `TestRun` 用途；内部专用验收可明确选择 `InternalValidation`。这两种用途只允许不存在的独立新路径，不得原地保存、覆盖现有目标或通过 `CommitMapSave` 切换工程的源图、保存基线及撤销历史。内容预检可带明确用途但不写地图；通过预检不授予后续正式保存资格。

编辑器活动地图的 MCP 提供：

- `map_runtime_query`：返回当前策略、锁定包身份、本工程批准状态及可信包列表；不自动批准或选择最新版。可不带身份查询，也可携带 `project_identity` 和十进制字符串 `expected_revision` 核对旧结果。
- `map_runtime_set`：必须携带上述工程与修订身份及 `mode`（`automatic`、`enabled`、`disabled`）。非关闭模式还必须传查询返回的 `package_sha256`，并在作者已明确授权后传 `approve_native_runtime: true`。关闭时不得同时选择包或授予批准。设置与本工程批准使用同一可撤销事务，不修改作者触发器代码，不保存地图，不启动游戏。

工具不接受本机包路径、任意文件内容或分发许可字段；权限关闭或修订过期时拒绝，失败不提交部分设置。独立 stdio 的上述活动工程设置工具返回 `RUNTIME_LIVE_HOST_REQUIRED`；独立归档查询和保存使用下面的入口。MCP 接线和离线测试不等于原版编辑器重开或游戏验证。

### CLI和独立归档保存

`war3-map-cli map-runtime-query <map> <source-sha256>` 与独立MCP的 `query_map_runtime`（`source_map_path`、`expected_source_sha256`）返回归档设置及安装包元数据。不包含活动工程overlay，也不把地图声明当成批准：查询中的 `approved` 总是false。源SHA已变化时拒绝旧查询。

CLI `map-edit-save` 可附以下选项，也可只提交一项运行库决定而不做其他地图编辑：

```text
--runtime-mode automatic|enabled|disabled
--runtime-package <查询返回的64位小写SHA-256>
--approve-native-runtime
--runtime-purpose normal|internal_validation|test_run
```

非关闭模式须明确批准及固定包身份；关闭时不传包和批准。用途默认normal，内部包仍只能进入独立、全新的内部验收/测试副本。示例中的包身份须来自当前安装目录，不能用占位SHA执行。

独立MCP `save_map_edit_candidate` 使用同一Core服务，参数为 `runtime: {mode, package_sha256, approve_native_runtime}` 和 `runtime_purpose`。决定可独立提交，或伴随原来的一项有界领域编辑；不会把两种普通领域编辑合并放行。包路径与分发许可只由宿主固定安装目录提供，不接受请求字段。批准与当前源地图SHA绑定在同一次保存任务内，不从归档自动继承；失败不写部分设置/成员。两入口回执均区分实际 `runtime_packaged` 与保存用途，不报告游戏通过。

活动编辑器的 `editor_save_map` 可显式传 `runtime_purpose: "internal_validation"`，只允许新的非原地路径。宿主仍调用MainWindow共用保存处理器，输出不会切换工程基线；不传该字段仍是普通保存。内部验收探针 `--probe-runtime-save <new-map>` 配合 `--open-map`、`--runtime-source-sha256` 和 `--runtime-package-sha256`，在专用无界面编辑器进程中复用同一批准事务和保存处理器；它不改作者触发器，不替代可见UI、原版编辑器或游戏验收。

**状态（2026-10-25）：固定 raw 候选已在经典 1.27a 专用沙箱完成 AI → 本方 DLL → JAPI 握手 → 内置 UI 的实机环路，并完成同一进程内重新开始同一地图一次；该结果在两次独立运行中复现。尚未取得发布资格。** 两进程各自首次进图和重开后都完成 20 次 UI 生命周期断言，DLL 日志分别建立新的 VM epoch；其中一次退出菜单也观察到 VM reset。不能把两次独立运行累计为同一进程连续重开两次。跨不同地图、返回菜单后重新选图进入、连续两次重开、多人环境和作者地图发布流程仍未验收。

本次通过的是下表锁定摘要的 `warvk_loader_raw.ai` 和冻结 DLL；不将结果推广到其他引导器版本或游戏映像。提供方将 raw 候选说明为 MemHack MIT 引导器的衍生交付物，**不是 clean-room 重写**；应保留对应版权和许可证。本方接入工作只使用其黑盒契约，没有读取 MH 通讯实现。历史上的最小／线程探针未取得标记，不能由此否定当前 raw 路径，也不能倒推那些文件的具体失败原因。

诊断注意：本机原版 `common.j` 的 `Preload` / `Preloader` 均为 `returns nothing`，**不能用它们的true/false返回值判断导入是否可见**。打包工具按精确名称回读MPQ成员只证明包内存在，不证明游戏运行时打开或AI编译成功。

## 1. 文件与启动顺序

| 内容 | 地图成员／用途 |
|---|---|
| 当前受验候选 `warvk_loader_raw.ai` | 根目录原名导入，仅作为不透明交付物使用 |
| 本项目构建的 `warvk_runtime.dll` | 以 `warvk_runtime.blp` 导入，字节不变 |
| `sdk/japi/jass/AudastraJapi.j` | 编入 `war3map.j`；在 UI 库之前，只含一次 |
| `assets/ui-library/AudastraUI/jass/AudastraUI.j` | 编入脚本，依赖 `AudastraJapi` library |
| 界面 `.toc`、`.fdf`、贴图等 | 按界面库使用的精确路径导入 |
| `LICENSE-MEMHACK.txt` | 随引导器分发保留许可证和版权声明 |

### 引导器版本与固定摘要

当前推荐用于专用验证的是 raw 系列，文件名不能与旧系列混用：

| 模式／用途 | 输入文件 | 实际地图导入名 |
|---|---|---|
| `raw-runtime`，完整 DLL／JAPI／UI 样例 | `warvk_loader_raw.ai` | `warvk_loader_raw.ai` |
| `raw-runtime`，可选阶段诊断版 | `warvk_loader_raw_debug.ai` | `warvk_loader_raw_debug.ai` |
| `raw-probe`，只验证载荷与入口 | `warvk_raw_probe.ai` | `warvk_raw_probe.ai` |
| 默认 `runtime`，保留的旧系列 | `warvk_loader.ai` 或 `warvk_loader_debug.ai` | 均为 `warvk_loader.ai` |

旧系列的打包器入口现在也采用 main 首句单次启动，但不因 raw 系列通过而取得同等实机验收。raw 阶段诊断版保留独立原名，不能套用旧阶段版重命名为 `warvk_loader.ai` 的规则；当前环路结果来自非 debug 的 raw 文件。

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `warvk_loader_raw.ai` | 26,772 | `7bbc430b8b2e4960dd6eb05ddd8f0f0607e5a1d8c9e1af12b726955848894eea` |
| `warvk_loader_raw_debug.ai` | 26,951 | `67f80aaa9edfe70bf26c0c6771975f7e6cd813bda7014088fcd755594b11952d` |
| `warvk_raw_probe.ai` | 26,772 | `38e9af50741f146fe57780614425a285cd9bf6808202cab181469ed5d934fbb5` |
| 冻结 `warvk_runtime.dll`／`warvk_runtime.blp` | 419,840 | `24272e6010512462d8546fd277f15b82bff78f67167760e523acec7734abbf82` |

`raw` 是这组提供方交付物的版本名称，**不表示 MPQ 成员不压缩**。当前打包器仍使用正常的 MPQ 压缩写出；未通过本轮结果验证“压缩／不压缩”的差异。`raw-probe` 与 `raw-runtime` 都强制校验上述对应文件和冻结载荷的大小／摘要；重新构建得到的不同 DLL 不能冒用本次验收结果或绕过冻结检查。

编辑器的 UI 库包已将 SDK 包在 `library AudastraJapi` 中，与 `AudastraUI` 一起提供。手工编译时也应如此包裹 SDK，不能再重复注入一份。使用项目已有 vJASS 翻译／检查流程，不手工删去 `library` 或 `private`。

按当前引导器使用契约，每局必须在最终生成的 `main` **第一条语句**启动一次，位于 `SetCameraBounds` 及所有初始化调用之前。纯 JASS 直接插入；YDWE 宏方式见使用契约。不能只检查源模板，必须检查编译后的 `war3map.j`：

```jass
function main takes nothing returns nothing
    call StartCampaignAI(Player(PLAYER_NEUTRAL_AGGRESSIVE), "warvk_loader_raw.ai")
    // 后面保留原有 SetCameraBounds 与其他初始化语句。
endfunction
```

这不是同步就绪保证。随后用有期限的计时器尝试 `AjInitialize()`；成功后再调用：

```jass
if AjInitialize() then
    if AjModuleLoad("audastraui") then
        if AudastraUIIsReady() then
            // 这里才加载 TOC、创建界面，检查每一步的 AjLastStatus。
        endif
    endif
endif
```

握手失败时可以重试握手，但**不能重复启动 AI**。UI 库的 `Connect` 只缓存成功；握手或 `module.load` 失败时保留最后状态用于诊断，下一次调用会重试，不会将第一次失败永久锁住。连接成功后不重复 hello／模块加载；`AudastraUIIsReady()` 仍会查询实际 UI 是否就绪。

`AudastraUILastError()` 在未连接或连接失败时只读取已记录的连接状态，不触发握手；因此“可用性检查失败后立即读取错误”不会因错误查询恰好重连成功而丢失诊断。尚未尝试连接时，连接状态为 `-1`。

生成的界面“创建”触发器每 0.2 秒检查一次 `AudastraUIIsReady()`，最多 150 次；库内不叠加重试上限或定时器，所以第 150 次仍可恢复。其他调用方也必须自行限制重试频率和期限，避免无运行库时高频反复请求。界面建立前，“更新”、状态与回到基础状态触发器应由“已建好”变量守卫，不能因连接已成功就访问未创建的框架。握手计时器可以在地图初始化完成后开始；AI 启动不能放进该计时器。

首次失败后的恢复修复有真实源码控制流回归与打包脚本检查；这些是离线证据，不把既有 raw 实机样例的通过扩称为生成器界面的延迟恢复实测。已成功连接后的模块卸载、跨 epoch 恢复仍是独立生命周期问题。

`MapRuntimeSmoke.j` 不再调用 AI：由打包器在 `main` 首句插入唯一调用，在 `main` 末尾调用 `AjMapSmokeStart()`。样例的 1 秒计时器只延迟开始握手轮询，此后每 0.2 秒尝试一次，最多 150 次；不得再额外安装一个 AI 启动计时器。打包器对最终生成脚本检查首句、紧随其后的 `SetCameraBounds` 和唯一 AI 调用，不能只凭源模板判断位置。

不要用启动器注入、手工 `Preload` 或手工 `LoadLibrary` 代替这条分发链。下面的离线契约测试在独立 fixture 进程加载 DLL，只检验二进制契约和拒绝路径，不是游戏部署方法。

## 2. 构建

需要 Windows、MSVC C++20、Windows SDK、仓库正常配置所需的 Lua 源码和已锁定共享 RenderCore。使用 Win32 构建目录，不把编辑器的 x64 目录切换为 Win32。

```powershell
cmake -S src/WarcraftRuntime32 -B build/map-runtime32 -G "Visual Studio 18 2026" -A Win32 `
  -DAUD_LUA53_SOURCE_DIR=<lua-5.3.6/src的绝对路径> `
  -DWAR3_RENDER_CORE_SOURCE_DIR=<锁定RenderCore的绝对路径> `
  -DBUILD_TESTING=ON
cmake --build build/map-runtime32 --config Release --target audastra-map-runtime-tests audastra-japi-runtime-tests audastra-game-ui-tests
ctest --test-dir build/map-runtime32 -C Release -R "audastra-(map-runtime-contract|japi-runtime|game-ui)$" --output-on-failure
```

产物是 `build/map-runtime32/Release/warvk_runtime.dll`。`audastra-map-runtime` 使用静态 CRT，固定导出未修饰的 C `__cdecl WarVKAIInit()`。当前依赖为系统 `KERNEL32.dll`、`USER32.dll`、`bcrypt.dll`，无 Lua／UI 伴随 DLL。测试会检查 PE32、x86、DLL 标志、唯一导出、导入白名单及无延迟依赖。

`AUD_JAPI_BRIDGE_EXPERIMENTAL` 只控制旧的**启动器**实验桥；它不控制地图 DLL。地图 DLL 是单独产物，只有按地图分发契约显式加载并调用入口才安装桥。正常构建不会自动启动游戏，也不会自动修改作者地图。

## 3. DLL 内部职责

入口不依赖启动器参数、共享内存或邮箱：

1. 找到已经加载的 `Game.dll`，取得只读来源租约，核对 SHA-256 和 PE 映像。
2. 校验本项目已有原版 ABI 核验所支持的 native／VM 生命周期入口。映像版本或入口字节不同即拒绝，不覆盖未知钩子。
3. 通过事务安装晚加载字符串桥和生命周期通知，枚举本进程线程参与指令位置调整。挂起阶段不执行本方日志、文件操作或容器扩容。
4. 第一次 `ajapi:` 请求在真实 JASS 调用线程上建立 `GameVm127` 租约、`NativeSession` 和 Host，不把 AI 初始化线程当作 JASS 线程。
5. 将内置 `audastraui` 注册到该 Host。地图的 `AjModuleLoad("audastraui")` 确认其可用；`audastraui.ready` 另外检验游戏 UI 是否就绪。
6. 响应经原 VM 的字符串分配函数返回带引用 ID。普通本地化字符串原样交给原版函数；私有请求格式错误也不回落原版解释器。

当前仅接受已核验经典 1.27a `Game.dll`：

```text
e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a
```

不能通过删除摘要检查来支持另一版本。新的游戏版本需要独立的 ABI 验证与实机验收。

### 生命周期与限制

- 同一模块再次调用入口不会重复安装钩子。安装成功后 DLL 固定驻留，不自卸载。当前开发实现也核实进程内其他副本：同DLL内容身份复用唯一拥有者，不同内容/歧义/无法核实则拒绝并要求重启游戏。版本字符串相同不代表内容相同。
- 已识别主 VM 的销毁／重建会失效旧租约、清理旧 Host，并递增 epoch；下一次请求创建新会话。
- 同一调用中发生重入重置时，不析构正在执行的 Host；禁用当前会话并记录故障。
- 错线程、VM 身份改变、字符串分配失败等故障使当前会话失效。地图 DLL 不擅自结束玩家进程；测试启动器可另外执行自己的终止策略。
- 不提供任意外部模块加载授权，不提供同步回调执行器；UI 事件使用本地轮询。UI 状态不能直接修改同步游戏状态。
- `module.unload` 后再次装入内置 UI 尚无重载工厂。正常流程每局只装入一次，换图创建新 Host；不要把手动 unload/reload 当作已支持功能。
- 跨副本初始化统一先取得进程锁，再委托已有拥有者或取得模块锁；避免两份DLL反向等待彼此的初始化锁。并发副本测试只证明离线fixture行为，不等于游戏内共存。UI外部token在进程拥有者范围内不复用，旧epoch和旧token都须拒绝。
- 旧冻结DLL并未实现上述新身份/副本策略，其固定raw实机记录只覆盖两个进程各首图及同图重开一次。当前开发版的连续两次重开、跨不同地图和返回菜单后重进均须重新验收。
- 本文记录的固定样例实机结果不覆盖所有晚附着时机、地图、多人环境或发布路径；离线通过不能替代这些验收。

## 4. 日志与分层诊断

入口写 `OutputDebugStringA`，并尝试独占新建 `%TEMP%/audastra-japi-<PID>-<tick>.log`。不会覆盖已有文件；文件创建失败时仍有调试输出。最多记录 2048 行，单条截断，不记录地图任意参数、文字返回值或内存地址。

主要标记：

| 标记 | 能证明什么 |
|---|---|
| `initialize-enter` | 入口已被实际调用 |
| `owner-rejected` | 进程拥有者内容身份、唯一性或初始化锁核验失败；不覆盖已有拥有者 |
| `game-identity-rejected` | 已进入入口，但当前映像不受支持 |
| `hook-sites-changed` | 入口字节不匹配，拒绝接管 |
| `thread-enumeration-failed` / `hook-install-failed` | 晚加载桥没有安装完成 |
| `initialize-ready` | 钩子已装好，尚在等待 JASS 请求 |
| `vm-attach ... ok` | 在 JASS 调用线程取得 VM 租约 |
| `builtin-install audastraui status=0` | 内置 UI 模块已注册；不等于游戏 UI 就绪 |
| `command=... status=... kind=...` | 响应已交给 VM 的字符串槽；布尔和句柄非零状态可见 |
| `vm-reset epoch=...` | 已处理已识别的 VM 重置 |

只有日志说明“交给 VM”仍不足以证明 JASS 正确收到并使用响应。需要地图侧断言和可见 UI 结果。

`raw-probe` 只导入黑盒、冻结 `warvk_runtime.blp` 和许可证，不编入 JAPI／UI。它的载荷文件标记是游戏进程 `%TEMP%` 下的 `WARVKPRB<8位十六进制>.dll`，大小应为 419,840 字节；本轮还实际取得 `initialize-enter` 和 `initialize-ready`。只有文件出现只证明释放阶段，只有 `initialize-ready` 也不证明 JASS 已握手。

正常 raw 加载器使用 `MHLOADER<8位十六进制>.dll` 临时文件名；名称前缀不代表其中仍是 MH 载荷，应以冻结 DLL 的大小／摘要和本方入口日志识别。若专用启动器为子进程单独设置了 `TEMP`／`TMP`，应检查那个目录，而不是诊断终端自己的 `%TEMP%`。临时文件可能短暂出现，单次目录检查未见文件不构成加载失败的证明。

如果地图已运行，但握手超时、无入口日志且模块快照中无 runtime，定位边界是**尚未观察到黑盒加载器到本 DLL 的控制权交接**。这不证明黑盒内部哪一步失败。先核对实际导入名、整体摘要、最终 `main` 唯一调用和游戏映像，再请提供方给出不含实现细节的分阶段诊断；不通过分析 `.ai` 越过边界。出现 `initialize-ready` 后仍超时，则继续检查 `vm-attach` 和地图侧请求，不能再统称为“DLL 没加载”。

### 内置界面的父框架与故障诊断

`AudastraUICreateSimpleFrame` 的父参数为 `0` 或 `AudastraUIGameUI()` 返回的根句柄时，运行库使用游戏的默认 Simple 根。根句柄是经过核验的别名，不会把 GameUI 对象指针直接交给 Simple 构造函数。其他父框架必须属于 Simple 家族；普通 Backdrop、普通 Frame、Texture 和 FontString 不能作为 Simple 的父框架。普通框架创建也不能以 Texture 或 FontString 为父框架。

`AudastraUISetParent` 只支持 Simple 父框架；父子关系不能形成循环，检查深度最多 64 层。父家族不受支持时返回 `AJ_UNSUPPORTED`，循环或超过深度限制返回 `AJ_ARGUMENT`。用失败请求号查询 `audastraui.error` 可取得具体原因。这些拒绝发生在调用游戏函数之前，界面库保持可用；不要把收到一次参数错误理解为运行库需要重新加载。创建时的父子关系和锚点关系是两个独立参数，跨家族定位仍按各自布局对象处理。

游戏函数发生结构化异常时，运行库保留 `ui-fault` 日志，包含 epoch、请求号、固定入口名、异常代码和已核验 Game.dll 范围内的相对偏移。范围外的位置标为 `outside_verified_game`；日志不输出绝对地址或地图参数。此时界面库会停止后续游戏函数调用，避免在可能已损坏的界面状态上继续操作。普通调用日志与故障日志各有独立的行数上限。

## 5. 原创验收样例

打包器负责唯一的 main 首句 AI 调用；[`MapRuntimeSmoke.j`](../sample/MapRuntimeSmoke.j) 依次测试握手、UI 装入与就绪、普通本地化、字符串保留、TriggerExecute 和计时器调用、TOC、20 次 UI 创建／定位／尺寸／文字／显示／销毁、销毁后句柄拒绝。完整成功标记是地图消息 `JAPI MAP PASS: loader + DLL + hello + UI + timer/trigger + stale handles`，同时应有对应 DLL 日志及实际可见 UI。不要仅凭界面上的短 `JAPI MAP PASS` 标签验收：样例先设置标签，随后才执行最终 JASS 断言；失败会另外显示具体阶段。

主工程的手动目标 `audastra-japi-map-fixture` 复用项目 MPQ writer、vJASS 翻译器和 JASS checker，只接受 SHA 绑定且名称明确的原创平图种子，拒绝已存在的输出目录。它不会自动运行游戏。

```text
audastra-japi-map-fixture <原创seed.w3x> <seed-sha256> <专用游戏目录> <仓库根目录> <runtime.dll或payload.blp或-> <opaque-loader> <全新输出目录> [probe|probe-early|probe-thread|probe-thread-p0|control-tiara|raw-probe|raw-runtime]
```

在仓库的既有 x64 构建目录生成打包工具：

```powershell
cmake --build build/vs2026 --config Release --target audastra-japi-map-fixture
```

以下参数均需替换为实际路径；两次输出目录必须分别不存在。先做载荷／入口探针，再做完整样例：

```text
audastra-japi-map-fixture "<原创seed.w3x>" "<seed-sha256>" "<专用游戏目录>" "<仓库根目录>" "<交付目录>\warvk_runtime.blp" "<交付目录>\warvk_raw_probe.ai" "<全新探针输出目录>" raw-probe
audastra-japi-map-fixture "<原创seed.w3x>" "<seed-sha256>" "<专用游戏目录>" "<仓库根目录>" "<交付目录>\warvk_runtime.blp" "<交付目录>\warvk_loader_raw.ai" "<全新完整样例输出目录>" raw-runtime
```

`raw-probe` 不带 JAPI／UI，不能用它要求握手或界面 PASS。`raw-runtime` 编入本方 SDK、UI 包装和 smoke 样例，并导入样例 TOC／FDF；可选阶段诊断版仅替换其 loader 参数为原名 `warvk_loader_raw_debug.ai`。不要把 `raw-runtime` 省略为默认模式，否则会使用旧系列文件名规则。

工具把 AI 作为不透明字节交给归档写入器，不解析或输出源码；为验证整体替换，对输入和地图内固定成员做有界大小／SHA-256 校验，输出实际导入名、大小、摘要。使用方应另与受信任提供方公布的版本摘要比较。其他自有成员逐字节回读。还应使用独立 `pjass` 检查本项目生成的 `war3map.j`，再用实际游戏核验。当前完整样例的主脚本检查通过，检查总计 13,983 行（含原版声明），**不包括 `.ai`**；这不是 AI 编译通过的证据，也不能把我们自己的编译器作为唯一裁判。

保留的历史诊断模式中，`probe` 使用 [`AiProbeSmoke.j`](../sample/AiProbeSmoke.j)，只在初始化后 1 秒调用一次，不作为当前推荐启动方式；`probe-early`、`probe-thread`、`probe-thread-p0` 使用早期单次调用，均不带 DLL／JAPI／UI，runtime 参数为 `-`。p0 只用于一次性诊断，不是发布调用方式；原对照未改为电脑控制槽，不能据此排除所有控制器条件。`AUD_PROBE`／`AUD_RAW_PROBE` 均只是调用端提示，不是黑盒执行成功标记。

默认 `runtime` 模式保留旧系列正常版和阶段版入口，已使用相同的 main 首句单次调用及延迟握手样例；旧阶段版仍按其契约导入为 `warvk_loader.ai`。它们的实机结果必须单独记录，不继承 raw 系列的通过结论。

`control-tiara` 仅供独立沙箱对照：参数分别使用提供方原样的 `tiara.blp` 和 `tiara.ai`，工具严格核对固定名称、大小及SHA，且不编入本方DLL/JAPI/UI。阳性判据是运行时实际出现预期载荷文件，不是调用返回提示、包内回读或仅有主脚本编译通过。该对照不加入作者地图保存／发布流程，不分析其AI或载荷内容。

只用专用游戏安装及原创地图副本启动普通 `war3.exe -window -loadfile "<相对地图路径>"`。进图成功后检查屏幕断言与 DLL 日志，再逐项验证同进程重新开始、返回菜单后重新选图、跨不同地图的 epoch、旧句柄失效、模块复用和无崩溃；不能把其中一种操作代替全部换图场景。目前已通过的是同一地图首次进入及同进程重新开始一次，退出菜单观察到 reset，尚未回菜单后重新进入。首次进图未成功握手时，不记录后续 UI 或换图为通过。

上述固定样例打包器不是作者地图的保存出口；编辑器保存接线及内部用途门禁见本文开头的开发接口说明。共用保存成功也不授予公开发布或游戏运行资格。完整协议、错误码和事件模型仍见 [PROTOCOL_V2.md](PROTOCOL_V2.md)；默认关闭的启动器路径见 [RUNTIME_BRIDGE.md](RUNTIME_BRIDGE.md)。
