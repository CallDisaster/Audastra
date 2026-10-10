# 本机 Lua 运行库接口观察

这些工具是开发验收入口。它们不提交、复制或打包本机 YDWE 的六个 DLL；不执行作者地图脚本，不改变 P2 / 运行期 Lua 地图保存门禁。运行库及游戏来源必须由参数指定，示例用 `FORGE_YDWE_ROOT`、`FORGE_WAR3_ROOT`；不提供本机安装路径默认值。

`Initialize` 的 PE 导出只证明该入口存在，不能作为 `jass.*` 字段清单。可信字段由实际 Lua state 中 `require` / 元数据枚举 / 候选名称 lookup 产生。清单的未观察字段、惰性字段和非 common 接口签名始终是未知，不推断不存在。

## 顺序

1. 集成负责人构建 `audastra-lua-api-probe-map`、同版本 Win32 launcher/bootstrap 和 `audastra-runtime-fixture-tests`。先通过 33 个原创控制台 fixture；覆盖独立 Desktop、nonce 观察、沙盒允许写、外部写拒绝、安装日志重定向、既有 hardlink 在 `CREATE_ALWAYS` 截断前拒绝，以及一/二条精确非致命拒写、未知第三条/作者路径、记录溢出、别名仍致命、重复/超过容量/非法参数提前拒绝。slash 同一文件的不同文本必须在创建游戏前拒绝；已有父链和叶子的 8.3 拼写只读展开后再去重。短名用例日志明确卷是否实际提供不同的短名；返回原长名时不计为 8.3 别名覆盖。fixture 不运行 Warcraft、YDWE、GUI 或 GPU，也不构成真实运行库清单。

   `RuntimeLuaProbePolicyTests.cpp` 另用仓库已验证的官方 Lua 5.3.6 核心及标准库运行同一原创模板，覆盖实际 C preload、二进制未知候选、嵌套 require、Lua preload 拒绝和失败时 searchers 还原。附加标准库只链接这个测试目标；`LUA_POLICY_TEST_SOURCE_MANIFEST.json` 的七个文件与固定 SHA 的官方源码包逐字节核对，不扩大应用的 compile-only 依赖。

   `audastra-runtime-witness-tests` / CTest `audastra-runtime32-witness` 独立包含 12 个 focused case：原五项显式隐藏窗口实际观察（含错误 owner/class/exact Job/缺 HWND 的实际反例）、默认 metadata probe 无窗口、非法值、重复参数、非 probe 拒绝；另三项窗口诊断参数和三项 renderer 参数各覆盖非法值、重复参数、非 probe 提前拒绝；最后一项用 fake Game 验证显式 OpenGL 请求的真实命令构造。参数拒绝项以原进程 exit 64、无 receipt、独立 Job 终态 total=1/active=0 证明只创建了本次 detached bridge，没有 Game/控制台助手。OpenGL 正例只证明 requested argv，实际后端仍为 unknown，不能证明游戏或 renderer 初始化成功。它使用同一原创 fake DLL，明确创建自有 Desktop 的隐藏 USER 窗口，**不属于默认 33 个 noGUI 控制台 fixture**，不运行真实 Warcraft/YDWE 或 GPU，也不构成真实运行库清单。窗口必须在原 bridge/主线程仍存活时实际观察，错过就失败；不以固定等待时长代替证明。
2. `audastra-lua-api-probe-map` 用现有 `CreateMapCandidate` 生成原创经典 64 平地单人图。输出目录必须不存在。参数全部显式传入：

   ```text
   --tool-root <本机 YDWE 根>
   --game-root <已有 War3_AutoTestSandbox>
   --output-root <不存在的独立输出目录>
   --nonce <本次随机 32 位小写十六进制>
   --common <实际 common.j> --common-sha256 <SHA256>
   --blizzard <实际 blizzard.j> --blizzard-sha256 <SHA256>
   --lua-template <本目录 lua-api-probe.lua> --lua-template-sha256 <SHA256>
   --runtime-engine <本机 yd_lua_engine.dll> --runtime-engine-sha256 <SHA256>
   ```

   原创图仅有自有 `config/main`、定时器和 `EXExecuteScript` nonce marker。正式 Lua 探针不嵌入长 JASS 字符串，不依赖旧版地图 Lua 文件搜索格式。common 候选与签名复用生产 `IndexJassDeclarations`；引擎字符串仅作为 module 候选，`preparation.json` 明确 `moduleCandidatesAreExports=false`。生产 JASS 检查和 Lua compile-only 通过后才生成地图。
3. `run-lua-api-probe.py` 只在构建结束、持有唯一游戏租约时运行。必填参数：

   ```text
   --adapter <tools/dev/p1-isolated-desktop.py> --adapter-sha256 <SHA256>
   --desktop-helper <已有 AutoTest war3_autotest_mcp.py> --desktop-helper-sha256 <SHA256>
   --preparation <preparation.json>
   --sandbox-root <已有 War3_AutoTestSandbox> --ydwe-root <本机 YDWE 根>
   --launcher <自有 Win32 launcher> --launcher-sha256 <SHA256>
   --bootstrap <配对自有 bootstrap DLL> --bootstrap-sha256 <SHA256>
   --output-root <不存在的回执目录> [--probe-timeout-ms 45000]
   [--probe-desktop-witness hidden-window]
   [--probe-window-diagnostics bounded]
   [--probe-renderer opengl]
   ```

   只复用已锁 SHA 的 AutoTest Desktop helper 函数 AST，不执行 MCP 顶层或调用禁用工具。只复制自己生成的微型地图进既有沙盒。移除本次子进程继承的 `LUA_INIT*` / `LUA_PATH*` / `LUA_CPATH*`，防止作者环境注入额外 chunk 或模块。bridge 与 game 在 `WinSta0\AudastraRuntimeProbe-<nonce>` 中，输入 Desktop 不切换。bridge 必须先进入同一独立 Desktop 和 Job，才允许创建游戏。

   `--probe-renderer opengl` 仅允许显式用于上述 metadata probe。缺省记录 `game-default`，实际 Game 命令不追加图形参数；普通 F9 保持同样的默认构造。启用时只向实际 `CreateProcessW` 的 Game 参数向量追加一个 `-opengl`，不修改安装、视频设置、AutoTest 或 GPU 缓存策略。launcher 的非法值、重复和非 probe 参数在 receipt/Game 创建前拒绝；runner 的非法或重复新参数在解析阶段 exit 2，不进入运行或文件写出。本机 Game.dll 的 `opengl` 命令字串及 OpenGL 实现符号只支持这个候选请求，不证明实际选中了该后端。

   raw/isolation 同次记录 `probeGraphicsBackendRequested`，raw `gameArgv` 来自用于构造实际 Game 命令的同一个向量。清单先要求 raw `processCreated=true` 及严格正整数 `gameProcessId`，再由 isolation 的物理 launcher argv 精确重建 `[game, -window, 可选 -opengl, -loadfile, map]`，要求请求与 raw 全部 token 完全匹配，并保留三份来源回执 SHA 的 canonical 重建。公开 `graphicsBackend` 固定 `actualBackend='unknown'`、`actualBackendVerified=false`、`argvSameRunBound=true`、`backendSpecificAbsenceNotProven=true`；顶层 `gameplayVerified=false`。成功观察也只是该受控宿主和请求下的 partial 元数据，不构成 Direct3D9、完整 K4、后端无关接口存在性或游戏地图验收。未知的 OpenGL/驱动外写仍按原 NT 保护拒绝且致命，不扩大拒写名单或目录授权。

   本环境实测挂起/短命 bridge 的外部 `GetThreadDesktop` 返回 NULL，原因仍未知；[Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getthreaddesktop) 说明 thread 创建时已关联 Desktop，不能把 USER 初始化解释当成已证实根因。runner 先把 bridge 纳入本次独立 Job 再恢复；launcher 自己在 `Game CreateProcess` 前严格核当前线程的 nonce Desktop 和 Job 归属。默认模式随后最多轮询 5 秒，必须取得完全匹配的实际借用 Desktop；显式 hidden-window 模式在同一期限取得下述独立原线程 HWND 正证据。NULL、线程提前退出或超时不能变成 Default 回退。before-game self 门禁来自已锁定 launcher 控制流，不能把它伪填成外部查询返回值；外部 proofKind 与 Job 终态另行记录。失败只停止本次 Job（尚未归入 Job 时仅停止这个挂起 bridge），从不枚举或停止作者进程。

   bootstrap 在加载任何用户 DLL 前安装 probe 专用 `NtCreateFile` / `NtOpenFile` 写保护。磁盘写请求仅允许实际沙盒根；`YDWE/logs/war3.log` 请求改写到本次地图旁的独立日志，其余越界请求被拒绝并记录路径。允许目标及父目录先取得实际句柄并拒 reparse；已有普通文件必须只有一个 link，父目录和目标保持 deny-delete 句柄直到原 NT 调用结束，先核身份再允许破坏性创建。该保护是本次已知运行链的调用观察与阻断，不能称为任意恶意程序的完整操作系统沙盒；已有句柄、NT rename/link 等非 open 路径仍不属于该能力证明。Windows IPC / 图形设备 I/O 不当成磁盘写出。普通 F9 不启用这些观察点或保护。

   如真实回执显示可选驱动 profile 写请求，集成负责人可为下一次运行显式重复传 `--nonfatal-blocked-path <上次实际记录的精确路径>`（bridge 对应 `--probe-nonfatal-blocked-path`），最多四个互不重复的精确路径。目录不能代替成员匹配。路径不获得写权限：每个请求仍在原 NT dispatch 前返回 `STATUS_ACCESS_DENIED`，只是保持观察继续。传入 runtimeRoot 安装树、沙盒、未知/内部错误、reparse/多 link、其他路径及 32 条记录溢出仍致命；路径来自参数，不写死机器目录。paired IPC 版本 5 逐项登记路径、ready、非致命分类、NT status 与 `originalNtDispatched=false`，保留真实 `writeDenied=true`。清单只接纳与同次 runner 有界唯一列表完全绑定、无 fatal/overflow、记录完整的精确拒写；这些记录只证明所观察的 NT open 请求未被转发，不能扩大成完整 OS 无外写证明。

   `--probe-desktop-witness hidden-window` 必须显式启用且仅适用于上述原创 metadata probe。bridge 先回读自己主线程 nonce Desktop 名称，再在同线程注册 nonce class、创建无 WS_VISIBLE/WS_CHILD 且带 NOACTIVATE|TOOLWINDOW 的 1×1 顶层窗口。没有 Show/Activate/输入/前台/切换操作；它引入 USER 初始化与窗口创建消息，因此只证明启用此模式的 probe。默认 F9、默认 runner 和默认 33-case suite 不创建此窗口。

   runner 只 Enum 本次 hDesktop，要求唯一 nonce class、实际 owner PID/TID 精确等于原 bridge/主线程、原 process/thread 观察前后仍存活且原 hProcess 归本次确切 Job；两次复核 owner/class/style/隐藏状态。GetThreadDesktop 的 NULL 与即时错误原样保留；非 NULL 但名称不同仍硬失败，绝不拿游戏窗口替代 bridge。raw `desktopWitness` 与 isolation `bridgeDesktop` 的实际 HWND/TID/class/style 交叉绑定，原线程确认 Destroy/Unregister 后才写最终回执，失败不接纳。manifest 按 `desktopProof.proofKind` 明确区分 `thread-desktop` 与 `hidden-window`，不只凭 Desktop name 授予证明。窗口在外部观察前已消失或枚举失败仍失败；没有 ACK 或默认桌面回退。

   本机原生诊断曾实际观察到初次 Enum 返回 FALSE、即时错误 0、零回调和零行，而原 bridge/process/thread 仍存活。只将这组精确状态记录为 pending：再次核原进程/线程活性和 exact Job 后，在原先累计 5 秒期限内继续观察，不授予 `enumerationComplete`，也不解释为 Windows 文档承诺的空桌面语义。最终仍必须获得真正返回 TRUE 的完整枚举及原见证 HWND 的全部门禁；任何 API 错误、回调失败、部分行或矛盾 Desktop 仍失败。

   bootstrap 观察实际 `lua_pcallk` 返回的本次 marker，再由本机 Lua 5.3 编译并执行已 SHA 捕获的**自有**只读元数据探针。不重复执行原 chunk，不调用列出的游戏函数。仅接受真实 loaded / native C preload 的模块，在本次观察期间禁用文件 Lua/C searcher（含嵌套 require），结束和失败均还原 searchers；未注册的二进制候选保持未知。探针来源最多 64 KiB，JSON 最多 512 KiB；捕获或失败后 launcher 只结束本次新建 game，超时最长 120 秒。raw 的 `luaApiProbe.captureStarted` 直接序列化已有 IPC 标志，仅说明进入了 exact marker 捕获路径；它不能代替 `captured`、地图开始或实际 API 清单验收。runner 核验本 Job 全部终态，关闭本次句柄、逐项核 SHA 清理自己地图/日志；其他新文件留在沙盒并登记，避免盲删。

   `--probe-window-diagnostics bounded` 是另一个默认关闭、仅 metadata probe 可用的只读诊断。以原 Game 进程 HANDLE/PID 和自己主线程已核 nonce 名称的借用 Desktop，恢复 Game 后约 5 秒及主动终止/超时前最多各取一次窗口样本；自然退出只能记录原句柄不再存活，不能重新打开 PID 补观察。每样本的顶层与子控件共享最多 64 次回调、32 行预算，每个 class/caption 最多 160 个 UTF-16 字符；触及边界保守标记不完整，所有失败保留 BOOL/Win32 错误和原句柄活性，不改变捕获或隔离门禁。只读取匹配原 Game PID 的 class/caption，并二次核 PID/TID；`modalClass` 只表示类名为 `#32770`，不推断实际模态状态。对这种自有对话框才额外有界访问最多 16 个子控件，只读取同 Game/同 dialog TID 的 Static/Button 文字，记录 dialog `parentHwnd`、父 PID/TID 与 `parentStable`，前后复核父类名、父子关系、父子 owner 与原进程活性。其它 child 类不读取正文。caption 动态调用 [InternalGetWindowText](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-internalgetwindowtext) 直接读窗口结构，导出不可用时记录 unavailable，没有 GetWindowText/WM_GETTEXT/SendMessage 回退。[EnumChildWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumchildwindows) 返回值官方明确不用于判定结果，所以只原样记录 return/error/callback count，`childCompletionProof=false`，不声称已取得完整对话框正文。没有截图、输入、激活、前台切换或窗口修改。raw `windowDiagnostics.acceptanceProof=false`；样本不构成 bridge Desktop、进图或 Lua 调用证明。非法值/重复/非 probe 参数在 receipt 和 Game 创建前拒绝。
4. 真正观察成功后生成清单：

   ```text
   lua-runtime-api-manifest.py generate --preparation <preparation.json>
       --launcher-receipt <launcher.local.json> --isolation-receipt <isolation.local.json>
       --common <同次 common.j> --output <不存在的清单.json>
   lua-runtime-api-manifest.py verify --manifest <清单.json>
   ```

   缺少真实 IPC、nonce、六库初始化/SHA、Game.dll 身份、写保护或独立 Job 终态时拒绝生成；fixture 观察也拒绝。生成的 `scope=actual-runtime-observed-partial` 清单只登记确实返回的模块和字段。common 函数存在性经实际 lookup；参数数量来自同次冻结的 common.j 原始声明。其他签名为 null，所有模块 `absenceProven=false` / `enumerationComplete=false`，不授予保存许可。每次 verify 重新核六库、Game.dll 与 common.j，锁定三份原始回执的 SHA，再复用生成入口重建 canonical 清单并逐字段比对；字段或签名篡改、缺失或过期都明确返回“目前只检查语法”。

## 验证与边界

`test_runtime_api_manifest.py` 当前有 24 个原创纯文件回归方法，保留原 16 项来源/签名/未知/ledger/CLI 回归。默认直接 Desktop 及显式 hidden-window 合同含 89 个独立负 subTest：mode/argv、owner/class/PID/TID、原线程/进程活性与 exact Job、风格/隐藏/缺失/枚举容量、原始 NULL 与矛盾 Desktop、raw 清理/交叉身份和 canonical 证据篡改；pending 精确谓词与仅 pending 不构成 proof 另有 33 个负 subTest。renderer 新增默认/OpenGL 请求正例及 76 个负 subTest（56 个参数来源/实际 Game 创建身份配对、16 个 canonical 篡改、4 个重新绑定回执反例），合计上述新增负 subTest 为 198。类型敏感 canonical 比较会拒绝布尔值改成 0/1、整数改成相同数值的浮点数，保留原三份回执 SHA 重建。OpenGL 正例只证明 requested argv，实际后端仍为 unknown。方法和场景数是当前源码合同；本次实际执行由集成负责人另行签收。纯 helper/JSON 合同不替代外部窗口实际取证，不加载实际 DLL。

接口清单的生成与 K4 的 Lua 语义分析是不同出口。当前代码编辑器仍明确显示“目前只检查语法”，直到它实际加载可信快照并完成对应语义分析；生成离线 partial 清单不能把语法成功升级成完整语义成功。未定义全局、未使用局部及完整调用分析尚需后续实现，不能用正则猜测替代 Lua 分析。
