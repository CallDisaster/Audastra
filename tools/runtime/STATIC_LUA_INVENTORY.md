# 本机 Lua 接口的静态清单

`audastra-lua-api-inventory` 与编辑器后台调用同一个 `CaptureRuntimeLuaApiInventory`。这一入口替代本单元的进图探针，不加载 DLL，不创建 Lua state，不执行地图或作者 Lua，不启动 Warcraft，不修改本机安装。六个 GPL 运行库只读取用户配置的原始 `bin` / `plugin/warcraft3` 文件；不复制、提交或分发这些文件。

```text
audastra-lua-api-inventory --root <用户本机YDWE> --out <不存在的清单.json>
```

未指定 `--root` 时只读取 `FORGE_YDWE_ROOT`。两者都缺失返回 77；配置的安装无效返回 1，不把无效配置当成跳过。`--common` 可显式指定同次检查所用的真实 common.j，默认是该安装的 `compiler/jass/24/common.j`。输出使用 `CREATE_NEW`，不覆盖现存回执。用户目录只用于本机运行，不写死在源码。

静态清单 schema 为 `audastra.lua-runtime-api.static.v1`，与历史的实际 Lua-state 观察清单分开。它只证明绑定 DLL 中的注册代码与模块表关系：模块 availability 为 `registration-code-present`，`initializeReachabilityVerified=false`。不从 Initialize 或其回调建立通用可达性图，不能称初始化已接通、模块运行可用或已实际游戏加载。证明步骤为：

1. 六个 DLL 各自检查 PE32、节区、实际导出、实际 IAT、HIGHLOW 重定位；六库之间的命名 import 必须能在同次捕获的 provider export 中找到。合法 DATA export 可以位于加载器清零的虚拟节区尾部，仅据此承认映像内导出，不读取或合成内容。用于证明的代码、名称、表、IAT 和重定位目标必须有实际文件字节；Lua C API 与 native 查找桥的 provider export 还必须位于可执行节区。解析失败时回执保留 provider 相对路径与无法映射的 RVA/读取长度。
2. 在实际引擎代码中验证完整的 C 注册 helper：`lua_getfield(registry,"_PRELOAD")`、`lua_pushcclosure(open,0)`、`lua_setfield(module)`、栈还原。每个 API 地址必须指向实际 `lua53.dll` import，每个绝对指针必须有真实重定位。
3. 只接受包含该 helper 直接调用的三种有界 x86 注册代码形态，生成模块名称、open 指针与注册调用 RVA。不是通过搜索 `jass.*` 字符串生成模块，也不据此宣称这些代码已从 Initialize 执行到达。
4. open 入口的直线前缀只做原创、有限的抽象值传递，读取确实传入 `luaL_setfuncs` 的静态或栈 `luaL_Reg` 表，以及明确的 C 闭包注册。遇分支或未知副作用停止；字段仍为部分，不把没找到的名字当成不存在。
5. `jass.common` 的 C `__index` 注册代码及 callback 必须包含本机 `ydwar3` 的 `jass_func` 与 `func_value::is_valid`、Lua C closure 的静态代码引用；然后通过保存链共用的 `IndexJassDeclarations` 解析同次 SHA 绑定的 common.j，生成 native 参数个数。这只提供声明契约与静态桥代码证据，不证明所有路径的数据流、一次实际游戏的字段 lookup，或地图所需 native 在游戏内可用。

清单保留七个来源 SHA、真实注册 RVA 与代码片段 SHA；不写出 DLL 字节。缓存只复用相同来源 SHA 的解析结果，每次请求重新核来源文件。整个读取、分块 SHA、解析和 common.j 检查共用请求 deadline / stop token；有文件大小、指令数、字段数、imports 数和元数据名称预算。

静态证据不能证明所有插件、脚本和惰性注册都已覆盖，因此 `modulesComplete=false`、`fieldsComplete=false`。未列出模块与未知惰性字段继续按未知处理，不能生成“该模块不存在”的警告。新引擎 ABI 不匹配、运行库缺失/变化、声明检查失败或超时时，返回“目前只检查语法”，不沿用旧缓存。这只向 K4 提供警告级接口契约，不授予整图保存或真实游戏验收资格。

`StaticRuntimeLuaApiTests.cpp` 的原创 PE 字节 fixture 不属于任何本机运行库；它验证实际 import / 重定位 / helper 调用 / C 表匹配，合法零初始化 DATA export，以及错误 registry、错误 import、无重定位、数据假代码、虚拟假代码、重叠/截断 PE、越界导出、重复注册、取消、deadline 和未配置时语法降级。真实清单必须由配置本机安装的单独执行回执证明，fixture 不构成作者运行库验收。
