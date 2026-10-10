#include "War3MapCore/Script/LuaTemplateTransaction.h"

#include "ExternalProcess.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <utility>
#include <set>

#ifdef _WIN32
#include <windows.h>
#endif

namespace War3Map::Script {
namespace detail {
extern const std::string_view kLuaTemplateHostSource;
extern const std::string_view kLuaTemplateRunnerSource;
} // namespace detail

namespace {

constexpr std::string_view kSubsystem = "Script.LuaTemplate";
// 宿主通过内部结构（upvalue 名称、生成 chunk 形状）绑定到这两个文件；换版本必须重新适配。
constexpr std::string_view kTemplateSha = "A90B1D795038B25EAE14C47F1DB1B0102DBCCFAA5DB61C25E394D1B6B2B39085";
constexpr std::string_view kSlkLibrarySha = "9BA2C9B8E5B53005CF5714E5188E533186D97F770C398E47E998B2B3D3CC74F1";
constexpr std::array<std::string_view, 7> kNativeNames{
    "war3map.w3u", "war3map.w3a", "war3map.w3t", "war3map.w3h", "war3map.w3q", "war3map.w3d", "war3map.w3b"};
constexpr std::size_t kMaxPreprocessed = 16 * 1024 * 1024;

template<class T> Result<T> Fail(std::string code, std::string message) {
    return Result<T>::Failure(std::string(kSubsystem), std::move(code), std::move(message));
}

std::string AsciiLower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool IsAscii(std::wstring_view text) {
    return std::all_of(text.begin(), text.end(), [](wchar_t c) { return c > 0 && c < 0x80; });
}

// lua.exe 用 ANSI 文件接口；非 ASCII 路径改用 8.3 短路径，仍无法表示时拒绝。
std::optional<std::string> AsciiPath(const std::filesystem::path& path) {
    std::wstring text = path.wstring();
#ifdef _WIN32
    if (!IsAscii(text)) {
        const DWORD size = GetShortPathNameW(text.c_str(), nullptr, 0);
        if (size == 0) return std::nullopt;
        std::wstring shortPath(size, L'\0');
        const DWORD written = GetShortPathNameW(text.c_str(), shortPath.data(), size);
        if (written == 0 || written >= size) return std::nullopt;
        shortPath.resize(written);
        text = std::move(shortPath);
    }
#endif
    if (!IsAscii(text)) return std::nullopt;
    std::string ascii;
    for (const wchar_t c : text) ascii.push_back(static_cast<char>(c));
    std::replace(ascii.begin(), ascii.end(), '/', '\\');
    while (ascii.size() > 3 && ascii.back() == '\\') ascii.pop_back();
    return ascii;
}

std::optional<std::string> ReadSmallFile(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes;
    char buffer[65536];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        if (static_cast<std::size_t>(input.gcount()) > limit - bytes.size()) return std::nullopt;
        bytes.append(buffer, static_cast<std::size_t>(input.gcount()));
    }
    return bytes;
}

bool WriteBytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output.flush());
}

// w3x2lni core 读取自身 config.ini 的 [global] data 作为数据集目录。
std::optional<std::string> ReadDataId(const std::filesystem::path& config) {
    const auto text = ReadSmallFile(config, 1024 * 1024);
    if (!text) return std::nullopt;
    bool global = false;
    std::size_t start = 0;
    while (start <= text->size()) {
        auto end = text->find('\n', start);
        if (end == std::string::npos) end = text->size();
        std::string line = text->substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line.erase(0, first);
        if (line.front() == '[') { global = line == "[global]"; continue; }
        if (!global || line.rfind("data", 0) != 0) continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos || line.find_last_not_of(" \t", equals - 1) != 3) continue;
        auto value = line.substr(equals + 1);
        value.erase(0, std::min(value.find_first_not_of(" \t"), value.size()));
        const bool valid = !value.empty() && value.size() <= 64 && value.front() != '.' &&
            std::all_of(value.begin(), value.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.';
            });
        return valid ? std::optional(value) : std::nullopt;
    }
    return std::nullopt;
}

#ifdef _WIN32
// 持有只读共享句柄：进程运行期间文件不能被改写，校验的就是实际读取的内容。
struct FileLease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    FileLease() = default;
    FileLease(const FileLease&) = delete;
    FileLease& operator=(const FileLease&) = delete;
    FileLease(FileLease&& other) noexcept : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)) {}
    ~FileLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
#endif

Status CheckPinned(const std::filesystem::path& path, std::string_view expected, std::string_view label
#ifdef _WIN32
    , std::vector<FileLease>* leases
#endif
) {
#ifdef _WIN32
    FileLease lease;
    lease.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lease.handle == INVALID_HANDLE_VALUE)
        return Status::Failure(std::string(kSubsystem), "LUA_TEMPLATE_TOOL_MISSING", "无法读取 " + std::string(label) + "：" + path.string());
    auto sha = Sha256Handle(lease.handle);
#else
    auto sha = Sha256File(path);
#endif
    if (!sha.Succeeded()) return Status{sha.diagnostics};
    if (AsciiLower(*sha.value) != AsciiLower(std::string(expected))) {
        return Status::Failure(std::string(kSubsystem), "LUA_TEMPLATE_TOOL_VERSION",
            std::string(label) + " 不是已适配的 YDWE 版本（SHA " + *sha.value + "）；模板物编生成需要重新适配后才能使用。");
    }
#ifdef _WIN32
    if (leases) leases->push_back(std::move(lease));
#endif
    return {};
}

void PutU32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}
bool PutString(std::string& out, std::string_view text, bool wide) {
    if (wide) {
        if (text.size() > 0xFFFFFFFFull) return false;
        PutU32(out, static_cast<std::uint32_t>(text.size()));
    } else {
        if (text.size() > 0xFFFF) return false;
        out.push_back(static_cast<char>(text.size() & 0xFF));
        out.push_back(static_cast<char>(text.size() >> 8));
    }
    out.append(text);
    return true;
}

struct Reader {
    std::string_view data;
    std::size_t at = 0;
    bool ok = true;
    std::string_view Take(std::size_t size) {
        if (!ok || size > data.size() - at) { ok = false; return {}; }
        const auto view = data.substr(at, size);
        at += size;
        return view;
    }
    std::uint32_t U32() {
        const auto bytes = Take(4);
        if (!ok) return 0;
        std::uint32_t value = 0;
        for (int i = 3; i >= 0; --i) value = value << 8 | static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]);
        return value;
    }
    std::uint8_t U8() { const auto bytes = Take(1); return ok ? static_cast<std::uint8_t>(bytes[0]) : 0; }
    std::string_view Short() {
        const auto bytes = Take(2);
        if (!ok) return {};
        return Take(static_cast<unsigned char>(bytes[0]) | static_cast<std::size_t>(static_cast<unsigned char>(bytes[1])) << 8);
    }
    std::string_view Long() { const auto size = U32(); return ok ? Take(size) : std::string_view{}; }
};

std::vector<std::string> SplitMessages(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size() && lines.size() < 256) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        if (end > start) lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::string Tail(std::string_view text, std::size_t size = 2048) {
    return std::string(text.size() > size ? text.substr(text.size() - size) : text);
}

} // namespace

Result<LuaTemplateToolchain> ResolveLuaTemplateToolchain(const std::filesystem::path& ydweRoot) {
    std::error_code error;
    if (ydweRoot.empty() || !std::filesystem::is_directory(ydweRoot, error))
        return Fail<LuaTemplateToolchain>("LUA_TEMPLATE_TOOL_MISSING", "未设置 YDWE 目录，无法执行模板物编生成。");
    for (const auto* relative : {"bin/lua.exe", "bin/lua53.dll", "bin/modules/lpeg.dll", "bin/modules/lni.dll",
             "bin/modules/w3xparser.dll", "bin/modules/lml.dll", "plugin/w3x2lni/script/core/init.lua",
             "plugin/w3x2lni/script/backend/sandbox.lua", "compiler/script/computed/order_list.lua",
             "compiler/script/computed/order_id.lua"}) {
        if (!std::filesystem::is_regular_file(ydweRoot / relative, error))
            return Fail<LuaTemplateToolchain>("LUA_TEMPLATE_TOOL_MISSING", std::string("YDWE 缺少模板工具文件：") + relative);
    }
    for (const auto& [relative, sha] : {std::pair{"compiler/script/template.lua", kTemplateSha},
             std::pair{"compiler/script/w3x2lni/slk_lib.lua", kSlkLibrarySha}}) {
        auto checked = CheckPinned(ydweRoot / relative, sha, relative
#ifdef _WIN32
            , nullptr
#endif
        );
        if (!checked.Succeeded()) return Result<LuaTemplateToolchain>{std::nullopt, checked.diagnostics};
    }
    LuaTemplateToolchain toolchain;
    toolchain.ydweRoot = ydweRoot;
    const auto dataId = ReadDataId(ydweRoot / "plugin/w3x2lni/script/core/config.ini");
    if (!dataId || !std::filesystem::is_directory(ydweRoot / "plugin/w3x2lni/data" / *dataId / "prebuilt", error))
        return Fail<LuaTemplateToolchain>("LUA_TEMPLATE_TOOL_MISSING", "无法确定 w3x2lni 数据集目录（core/config.ini [global] data）。");
    toolchain.dataId = *dataId;
    if (!std::filesystem::is_directory(ydweRoot / "share" / toolchain.locale / "mpq", error))
        return Fail<LuaTemplateToolchain>("LUA_TEMPLATE_TOOL_MISSING", "YDWE 缺少 share/zh-CN/mpq 数据。");
    if (!AsciiPath(ydweRoot))
        return Fail<LuaTemplateToolchain>("LUA_TEMPLATE_TOOL_PATH", "YDWE 路径无法以 ASCII 形式传给 lua.exe。");
    return Result<LuaTemplateToolchain>::Success(std::move(toolchain));
}

Result<LuaTemplateOutput> RunLuaTemplateTransaction(std::string_view preprocessed,
    const SlkMapSourceSnapshot& sources, const LuaTemplateToolchain& toolchain,
    const std::filesystem::path& workDir, const LuaTemplateLimits& limits) {
    using Output = LuaTemplateOutput;
    if (preprocessed.size() > kMaxPreprocessed)
        return Fail<Output>("TEMPLATE_LIMIT", "预处理脚本超过 16 MiB，模板宿主拒绝执行。");
    std::error_code error;
    if (workDir.empty() || !std::filesystem::is_directory(workDir, error))
        return Fail<Output>("LUA_TEMPLATE_WORKDIR", "模板执行目录不存在。");
    const auto dir = workDir / "lua-template";
    std::filesystem::create_directories(dir, error);
    const auto root = AsciiPath(toolchain.ydweRoot);
    const auto dirText = AsciiPath(dir);
    if (error || !root || !dirText)
        return Fail<Output>("LUA_TEMPLATE_WORKDIR", "无法建立可传给 lua.exe 的模板执行目录：" + dir.string());
    const auto runner = dir / "LuaTemplateRunner.lua";
    const auto requestPath = dir / "request.bin";
    const auto resultPath = dir / "result.bin";
    std::filesystem::remove(resultPath, error);
    std::filesystem::remove(dir / "result.bin.tmp", error);
    if (!WriteBytes(runner, detail::kLuaTemplateRunnerSource) ||
        !WriteBytes(dir / "LuaTemplateHost.lua", detail::kLuaTemplateHostSource))
        return Fail<Output>("LUA_TEMPLATE_WORKDIR", "无法写入模板宿主文件。");

    // v2 显式传递完整固定域的 Present/Missing；域外不能以清单缺失降级到 nil。
    std::string request = "FORGE-LTQ2";
    bool fits = PutString(request, *root, false) && PutString(request, toolchain.dataId, false) &&
        PutString(request, toolchain.locale, false) && PutString(request, preprocessed, true);
    request.push_back(sources.FinalMemberNamesComplete() ? '\1' : '\0');
    PutU32(request, static_cast<std::uint32_t>(sources.FrontendMembers().size()));
    for (const auto& member : sources.FrontendMembers()) {
        const std::string_view bytes(reinterpret_cast<const char*>(member.bytes.data()), member.bytes.size());
        fits = fits && PutString(request, AsciiLower(member.name), false);
        request.push_back(member.identity.presence == SlkSessionSourcePresence::Present ? '\1' : '\0');
        fits = fits && PutString(request, bytes, true);
    }
    PutU32(request, static_cast<std::uint32_t>(sources.FinalMemberNames().size()));
    for (const auto& name : sources.FinalMemberNames()) fits = fits && PutString(request, AsciiLower(name), false);
    request += "END!";
    if (!fits || !WriteBytes(requestPath, request))
        return Fail<Output>("LUA_TEMPLATE_WORKDIR", "无法写入模板执行请求。");
    request.clear();
    request.shrink_to_fit();

#ifdef _WIN32
    std::vector<FileLease> leases;
    for (const auto& [relative, sha] : {std::pair{"compiler/script/template.lua", kTemplateSha},
             std::pair{"compiler/script/w3x2lni/slk_lib.lua", kSlkLibrarySha}}) {
        auto checked = CheckPinned(toolchain.ydweRoot / relative, sha, relative, &leases);
        if (!checked.Succeeded()) return Result<Output>{std::nullopt, checked.diagnostics};
    }
#endif
    detail::ProcessRequest process;
    process.executable = toolchain.ydweRoot / "bin" / "lua.exe";
    process.arguments = {L"-E", runner.wstring(), requestPath.wstring(), resultPath.wstring()};
    for (auto* argument : {&process.arguments[1], &process.arguments[2], &process.arguments[3]}) {
        const auto ascii = AsciiPath(*argument);
        if (!ascii) return Fail<Output>("LUA_TEMPLATE_WORKDIR", "模板执行路径无法以 ASCII 形式传给 lua.exe。");
        *argument = std::wstring(ascii->begin(), ascii->end());
    }
    process.workingDirectory = dir;
    process.pathPrefix = {toolchain.ydweRoot / "bin"};
    process.timeout = limits.timeout;
    process.outputFile = dir / "lua.log";
    process.maxOutputBytes = 1024 * 1024;
    process.maxJobMemoryBytes = limits.jobMemoryBytes;
    const auto outcome = detail::RunProcess(process);
    if (!outcome.started) return Fail<Output>("LUA_TEMPLATE_START", "无法启动 YDWE lua.exe：" + outcome.error);
    if (outcome.timedOut) return Fail<Output>("TEMPLATE_TIMEOUT", "模板执行超时，已结束进程。");
    if (outcome.outputLimitExceeded || outcome.supervisionFailed)
        return Fail<Output>("TEMPLATE_PROCESS_LIMIT", "模板执行进程超出限制或监督失败：" + outcome.error);
    if (outcome.exitCode != 0)
        return Fail<Output>("TEMPLATE_PROCESS_FAILED", "模板执行进程异常退出（退出码 " + std::to_string(outcome.exitCode) +
            "，可能超出内存限制）：\n" + Tail(outcome.output));

    const auto bytes = ReadSmallFile(resultPath, limits.resultBytes);
    if (!bytes) return Fail<Output>("TEMPLATE_RESULT_INVALID", "模板执行没有产生完整结果。\n" + Tail(outcome.output));
    Reader reader{*bytes};
    if (reader.Take(10) != "FORGE-LTR1") return Fail<Output>("TEMPLATE_RESULT_INVALID", "模板结果格式不符。");
    const auto succeeded = reader.U8();
    Output output;
    std::string failureCode, failureMessage;
    if (succeeded == 1) {
        output.script = std::string(reader.Long());
        const auto count = reader.U32();
        if (reader.ok && count > kNativeNames.size()) return Fail<Output>("TEMPLATE_RESULT_INVALID", "模板结果成员数量超出七类物编。");
        std::set<std::string> seen;
        for (std::uint32_t i = 0; reader.ok && i < count; ++i) {
            const auto name = AsciiLower(std::string(reader.Short()));
            const auto data = reader.Long();
            if (!reader.ok) break;
            if (std::find(kNativeNames.begin(), kNativeNames.end(), name) == kNativeNames.end() || !seen.insert(name).second)
                return Fail<Output>("TEMPLATE_RESULT_INVALID", "模板结果含未知或重复成员：" + name);
            const auto* first = reinterpret_cast<const std::byte*>(data.data());
            output.objectMembers.push_back({name, std::vector<std::byte>(first, first + data.size())});
        }
    } else if (succeeded == 0) {
        failureCode = std::string(reader.Short());
        failureMessage = std::string(reader.Long());
    } else {
        return Fail<Output>("TEMPLATE_RESULT_INVALID", "模板结果状态无效。");
    }
    output.toolMessages = SplitMessages(reader.Long());
    if (!reader.ok || reader.Take(4) != "END!" || reader.at != bytes->size())
        return Fail<Output>("TEMPLATE_RESULT_INVALID", "模板结果被截断或含多余数据。");
    if (succeeded == 0) {
        auto result = Fail<Output>(failureCode.empty() ? "TEMPLATE_HOST_FAILED" : failureCode,
            "模板执行失败：" + failureMessage.substr(0, 4096));
        for (std::size_t i = 0; i < output.toolMessages.size() && i < 20; ++i)
            result.diagnostics.push_back({DiagnosticSeverity::Info, std::string(kSubsystem), "TEMPLATE_TOOL_MESSAGE", output.toolMessages[i], {}});
        return result;
    }
    return Result<Output>::Success(std::move(output));
}

} // namespace War3Map::Script
