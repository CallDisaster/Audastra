# 从 WarVK 模块原型迁移到 Audastra JAPI v2

## 为什么不是“修几个字段仍叫 v1.2”

旧原型的字符串返回、同步事件嵌套语义、异步卸载条件存在不兼容的合同问题。v2 显式更换前缀、C ABI 和客户端，而不是让旧模块在新宿主上悄悄改变行为。

| 原型 | v2 |
|---|---|
| `warvk:v1;<command>...` | `ajapi:2;<epoch>;<request>;<command>...` |
| Preloader／Hotkey／LocalizedString 多载体 | 单一字符串响应信封，业务值与错误分离 |
| ABI v1，返回借用的 `const char*` | ABI `0x00020000`，宿主固定结果缓冲区 |
| 模块名称作为事件归属 | epoch + 不复用 instance + event ID |
| 同步重入先入队并返回成功 | 当前栈内完成；嵌套同步明确 REENTRANT |
| 有界队列淘汰最旧事件 | 满时拒绝新事件并返回 BUSY；统计取消／拒绝／失败 |
| 单步 shutdown／引用计数析构 | requestStop／pollStopped／destroy，游戏线程显式完成卸载 |
| `lastError` 等驼峰名称 | 所有控制命令小写；每条响应携带状态 |
| v1.2 草案与 v1.1 实现混用 | 代码、公共 C 头、JASS、manifest、测试使用同一 v2 合同 |

不自动加载 v1 DLL，不兼容旧导出名，不把旧状态值当作新返回值，不改旧 WarVK 渲染命令。旧原型文件保留供迁移核对，但历史 v1.2 文档不再作为新实现的规范。

## 模块迁移步骤

1. 建立 `audastra.japi.module.v2` manifest，写明每条命令的 domain。纯 UI／本地 I/O 为 local，必须遵守确定性规则的玩法命令为 sync。
2. 导出 `AudastraJapiInitV2`，使用 `AjHost`／`AjModule`／`AjCommand`；Init 仅返回静态元数据。
3. 把文本结果改为 `AjSetText(out, local.data(), size)`；不保存 `AjResult*` 或输入参数指针供调用结束后使用。
4. 把初始化和线程创建移到 start；提供非阻塞 requestStop 和可证明线程实际退出的 pollStopped。
5. 每次 emit 检查返回状态。BUSY 不是发送成功；SYNC 失败不应继续假设地图回调已执行。重试必须由业务定义是否会重复副作用。
6. 编译自包含 Win32 DLL，生成 C++ 契约／JASS 包装；由可信安装者审核摘要并配置 pin。
7. 先跑 SDK 测试，再按游戏接入门禁验收；没有受信任宿主时明确报缺失。

## 地图迁移步骤

- 显式包含 `AudastraJapi.j` 和生成包装；旧调用者不做隐式文本替换。
- 初始化调用 `AjInitialize`，每次取值后立即检查 `AjLastStatus`。
- 事件用 `AjOn(module,type,code)` 的监听 ID 管理，`AjBindEvents` 单独确认推送可用。
- 保留 `AjDispatch` 函数名，回调禁止等待／yield。
- LOCAL／ASYNC 中不直接改变同步游戏状态；使用游戏已经验证的同步业务命令。
- 换图后视为新会话，不跨 epoch 缓存实例 ID、监听 ID 或协议请求编号。

v2 不覆盖历史地图对第三方商业运行时的兼容需求。必须保留第三方功能时，使用作者批准的公开适配边界或官方运行时；不要重建受限通讯内容。本协议本身没有发现必须引入该受限层的依赖。
