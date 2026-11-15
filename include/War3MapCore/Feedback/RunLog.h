#pragma once

#include "War3MapCore/Types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 运行日志：一次运行一份，记录启动、打开、编辑、保存、测试、助手调用和所有诊断，供作者出错时导出错误报告。
// 写入时逐行刷新，程序崩溃也能留下崩溃前的记录。一轮（一次运行）最多 maxBytesPerRun：
// 写满一半就把当前段改名为 .prev.log 再开新段，所以始终保留最近的记录，会话信息在每段开头重复一次。
namespace War3Map::RunLog {

enum class Level : std::uint8_t { Info, Warning, Error };

struct Options {
    std::filesystem::path directory;
    std::string header;                                   // 每段开头写入的会话信息（版本、参数、系统等）
    std::uint64_t maxBytesPerRun = 10ull * 1024 * 1024;   // 当前段 + 上一段合计上限
    std::size_t keepRuns = 20;                            // 目录里最多保留的运行份数
};

// 开始本次运行的日志；同一进程只能开始一次。失败时日志保持关闭，其余功能照常。
[[nodiscard]] Status Start(const Options& options);
[[nodiscard]] bool Active() noexcept;
void Write(Level level, std::string_view area, std::string_view message) noexcept;
inline void Info(std::string_view area, std::string_view message) noexcept { Write(Level::Info, area, message); }
inline void Warning(std::string_view area, std::string_view message) noexcept { Write(Level::Warning, area, message); }
inline void Error(std::string_view area, std::string_view message) noexcept { Write(Level::Error, area, message); }
// 正常退出时调用：写结束行并删除本次的运行标记。
void Stop(std::string_view reason) noexcept;

[[nodiscard]] std::filesystem::path Directory();
// 本次运行的日志文件：有上一段时先上一段，再当前段。
[[nodiscard]] std::vector<std::filesystem::path> SessionFiles();
// 上一次没有正常退出（崩溃、被结束）的运行留下的日志文件；没有时为空。
[[nodiscard]] std::vector<std::filesystem::path> UncleanPreviousRunFiles();
[[nodiscard]] std::chrono::system_clock::time_point Started();
[[nodiscard]] std::chrono::steady_clock::duration Uptime();
// 把时长写成“2 小时 3 分 4 秒”。
[[nodiscard]] std::string FormatDuration(std::chrono::steady_clock::duration duration);

// 一段操作：构造时写“开始”，结束时写“完成 / 失败”和用时。
class Scope {
public:
    Scope(std::string_view area, std::string what) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Succeeded(std::string_view detail = {}) noexcept;
    void Failed(std::string_view reason) noexcept;
private:
    std::string area_, what_;
    std::chrono::steady_clock::time_point started_;
    bool finished_ = false;
};

} // namespace War3Map::RunLog
