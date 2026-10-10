#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

// 外部编译器进程（Wave、jasshelper）。无窗口、带超时；超时时结束整个进程树（含 jasshelper 启动的 pjass）。
namespace War3Map::Script::detail {

struct ProcessRequest {
    std::filesystem::path executable;
    std::vector<std::wstring> arguments; // 每项按 Windows 命令行规则加引号
    std::filesystem::path workingDirectory;
    std::vector<std::filesystem::path> pathPrefix; // 追加到子进程 PATH 前（DLL 搜索）
    std::chrono::milliseconds timeout{600000};
    std::filesystem::path outputFile; // 标准输出与标准错误合并写入此文件
    std::size_t maxOutputBytes = 8 * 1024 * 1024; // 内存及日志均截在此界限，超限失败；硬限64MiB
    std::size_t maxJobMemoryBytes = 1024ull * 1024 * 1024; // 整个子进程树提交内存；硬限2GiB
};

struct ProcessOutcome {
    bool started = false;
    bool timedOut = false;
    bool outputLimitExceeded = false;
    bool supervisionFailed = false;
    int exitCode = -1;
    std::string output; // outputFile 的内容
    std::string error;  // 启动失败原因
    [[nodiscard]] bool Succeeded() const noexcept {
        return started && !timedOut && !outputLimitExceeded && !supervisionFailed && exitCode == 0;
    }
};

// 强制Job成功接管后才恢复主线程；全部子进程结束才返回成功。仅是资源/生命周期监督，
// 不隔离文件、网络或继承的环境，不能直接用来执行任意不可信地图Lua。
[[nodiscard]] ProcessOutcome RunProcess(const ProcessRequest& request);

// 把参数按 CommandLineToArgvW 的规则加引号。
[[nodiscard]] std::wstring QuoteArgument(const std::wstring& argument);

} // namespace War3Map::Script::detail
