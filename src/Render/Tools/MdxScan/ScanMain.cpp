#include "ScanTypes.h"
#include <charconv>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include "ScanPreview.h"
#include <Windows.h>
#else
#include <array>
#include <unistd.h>
#endif
namespace {
using namespace War3Map::Render::Scan;
std::filesystem::path Executable() {
#ifdef _WIN32
    std::wstring path(32768,L'\0');
    const auto n = GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) throw std::runtime_error("SCAN_EXECUTABLE_PATH_FAILED");
    path.resize(n); return std::filesystem::path(path);
#else
    std::array<char,16384> path{};
    const auto n = readlink("/proc/self/exe",path.data(),path.size());
    if (n <= 0 || static_cast<std::size_t>(n) >= path.size()) throw std::runtime_error("SCAN_EXECUTABLE_PATH_FAILED");
    return std::filesystem::path(std::string(path.data(),static_cast<std::size_t>(n)));
#endif
}
int Main(const std::vector<std::string>& args) {
    try {
        if (args.size() == 1 && args[0] == "--help") {
            std::cout << "war3-mdx-scan --game-root <游戏根> [--list <UTF-8虚拟路径清单>] --tsv <新报告.tsv>\n"
                "war3-mdx-scan --list <UTF-8磁盘模型路径清单> --tsv <新报告.tsv>\n"
                "Windows 宿主：--preview 1 [--map <只读地图副本>]，同时报告 strict/interactive 诊断分类。\n"
                "游戏地形集：--preview 1 --game-root <游戏根> --tileset <A-Z>；可搭配 --list 冻结该上下文的模型名单。\n"
                "预算：--model-ms 5000 --total-seconds 300 --memory-mib 512 --max-models 10000\n"
                "      --file-mib 32 --max-samples 4096 --max-nodes 4096 --max-vertices 262144\n"
                "只读、分进程；每动作起点/中点/非按键点，分别无相机及显式模型局部相机。\n"
                "列表相对路径相对清单目录；game-root模式则为MPQ内部名。已有输出不覆盖。\n"
                "返回：0=已选范围扫完且无差分；2=范围/预算未覆盖；3=发现差分；1=工具/输入错误。\n";
            return 0;
        }
        Options options; options.executable = Executable(); std::set<std::string> seen;
        bool preview = false;
        std::filesystem::path mapPath;
        std::optional<std::uint8_t> tileset;
        for (std::size_t i = 0; i < args.size(); i += 2) {
            if (i+1 == args.size() || !seen.insert(args[i]).second) throw std::runtime_error("SCAN_ARGUMENT_INVALID");
            const auto& key = args[i]; const auto& value = args[i+1];
            if (key == "--game-root") options.gameRoot = std::filesystem::absolute(FromUtf8(value));
            else if (key == "--list") options.list = std::filesystem::absolute(FromUtf8(value));
            else if (key == "--tsv") options.tsv = std::filesystem::absolute(FromUtf8(value));
            else if (key == "--model") options.model = value;
            else if (key == "--internal") options.mode = value;
            else if (key == "--preview" && value == "1") preview = true;
            else if (key == "--map") mapPath = std::filesystem::absolute(FromUtf8(value));
            else if (key == "--tileset") {
                if (value.size() != 1 || value.front() < 'A' || value.front() > 'Z')
                    throw std::runtime_error("SCAN_TILESET_INVALID");
                tileset = static_cast<std::uint8_t>(value.front());
            }
            else {
                std::uint32_t number = 0;
                const auto parsed = std::from_chars(value.data(),value.data()+value.size(),number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size() || !number) throw std::runtime_error("SCAN_BUDGET_INVALID");
                auto& v = options.limits;
                if (key == "--model-ms" && number <= 60000) v.modelMs = number;
                else if (key == "--total-seconds" && number <= 86400) v.totalSeconds = number;
                else if (key == "--memory-mib" && number >= 32 && number <= 2048) v.memoryMiB = number;
                else if (key == "--max-models" && number <= 100000) v.maxModels = number;
                else if (key == "--file-mib" && number <= 32) v.fileMiB = number;
                else if (key == "--max-samples" && number <= 32768) v.maxSamples = number;
                else if (key == "--max-nodes" && number <= 16384) v.maxNodes = number;
                else if (key == "--max-vertices" && number <= 1048576) v.maxVertices = number;
                else throw std::runtime_error("SCAN_ARGUMENT_OR_BUDGET_INVALID");
            }
        }
        if (options.tsv.empty() || (!options.mode.empty() && options.mode != "model" && options.mode != "enumerate")
            || (options.mode == "model" ? options.model.empty() : options.gameRoot.empty() && options.list.empty() && mapPath.empty()))
            throw std::runtime_error("SCAN_REQUIRED_ARGUMENT_MISSING");
        if (!mapPath.empty() && !preview) throw std::runtime_error("SCAN_MAP_REQUIRES_PREVIEW");
        if (tileset && !preview) throw std::runtime_error("SCAN_TILESET_REQUIRES_PREVIEW");
        if (tileset && options.gameRoot.empty()) throw std::runtime_error("SCAN_TILESET_REQUIRES_GAME_ROOT");
#ifdef _WIN32
        if (preview) return RunPreview(options, mapPath, tileset);
#else
        if (preview) throw std::runtime_error("SCAN_PREVIEW_REQUIRES_WINDOWS");
#endif
        return Run(options);
    } catch (const std::exception& e) { std::cerr << "SCAN_ERROR " << e.what() << '\n'; return 1; }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(War3Map::Render::Scan::Utf8(std::filesystem::path(argv[i])));
    return Main(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return Main(args);
}
#endif
