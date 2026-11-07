# h-lua 工程构建与测试

h-lua 为 AGPL-3.0，由用户提供。Audastra 不附带框架、SDK 默认表或系统物体，也不执行 SDK、外部转换器或它们的启动脚本。构建期只在有预算的 Lua 沙箱内加载用户提供的构建文件。地图中使用这些文件的分发义务由地图作者按其许可证处理。

多 index 字段应使用 metadata 给出的带下标键。Audastra 另有一条兼容策略：未带下标的单值只有在每个下标的实际有效值（包括父链和本物体显式 `_N` 字段）都与它完全相同时，才作为冗余声明忽略，并逐处警告。不会选一个下标或广播。数组、未知父值、任一下标不同仍拒绝，并列出带下标键名。这是 Audastra 的策略，不是外部转换器规则。构建回执包含 `redundant_indexed_fields`；`hlua-ids` 提供 `--game` 和 `--workspace` 时同时做字段验证并列出同一证据，否则 `field_mapping_checked=false`，仅证明 ID 分配与回放。

<a id="profile-text-default-evidence"></a>

### 缺省文本字段的原版核对

兼容记录 `we127a-profile-text-empty-v1`：经典 1.27a（Game.dll 1.27.0.52240）的原版专用实例中，标准 Hpal 的攻击 1/攻击 2 投射物图像均显示“没有”，原始数据显示两处 Missileart 为空。其完整 HumanUnitFunc profile 节没有 Missileart 键。从 Hpal 派生 H000，在原版模型对话框选择自定义并提交空路径，再正常保存；生成的 w3u 是版本 2、24 字节，只有 `H000 ← Hpal`，修改项为 0，没有 ua2m 覆盖。原版编辑器 SHA-256 为 `811130e637b4885c08c3c8cb707d0e02a7692ddcddc4c1d57c783c3379ef4b90`；w3u SHA-256 为 `d305cf68ba5ce16d7d79c78873daf770732ad9ccbd556a8dc167def2d1481a71`。这份说明给出可复现实验和产物身份；游戏数据、截图及地图不随编辑器分发。

据此，只有标准父物体的 profile 节和全部 SLK 行已完整、严格读入且无重复/歧义，该 profile 又没有该键时，才把非列表、非引用的文本字段缺省值视为空串。数字和布尔不适用；profile 键已存在但缺少某个分量时也不适用。读取失败、缺文件、缺节、未解释行和自定义父物体不获此资格。每处诊断注明“缺省按空串”，回执以 `defaulted_text_empty`、`default_evidence` 关联本核对记录。未满足条件时继续拒绝。

## 命令行

以下路径均为示例。工作目录必须尚不存在；输出默认也必须不存在。只有显式传入 `--replace-sha256 <原目标SHA-256>` 时才允许替换已有输出，使用与地图保存相同的备份、回读与原子写回。目标身份不同就拒绝。替换路径暂不支持匿名归档或含引导成员的候选，这类地图须另存到新路径以保留原字节透传。

```powershell
war3-map-cli hlua-inspect D:/Projects/MyMap --framework D:/Frameworks/h-lua
war3-map-cli hlua-ids D:/Projects/MyMap --framework D:/Frameworks/h-lua --offset 0 --limit 128
war3-map-cli hlua-build D:/Projects/MyMap --framework D:/Frameworks/h-lua `
  --game D:/Games/War3 --workspace D:/Builds/MyMap-test `
  --output D:/Builds/MyMap-test.w3x --target test
```

框架目录可以替换成一对 `--runtime-framework <运行期目录> --build-framework <构建期目录>`；不能与 `--framework` 同传。框架缺文件、链接、无效 UTF-8、Lua 语法和模块重名都有明确拒绝。静态 require 使用与 Lua 编辑服务相同的词法扫描器，动态 require 仍须游戏验证。

`hlua-inspect --game <游戏目录>` 还会把用户 engine 中的字面 native 名称逐项对实际 common.j 声明核对并记录 SHA。预计的 japi 名称按字面访问与框架分发调用查询，使用相同 offset/limit 分页；动态名称始终保留“不完整”的声明。构建与自带环境的数字 tostring 共用最短往返规则，整数不附加 `.0`；构建期数值字段中的整数超出 double 的精确整数范围时拒绝，避免 Lua 5.1/5.3 差异被静默舍入。

默认底图来自工程的 `map/w3x`、`map/slk`、`map/resource`。也可以用 `--base D:/Maps/Base.w3x`。已有地图沿归档保存服务透传未知资源；目录构建在打开前拒绝引导成员。不通过 Lua、脚本预处理或模块加载器解释引导内容。

`hlua-new <新目录> --framework <目录> --tool <已安装工具数据根> --game <游戏目录> [--ui-root <定义目录>]` 使用 Audastra 自己的地图创建服务生成 `base.w3x` 和原创 `main.lua`。后续构建显式传 `--base <新目录>/base.w3x`。

`test` 与 `build` 都写原生物编；`dist` 使用共用 SLK 优化器及其语义核验。所有目标均把框架和作者 Lua 写进地图，不设置本机 `package.path`。改变 hslk 后要重新构建；普通 Lua 的测试热重载沿原测试邮箱。

```powershell
war3-map-cli hlua-test D:/Projects/MyMap --framework D:/Frameworks/h-lua `
  --game D:/Games/War3 --workspace D:/Builds/MyMap-run `
  --output D:/Builds/MyMap-run.w3x --helpers $AudastraInstall `
  --engine owned -- --cloaked --press-space --seconds 90 `
  --screenshot-after 60 D:/Builds/MyMap-shot.tga --close-after 80
```

`$AudastraInstall` 为已安装 Audastra 的目录。分隔符后的参数采用 `map-test` 的现有合同，包括计划输入、截图、关闭、热重载。只操作本次测试持有的进程。`--engine ydwe` 使用用户已安装的数据根，并须在分隔符后给 `--ydwe <目录>`。所有 h-lua 测试先扫描地图中全部 Lua 的 `io`、`os.execute`、`package.loadlib` 访问，发现则拒绝；回执记文件数、地图身份和请求环境。这样已有地图因扩展 JASS 接口选择外部环境时也不能绕过扫描。此检查不证明动态代码完整性，环境选择与回退原因另列于测试信息。

## 编辑器中的 h-lua 工程

关联外部文件夹后，根目录同时有 `main.lua` 和 `hslk/` 或 `map/w3x` 就识别为 h-lua 工程。在应用设置的地图测试页选择“h-lua 框架目录”；也可在工程工具条的“工程框架…”中覆盖。覆盖写在工程目录 `.audastra-hlua.json`，使用比较配置SHA的写入；损坏配置不会被忽略。CLI/MCP可省略框架参数使用同一工程配置；显式请求的框架优先于工程配置，工程配置优先于应用默认。

工具条提供检查、构建测试图、测试、构建、发布版和名称与 ID 表。检查执行同一字段映射和 ID 回放，但不发布地图；测试或 F9 先构建 test 再运行现有地图测试。测试信息说明实际环境及选择原因，自带环境通过测试邮箱转发带来源的打印和异常。第一次保存先选择输出，之后写回该输出；构建不会默认覆盖工程底图。正常保存保留外部文件夹关联，test/dist产物不保留编辑器关联。

test 带测试期控制台，build 写原生物编，dist 使用同一物编优化服务输出 SLK。修改作者 `scripts/` 或 `main.lua` 后，热重载只发送工程扫描器确认的作者模块；不发送框架、hslk 或地图资源。`hslk/` 或框架身份改变时要求重新构建，不发送热重载。删除模块仍须重新测试以清除旧模块。

自带环境尚未实现 DzAPI、EX 系列，以及生命周期未完整核对的 texttag、weathereffect；调用会明确拒绝，不能以任意整数冒充句柄。lightning 支持本方创建、借用已登记别名和显式销毁，范围见下节。未知 japi 名称查询返回 nil 并记诊断，依赖它们的伤害、属性、同步和帧界面不能据此声称可用。AGPL 框架由用户自行提供，编辑器不分发、不复制框架实现或默认数据。

### 非 agent 句柄的覆盖范围

原创 `native-handles` 探针在 Game.dll 1.27a（SHA-256 `e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a`）实际运行，地图 SHA-256 `06270f863e4ddbdcdd3a81bf7730054e5c5fe3d6d4d996c8eeb0b7821e5f2440`。通过原生 JASS 分配并记录编号，不使用第三方通讯或读取游戏内存。第一个结果文件 SHA-256 `45b1ddeceab3d9e9282ff9b6ad1c925690bdfc56a262dc146be3f6f88f8de591`；游戏自然退出 0。这些标识供复现实验核对，不随产品分发游戏资料或地图。

- lightning：可见性检查开启且端点在未探索区时返回 0、DestroyLightning 为 false；关闭可见性检查后两次同时创建为 1、2，销毁返回 true，随后复用 1，旧变量的数字仍是 1。桥使用独立池生命期，不做 agent 引用计数。销毁使本方所有别名失效；再次观察到同一编号的创建会更换生命期，旧 userdata 不会复活。VM 代际变化时废弃全部旧编号。最多记录 4096 个存活对象，超额在 native 创建前拒绝。Lua GC 不等于游戏 DestroyLightning，作者仍应显式销毁。
- texttag：100 次分配依次为 99 至 0，容量耗尽也返回 0；销毁 59 后复用了 59。0 既可能是有效池编号，也可能是失败返回，不能直接按 nil 或 agent 句柄处理。探针还覆盖了非永久和到期后分配，但仅凭返回数字不足以完整检测自动失效，因此仍在调用 native 前拒绝。
- weathereffect：两次同时创建为 0、1，移除后复用 0。有效 0 与失败值、隐式销毁边界尚未完整核对，仍拒绝。

lightning 的本地账本只能观察经过本方桥的创建/销毁。其它 JASS 或扩展代码在桥外销毁、复用同一编号时，不能保证立即识别；未知外部编号不会自动接管。不要把这项覆盖范围当作跨任意扩展库的全局对象跟踪。VM 已失效时不会跨代调用 native 清理。对不在覆盖范围内的类型，保留明确拒绝。

## 数据与诊断

字段名在该类型完整 metadata 中完全不存在时，Audastra 不把它写入原生物编，逐处优先警告“该类型没有这个字段”，并在回执 `unwritten_fields`（未写入的字段）中记录文件/行、物体、字段及其它具有此字段的类型。该节支持 offset/limit 分页。已知字段的大小写变体、带下标成员、值类型错误、布尔或嵌套表不属于此策略；metadata 读取不完整也不能据此跳过。此策略不改写作者框架资料。

构建冻结框架/工程身份，分配四字节 ID，在独立运行期沙箱领取 ID，数量或顺序不一致就拒绝。回执记录框架、工程、底图和输出 SHA-256、生成名称与 ID、ID 自检、归档回读、峰值 Lua 内存、指令数和差异。名称与诊断有分页/长度预算；回执的 `game_verified=false` 和 `original_editor_verified=false` 不由出图成功改成 true。

物编字段从实际游戏 metadata 映射：例如 `Buttonpos_1/2` 是两个分量、`DataA` 带数据字母、`Cool` 数组对应等级。不能映射或地址歧义时拒绝。字符串、引号、换行和数组经过原领域文档/事务；实数直接写原生 float32，不经过 SDK 的两位小数格式化。显式空文本数值按 LNI 约定为零，其它非法数值、溢出、小数整数、布尔值、嵌套表均拒绝。安全路径的孤立 imp 记录原样保留并警告，没有资源就不造资源。未支持的 LNI 文件或结构不丢弃后继续出图。

目录路径或成员名受现有资源服务的支持范围约束；不能表示的成员名明确拒绝。用户扩展目录中的 Lua 也打包，保留 SDK 曾遗漏的作者模块。

## 自带 Lua 环境

当前目标为经典 1.27a。`jass.common` 从实际游戏声明和当前 VM 查找，不能把静态名字存在视为调用已验证。测试预检列出 Lua 的 `jass.*` 依赖和所选引擎原因。

| 接口 | 行为与边界 |
|---|---|
| `jass.slk` | 本次测试图有效物编的冻结只读视图，七类对象及 misc；WE 字段名，值为字符串，缺失为 nil；不能 rawset 或热重载资料成员 |
| `jass.japi` | 未实现名字返回 nil，并记录一次缺失诊断；没有 Dz/EX 伤害、属性、同步、帧界面能力 |
| `jass.globals` | 当前 VM 的有类型标量读取/写入；常量、数组、code、失效代际及未声明类型拒绝 |
| `jass.debug` | `handlemax()` 返回句柄表槽数；`handledef()` 返回 reference 和可确认的 type；低于句柄范围或越界返回 nil，范围内空槽仅有 reference 字段 |
| `jass.runtime` | console 控制测试输出；debugger 端口设置识别后明确报告未实现，不监听网络；原 sleep/handle_level 限制仍生效 |
| `jass.console.write` | 参数经 tostring 转为 UTF-8 文本，制表分隔并换行，进入本次测试回执/问题列表；输出有界 |
| `collectgarbage` | 只支持 collect 和 count（KiB） |
| `os` | 只支持 date、time、clock；date 格式/结果有界，年份 1970–3000；没有文件、环境或进程接口 |
| `debug.traceback` | 非文本且非 nil 参数原样返回；不提供线程、注册表或任意内存访问 |

外部调试器、未实现的 jass 模块和框架对缺失能力的具体反应，应在测试信息里查看。打包成功不代表这些功能可用。控制台输出、运行时错误、截图和物体状态才是进入作者 `main()` 的证据。

原生参数保持类型检查：boolean 参数需要 boolean，整数不能含小数，字符串必须为有界 UTF-8，句柄必须属于当前有效 VM。条件/过滤器回调返回 nil 时按 false 处理，其它非 boolean 返回值拒绝；等待类、文件访问和地图切换 native 仍未开放。这不是对外部引擎全部转换行为的模拟。

入口模块的冷启动时间预算为 10000 毫秒，指令预算仍为 200 万，内存仍为 64 MiB；普通回调与热重载保持 250 毫秒。入口回执的 `LUA_ENTRY_STATS` 记录实测耗时、指令数、峰值内存、读取字节及模块数。首次预读模型/技能的同步游戏调用本身无法被 Lua 指令 hook 抢占，每次 native 前后仍检查预算；这不是硬实时中断保证。

## MCP 与共用服务

`hlua_inspect`、`hlua_ids`、`hlua_build`、`hlua_new`、`hlua_test` 调用无 Qt 的 `War3Map::HLua` 服务。工具接受明确工程/框架/底图/游戏/输出路径，不接受 shell 命令或任意 PID。ID 输出默认 128 项，可用 offset/limit。测试返回本工具创建的 session_id；`hlua_test_result`、`hlua_test_stop` 要求该身份，只查询或结束本工具的测试租约。`hlua_test_continue` 用同一身份向测试窗口发送一次开局空格；窗口未就绪或输入未确认时返回 `input_sent=false`，客户端可以稍后重试。它不抢焦点，也不移动真实鼠标。其余计划输入/截图使用 CLI 的 `map-test` 合同。普通编辑器地图测试与 CLI 经过同一物编资料冻结和测试准备服务。

## 格式依据

仅核对公开格式事实，不复制第三方实现：

- [w3x2lni 字段命名](https://github.com/sumneko/w3x2lni/blob/master/script/prebuilt/metadata.lua)；其当前 [许可证为 GPL-3.0](https://github.com/sumneko/w3x2lni/blob/master/LICENSE.txt)。
- [原生字段输出](https://github.com/sumneko/w3x2lni/blob/master/script/core/slk/backend_obj.lua) 与 [空数字处理](https://github.com/actboy168/w3xparser/blob/master/src/main.cpp)；w3xparser 为 Apache-2.0。仅实现空文本为零，不继承错误文本静默转零、溢出回零或整数字段截断。
- [jass2lua 接口语义](https://github.com/actboy168/jass2lua/blob/master/lua-engine.md) 与 [Lua 5.3 手册](https://www.lua.org/manual/5.3/manual.html)。
- YDWE 公开源码中的 [控制台参数转换与分隔](https://github.com/actboy168/YDWE/blob/master/Development/Plugin/Warcraft3/yd_lua_engine/lua_engine/libs_console.cpp)、[回调结果转换](https://github.com/actboy168/YDWE/blob/master/Development/Plugin/Warcraft3/yd_lua_engine/lua_engine/callback.cpp) 和 [boolean 的 nil 语义](https://github.com/actboy168/YDWE/blob/master/Development/Plugin/Warcraft3/yd_lua_engine/lua_engine/jassbind.cpp)。仅取接口事实，没有复制 GPL 实现。
