#pragma once

#include <filesystem>
#include <stdexcept>

#ifndef AUD_TEST_LUA_EXECUTABLE
#error Use audastra_use_test_lua to bind the independently built test interpreter.
#endif

namespace War3Map::Tests {

// 路径由当前配置的构建目标提供，不接受 YDWE、PATH 或进程环境中的解释器替代。
inline std::filesystem::path LuaTestExecutable() {
    constexpr char8_t executableText[] = u8"" AUD_TEST_LUA_EXECUTABLE;
    const auto executable = std::filesystem::path(executableText);
    if (!executable.is_absolute() || !std::filesystem::is_regular_file(executable))
        throw std::runtime_error("The independently built Lua 5.3.6 test interpreter is missing.");
    return executable;
}

} // namespace War3Map::Tests
