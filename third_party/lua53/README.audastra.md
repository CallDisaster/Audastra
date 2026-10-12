# Lua 5.3.6 依赖

固定来源：https://www.lua.org/ftp/lua-5.3.6.tar.gz ，MIT 许可。
归档 SHA256：`fc5fd69bb8736323f026672b1b7235da613d7177e72558893a0bdcd320466d60`。

源码保持上游原样，放在开发缓存，仓库仅记录源码摘要与许可。新环境运行 `tools/dev/prepare-update-deps.ps1`；既有缓存可单独运行 `tools/dev/prepare-lua-check-deps.ps1 -OutputDirectory <新缓存>`，将返回的 `AUD_LUA53_SOURCE_DIR` 传给 CMake。

`src/Lua` 只静态编译 Lua 核心二十个 C 文件。不编译 standalone lua/luac、lauxlib、linit 或任何标准库；没有打开 io/os/package、注册本机接口或执行脚本的入口。检查仅调用 `lua_load`，模式限定为 text，使用内存、取消与时间预算，结束后关闭状态。游戏用 YDWE 六 DLL 与此 x64 编辑器依赖分别管理；本依赖不会加载或分发 YDWE 文件。

源码及头文件身份逐项由 `SOURCE_MANIFEST.json` 检查；完整许可 `LICENSE.txt` 随产品发布。

`src/WarcraftRuntime32/OwnLua` 另有独立的 Win32 宿主目标，使用同一官方缓存和
`RUNTIME_SOURCE_MANIFEST.json` 核验的辅助库、base/table/string/math/utf8。
它使用上游支持的 C++ 异常模式编译，确保宿主回调中的 C++ 拥有者在 Lua 错误时正常释放；
不修改上游源码，不与编辑器的 compile-only C 库混链。不编译 io、os、package、debug、
动态加载器或 standalone 解释器，模块由宿主的只读地图成员读取器提供。
自有宿主是否可用于真实游戏取决于其 native、入口和启动验证；有 Lua VM 不代表已经可运行地图。
