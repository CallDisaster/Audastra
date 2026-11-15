#include "Fixtures.h"

#include "War3MapCore/Feedback/ErrorReport.h"
#include "War3MapCore/Feedback/RunLog.h"
#include "War3MapCore/IO/FileBytes.h"

#include <cstdlib>
#include <iostream>

using namespace War3Map;
using namespace FeedbackFixtures;

namespace {

namespace fs = std::filesystem;

void WriteText(const fs::path& path, std::string_view text) {
    Require(IO::WriteFileBytes(path, std::span(reinterpret_cast<const std::byte*>(text.data()), text.size())).Succeeded(),
        "write owned fixture file");
}

std::string ZipMember(const std::vector<std::byte>& zip, const char* name) {
    mz_zip_archive archive{};
    Require(mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0) != 0, "reopen error report ZIP");
    std::size_t size = 0;
    void* heap = mz_zip_reader_extract_file_to_heap(&archive, name, &size, 0);
    std::string result = heap ? std::string(static_cast<const char*>(heap), size) : std::string("<missing>");
    if (heap) mz_free(heap);
    mz_zip_reader_end(&archive);
    return result;
}

// 运行日志：会话信息每段重复、一轮总量受限、上次没有正常退出的运行能被找到、旧运行按份数清理。
void RunLogLifecycle(const fs::path& root) {
    const auto directory = root / "logs";
    fs::create_directories(directory);
    // 上次运行：标记里的进程已不存在（序号 0 不是有效进程）。
    WriteText(directory / "audastra-20200101-000000-11.log", "earlier unclean run\r\n");
    WriteText(directory / "audastra-20200101-000000-11.prev.log", "earlier unclean run first part\r\n");
    WriteText(directory / "running-0.txt", IO::PathUtf8(directory / "audastra-20200101-000000-11.log"));
    // 更早的几次运行，超过保留份数的要删掉。
    for (int i = 0; i < 4; ++i) WriteText(directory / ("audastra-2019010" + std::to_string(i) + "-000000-1.log"), "old\r\n");

    RunLog::Options options;
    options.directory = directory;
    options.header = "版本 测试\n参数 --fixture";
    options.maxBytesPerRun = 64 * 1024;
    options.keepRuns = 3;
    Require(RunLog::Start(options).Succeeded() && RunLog::Active(), "start run log");
    Require(!RunLog::Start(options).Succeeded(), "run log starts only once per process");
    const auto unclean = RunLog::UncleanPreviousRunFiles();
    Require(unclean.size() == 2 && unclean.back().filename() == "audastra-20200101-000000-11.log"
        && unclean.front().filename() == "audastra-20200101-000000-11.prev.log", "previous unclean run and its first part are found");
    Require(!fs::exists(directory / "running-0.txt"), "stale marker of a dead process is consumed");
    std::size_t runs = 0;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().extension() == ".log" && entry.path().filename().string().find(".prev.") == std::string::npos) ++runs;
    Require(runs == 3, "only the newest runs are kept, including this one");
    Require(fs::exists(directory / "audastra-20200101-000000-11.log"), "the unclean run is recent enough to be kept");

    RunLog::Info("测试", "第一行\n第二行");
    {
        RunLog::Scope scope("保存", "fixture.w3x");
        scope.Failed("磁盘已满");
    }
    {
        RunLog::Scope scope("打开", "other.w3x");
    }
    const auto early = Read(RunLog::SessionFiles().back());
    Require(early.find("[测试] 第一行\r\n    第二行") != std::string::npos, "multi-line messages are indented");
    Require(early.find("开始：fixture.w3x") != std::string::npos && early.find("失败：fixture.w3x（") != std::string::npos
        && early.find("磁盘已满") != std::string::npos && early.find("[错误]") != std::string::npos, "a failed scope records its reason");
    Require(early.find("结束：other.w3x（") != std::string::npos, "an unreported scope still records its end");
    const std::string filler(1000, 'x');
    for (int i = 0; i < 120; ++i) RunLog::Warning("填充", filler);
    const auto files = RunLog::SessionFiles();
    Require(files.size() == 2 && files.front().filename().string().ends_with(".prev.log"), "a full segment rotates to .prev.log");
    std::uint64_t total = 0;
    for (const auto& file : files) total += fs::file_size(file);
    Require(total <= 64 * 1024 + 4096, "one run stays within its byte limit");
    const auto current = Read(files.back());
    Require(current.starts_with("==== Audastra 运行日志 · 第 ") && current.find("第 1 段") == std::string::npos
        && current.find("本轮更早的记录在") != std::string::npos, "new segment has its own header and points to the earlier part");
    Require(current.find("参数 --fixture") != std::string::npos, "session header repeats in every segment");
    Require(current.find("[警告]") != std::string::npos && current.find("[填充]") != std::string::npos, "level and area are written");
    const auto marker = directory / ("running-" + std::to_string(GetCurrentProcessId()) + ".txt");
    Require(fs::exists(marker), "this run has a running marker");
    RunLog::Stop("测试结束");
    Require(!RunLog::Active() && !fs::exists(marker), "clean stop removes the running marker");
    Require(Read(files.back()).find("正常退出：测试结束") != std::string::npos, "clean stop is recorded");
    RunLog::Info("测试", "after stop");
    Require(Read(files.back()).find("after stop") == std::string::npos, "nothing is written after stop");
    Require(RunLog::FormatDuration(std::chrono::seconds(3723)) == "1 小时 2 分 3 秒", "durations read naturally");
}


void ErrorReportPackage(const fs::path& root) {
    const auto profile = fs::path(L"C:\\Users\\SyntheticUser");
    Require(Feedback::MaskUserProfile("C:\\Users\\SyntheticUser\\Desktop\\a.w3x c:/users/syntheticuser/x C:\\\\Users\\\\SyntheticUser\\\\y", profile)
        == "%USERPROFILE%\\Desktop\\a.w3x %USERPROFILE%/x %USERPROFILE%\\\\y", "user directory is masked in every spelling");
    Require(Feedback::MaskUserProfile("Maps\\SyntheticUser\\map.w3x", profile) == "Maps\\SyntheticUser\\map.w3x", "other paths are kept");

    const auto log = root / "report-log.txt";
    const auto dump = root / "report.dmp";
    wchar_t homeBuffer[1024]{};
    Require(GetEnvironmentVariableW(L"USERPROFILE", homeBuffer, 1024) > 0, "USERPROFILE is set");
    const auto home = IO::PathUtf8(fs::path(homeBuffer));
    WriteText(log, "打开 " + home + "\\Desktop\\map.w3x\r\n");
    WriteText(dump, std::string("MDMP\0binary", 11));
    Feedback::ErrorReportInput input;
    input.description = "点保存后卡住\n然后闪退";
    input.sections.push_back({"程序", {{"版本", "0.5.4"}, {"安装目录", home + "\\Audastra"}}});
    input.diagnostics.push_back("[错误] 保存 SAVE_FAILED 写入失败");
    input.files.push_back({"logs/current.log", log, true});
    input.files.push_back({"dumps/crash.dmp", dump, false});
    input.files.push_back({"logs/missing.log", root / "absent.log", true});
    const auto text = Feedback::RenderErrorReportText(input);
    Require(text.find("【经过】\r\n点保存后卡住\r\n然后闪退") != std::string::npos, "description keeps its lines");
    Require(text.find("版本：0.5.4") != std::string::npos && text.find("SAVE_FAILED") != std::string::npos, "sections and diagnostics are listed");
    Require(text.find("%USERPROFILE%\\Audastra") != std::string::npos && text.find(home) == std::string::npos, "report text masks the user directory");
    Require(text.find("logs/missing.log（无法读取）") != std::string::npos, "unreadable attachments are named");
    const auto zip = Take(Feedback::BuildErrorReportZip(input), "build error report ZIP");
    Require(ZipMember(zip, "report.txt").starts_with("\xEF\xBB\xBF" "Audastra 错误报告"), "report.txt opens as UTF-8");
    const auto zippedLog = ZipMember(zip, "logs/current.log");
    Require(zippedLog.find("%USERPROFILE%\\Desktop\\map.w3x") != std::string::npos && zippedLog.find(home) == std::string::npos,
        "attached text logs mask the user directory");
    Require(ZipMember(zip, "dumps/crash.dmp") == std::string("MDMP\0binary", 11), "binary attachments are kept byte for byte");
    Require(ZipMember(zip, "logs/missing.log.missing.txt").find("无法读取") != std::string::npos, "missing attachments leave a note");
    const auto target = root / "out" / "report.zip";
    fs::create_directories(target.parent_path());
    Require(Feedback::WriteErrorReport(target, input).Succeeded() && fs::file_size(target) > 0, "report is written to the chosen path");
    Require(!fs::exists(fs::path(target) += ".part"), "no partial file is left behind");
    Require(!Feedback::WriteErrorReport("relative.zip", input).Succeeded(), "relative targets are refused");
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path() / ("audastra-runlog-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    try {
        RunLogLifecycle(root);
        ErrorReportPackage(root);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        fs::remove_all(root, ignored);
        return 1;
    }
    fs::remove_all(root, ignored);
    std::cout << "run log and error report checks passed: " << checks << '\n';
    return 0;
}
