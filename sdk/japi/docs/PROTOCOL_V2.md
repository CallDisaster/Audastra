# Audastra JAPI v2 协议规范

规范版本：2.0.0。ABI：`0x00020000`。状态：已有实现与离线一致性测试；固定地图 runtime 组合在经典 1.27a 的加载、握手、可见 UI 与同图重开一次已有实机证据，完整 VM／跨图／联机资格未完成，详见[部署边界](MAP_RUNTIME.md)。本文规定自有协议，不依赖旧 WarVK 模块原型的隐含行为。

## 1. 目标与边界

目标是地图脚本调用受信任 C/C++ 模块并接收有界通知；协议层不操作游戏地址，不执行脚本字符串，不安装 native，不进行网络传输。模块加载、命令语义与游戏 VM 桥接分层。

所有请求与响应走一个**字符串载体**，集成接缝为 `GetLocalizedString`。避免同一个返回整数既表示业务值又表示错误、以及 VOID／INT／TEXT 载体路由错配后已产生副作用。宿主 API 的 `stringCarrier=false` 对自有帧返回 `AJ_CARRIER`，不调用模块。

前缀所有权为 `ajapi:`：不属于此前缀的输入完全透传给原 native；以此前缀开头但版本／语法不合法的输入必须消费并返回错误，不能落入其他系统。旧 `warvk:v1` 不属于本协议。

## 2. 请求和响应

```text
请求：ajapi:2;<epoch>;<request>;<command>[;<type>:<value>...]
响应：ajapi:2;<epoch>;<request>;<status>;<type>:<value>

ajapi:2;0;1;host.hello
ajapi:2;1;1;0;i:3

ajapi:2;1;2;demo.add;i:20;i:22
ajapi:2;1;2;0;i:42

ajapi:2;1;3;demo.echo;s:hello%3Bworld%25
ajapi:2;1;3;0;s:hello%3Bworld%25

ajapi:2;1;4;unknown.call
ajapi:2;1;4;6;v:
```

两方向都不超过 512 字节，不含底层 NUL 终止符。所有数字字段和结构字符均为 ASCII；文本为 UTF-8。解析不修剪空白、不忽略后缀、不做模糊匹配，不截断后继续执行。

`epoch` 与 `request` 为不带符号、无多余前导零的十进制数，上限 `2147483647`。普通请求 epoch 必须与宿主当前地图／VM 身份一致。只有 `host.hello` 使用 epoch=0；它不重置队列、请求计数器或模块。

request 必须大于当前会话已接受的最大请求编号，不能为零。合法帧通过线程、载体、宿主状态及 epoch 检查后，在执行前占用 request；即使命令失败，也不能重放该编号。嵌套调用先取得更高编号；外层返回不回退编号。语法／版本／epoch 等前置拒绝不占用编号。

编号不能循环使用，耗尽返回限制错误，需由集成方建立新的 VM 会话。宿主 `Reset` 接受新 epoch 后重置 request 水位，但加载实例和事件编号仍不复用。新建 Host 时，嵌入者也须提供整个进程内不与旧 VM 混淆的 epoch，不能每次都硬编码为 1。

epoch 和 request 防止误用与陈旧重放，**不是凭据、加密或网络认证**。本协议不承诺跨崩溃 exactly-once；响应丢失后不能用新编号盲目重试有副作用命令。

无法从坏帧读到 request 时，错误响应 request=0；客户端必须拒绝与自身请求不匹配的响应。非零 status 的返回值永远是 `v:`，不得夹带看似成功的业务值。

## 3. 类型与编码

| 类型 | 字符 | 规则 |
|---|---|---|
| VOID | `v` | 仅响应；值为空 |
| int32 | `i` | `-?[0-9]+`，完整 int32 范围 |
| handle | `d` | 同 int32；负数有效；不自动解释为地址 |
| bool | `b` | 恰为 `0` 或 `1` |
| real | `r` | `-?(digits(.digits*)?|.digits)([eE][+-]?digits)?`；有限 binary32 |
| text | `s` | UTF-8；百分号解码一次 |

整数／real 不接受前导 `+`、空白、hex、下划线、NaN、Infinity 或尾随内容。real 解析上溢／下溢报告参数错误；输出使用与 locale 无关的 9 位有效数字。

文本编码器把 `%` 编为 `%25`、`;` 编为 `%3B`，其他合法 UTF-8 字节不变。宿主请求解码器接受合法 `%HH`（hex 大小写均可）；解码结果拒绝 NUL、C0 控制字节、DEL、不完整 UTF-8、过长编码、代理码点及超出 Unicode 范围的码点。响应编码器只产生上述两种规范转义，因此 JASS 客户端无需任意字节构造函数。

参数最多 16 个；每个文本参数**编码后**不超过 384 字节，整帧仍须不超过 512。结果文本编码后不超过 416 字节。长度上限包含所有原始 UTF-8 字节；不按可视字符或 UTF-16 字符数计数。超长返回错误，不截断 UTF-8、不静默删字符。

调用模块前，参数数量／类型必须逐项匹配命令表。返回类型必须匹配，bool 仍须 0／1，real 必须有限。宿主不对 handle 做游戏对象有效性保证；具体能力层负责实例生命周期与种类校验。

## 4. 名称与命令表

模块名：`[a-z][a-z0-9_-]{0,31}`，`host`／`module` 保留。命令为点分段的小写名称，各段必须以字母开头，仅含字母／数字／下划线／连字符，总长不超过 63。模块命令必须位于自身 `<module>.` 前缀中。版本串为 1..63 个 `[A-Za-z0-9._+-]` 字符。

每模块最多 256 条命令，必须唯一。manifest 生成的 `ModuleContract` 与 DLL 的名称、版本、命令顺序、参数、返回类型、domain **完全一致**才可启动。未知字段、重复 JSON key 和重复命令在生成阶段被拒绝。

domain=0（local）或 1（sync）。LOCAL／ASYNC 事件处理期间，以及 local 命令的嵌套调用链中，宿主拒绝调用 sync 命令。domain 只是额外的误用防护；无法识别作者在 `GetLocalPlayer` 分支中调用、非确定性 DLL 内部行为或游戏本身的同步顺序。

## 5. 控制命令

所有控制命令均为小写，不提供驼峰别名。失败见响应 status；成功的 VOID 返回 `v:`。

| 命令 | 参数 | 返回 | 语义 |
|---|---|---|---|
| `host.hello` | 无，epoch=0 | INT | 当前能力位，响应头提供 epoch；不改变会话 |
| `host.version` | 无 | TEXT | `2.0.0` |
| `host.bind` | `s:function` | VOID | 注册 dispatcher 并做事件零握手；同名幂等，异名拒绝 |
| `host.pump` | 无 | VOID | 显式安全点；有未派完事件／未停止模块时返回 BUSY |
| `host.event` | 无 | TEXT | 仅 dispatcher 内可读一次当前事件；第二次返回空串 |
| `host.ack` | `i:eventId` | VOID | 已读、编号相符且尚未确认的当前事件；重复确认拒绝 |
| `host.status` | `i:status` | TEXT | 状态诊断名称；未知值返回 `unknown status` |
| `module.load` | `s:name` | VOID | 受信任加载器；active 同名幂等，stopping 同名 BUSY |
| `module.unload` | `s:name` | VOID | 请求停止；仅实际卸载完成才 OK；仍在停止时 BUSY |
| `module.count` | 无 | INT | 注册表实例数，**含 stopping** |
| `module.info` | `s:name` | TEXT | `name|version|instance|state|commandCount` |
| `module.commandcount` | `s:name` | INT | 命令数 |
| `module.commandat` | `s:name;i:index` | TEXT | `name|argumentTypes|resultTypeChar|domain`，索引从零开始 |
| `module.stats` | `s:name` | TEXT | `instance|state|queued|rejected|delivered|failed|cancelled` |

能力位：1=基本调用，2=事件队列，4=已有 VM 执行器，8=执行器已获同步资格，16=已配置模块加载器。4／8 反映适配器配置，不等于 dispatcher 已通过握手。发送同步事件还要求绑定成功。只有字符串调用能力也可以使用基本模块命令。

没有 `lastError`：每条响应携带自身状态；C++ `Diagnostic()` 是宿主线程上的补充日志文本，不是 JASS 的错误关联机制。没有地图可调用的 VM reset；地图不能自行更换宿主 epoch。

## 6. 事件

三类事件为 SYNC=0、LOCAL=1、ASYNC=2。事件类型是最长 31 字节的合法小写名称或点分段名称。payload UTF-8 及控制字符规则与文本相同，编码后最多 256 字节。

`host.event` 返回一段普通 TEXT，其解码内容为：

```text
eventId|instance|class|module|type|payload
```

只分割前五个 `|`；payload 可含 `|`、`;`、`%` 和空串。epoch 在外层响应头；实例 ID 区分同名模块的不同加载期。内层字段加上最大 payload 仍在结果预算内。

### 6.1 绑定和握手

dispatcher 名须匹配 `[A-Za-z_][A-Za-z0-9_]{0,62}`，签名 `takes nothing returns nothing`。宿主设置临时绑定，调用它一次，提供事件 `0|0|1|host|ready|`。客户端必须读取并确认 0，但不能调用作者监听器。

宿主只有在执行器返回 true、观察到 `host.event` 读取及匹配的 `host.ack` 后才保留绑定。函数不存在、被优化器改名、没有确认、执行器失败或抛 C++ 异常均清除绑定并返回 DISPATCH。`AjDispatch` 名称必须在优化器中保留。

### 6.2 SYNC 的成功条件

1. 发出者在绑定的游戏线程，模块 active 且 epoch 有效。
2. 适配器已明确提供同步资格，dispatcher 握手通过。
3. 当前不在事件派发中，也不在 local 命令调用链内。
4. 直接向 dispatcher 提供**这一个事件**；不进入异步队列。
5. 读取事件 → 作者回调全部返回 → 确认匹配 ID → 执行器返回；全部完成才返回 OK。

同步事件重入返回 REENTRANT，错误线程返回 THREAD，能力缺失返回 UNSUPPORTED。没有“先返回成功、下帧再处理”的降级。异步 FIFO 满不挤掉或阻挡本条同步通知。

SYNC 不做客户端间广播，也不提供当前事件的业务决策返回值。不能用它替代游戏已有同步命令或推导任意事件可取消。失败可能发生在某些作者回调已经产生副作用之后；宿主不能回滚，不能自动重发。

### 6.3 LOCAL／ASYNC 和预算

任何线程都可以发布 LOCAL／ASYNC；无论是否游戏线程，均先进入共享 FIFO。OK 只表示已复制、接受入队，不表示 JASS 已处理。

默认全局队列容量 256，满时拒绝**新事件**并返回 BUSY、增加该实例 rejected；保留所有已接受事件的顺序。停止／reset 撤销实例的排队事件并计入 cancelled。同步通知不占此 FIFO。

只有游戏安全点 `Pump()` 或地图显式 `host.pump` 才派发。默认每轮最多 64 条／64 次执行器调用。预算耗尽且仍有工作返回 BUSY，保留待处理队列。派发失败记录 failed、关闭绑定、移除当前已尝试的事件，不自动重试；尚未尝试事件仍留在队列中。

作者必须禁止 Sleep、等待、yield、阻塞 I/O；一次执行器调用必须真实代表回调返回。JASS 指令预算中断、异常或挂起属于集成方必须识别的失败。2ms 是作者预算建议，不是宿主可以抢占原生代码的承诺。

### 6.4 JASS 监听器

`AjOn(moduleFilter,typeFilter,code)` 返回正整数监听 ID，0 为失败；模块及事件支持精确匹配或 `*`。最多 64 个活动监听器，按注册顺序执行，精确／通配监听器共用该顺序。每次注册都是独立监听器，不根据 code 指针猜测是否重复。

`AjOff(id)` 注销一个监听器。ID 不复用；删除后保持其他监听器顺序。派发期间不允许增删监听器。事件全局仅在回调期间有效，返回后清空。事件零不分发给作者。

## 7. DLL ABI 与生命周期

唯一导出为 `AudastraJapiInitV2`，统一 C linkage、cdecl、pack(8)。`AjHost`／`AjModule` 的 structSize 和 ABI 必须精确匹配；v2.0 不承诺通过偷偷追加字段兼容不同结构。所有参数／结果布局见公共 C 头；x86 与 x64 指针布局不同，不能混载。

Init 只提供静态描述表，不能创建线程、发布事件或留下需宿主清理的资源；失败由模块自行清理。`DllMain` 同样不能做工作线程／重入加载等实质初始化。宿主先核对完整契约，才调用 `start` 分配资源／启动工作。

`AjHost*` 在此加载实例的完整生命周期内有效。`emit` 的 context 不可转交其他模块。参数字符串仅在 invoke 内有效；需要保留必须自行复制。`AjResult` 是宿主的固定存储，模块用 `AjSetText` 在自己局部对象仍活着时复制；`textSize` 是权威长度，不返回指针，不跨 DLL 释放内存。

```text
静态 Init/核对 → Starting/start → Active
                                  ↓ requestStop（一次）
                                Stopping
                                  ↓ pollStopped == 1
                                destroy → FreeLibrary → Unloaded
```

start 失败也经过停止／确认／销毁流程。Starting 不接受事件，worker 必须处理 early emit 的 STALE；示例 worker 在 active 后由命令显式启动业务。

停止期间拒绝新调用、新事件及同名重新加载。requestStop 不等待；pollStopped 只有在所有工作线程**实际退出并完成 join／句柄确认**后才能返回 1；0 表示忙。返回其他值、抛异常、destroy 失败均保留实例，不重复 destroy，不强制 FreeLibrary。

同一模块 invoke 不可重入；跨模块允许，最大嵌套深度 8。事件回调或 invoke 内请求 load／unload／reset 均拒绝 BUSY；作者应在返回后的安全点操作。所有 Init、start、生命周期、invoke 和 VM 执行均在 Host 创建线程上运行，嵌入者必须保证这就是核验过的游戏线程。

`Reset(newEpoch)` 为 C++ 集成 API，返回 `{status, applied, stoppingModules}`。无法在当前栈重置时 applied=false。接受新代际后返回 OK/applied=true，即使旧模块仍在异步停止；stoppingModules 明确报告数量。绑定与请求水位清空，旧上下文失效，新地图需重新握手。不得把“新 epoch 已生效”误读成“所有旧 DLL 已卸载”。

`Close()` 只有全部卸载才返回 OK，否则 BUSY。宿主析构时仍未证明安全停止的实例进入进程生命周期隔离保留区；模块代码、服务上下文和暂存文件均保留，后续 emit 返回 STALE。必须诊断并在安全时重启进程，不以超时后强卸载解决。此兜底有意占用资源，不是正常退出路径。

## 8. 安全策略

`PinnedDllLoader` 只接受由受信任安装者传入的模块名／命令契约／SHA-256 pin。默认没有 pin；不从地图读取允许清单，不提供开发模式绕过。只读指定可信目录下 `<name>.dll`，拒绝路径注入、文件级重解析点、空文件和超过 64 MiB 的文件。

加载前核验固定摘要、PE32/i386/DLL、直接的可执行 V2 导出，拒绝 forwarded export。逐次创建随机名称、当前用户和 SYSTEM 可访问的私有目录，CREATE_NEW 写普通 DLL，持有禁止写／删除共享的文件句柄。使用完整绝对路径及 `LOAD_LIBRARY_SEARCH_SYSTEM32`；不改进程全局搜索目录。仅支持自包含模块加系统依赖，不提供相邻依赖 DLL 打包。

可信来源目录／暂存父目录本身必须由可信安装者管理。用户或同进程恶意原生代码拥有同等权限时，本 SDK 不是隔离边界。摘要固定不证明来源合法、不替代作者授权／签名审核；预检也不把恶意 DLL 变安全。DllMain 在 LoadLibrary 中已经执行，所以摘要授权必须先于加载，描述表核对不是授权替代品。

正常卸载只删除本实例创建的确定 DLL 和目录，不递归清理父目录。创建私有目录失败必须报告错误；配置一个明确可写的受信任暂存父目录，不降级 ACL。

## 9. 状态码

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | OK | 本操作按其合同完成；异步 emit 的合同仅为入队 |
| 1 | FRAME | 帧／字段格式错误 |
| 2 | VERSION | 版本／ABI／目标架构不兼容 |
| 3 | LIMIT | 长度、数量、编号或模块自身范围限制 |
| 4 | ENCODING | UTF-8 或百分号转义非法 |
| 5 | ARGUMENT | 参数值、数量、类型或名称错误 |
| 6 | UNKNOWN | 命令／模块不存在 |
| 7 | CARRIER | 非字符串载体 |
| 8 | STATE | 当前生命周期／domain／派发状态不允许 |
| 9 | THREAD | 不在所属游戏线程 |
| 10 | BUSY | 队列满、还有待处理工作或不能在当前调用栈变更生命周期 |
| 11 | REENTRANT | 同模块重入／嵌套同步事件／深度超限 |
| 12 | UNSUPPORTED | 未配置所需宿主能力 |
| 13 | BACKEND | 模块契约／返回类型／返回值错误 |
| 14 | POLICY | 摘要授权、文件或加载策略拒绝 |
| 15 | STALE | 旧 epoch／旧实例／重复 request |
| 16 | DISPATCH | 回调读取／确认／返回链未完成 |
| 17 | INTERNAL | C++ 异常或内部失败 |

只捕获正常 C++ 异常；不尝试把访问违例转换成“安全继续”。如果模块破坏进程，必须按崩溃处理，而不是信任状态码。

## 10. 固定上限

| 项 | 默认／硬限制 |
|---|---|
| 请求／响应帧 | 512 bytes |
| 参数／参数文本 | 16／384 encoded bytes |
| 输出文本／事件 payload | 416／256 encoded bytes |
| 模块／命令／事件类型／dispatcher 名 | 32／63／31／63 ASCII bytes |
| 命令数／模块 | 256 |
| 活动及 stopping 模块 | 默认 32，可配置 1..256 |
| 延迟事件队列 | 默认 256，可配置 1..4096 |
| 单轮派发预算 | 默认 64，可配置 1..256 |
| invoke 嵌套深度 | 8；同一模块不得重入 |
| JASS 活动监听器 | 64 |
| DLL／manifest | 64 MiB／256 KiB |

## 11. 兼容性规则

v2.0 的帧、状态、类型及 ABI 布局是实现合同；不声称已经过所有游戏场景验收。修复违反本合同的实现可以保留版本；改变帧解释、ABI 布局或已有命令语义必须显式升版，并添加迁移和测试。不能用文件标题“Final”替代实际证据。
