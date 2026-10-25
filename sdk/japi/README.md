# Audastra JAPI 2.0

这是 Audastra 自有的地图脚本与受信任原生模块之间的进程内通讯 SDK。实现包括严格编解码、C ABI、模块调用、同步／本地／异步事件、两阶段卸载、Windows x86 摘要固定加载器、JASS 客户端和 manifest 生成器。

当前交付包含**可构建、可测试的协议 SDK，以及默认关闭的实验字符串桥**，不是已经验收的 Warcraft 游戏运行时。SDK 的 `NativeBridge.h` 本身不安装 hook；编辑器 x86 自有测试运行时另有基于既有 native 观察器与 GameVm127 的[实验接入](docs/RUNTIME_BRIDGE.md)。它不自动启动游戏，不授予模块加载或脚本回调能力。真实 VM 的字符串引用、执行栈与换图仍须独立验收；同步回调保持关闭。

另有按黑盒引导器使用契约实现的[地图自带 runtime DLL](docs/MAP_RUNTIME.md)：导出 `WarVKAIInit`，独立安装桥／Host 并注册内置 UI。固定 raw 引导器与冻结 DLL 已在经典 1.27a 专用原创样例中完成加载、握手及可见 UI，两个进程各完成首图与同图重开一次。此证据仅属于该固定组合，不自动覆盖新编译 DLL；编辑器保存时自动打包、生成器界面、跨图、菜单重进、同进程连续两次重开及联机仍待验收。它与上述启动器实验桥是不同部署路径。

## 阅读入口

- [完整协议规范](docs/PROTOCOL_V2.md)：线格式、状态码、限制、事件完成语义和生命周期。
- [宿主接入与验收](docs/INTEGRATION.md)：怎样接入编辑器／游戏宿主，信任配置放在哪里，哪些结果不能当作游戏验收。
- [1.27a 实验字符串桥](docs/RUNTIME_BRIDGE.md)：默认关闭开关、实际调用链、基础能力、VM 所有权和手动验收样例。
- [地图自带 DLL 与 UI 接入](docs/MAP_RUNTIME.md)：构建、固定导入名、唯一启动、异步握手、日志及原创实机样例。
- [从旧原型迁移](docs/MIGRATION.md)：为什么升 major、哪些接口不兼容、保留哪些旧路径。
- [C ABI](include/audastra_japi.h)、[宿主 API](include/aj/Host.h)、[JASS 库](jass/AudastraJapi.j)。
- [原创 C 示例模块](sample/Demo.c)、[单一命令清单](sample/demo.manifest.json)、[生成器](tools/generate.py)。

## 构建与测试

需要 CMake 3.24+、支持 C++20 的 MSVC、Windows SDK、Python 3。游戏 DLL 必须构建为 Win32；x64 可运行纯协议／宿主测试，但加载器明确拒绝游戏 DLL。

在仓库根目录、Visual Studio 开发终端中执行：

```powershell
cmake -S sdk/japi -B build/japi-v2-x86 -G "Visual Studio 18 2026" -A Win32
cmake --build build/japi-v2-x86 --config Release
ctest --test-dir build/japi-v2-x86 -C Release --output-on-failure
```

安装了其他受支持的 Visual Studio 版本时，用对应的 CMake generator；不要把 x64 构建目录直接切换为 Win32。

测试分为：

1. `audastra-japi-contract`：参数／返回值边界、45,000 组确定性编解码样本、线程、重入、事件完成、溢出、VM 代际、停机和保活。
2. `audastra-japi-codegen`：manifest 校验、所有参数类型、保留名、重复字段／命令、符号冲突、拒绝覆盖已有输出。
3. `audastra-japi-loader`：原创 DLL 的 PE／摘要预检、加载／调用／卸载，以及真实工作线程模块的五次完整生命周期。使用构建目录作为显式暂存父目录，不读取游戏文件。
4. 编辑器主工程另登记 `audastra-japi-jass`，复用编辑器的 JASS 检查器，对库做声明顺序、语法和类型检查。它不是原版 VM 的运行证明。

SDK 已接入编辑器及 x86 runtime 的构建配置。启动器路径只有显式开启 `AUD_JAPI_BRIDGE_EXPERIMENTAL` 才改派字符串 native；默认关闭。独立地图 DLL 另由地图明确加载并调用入口，不依赖该开关。测试和样例不随游戏自动安装。构建产物留在 `build/`。

## 生成地图侧包装

```powershell
python sdk/japi/tools/generate.py sdk/japi/sample/demo.manifest.json --out build/japi-demo-generated --dll build/japi-v2-x86/Release/demo.dll
```

输出 `demo.j`、`demo.contract.h`、规范化 manifest，以及可选的 `demo.package.json` 摘要描述。输出目录中的同名文件已存在时拒绝覆盖；重新生成请使用新的目录。

还可在配置 SDK 时传 `-DAJ_GENERATED_CONTRACT_HEADER=<生成头的绝对路径>`，增加生成 C++ 契约的编译及 CTest 门禁。编辑器主工程的 `audastra-japi-jass-tests` 接受多个显式 `.j` 文件路径，可同时检查本库和生成包装。

这些文件**不构成 DLL 授权**。安装者必须另行核验并在地图外的宿主信任配置中批准 DLL 摘要。生成器不修改地图、安装目录、白名单或协议实现。

manifest 是命令契约的单一输入：生成的 C++ `ModuleContract` 用来核对 DLL 的实际命令表，生成的 JASS 包装使用同样的名称、参数和返回类型。命令顺序也参与一致性检查。

## 地图作者示例

将 `AudastraJapi.j` 放在生成的 `demo.j` 及下列函数之前；两个文件只包含一次。编辑器集成方应通过现有脚本构建／导入流程安装这些源，不要把作者已有 GUI 触发器改写成代码。

```jass
function OnDemoMessage takes nothing returns nothing
    // LOCAL 通知只更新本地显示，不直接影响同步游戏状态。
    call DisplayTextToPlayer(GetLocalPlayer(), 0.0, 0.0, AjEventPayload)
endfunction

function InitializeDemo takes nothing returns nothing
    local integer listener
    local integer answer
    local boolean accepted
    if not AjInitialize() then
        // AjLastStatus 给出明确失败；没有宿主时不会假报可用。
        return
    endif
    if not AjModuleLoad("demo") then
        return
    endif
    set answer = AjCallInt("demo.add", AjArgInt(20) + AjArgInt(22))
    if AjLastStatus != 0 then
        return
    endif
    set listener = AjOn("demo", "message", function OnDemoMessage)
    if listener == 0 or not AjBindEvents() then
        return
    endif
    set accepted = AjCallVoid("demo.notify", AjArgText("hello; 中文 %"))
    // 这里只表示本地事件已入队。由宿主安全点 Pump() 派发；不是已经显示。
endfunction
```

值包装失败时返回 0／false／空串，必须立刻检查 `AjLastStatus`；这些默认值本身不代表成功。VOID 包装直接返回是否成功。每条响应自带状态，宿主不存在会被其他调用覆盖的全局“最后错误”。

## 关键约定

- `SYNC` 是**当前栈内通知完成**，不是网络同步，也不提供取消伤害／改写返回值的决策接口。只有读事件、所有回调执行、确认事件、执行器返回全部完成，emit 才返回成功。
- 同步事件重入明确返回 `AJ_REENTRANT`，不伪装成已完成并留到下一帧。LOCAL／ASYNC 永远排队；队列满拒绝新事件并计数，不淘汰已接收事件。
- 结果字符串写进宿主提供的固定缓冲区，禁止返回栈指针／共享静态字符串指针。
- `requestStop → pollStopped → destroy → FreeLibrary`：没有证明线程全部退出，就保持 DLL 与上下文。宿主销毁时仍未停止的模块被隔离保留到进程结束，不强制卸载。
- DLL 是全信任原生代码。摘要允许清单不是沙箱，C++ 异常捕获也不能隔离访问违例或进程破坏。
- 本 SDK 不提供任意游戏内存读写、任意 native 调用、网络同步、第三方通讯兼容、自动地图打包或现有 UI 对象 API。

## 数值与字符串

线上 real 为有限 binary32 的十进制表示，宿主不依赖系统区域设置，输出保留 9 位有效数字。C++ 编解码已做随机位模式往返测试。JASS 的 real 算术和字符串转换由原版 VM 决定；`AjArgReal` 使用归一化及 `R2SW` 避免普通 `R2S` 的低小数位问题，但**不宣称原版 VM 下逐位无损**。需要明确十进制值时用 `AjArgRealText("0.000000001")`；需要保留返回的原始十进制文本时读取 `AjLastText`。游戏适配验收必须另测极小值、最大有限值和中文字符串编码。

## 来源与发布边界

仓库只提供自有 SDK、运行库源码及接入说明，不附带 AI 引导文件、提供方交付目录或构建出的 DLL／BLP。黑盒引导器的来源、许可与公开分发仍须由项目作者确认；本次源码集成不表示编辑器保存时已经自动打包运行库。

此实现依据本项目的需求、类型／生命周期约束和公开 Windows 平台文档编写。不包含、不依赖第三方受限通讯层，也不提供其兼容／解码逻辑。这个源码说明不是对历史上所有研究活动的 clean-room 认证。未经游戏接入、原版 VM 和双客户端验收，不应将 SDK 的离线通过宣称为可商用游戏运行验收。
