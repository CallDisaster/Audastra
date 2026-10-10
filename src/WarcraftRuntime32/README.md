# 独立经典版 Warcraft 运行时桥

这是独立 Win32 CMake 项目。主编辑器仍为 x64；桥只创建新的 x86 游戏进程，不连接已有游戏，不运行 YDWE.exe 或 LuaEngine，不执行原安装的 Lua 主脚本，也不读取注册表。

配置使用与主工程相同的已安装 Visual Studio 生成器，但必须独立指定 `-A Win32`，不能混用主工程 x64 cache：

```powershell
& $cmakePath -S src/WarcraftRuntime32 -B build/runtime32 -G 'Visual Studio 18 2026' -A Win32 -DBUILD_TESTING=ON
& $cmakePath --build build/runtime32 --config Release --target audastra-runtime-fixture-tests -- /m:1 /nr:false /p:CL_MPCount=1
& $ctestPath --test-dir build/runtime32 -C Release -j 1 --output-on-failure
```

统一构建负责人用 BelowNormal 启动以上步骤；子代理不另开编译。两项产品在该 cache 的 `Release/`：

- `audastra-warcraft-launcher.exe`
- `audastra-warcraft-bootstrap.dll`

CLI 固定合同：

```text
audastra-warcraft-launcher.exe --game <绝对war3.exe> --map <短Maps相对路径.w3x或.w3m> --runtime <绝对flat运行时目录> --bootstrap <绝对bootstrap.dll> --receipt <本次尚不存在的绝对JSON路径>
```

调用者必须先将桥的原进程放进可观察的 Job，桥才会创建游戏。新游戏不设置 BREAKAWAY、不新加 kill 策略，自动继承桥的 Job；原句柄归属成立后才注入并恢复。启动后初始化握手最多15秒：未观测Game.dll、插件未完成或bootstrap失败时仅结束本次新建游戏，并记录终态和具体失败阶段。初始化通过后，桥才进入正常的游戏全生命周期等待，成功时返回真实游戏退出码，未证实初始化时返回 `0xA001`，参数或回执失败为 64/65。游戏开始和持续帧不由本桥证明。

`--map`必须小于54个ASCII字符、是Maps下至少三级的短安全相对路径。磁盘副本已由上层通过独占创建及SHA回读准备；桥再次取得原文件只读租约。桥只创建JSON回执；不复制/删除地图，不改游戏或工具配置。

自有 bootstrap 在游戏主入口前将主EXE中存在的`LoadLibraryA/W`和`LoadLibraryExA/W`导入改为观察点，以实际加载模块名识别`Game.dll`，兼容相对/完整ANSI/Unicode路径；资源或不解析装载不当作游戏执行。核对其路径和SHA后，再按下列固定顺序加载运行时：`ydbase.dll`、`lua53.dll`、`ydwar3.dll`、`yd_loader.dll`、`yd_jass_api.dll`、`yd_lua_engine.dll`。后二者必须存在`Initialize`导出并无异常返回。同一初始化线程重入可以返回，其余线程必须等完整初始化或失败，最多15秒；不能在插件尚未完成时越过Game.dll装载边界。桥与bootstrap分别持有只读文件租约并比较预期/实际模块路径和SHA，发现DLL搜索shadow、缺依赖、导出或异常时只结束本次新游戏，不弹系统错误框。运行时目录还须具备这些DLL的真实x86 CRT依赖；不能使用主编辑器的x64 CRT充数。

匿名共享映射句柄是新游戏唯一继承的句柄，由`AUDASTRA_TEST_CHANNEL`传递。继承环境中的旧`AUDASTRA_TEST_*`和`ydwe-process-name`先清除；不修改父进程或机器环境。桥预先以`CREATE_NEW`独占JSON输出，在目标退出后写一次最终回执。schema为`audastra-warcraft-launch-v1`，包含原游戏PID/退出码、hook观察、Game.dll路径/SHA、6项模块路径/SHA/Initialize调用状态和失败阶段。`gameRuntimeVerified`固定为false，因为DLL导出无异常返回不能证明地图进图或API语义。

原创控制台fixture使用单独`fixtures/`产品目录。它们模拟在主入口加载`Game.dll`，后者确认早期hook和两项真实跨进程`Initialize`调用位。十例保留原八例的普通文件名、完整ANSI路径、中文目录完整Unicode路径、相对Unicode调用、缺运行时DLL、缺Initialize导出、未加载Game.dll直接退出及15秒握手超时，再增加ExA装载和双线程并发装载。并发例等待第一个插件正在Initialize，再让第二个线程通过ExW装载Game.dll；证据要求真实命中并发等待分支，两个Game进入点都看到全部初始化完成。它们不执行Warcraft、YDWE、GUI或GPU。每个成功完成断言的case先核自己新建Temp目录的范围、原Job完全终态及所有复制文件的创建SHA，再逐项删除自己生成的EXE/DLL/地图并移除空目录，只保留小JSON回执；不递归删除任何目录，失败现场保留。

Detours源代码及MIT许可证位于`third_party/detours`；原版插件二进制不进入源码库。本项目的bootstrap、协议、桥和fixture均为原创代码，未复制GPL启动实现。正式支持的Game.dll版本/运行时SHA档案与安装许可由上层工具链档案绑定；本桥不猜测未验证版本，也不负责原版补丁替换。
