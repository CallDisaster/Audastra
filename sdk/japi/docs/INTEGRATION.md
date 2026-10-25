# 宿主接入、部署与验收

## 已实现与未接通的边界

| 层 | 当前交付 |
|---|---|
| 字节协议与状态机 | 可执行 C++ 实现及 x86/x64 离线测试 |
| DLL ABI | C 头、原创 C 示例、独立契约核对 |
| Windows x86 加载 | 受信任摘要 pin、PE 预检、私有暂存、正常系统加载 |
| 工作线程停机 | 两阶段停止、真实 worker DLL 回归、失败保活 |
| 地图客户端 | JASS 库、manifest 生成包装、编辑器语义检查 |
| 真实 native 桥 | 基于既有观察器/VM 的[实验字符串桥](RUNTIME_BRIDGE.md)已实现，默认关闭；真实游戏尚未验收 |
| 编辑器构建 | 主工程／x86 runtime 编译同一 SDK；实验桥显式 opt-in，无加载器/脚本执行器 |
| 编辑器地图安装服务／UI 命令 | 通用模块安装尚未接入既有脚本构建／导入事务，不宣称已自动打包地图；内置界面模块的地图侧客户端随 AudastraUI 触发器函数库注入（见[实验桥说明](RUNTIME_BRIDGE.md)） |
| 地图自带 runtime | 固定 raw 引导器与冻结 DLL 在经典 1.27a 专用原创样例完成加载、握手与可见 UI；两个进程各首图及同图重开一次，见[地图 runtime](MAP_RUNTIME.md) |
| 完整游戏资格／联机 | 编辑器自动打包、生成器界面、跨图、菜单重进、连续两次重开及联机**未验收**；上述固定组合证据不自动覆盖新编译 DLL |

## 嵌入顺序

1. 在已验证的游戏映像和正确 VM／游戏线程中建立会话。Host 只绑定构造线程，不能自行证明这是游戏线程。
2. 从可信安装配置取得模块 pin；生成的 map package 只作为待审核材料。不能将地图自带 SHA 当成宿主授权。
3. 创建 `PinnedDllLoader(trustedModuleDirectory, pins, trustedWritableStagingParent)`。三个对象来自安装者；文件路径为绝对路径。
4. 创建 `HostOptions`，设置整个宿主生命周期内有意义的 `initialEpoch`；普通回调适配器使用 `execute`，但 `synchronousCallbacksVerified` 默认保持 false。
5. 在已经核验过的字符串 native 入口，把输入交给 `HandleLocalizedString`；返回 false 时逐字透传给原函数。返回 true 时把结果**复制进 VM 管理的字符串存储**，不能返回临时 C++ `c_str()`。
6. 只在可安全重新进入脚本的游戏线程安全点调用 `Pump()`。禁止从渲染线程或 worker 直接运行 JASS。
7. VM 失效前从宿主调用 `Reset(newEpoch)`，检查 applied 与 stoppingModules；换图后清除旧脚本全局，重新 `AjInitialize`／`AjBindEvents`。若 applied=false，不能假设旧上下文已失效。
8. 退出时调用 `Close`，在后续安全点推进停止。BUSY 不能被解释为已经卸载。宿主析构兜底会保留未停止 DLL，不主动终止线程。

示意代码（适配器本身须由游戏集成方提供）：

```cpp
#include "aj/NativeBridge.h"
#include "aj/WindowsLoader.h"

auto loader = std::make_shared<audastra::japi::PinnedDllLoader>(
    trustedModuleDirectory, approvedPins, writablePrivateStagingParent);
audastra::japi::HostOptions options;
options.initialEpoch = verifiedVmEpoch;
options.execute = verifiedExecuteNamedFunction;
options.synchronousCallbacksVerified = false;
audastra::japi::Host host(options, loader);

// Inside a verified GetLocalizedString adapter:
std::string ownedResult;
if (audastra::japi::HandleLocalizedString(host, utf8Input, ownedResult)) {
    // Return through the actual VM-owned string allocator, NOT ownedResult.c_str().
} else {
    // Original native receives exactly its original input.
}
```

此示意只描述 SDK 接缝，不提供游戏地址表。编辑器的实验桥复用既有 GameVm127 与注册观察器的 ABI 合同；具体开关、返回引用和证据边界见[实验桥说明](RUNTIME_BRIDGE.md)。

## 同步执行器的资格

`execute(name)` 的 true 必须表示指定函数正常执行并返回。仅仅找到 native、压入任务、启动脚本线程或“ExecuteFunc 已返回”不一定满足这个语义。必须验证原版 VM 在 native 回入、异常、指令预算耗尽、TriggerExecute 内动作等待等情况下的真实行为。

明确不支持等待／yield 的作者回调；宿主的事件读取和 ack 只能确认协议路径，不能自行证明任意游戏调度器的执行栈已经完成。未建立证明前，保持 `synchronousCallbacksVerified=false`，SYNC emit 返回 UNSUPPORTED，绝不自动排队降级。

## DLL 作者的停机模板

`start` 分配资源并创建等待业务开始的 worker。active 后再由命令通知 worker 开始；这样 worker 不会与 Starting 状态竞争并误以为早期事件已发出。

`requestStop` 只设置取消信号并返回。worker 释放自身资源并真正退出；`pollStopped` 使用非阻塞线程完成检测，确认后完成 join／关闭线程句柄，最后返回 1。`destroy` 仅释放没有任何 worker 再访问的模块资源。

测试模块 `tests/WorkerFixture.cpp` 给出了 Windows 事件／线程句柄的原创实现。它不是通用网络库，也不建议直接把阻塞 I/O 放进主线程。慢 I/O 必须设计可取消性，不能通过强杀线程或强卸载 DLL 回收。

## 编辑器接入规则

以后接入“安装 JAPI 模块”时，应使用现有 MapDocument／EditorSession／ScriptBuildService 的事务与保存链：

- manifest 验证及生成是一项共享服务，UI、CLI、MCP 使用同一入口。
- 生成物是显式新增的库和包装，作者 GUI T 不应被转换为自定义代码。
- 原有自定义代码逐字保留；模块更新有明确输入摘要、旧版本检查、撤销及失败诊断。
- 地图只能声明模块需求；可信宿主是否加载由地图外策略决定。不能在导入地图时顺便把 DLL 加入允许清单。
- 游戏宿主缺失时给出部署缺失诊断，不回退到任何第三方私有通讯层。

本轮没有实现或更改上述地图安装事务，不把单独的代码生成当成保存／打包完成。

## 验收矩阵

| 门禁 | 最小证据 | SDK 离线测试能否替代 |
|---|---|---|
| 输入输出协议 | 非法帧、边界、UTF-8、locale、类型和错误关联 | 可以验证自身实现 |
| DLL 生命周期 | 真实原创 DLL、worker 退出、失败保活、重复加载 | 可以验证离线 Windows 路径 |
| JASS 源 | 声明、语法、类型、生成名称与契约一致 | 仅静态语义 |
| 游戏映像及 ABI | 版本身份、目标函数签名、调用约定和字符串分配 | 不可以 |
| 原版脚本 | hello、整数负值、中文、百分号、real 极值、缺宿主 | 不可以 |
| 同步栈 | 回调发生在 emit 返回前；失败／重入／等待路径不假成功 | 不可以 |
| 换图 | 20 次进退／VM 重建，旧实例无效，线程全部退出 | 不可以 |
| 双客户端 | 同版本、同模块摘要、可重放逻辑、本地输入隔离、无 desync | 不可以 |
| 发布部署 | 配置授权、安装更新、目录 ACL、受限账户、崩溃恢复 | 不可以 |

真实验收必须使用专用实例和原创测试地图／地图副本，不修改作者常用安装或原地图；记录映像、模块、地图摘要及实际日志顺序。

## 平台依据

完整路径及受控 DLL 搜索依据 [Microsoft LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw) 和 [Dynamic-Link Library Security](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-security)。文件创建／共享模式依据 [Microsoft CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)。这些是平台 API 依据，不是游戏 ABI 或实时调度保证。
