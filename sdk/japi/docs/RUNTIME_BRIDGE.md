# 经典 1.27a 实验字符串桥

本桥把 JASS 的 `GetLocalizedString` 接到同一份 JAPI v2 Host。它是**默认关闭、尚未通过真实游戏验收**的基础实现，不是完整的模块运行环境。不会加载地图自带 DLL，也不会调用第三方通讯层。

本文描述启动器实验桥；[地图自带 runtime DLL](MAP_RUNTIME.md) 是独立部署路径。后者固定组合已有经典 1.27a 加载、握手、可见 UI 与同图重开一次的实机证据，不代表本文路径或完整跨图生命周期已经验收。

## 启用范围

独立 x86 runtime 的 CMake 选项 `AUD_JAPI_BRIDGE_EXPERIMENTAL` 默认为 `OFF`。只有专用实验构建开启该选项、且启动器使用既有 `OwnedLua` 自有测试模式，才给 native 观察器提供 JAPI 字符串处理器。关闭时不改派 `GetLocalizedString`；外部兼容运行时不接入本桥。

在仓库根目录配置一个**新的构建目录**，不要替换日常编辑器安装或常用游戏文件：

```powershell
cmake -S src/WarcraftRuntime32 -B build/japi-runtime-experiment -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=ON -DAUD_JAPI_BRIDGE_EXPERIMENTAL=ON -DAUD_LUA53_SOURCE_DIR=<已核验的Lua5.3.6源码目录> -DWAR3_RENDER_CORE_SOURCE_DIR=<本机锁定的渲染核心目录>
cmake --build build/japi-runtime-experiment --config Release --target audastra-japi-runtime-tests audastra-warcraft-bootstrap
ctest --test-dir build/japi-runtime-experiment -C Release -R '^audastra-japi-runtime$' --output-on-failure
```

这些命令只编译和运行原创离线 fixture，不启动 Warcraft、不部署地图。不同 Visual Studio 安装使用对应 generator。主工程不自动向子构建传入实验开关。

## 请求路径与当前能力

`JASS → 原有 native 注册观察器 → 有界前缀检查 → 当前 VM 会话 → JAPI Host → VM intern → JASS string ID`

1. 继续复用 `GameImage127` 的固定游戏映像 SHA、文件租约、映像/入口检查，拒绝未知版本或已经修改的观察入口。
2. 仅在实际登记 `GetLocalizedString`、签名严格为 `(S)S`、函数位于已核验游戏映像时替换登记地址。不是按名称扫描或猜测新函数地址。
3. 非 `ajapi:` 输入把**原字符串对象**交给原版函数，并原样返回其结果。包括 null、非 UTF-8 的普通本地化键以及旧协议前缀；这些不是新协议的兼容入口。
4. 自有前缀的输入最多复制 513 字节。512 字节上限之外直接由 SDK 回 `AJ_LIMIT`；不扫描超长字符串的剩余部分。错误版本/格式由 SDK 返回独立错误，绝不回退为普通本地化。
5. 首次自有请求只在完整、封存的 native 登记上下文中附着 `GameVm127`，在该游戏线程创建 Host。对象读失败、VM 身份变化、错线程或响应分配失败属于运行时故障：停止本次专用测试进程，不返回伪造的成功。

基础桥的 `host.hello` 能力位为 **3**：协议具有 typed call 和事件格式，但**没有** VM 执行器、经验证的同步执行器或加载器。事件格式位不等于能在游戏中派发回调。

| 操作 | 基础桥行为 |
|---|---|
| `host.hello/version/status` | 正常协议回复 |
| `module.count` | 1：运行时自带的界面模块 `audastraui`（见下节），没有其他隐式模块 |
| `module.load` | `audastraui` 返回 OK（已装入）；其他名称 `AJ_UNSUPPORTED`，地图摘要或 manifest 不授予信任 |
| `host.bind` / `AjBindEvents` | `AJ_UNSUPPORTED`；尚无 JASS 回入执行器 |
| `host.pump` | 空宿主正常完成；不是回调实际执行证明 |
| 同步事件、LOCAL/ASYNC 派发 | 此桥尚未开放；SDK 状态机不等于 VM 执行能力 |

游戏 VM 的整数、实数、布尔与文本参数仍在同一个字符串帧内，由 SDK 严格解析。没有另外开放内存、Hashtable 或任意 native 通道。

现有 Lua provider 保留原版 native 地址；Lua 直接调用 `jass.GetLocalizedString` **不会**走此 JASS 登记替换路径。JAPI 基础桥也不把 Lua 的 ready/mailbox 指标当作 JAPI 运行成功证明。当前仍沿用自有测试模式的启动前提，包括地图/邮箱身份与游戏类型声明核验；并非新增了无 Lua 配套的部署模式。

## 内置界面模块 audastraui

自有界面库（`src/WarcraftRuntime32/GameUi/`，调用 1.27a 自己的界面函数）作为标准 v2 模块内置在 bootstrap 里，不是单独的 DLL，也不经过加载器：

- 每次 Host 新建后，在同一游戏线程用 `Host::Install` 装入，契约、描述表、`start → requestStop → pollStopped → destroy` 生命周期与 DLL 模块同一套检查。装入失败按运行时故障处理。
- 入口用这次会话已核验的 `GameImage127` 解析；不完整时模块照样装入，`audastraui.ready` 为假，`audastraui.error` 给出原因。界面句柄带代数，VM 重建时随 Host 一起作废。
- 全部命令是 local 域。界面事件（进入、离开、点击）由模块按鼠标状态记录，地图侧计时器用 `audastraui.poll` / `audastraui.event` 取回，再运行触发器。这不依赖尚未接通的 JASS 回调执行器。事件只发生在操作的那台电脑上，影响游戏的动作须由地图自行同步。
- 失败时状态码只给大类；`audastraui.error(i:请求号)` 只回答模块自己记下的那次失败，宿主层面的失败返回空。

地图侧对应编辑器自带的 AudastraUI 触发器函数库。发布包里的 `AudastraUI.j` 由构建时把本目录的 `jass/AudastraJapi.j` 包成 vJass `library AudastraJapi`，拼在界面库前面，两者随同一个注入文件进地图脚本（见 `cmake/AudastraUiLibraryBundle.cmake`）。界面描述生成的自然 T 只调用 `AudastraUI*` 函数，不需要作者写代码。运行库测试 `audastra-game-ui` 逐条核对脚本里的模块调用与契约的命令名、参数类型和返回类型。

## 字符串所有权

输入为借用的 VM 字符串对象，而非 UTF-8 `char*`。普通输入不在桥内进行 UTF-8 校验。自有帧经 SDK 编码后，使用既有 `GameVm127::InternString` 复制到 VM 字符串存储，返回**带一个引用的非零 string ID**。

该引用转交原版 native 返回槽，桥不能马上 `ReleaseString`，也不能返回临时 `std::string::c_str()`。桥不另外缓存响应 ID；后续分配不会覆盖先前的 C++ 临时缓冲。此所有权实现复用现有 VM 适配器的 intern/native-return/release 约定；真实 JASS 消费后的引用平衡、长时内存行为仍必须单独测量，离线复制 fixture 不能证明游戏无泄漏。

## 地图、线程与失效

- 一个会话永久绑定首次请求的游戏线程。其它线程的普通本地化照旧；其它线程尝试自有协议时拒绝，不把请求搬到后台线程。
- epoch 取实际 native 登记代际，非零且不超过 `INT32_MAX`。不截断高位，不回绕；用完即拒绝。
- 同线程重新初始化 native，或销毁所属主 VM 前，关闭旧 Host 并使 VM lease 失效。其它线程/子 VM 销毁不误伤主会话。
- 新会话必须看到更大的登记代际；同地址、同旧代际不能复活。旧 epoch 请求返回 `AJ_STALE`。单个新 VM 内仍按 Host 的 request ID 规则防重复。
- 在附着、调用或响应分配期间销毁/重置 VM，会失效并终止测试，不能先拆宿主再继续执行悬空调用。
- Host 关闭仍遵循 SDK 停机合同。将来接入加载器时，未证明停止的模块会被隔离保活；这不等于已经卸载。本基础桥没有加载器，因而不会有此类模块实例。
- 原有 Lua 会话仍不允许带着已启动 Lua 跨图复用；JAPI 的离线重建测试不改变这一产品限制。

## 验收方法与剩余门禁

`audastra-japi-runtime` 使用原创 VM port、合成字符串对象和保护页，验证握手/错误、普通输入不解码、有界扫描、结果复制、线程、重入、主/子 VM 区分、重复通知、20 次伪 VM 重建和旧代际拒绝。它不加载真实 Game.dll，也不绕过映像 SHA 来制造“游戏通过”。

下一步真实验收仅限专用实例和原创地图/副本：

1. 记录实验 bootstrap、Game.dll、地图和脚本的摘要。界面模块另用编辑器“界面”工作区生成的自然 T 做原创测试图：确认 `AudastraUIIsReady` 为真、框架按描述出现、文字/数值更新、点击事件触发；关闭实验开关时同一张图照常运行、界面不出现。开启/关闭桥各跑同一普通本地化样例，对比原样转发、null 和非 UTF-8 键行为。
2. 在原创图中显式编入 `AudastraJapi.j`，随后编入 [StringBridgeSmoke.j](../sample/StringBridgeSmoke.j)，在每个新 VM 调用一次 `AjRunStringBridgeSmoke()` 并记录结果。返回 1–10 对应函数内该项失败，0 只代表基础检查完成。
3. 分别在 main、timer/trigger 等实际调用上下文检查返回 ID 的字符串表归属、多次分配后旧字符串仍可读、长时间调用后的内存/引用趋势、中文及 `% ; |` 编码、极端实数转换和畸形/超长帧。现有 VM 适配合同不能替代对子 VM 返回槽的真实验证。没有模块时，smoke 的中文检查仅证明参数编码经过解析后被正确的签名错误拒绝，不是中文结果回传验收。
4. 跑实际主 VM 进退与重建 20 次，记录登记代际、旧请求拒绝、故障路径和进程退出。没有新的登记代际就拒绝，不为通过测试放宽检查。
5. 单独实现地图外可信模块配置，再验证原创 DLL 调用。JASS 回入另做真实栈/等待/异常实验，证明以前保持禁用。最后才做双客户端一致性验收。

上述真实运行门禁目前未通过，不能将编译、离线测试或 JASS 静态检查视作替代。若需要新增部署权限或操作常用安装，应先取得作者明确决定。
