# h-lua 工程构建与测试

h-lua 为 AGPL-3.0，由用户提供。Audastra 不附带框架、SDK 默认表或系统物体，也不执行 SDK、外部转换器或它们的启动脚本。构建期只在有预算的 Lua 沙箱内加载用户提供的构建文件。地图中使用这些文件的分发义务由地图作者按其许可证处理。

## 命令行

以下路径均为示例。输出地图和工作目录必须尚不存在，命令不会覆盖源工程或现有地图。

```powershell
war3-map-cli hlua-inspect D:/Projects/MyMap --framework D:/Frameworks/h-lua
war3-map-cli hlua-ids D:/Projects/MyMap --framework D:/Frameworks/h-lua --offset 0 --limit 128
war3-map-cli hlua-build D:/Projects/MyMap --framework D:/Frameworks/h-lua `
  --game D:/Games/War3 --workspace D:/Builds/MyMap-test `
  --output D:/Builds/MyMap-test.w3x --target test
```

框架目录可以替换成一对 `--runtime-framework <运行期目录> --build-framework <构建期目录>`；不能与 `--framework` 同传。框架缺文件、链接、无效 UTF-8、Lua 语法和模块重名都有明确拒绝。静态 require 使用与 Lua 编辑服务相同的词法扫描器，动态 require 仍须游戏验证。

默认底图来自工程的 `map/w3x`、`map/slk`、`map/resource`。也可以用 `--base D:/Maps/Base.w3x`。已有地图沿归档保存服务透传未知资源；目录构建在打开前拒绝引导成员。不通过 Lua、脚本预处理或模块加载器解释引导内容。

`hlua-new <新目录> --framework <目录> --tool <已安装工具数据根> --game <游戏目录> [--ui-root <定义目录>]` 使用 Audastra 自己的地图创建服务生成 `base.w3x` 和原创 `main.lua`。后续构建显式传 `--base <新目录>/base.w3x`。

`test` 与 `build` 都写原生物编；`dist` 使用共用 SLK 优化器及其语义核验。所有目标均把框架和作者 Lua 写进地图，不设置本机 `package.path`。改变 hslk 后要重新构建；普通 Lua 的测试热重载沿原测试邮箱。

```powershell
war3-map-cli hlua-test D:/Projects/MyMap --framework D:/Frameworks/h-lua `
  --game D:/Games/War3 --workspace D:/Builds/MyMap-run `
  --output D:/Builds/MyMap-run.w3x --helpers D:/Apps/Audastra `
  --engine owned -- --cloaked --press-space --seconds 90 `
  --screenshot-after 60 D:/Builds/MyMap-shot.tga --close-after 80
```

分隔符后的参数采用 `map-test` 的现有合同，包括计划输入、截图、关闭、热重载。只操作本次测试持有的进程。`--engine ydwe` 使用用户已安装的数据根，并须在分隔符后给 `--ydwe <目录>`；启动前扫描地图中所有 Lua 的 `io`、`os.execute`、`package.loadlib` 访问，发现则拒绝。此检查不证明动态代码完整性，也不会自动切到更宽松环境。

## 数据与诊断

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
| `jass.debug` | `handlemax()` 返回句柄表槽数；`handledef()` 返回 reference 和可确认的 type，无效句柄返回 nil |
| `jass.runtime` | console 控制测试输出；debugger 端口设置识别后明确报告未实现，不监听网络；原 sleep/handle_level 限制仍生效 |
| `jass.console.write` | UTF-8 文本进入本次测试回执/问题列表，不自动补换行；输出有界 |
| `collectgarbage` | 只支持 collect 和 count（KiB） |
| `os` | 只支持 date、time、clock；date 格式/结果有界，年份 1970–3000；没有文件、环境或进程接口 |
| `debug.traceback` | 非文本且非 nil 参数原样返回；不提供线程、注册表或任意内存访问 |

外部调试器、未实现的 jass 模块和框架对缺失能力的具体反应，应在测试信息里查看。打包成功不代表这些功能可用。控制台输出、运行时错误、截图和物体状态才是进入作者 `main()` 的证据。

## MCP 与共用服务

`hlua_inspect`、`hlua_ids`、`hlua_build`、`hlua_new`、`hlua_test` 调用无 Qt 的 `War3Map::HLua` 服务。工具接受明确工程/框架/底图/游戏/输出路径，不接受 shell 命令或任意 PID。ID 输出默认 128 项，可用 offset/limit。测试返回本工具创建的 session_id；`hlua_test_result`、`hlua_test_stop` 要求该身份，只查询或结束本工具的测试租约。普通编辑器地图测试与 CLI 经过同一物编资料冻结和测试准备服务。

## 格式依据

仅核对公开格式事实，不复制第三方实现：

- [w3x2lni 字段命名](https://github.com/sumneko/w3x2lni/blob/master/script/prebuilt/metadata.lua)；其当前 [许可证为 GPL-3.0](https://github.com/sumneko/w3x2lni/blob/master/LICENSE.txt)。
- [原生字段输出](https://github.com/sumneko/w3x2lni/blob/master/script/core/slk/backend_obj.lua) 与 [空数字处理](https://github.com/actboy168/w3xparser/blob/master/src/main.cpp)；w3xparser 为 Apache-2.0。仅实现空文本为零，不继承错误文本静默转零、溢出回零或整数字段截断。
- [jass2lua 接口语义](https://github.com/actboy168/jass2lua/blob/master/lua-engine.md) 与 [Lua 5.3 手册](https://www.lua.org/manual/5.3/manual.html)。
