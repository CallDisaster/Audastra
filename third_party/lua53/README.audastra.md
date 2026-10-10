# Lua 5.3.6 编辑器语法检查依赖

固定来源：https://www.lua.org/ftp/lua-5.3.6.tar.gz ，MIT 许可。
归档 SHA256：`fc5fd69bb8736323f026672b1b7235da613d7177e72558893a0bdcd320466d60`。

源码保持上游原样，放在开发缓存，仓库仅记录源码摘要与许可。新环境运行 `tools/dev/prepare-update-deps.ps1`；既有缓存可单独运行 `tools/dev/prepare-lua-check-deps.ps1 -OutputDirectory <新缓存>`，将返回的 `AUD_LUA53_SOURCE_DIR` 传给 CMake。

`src/Lua` 只静态编译 Lua 核心二十个 C 文件。不编译 standalone lua/luac、lauxlib、linit 或任何标准库；没有打开 io/os/package、注册本机接口或执行脚本的入口。检查仅调用 `lua_load`，模式限定为 text，使用内存、取消与时间预算，结束后关闭状态。游戏用 YDWE 六 DLL 与此 x64 编辑器依赖分别管理；本依赖不会加载或分发 YDWE 文件。

源码及头文件身份逐项由 `SOURCE_MANIFEST.json` 检查；完整许可 `LICENSE.txt` 随产品发布。
