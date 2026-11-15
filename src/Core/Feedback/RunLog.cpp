#include "War3MapCore/Feedback/RunLog.h"

#include "War3MapCore/IO/FileBytes.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <share.h>
#include <span>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace War3Map::RunLog {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t kMessageLimit = 64 * 1024;
constexpr std::string_view kPrefix = "audastra-";
constexpr std::string_view kPreviousSuffix = ".prev.log";
constexpr std::string_view kMarkerPrefix = "running-";

struct State {
    std::mutex mutex;
    bool active = false;
    bool started = false;
    Options options;
    fs::path current, previous, marker;
    std::FILE* file = nullptr;
    std::uint64_t written = 0;
    std::uint64_t segmentLimit = 0;
    unsigned segment = 1;
    std::vector<fs::path> unclean;
    std::chrono::system_clock::time_point startedWall{};
    std::chrono::steady_clock::time_point startedSteady = std::chrono::steady_clock::now();
};

State& Instance() {
    static State state;
    return state;
}

std::string Timestamp(bool fileName) {
#ifdef _WIN32
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char text[40]{};
    if (fileName)
        std::snprintf(text, sizeof(text), "%04u%02u%02u-%02u%02u%02u", unsigned{time.wYear}, unsigned{time.wMonth},
            unsigned{time.wDay}, unsigned{time.wHour}, unsigned{time.wMinute}, unsigned{time.wSecond});
    else
        std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u", unsigned{time.wYear}, unsigned{time.wMonth},
            unsigned{time.wDay}, unsigned{time.wHour}, unsigned{time.wMinute}, unsigned{time.wSecond}, unsigned{time.wMilliseconds});
    return text;
#else
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char text[40]{};
    std::strftime(text, sizeof(text), fileName ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return text;
#endif
}

unsigned long ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return 0;
#endif
}

unsigned long ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

bool ProcessAlive(unsigned long pid) {
#ifdef _WIN32
    if (!pid) return false;
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    (void)pid;
    return false;
#endif
}

std::FILE* OpenAppend(const fs::path& path) {
#ifdef _WIN32
    return _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

std::string LevelText(Level level) {
    switch (level) {
    case Level::Info: return "信息";
    case Level::Warning: return "警告";
    case Level::Error: return "错误";
    }
    return "错误";
}

std::string Line(Level level, std::string_view area, std::string_view message) {
    std::string text = Timestamp(false) + " [" + LevelText(level) + "] #" + std::to_string(ThreadId()) + " [" + std::string(area) + "] ";
    const auto bounded = message.substr(0, kMessageLimit);
    for (std::size_t i = 0; i < bounded.size(); ++i) {
        const char c = bounded[i];
        if (c == '\r') continue;
        if (c == '\n') text += "\r\n    ";
        else text += c;
    }
    if (message.size() > kMessageLimit) text += "…（截断 " + std::to_string(message.size() - kMessageLimit) + " 字节）";
    text += "\r\n";
    return text;
}

void WriteRaw(State& state, std::string_view text) {
    if (!state.file) return;
    if (std::fwrite(text.data(), 1, text.size(), state.file) == text.size()) state.written += text.size();
    std::fflush(state.file);
}

void WriteHeader(State& state) {
    std::string header = "==== Audastra 运行日志 · 第 " + std::to_string(state.segment) + " 段 · " + Timestamp(false)
        + " · 进程 " + std::to_string(ProcessId()) + " ====\r\n";
    for (const char c : state.options.header) {
        if (c == '\r') continue;
        if (c == '\n') header += "\r\n"; else header += c;
    }
    if (!header.ends_with("\r\n")) header += "\r\n";
    if (state.segment > 1)
        header += "（本轮更早的记录在 " + IO::PathUtf8(state.previous.filename()) + "，再早的已按 "
            + std::to_string(state.options.maxBytesPerRun / (1024 * 1024)) + " MB 上限丢弃）\r\n";
    header += "====\r\n";
    WriteRaw(state, header);
}

// 写满一段：当前段改名为 .prev.log（替换更早的一段），再开新段。
void Rotate(State& state) {
    if (state.file) std::fclose(state.file);
    state.file = nullptr;
    std::error_code error;
    fs::remove(state.previous, error);
    fs::rename(state.current, state.previous, error);
    state.file = OpenAppend(state.current);
    state.written = 0;
    ++state.segment;
    WriteHeader(state);
}

std::string RunStem(const fs::path& path) {
    auto name = IO::PathUtf8(path.filename());
    if (name.ends_with(kPreviousSuffix)) return name.substr(0, name.size() - kPreviousSuffix.size());
    if (name.ends_with(".log")) return name.substr(0, name.size() - 4);
    return name;
}

// 只保留最近 keepRuns 次运行的日志（文件名里带时间，按名字排序即按时间排序）。
void Prune(const fs::path& directory, std::size_t keepRuns) {
    std::map<std::string, std::vector<fs::path>> runs;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto name = IO::PathUtf8(it->path().filename());
        if (!name.starts_with(kPrefix) || !name.ends_with(".log")) continue;
        runs[RunStem(it->path())].push_back(it->path());
    }
    while (runs.size() > keepRuns) {
        for (const auto& path : runs.begin()->second) fs::remove(path, error);
        runs.erase(runs.begin());
    }
}

// 别的进程留下的运行标记：进程已不在说明那次没有正常退出，记下它的日志。
std::vector<fs::path> CollectUnclean(const fs::path& directory) {
    std::vector<std::pair<fs::path, fs::path>> found;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto name = IO::PathUtf8(it->path().filename());
        if (!name.starts_with(kMarkerPrefix) || !name.ends_with(".txt")) continue;
        const auto pid = std::strtoul(name.c_str() + kMarkerPrefix.size(), nullptr, 10);
        if (ProcessAlive(pid)) continue;
        const auto bytes = IO::ReadFileBytes(it->path(), 4096);
        fs::path log;
        if (bytes.Succeeded())
            log = IO::PathFromUtf8(std::string_view(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size()));
        found.emplace_back(it->path(), log);
    }
    std::vector<fs::path> logs;
    fs::path newest;
    for (const auto& [marker, log] : found) {
        fs::remove(marker, error);
        if (!log.empty() && log.parent_path() == directory && fs::exists(log, error) && IO::PathUtf8(log) > IO::PathUtf8(newest))
            newest = log;
    }
    if (!newest.empty()) {
        auto previous = newest;
        previous.replace_filename(IO::PathFromUtf8(RunStem(newest) + std::string(kPreviousSuffix)));
        if (fs::exists(previous, error)) logs.push_back(previous);
        logs.push_back(newest);
    }
    return logs;
}

} // namespace

Status Start(const Options& options) {
    auto& state = Instance();
    std::lock_guard lock(state.mutex);
    if (state.started) return Status::Failure("RunLog", "RUN_LOG_STARTED", "运行日志已经开始。");
    state.started = true;
    state.startedWall = std::chrono::system_clock::now();
    if (options.directory.empty() || !options.directory.is_absolute())
        return Status::Failure("RunLog", "RUN_LOG_DIRECTORY", "运行日志目录必须是绝对路径。");
    std::error_code error;
    fs::create_directories(options.directory, error);
    if (error) return Status::Failure("RunLog", "RUN_LOG_DIRECTORY", "无法创建运行日志目录 " + IO::PathUtf8(options.directory) + "。");
    state.options = options;
    state.options.maxBytesPerRun = (std::max)(options.maxBytesPerRun, std::uint64_t{64 * 1024});
    state.options.keepRuns = (std::max)(options.keepRuns, std::size_t{2});
    state.segmentLimit = state.options.maxBytesPerRun / 2;
    state.unclean = CollectUnclean(options.directory);
    Prune(options.directory, state.options.keepRuns - 1);
    const auto stem = std::string(kPrefix) + Timestamp(true) + "-" + std::to_string(ProcessId());
    state.current = options.directory / IO::PathFromUtf8(stem + ".log");
    state.previous = options.directory / IO::PathFromUtf8(stem + std::string(kPreviousSuffix));
    state.file = OpenAppend(state.current);
    if (!state.file) return Status::Failure("RunLog", "RUN_LOG_OPEN", "无法写入运行日志 " + IO::PathUtf8(state.current) + "。");
    state.marker = options.directory / IO::PathFromUtf8(std::string(kMarkerPrefix) + std::to_string(ProcessId()) + ".txt");
    const auto current = IO::PathUtf8(state.current);
    (void)IO::WriteFileBytes(state.marker, std::span(reinterpret_cast<const std::byte*>(current.data()), current.size()));
    state.active = true;
    WriteHeader(state);
    if (!state.unclean.empty())
        WriteRaw(state, Line(Level::Warning, "启动", "上次运行没有正常退出，日志：" + IO::PathUtf8(state.unclean.back().filename())));
    return Status::Ok();
}

bool Active() noexcept {
    auto& state = Instance();
    std::lock_guard lock(state.mutex);
    return state.active;
}

void Write(Level level, std::string_view area, std::string_view message) noexcept {
    try {
        auto& state = Instance();
        const auto text = Line(level, area, message);
        std::lock_guard lock(state.mutex);
        if (!state.active || !state.file) return;
        if (state.written + text.size() > state.segmentLimit) Rotate(state);
        WriteRaw(state, text);
    } catch (...) {}
}

void Stop(std::string_view reason) noexcept {
    try {
        auto& state = Instance();
        std::lock_guard lock(state.mutex);
        if (!state.active) return;
        WriteRaw(state, Line(Level::Info, "退出", "正常退出：" + std::string(reason) + "；运行了 "
            + FormatDuration(std::chrono::steady_clock::now() - state.startedSteady) + "。"));
        if (state.file) std::fclose(state.file);
        state.file = nullptr;
        state.active = false;
        std::error_code error;
        fs::remove(state.marker, error);
    } catch (...) {}
}

fs::path Directory() {
    auto& state = Instance();
    std::lock_guard lock(state.mutex);
    return state.options.directory;
}

std::vector<fs::path> SessionFiles() {
    auto& state = Instance();
    std::lock_guard lock(state.mutex);
    std::vector<fs::path> files;
    if (state.current.empty()) return files;
    std::error_code error;
    if (state.segment > 1 && fs::exists(state.previous, error)) files.push_back(state.previous);
    files.push_back(state.current);
    return files;
}

std::vector<fs::path> UncleanPreviousRunFiles() {
    auto& state = Instance();
    std::lock_guard lock(state.mutex);
    return state.unclean;
}

std::chrono::system_clock::time_point Started() {
    auto& state = Instance();
    std::lock_guard lock(state.mutex);
    return state.startedWall;
}

std::chrono::steady_clock::duration Uptime() {
    return std::chrono::steady_clock::now() - Instance().startedSteady;
}

std::string FormatDuration(std::chrono::steady_clock::duration duration) {
    const auto total = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    if (total < 1) return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()) + " 毫秒";
    const auto days = total / 86400, hours = total / 3600 % 24, minutes = total / 60 % 60, seconds = total % 60;
    std::string text;
    if (days) text += std::to_string(days) + " 天 ";
    if (days || hours) text += std::to_string(hours) + " 小时 ";
    if (days || hours || minutes) text += std::to_string(minutes) + " 分 ";
    return text + std::to_string(seconds) + " 秒";
}

Scope::Scope(std::string_view area, std::string what) noexcept : started_(std::chrono::steady_clock::now()) {
    try {
        area_ = area;
        what_ = std::move(what);
        Info(area_, "开始：" + what_);
    } catch (...) {}
}

Scope::~Scope() {
    if (finished_) return;
    try {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
        Info(area_, "结束：" + what_ + "（" + std::to_string(elapsed) + " 毫秒）");
    } catch (...) {}
}

void Scope::Succeeded(std::string_view detail) noexcept {
    if (finished_) return;
    finished_ = true;
    try {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
        Info(area_, "完成：" + what_ + "（" + std::to_string(elapsed) + " 毫秒）" + std::string(detail));
    } catch (...) {}
}

void Scope::Failed(std::string_view reason) noexcept {
    if (finished_) return;
    finished_ = true;
    try {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
        Error(area_, "失败：" + what_ + "（" + std::to_string(elapsed) + " 毫秒）：" + std::string(reason));
    } catch (...) {}
}

} // namespace War3Map::RunLog
