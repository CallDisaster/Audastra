# Lua 地图保存目标

Lua 保存需要明确选择目标档案。地图的扩展名不会自动改变语言或客户端版本。

| 目标 | 地图格式与入口 | 检查方式 |
|---|---|---|
| `native-v31`（默认） | v31，根 `war3map.lua` | 显式捕获身份的外部 Lua 5.3 检查器 |
| `classic-127-ydwe` | v25，根 `war3map.j` 和地图内 Lua 模块 | 内置 Lua 5.3 编译检查，加完整 JASS 语法和语义检查 |

经典目标保留原 `war3map.w3i` 字节和 JASS 语言字段，只接受 12 玩家布局。
它使用公开的 `exec-lua` 入口和地图内模块约定。配置和原版环境、Blizzard 初始化仍在
JASS 中执行；环境、对象和传统 T 的 Lua 阶段依次加载。T 保持可编辑结构，不转换为
作者代码。当前经典目标只接受纯 T，已有原生 Lua 作者代码不作为该目标的保存资格。

经典目标从本次地图最终视图和所选游戏归档读取 `common.j`、`blizzard.j`，不要求
YDWE 编译工具或检查器。所需 native 签名、句柄父类型和常量必须对应实际声明；
任意 BJ 函数不能作为 native 使用。缺少定义、类型不符、检查预算不足或取消时，
不会发布部分脚本。支持的原创兼容函数为四种玩家联盟操作、`FourCC` 和
`ConditionalTriggerExecute`；其他 BJ 仍须有完整支持才可保存。

经典保存生成 `audastra/map.lua`、`compat.lua`、`bootstrap.lua`、`objects.lua`、
`continue.lua`，以及 `audastra/generated.manifest`。这些路径保留给同一保存事务。
清单绑定 JASS 入口和五个 Lua 模块的 SHA256；重复保存前重新核对全部源成员。
同名已有资源缺少清单、模块被手动修改或清单不一致时会拒绝覆盖，保留作者内容。

目标配置沿用 UTF-8 TSV：`schema`、`profile`、`player_layout`、`language` 为必填项。
增加 `dialect` 字段可选 `native-v31` 或 `classic-127-ydwe`；未写时仍使用 v31。
经典目标要求 `player_layout` 为 `classic12`、`language` 为 `preserve`，不接受
`checker` 或 `runtime` 字段；v31 的外部检查器要求保持不变。档案路径与 SHA256
必须来自用户明确选择的文件。

经典目标的测试热重载沿用本局启动时安装的全图符号名单。若新动作需要本局未安装的
函数、对象全局或常量，会在发送差异前提示重新测试；已确认的差异不会扩大安装名单。
关闭热重载后，运行时错误仍通过本局测试通道报告。

生成、完整检查和保存回读是不同于游戏运行的验证。经典保存成功不代表已支持所有
YDWE 模块，也不证明对战平台兼容。编辑器测试仍需匹配的自有运行时与真实游戏验证；
平台引擎对同一地图的执行须另行确认。
