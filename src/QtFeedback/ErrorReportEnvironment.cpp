#include "ErrorReportEnvironment.h"

#include "AudastraVersion.generated.h"
#include "War3MapCore/Feedback/RunLog.h"
#include "War3MapCore/IO/FileBytes.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QLocale>
#include <QScreen>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTimeZone>

#include <windows.h>
#include <dbghelp.h>
#include <dxgi.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>

namespace War3Map::QtFeedback {
namespace {

namespace fs = std::filesystem;
using Rows = std::vector<std::pair<std::string, std::string>>;

std::string Utf8(const QString& text) { return text.toStdString(); }

std::string Size(std::uint64_t bytes) {
    char text[32]{};
    if (bytes >= 1024ull * 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024));
    else std::snprintf(text, sizeof(text), "%llu KB", static_cast<unsigned long long>(bytes / 1024));
    return text;
}

std::string Wide(const wchar_t* text) {
    return QString::fromWCharArray(text).toStdString();
}

std::wstring RegistryText(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[512]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

std::optional<DWORD> RegistryNumber(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD number = 0, size = sizeof(number);
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &number, &size) != ERROR_SUCCESS) return std::nullopt;
    return number;
}

bool Elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

Feedback::ErrorReportSection Program(const QString& buildStamp) {
    Rows rows;
    rows.emplace_back("版本", std::string(BuildInfo::Version));
    rows.emplace_back("提交", BuildInfo::Commit.empty() ? std::string("未知") : std::string(BuildInfo::Commit));
    if (!buildStamp.isEmpty()) rows.emplace_back("构建", Utf8(buildStamp));
    rows.emplace_back("程序", Utf8(QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
    rows.emplace_back("设置档", Utf8(QCoreApplication::applicationName()));
    rows.emplace_back("进程", std::to_string(GetCurrentProcessId()) + (Elevated() ? "（以管理员身份运行）" : ""));
    const auto started = QDateTime::fromSecsSinceEpoch(std::chrono::duration_cast<std::chrono::seconds>(
        RunLog::Started().time_since_epoch()).count());
    rows.emplace_back("本次启动", Utf8(started.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    rows.emplace_back("已运行", RunLog::FormatDuration(RunLog::Uptime()));
    rows.emplace_back("报告时间", Utf8(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    rows.emplace_back("Qt", qVersion());
    rows.emplace_back("日志目录", RunLog::Directory().empty() ? std::string("本次未开启") : IO::PathUtf8(RunLog::Directory()));
    return {"程序", std::move(rows)};
}

Feedback::ErrorReportSection System() {
    Rows rows;
    constexpr auto key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string build = Utf8(QSysInfo::kernelVersion());
    if (const auto ubr = RegistryNumber(HKEY_LOCAL_MACHINE, key, L"UBR")) build += "." + std::to_string(*ubr);
    rows.emplace_back("Windows", Utf8(QSysInfo::prettyProductName()));
    rows.emplace_back("内部版本", build);
    if (const auto display = RegistryText(HKEY_LOCAL_MACHINE, key, L"DisplayVersion"); !display.empty())
        rows.emplace_back("功能更新", Wide(display.c_str()));
    if (const auto edition = RegistryText(HKEY_LOCAL_MACHINE, key, L"EditionID"); !edition.empty())
        rows.emplace_back("版本类型", Wide(edition.c_str()));
    rows.emplace_back("体系结构", Utf8(QSysInfo::currentCpuArchitecture()));
    rows.emplace_back("系统语言", Utf8(QLocale::system().name()) + "；界面语言 " + Utf8(QLocale::system().uiLanguages().join(QStringLiteral("、"))));
    rows.emplace_back("代码页", "ANSI " + std::to_string(GetACP()) + "，OEM " + std::to_string(GetOEMCP()));
    const auto zone = QTimeZone::systemTimeZone();
    const auto offset = zone.offsetFromUtc(QDateTime::currentDateTime()) / 60;
    char utc[24]{};
    std::snprintf(utc, sizeof(utc), "UTC%c%02d:%02d", offset < 0 ? '-' : '+', std::abs(offset) / 60, std::abs(offset) % 60);
    rows.emplace_back("时区", Utf8(QString::fromUtf8(zone.id())) + "（" + utc + "）");
    rows.emplace_back("系统已运行", RunLog::FormatDuration(std::chrono::milliseconds(GetTickCount64())));
    return {"系统", std::move(rows)};
}

Feedback::ErrorReportSection Processor() {
    Rows rows;
    const auto name = RegistryText(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    rows.emplace_back("型号", name.empty() ? std::string("未知") : Utf8(QString::fromWCharArray(name.c_str()).trimmed()));
    rows.emplace_back("逻辑处理器", std::to_string(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    return {"处理器", std::move(rows)};
}

Feedback::ErrorReportSection Memory() {
    Rows rows;
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        rows.emplace_back("物理内存", Size(status.ullTotalPhys) + "，可用 " + Size(status.ullAvailPhys)
            + "（已用 " + std::to_string(status.dwMemoryLoad) + "%）");
        rows.emplace_back("提交上限", Size(status.ullTotalPageFile) + "，可用 " + Size(status.ullAvailPageFile));
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        rows.emplace_back("本程序工作集", Size(counters.WorkingSetSize) + "，峰值 " + Size(counters.PeakWorkingSetSize));
        rows.emplace_back("本程序专用内存", Size(counters.PrivateUsage));
    }
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    rows.emplace_back("句柄 / GDI / USER", std::to_string(handles) + " / " + std::to_string(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS))
        + " / " + std::to_string(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)));
    return {"内存", std::move(rows)};
}

Feedback::ErrorReportSection Graphics() {
    Rows rows;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        rows.emplace_back("显卡", "无法查询");
        return {"显卡", std::move(rows)};
    }
    for (UINT index = 0; index < 8; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            std::string value = Wide(desc.Description);
            char ids[48]{};
            std::snprintf(ids, sizeof(ids), "（%04X:%04X）", desc.VendorId, desc.DeviceId);
            value += ids;
            value += "，显存 " + Size(desc.DedicatedVideoMemory) + "，共享 " + Size(desc.SharedSystemMemory);
            LARGE_INTEGER driver{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
                char version[64]{};
                std::snprintf(version, sizeof(version), "，驱动 %u.%u.%u.%u",
                    HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart));
                value += version;
            }
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) value += "（软件）";
            rows.emplace_back("显卡 " + std::to_string(index + 1), value);
        }
        adapter->Release();
    }
    factory->Release();
    if (rows.empty()) rows.emplace_back("显卡", "没有找到");
    return {"显卡", std::move(rows)};
}

Feedback::ErrorReportSection Screens() {
    Rows rows;
    const auto* primary = QGuiApplication::primaryScreen();
    int index = 0;
    for (const auto* screen : QGuiApplication::screens()) {
        const auto ratio = screen->devicePixelRatio();
        const auto size = screen->geometry().size() * ratio;
        char scale[32]{};
        std::snprintf(scale, sizeof(scale), "%.0f%%", ratio * 100);
        rows.emplace_back("显示器 " + std::to_string(++index), Utf8(screen->name()) + "，" + std::to_string(size.width()) + "×"
            + std::to_string(size.height()) + "，缩放 " + scale + "，" + std::to_string(static_cast<int>(screen->refreshRate() + 0.5))
            + " Hz" + (screen == primary ? "（主显示器）" : ""));
    }
    if (rows.empty()) rows.emplace_back("显示器", "没有找到");
    return {"显示器", std::move(rows)};
}

Feedback::ErrorReportSection Disks() {
    Rows rows;
    const std::pair<const char*, QString> places[] = {
        {"程序所在盘", QCoreApplication::applicationDirPath()},
        {"日志所在盘", QString::fromStdWString(RunLog::Directory().wstring())},
        {"临时目录所在盘", QDir::tempPath()}};
    std::vector<QString> seen;
    for (const auto& [label, path] : places) {
        if (path.isEmpty()) continue;
        const QStorageInfo storage(path);
        if (!storage.isValid() || std::find(seen.begin(), seen.end(), storage.rootPath()) != seen.end()) continue;
        seen.push_back(storage.rootPath());
        rows.emplace_back(label, Utf8(QDir::toNativeSeparators(storage.rootPath())) + " " + Utf8(QString::fromUtf8(storage.fileSystemType()))
            + "，可用 " + Size(static_cast<std::uint64_t>(storage.bytesAvailable())) + " / " + Size(static_cast<std::uint64_t>(storage.bytesTotal())));
    }
    return {"磁盘", std::move(rows)};
}

std::wstring crashDirectory;
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
std::terminate_handler previousTerminate = nullptr;
volatile LONG crashing = 0;

std::wstring DumpPath() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t name[96]{};
    swprintf_s(name, L"\\crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", unsigned{time.wYear}, unsigned{time.wMonth},
        unsigned{time.wDay}, unsigned{time.wHour}, unsigned{time.wMinute}, unsigned{time.wSecond}, GetCurrentProcessId());
    return crashDirectory + name;
}

bool WriteDump(const std::wstring& path, EXCEPTION_POINTERS* info) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
    const bool written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, info ? &exception : nullptr, nullptr, nullptr) != FALSE;
    CloseHandle(file);
    return written;
}

std::string Location(void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module) || !module) return "未知模块";
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    char offset[32]{};
    std::snprintf(offset, sizeof(offset), "+0x%llx", static_cast<unsigned long long>(
        reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module)));
    return IO::PathUtf8(fs::path(path).filename()) + offset;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    if (InterlockedExchange(&crashing, 1) == 0) {
        const auto path = DumpPath();
        const bool dumped = WriteDump(path, info);
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lX", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul);
        RunLog::Error("崩溃", std::string("程序崩溃：异常 ") + code + "，位置 "
            + (info && info->ExceptionRecord ? Location(info->ExceptionRecord->ExceptionAddress) : std::string("未知"))
            + (dumped ? "；转储 " + IO::PathUtf8(fs::path(path).filename()) : std::string("；转储没有写成")));
    }
    return previousFilter ? previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void Terminate() {
    if (InterlockedExchange(&crashing, 1) == 0) {
        std::string what = "未知异常";
        try {
            if (const auto current = std::current_exception()) std::rethrow_exception(current);
            what = "没有活动异常";
        } catch (const std::exception& error) {
            what = std::string("std::exception：") + error.what();
        } catch (...) {}
        const auto path = DumpPath();
        const bool dumped = WriteDump(path, nullptr);
        RunLog::Error("崩溃", "程序因未捕获的异常终止：" + what
            + (dumped ? "；转储 " + IO::PathUtf8(fs::path(path).filename()) : std::string("；转储没有写成")));
    }
    if (previousTerminate) previousTerminate();
    std::abort();
}

} // namespace

std::vector<Feedback::ErrorReportSection> CollectErrorReportEnvironment(const QString& buildStamp) {
    return {Program(buildStamp), System(), Processor(), Memory(), Graphics(), Screens(), Disks()};
}

QString FileVersion(const fs::path& path) {
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return {};
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info || length < sizeof(VS_FIXEDFILEINFO)) return {};
    return QStringLiteral("%1.%2.%3.%4").arg(HIWORD(info->dwFileVersionMS)).arg(LOWORD(info->dwFileVersionMS))
        .arg(HIWORD(info->dwFileVersionLS)).arg(LOWORD(info->dwFileVersionLS));
}

std::vector<fs::path> RecentCrashDumps(const fs::path& directory, std::size_t count) {
    std::vector<fs::path> dumps;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto name = IO::PathUtf8(it->path().filename());
        if (name.starts_with("crash-") && name.ends_with(".dmp")) dumps.push_back(it->path());
    }
    std::sort(dumps.begin(), dumps.end(), [](const auto& left, const auto& right) { return left.filename() > right.filename(); });
    if (dumps.size() > count) dumps.resize(count);
    return dumps;
}

void InstallCrashCapture(const fs::path& directory) {
    if (!crashDirectory.empty() || directory.empty()) return;
    crashDirectory = directory.wstring();
    // 只保留最近 5 个转储。
    const auto all = RecentCrashDumps(directory, 1000);
    std::error_code error;
    for (std::size_t i = 5; i < all.size(); ++i) fs::remove(all[i], error);
    previousFilter = SetUnhandledExceptionFilter(CrashFilter);
    previousTerminate = std::set_terminate(Terminate);
}

} // namespace War3Map::QtFeedback
