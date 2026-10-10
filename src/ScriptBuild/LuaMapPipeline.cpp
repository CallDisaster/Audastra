#include "War3MapCore/Script/LuaMapPipeline.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "ExternalProcess.h"
#include "LuaMapScriptInternal.h"
#include "ScriptSourceBinding.h"

#include <Windows.h>
#include <algorithm>
#include <exception>
#include <system_error>

namespace War3Map::Script {
struct LuaSyntaxChecker::State { LuaSyntaxCheckerIdentity identity; };

namespace {
using Kind = ScriptToolchainBindingKind;
constexpr std::uint64_t kCheckerBytes = 128ull * 1024 * 1024;
constexpr std::uint64_t kScriptBytes = 64ull * 1024 * 1024;
constexpr std::string_view kMarker = "FORGE_LUA_SYNTAX_OK_V1 ";
// 固定runner仅验证宿主前提、解析文本。chunk永远不调用；不读任何地图归档成员。
constexpr std::string_view kRunner = R"lua(assert(_VERSION == "Lua 5.3", "LUA_CHECKER_VERSION: expected Lua 5.3")
assert(type(math.type) == "function" and math.type(1) == "integer" and math.type(1.0) == "float",
    "LUA_CHECKER_NUMERIC_MODEL: integer and float variants required")
assert(math.maxinteger == 0x7fffffffffffffff and math.mininteger == -0x7fffffffffffffff - 1,
    "LUA_CHECKER_INTEGER_WIDTH: 64-bit host integers required")
assert((-1 & 0xffffffff) == 4294967295 and (0x100000000 >> 32) == 1 and (-9 // 4) == -3 and (9 / 4) == 2.25,
    "LUA_CHECKER_OPERATORS: expected Lua 5.3 bit and division behavior")
assert(type(arg[1]) == "string" and #arg[1] == 64 and not arg[1]:find("[^0-9a-f]"), "LUA_CHECKER_REQUEST_ID")
local chunk, problem = loadfile("war3map.lua", "t", {})
if not chunk then
    io.stderr:write("LUA_MAP_SYNTAX: ", tostring(problem), "\n")
    os.exit(21, false)
end
print("FORGE_LUA_SYNTAX_OK_V1 " .. arg[1])
)lua";

bool Digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
std::string CanonicalDigest(std::string_view value) {
    std::string result(value);
    for (char& c : result) if (c >= 'A' && c <= 'F') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}
bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.native().size()), right.c_str(),
        static_cast<int>(right.native().size()), TRUE) == CSTR_EQUAL;
}
template<class T>
Result<T> CaptureFailure(const std::filesystem::path& path, const char* code, std::string message,
    const std::vector<Diagnostic>& cause = {}) {
    auto result = Result<T>::Failure("Script.LuaPipeline", code, std::move(message));
    result.diagnostics.back().origin = OriginSpan{path};
    result.diagnostics.insert(result.diagnostics.end(), cause.begin(), cause.end());
    return result;
}
struct PipelineFailure { Diagnostic diagnostic; };
[[noreturn]] void Fail(const char* code, std::string message, const std::filesystem::path& path = {}) {
    Diagnostic diagnostic{DiagnosticSeverity::Error, "Script.LuaPipeline", code, std::move(message), {}};
    if (!path.empty()) diagnostic.origin = OriginSpan{path};
    throw PipelineFailure{std::move(diagnostic)};
}
void VerifyFile(const std::filesystem::path& path, std::string_view hash, std::uint64_t bytes, std::uint64_t maximum,
    const char* code, std::vector<Diagnostic>& diagnostics) {
    const auto captured = Detail::CaptureSourceBinding(path, Kind::RegularFile, true, maximum);
    if (!captured.Succeeded()) {
        diagnostics.insert(diagnostics.end(), captured.diagnostics.begin(), captured.diagnostics.end());
        Fail(code, "无法重新确认绑定文件。", path);
    }
    if (captured.value->binding.sha256 != hash || captured.value->binding.bytes != bytes)
        Fail(code, "文件实际字节与捕获身份不一致。", path);
}
void VerifyChecker(const LuaSyntaxCheckerIdentity& identity, std::vector<Diagnostic>& diagnostics) {
    VerifyFile(identity.executable, identity.sha256, identity.bytes, kCheckerBytes, "LUA_CHECKER_CHANGED", diagnostics);
    for (const auto& path : identity.runtimeDirectories) {
        const auto directory = Detail::CaptureSourceBinding(path, Kind::Directory, true, 0);
        if (!directory.Succeeded()) {
            diagnostics.insert(diagnostics.end(), directory.diagnostics.begin(), directory.diagnostics.end());
            Fail("LUA_CHECKER_RUNTIME_DIRECTORY_CHANGED", "显式运行时目录缺失或路径链变化。", path);
        }
    }
}
void WriteNewFile(const std::filesystem::path& path, std::string_view text) {
    struct File {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } file;
    file.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) Fail("LUA_PIPELINE_WRITE_FAILED", "独占输入文件创建失败（Win32 " + std::to_string(GetLastError()) + "）。", path);
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file.value, text.data() + offset, count, &written, nullptr) || written == 0)
            Fail("LUA_PIPELINE_WRITE_FAILED", "独占输入文件写入不完整。", path);
        offset += written;
    }
    if (!FlushFileBuffers(file.value)) Fail("LUA_PIPELINE_WRITE_FAILED", "独占输入文件刷新失败。", path);
}
struct Stage {
    LuaMapPipelineResult& result;
    std::size_t index;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    Stage(LuaMapPipelineResult& value, std::string name, std::filesystem::path output,
        const std::function<void(std::string_view)>& callback) : result(value), index(value.stages.size()) {
        result.stages.push_back({std::move(name), std::move(output), {}, -1, {}});
        if (callback) callback(result.stages[index].name);
    }
    ~Stage() {
        result.stages[index].elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    }
    PipelineStage& Value() { return result.stages[index]; }
    void Pass(std::string log = {}) { Value().exitCode = 0; Value().log = std::move(log); }
};
std::string ProcessLog(const detail::ProcessOutcome& outcome, const std::filesystem::path& log) {
    const auto flag = [](bool value) { return value ? "true" : "false"; };
    std::string result = "started=" + std::string(flag(outcome.started)) + " timedOut=" + flag(outcome.timedOut)
        + " outputLimitExceeded=" + flag(outcome.outputLimitExceeded) + " supervisionFailed=" + flag(outcome.supervisionFailed)
        + " actualExitCode=" + std::to_string(outcome.exitCode) + "\n";
    const auto encoded = log.u8string();
    result += "rawLog=";
    for (const auto c : encoded) result.push_back(static_cast<char>(c));
    result += "\n";
    result += outcome.output;
    if (!outcome.error.empty()) result += "\nprocessError=" + outcome.error;
    return result;
}
}

const LuaSyntaxCheckerIdentity* LuaSyntaxChecker::Identity() const noexcept {
    return state_ ? &state_->identity : nullptr;
}

Result<LuaSyntaxChecker> CaptureLuaSyntaxChecker(const std::filesystem::path& executable,
    std::string_view expectedSha256, std::span<const std::filesystem::path> runtimeDirectories) {
    try {
        if (!Digest(expectedSha256)) return CaptureFailure<LuaSyntaxChecker>(executable, "LUA_CHECKER_EXPECTED_SHA_INVALID",
            "语法检查器必须具有明确的64位SHA256，而不是隐式信任路径。");
        if (runtimeDirectories.size() > 8) return CaptureFailure<LuaSyntaxChecker>(executable, "LUA_CHECKER_RUNTIME_DIRECTORY_BUDGET",
            "显式运行时目录最多8项。");
        const auto captured = Detail::CaptureSourceBinding(executable, Kind::RegularFile, true, kCheckerBytes);
        if (!captured.Succeeded()) return CaptureFailure<LuaSyntaxChecker>(executable, "LUA_CHECKER_CAPTURE_FAILED",
            "无法有界捕获语法检查器。", captured.diagnostics);
        if (captured.value->binding.bytes == 0 || captured.value->binding.sha256 != CanonicalDigest(expectedSha256))
            return CaptureFailure<LuaSyntaxChecker>(executable, "LUA_CHECKER_SHA_MISMATCH", "语法检查器内容与指定SHA256不一致。");
        auto state = std::make_shared<LuaSyntaxChecker::State>();
        state->identity.executable = captured.value->binding.path;
        state->identity.sha256 = captured.value->binding.sha256;
        state->identity.bytes = captured.value->binding.bytes;
        for (const auto& path : runtimeDirectories) {
            const auto directory = Detail::CaptureSourceBinding(path, Kind::Directory, true, 0);
            if (!directory.Succeeded()) return CaptureFailure<LuaSyntaxChecker>(path, "LUA_CHECKER_RUNTIME_DIRECTORY_INVALID",
                "运行时目录必须是显式普通绝对目录，不接受reparse路径。", directory.diagnostics);
            const auto& normalized = directory.value->binding.path;
            if (std::any_of(state->identity.runtimeDirectories.begin(), state->identity.runtimeDirectories.end(),
                    [&](const auto& existing) { return SamePath(existing, normalized); }))
                return CaptureFailure<LuaSyntaxChecker>(path, "LUA_CHECKER_RUNTIME_DIRECTORY_DUPLICATE", "运行时目录重复。");
            state->identity.runtimeDirectories.push_back(normalized);
        }
        LuaSyntaxChecker result; result.state_ = std::move(state);
        return Result<LuaSyntaxChecker>::Success(std::move(result));
    } catch (const std::exception&) {
        return CaptureFailure<LuaSyntaxChecker>(executable, "LUA_CHECKER_CAPTURE_FAILED", "检查器捕获失败，未返回部分身份。");
    }
}

bool LuaMapPipelineResult::Succeeded() const noexcept {
    return syntaxChecked && !script.empty() && !scriptSha256.empty() && std::none_of(diagnostics.begin(), diagnostics.end(),
        [](const auto& value) { return value.severity == DiagnosticSeverity::Error; });
}

LuaMapPipelineResult CheckLuaMapScript(const LuaMapScriptResult& generated, const LuaSyntaxChecker& checker,
    const std::filesystem::path& workDir, std::chrono::seconds timeout,
    const std::function<void(std::string_view)>& onStage) {
    LuaMapPipelineResult result;
    result.diagnostics = generated.diagnostics;
    try {
        {
            Stage stage(result, "lua-verify-generated", {}, onStage);
            if (!generated.Succeeded() || generated.targetIdentity.empty() || generated.sourceMapSha256.empty())
                Fail("LUA_PIPELINE_GENERATED_INVALID", "输入不是成功且带完整身份的整图Lua结果。");
            const auto sourceMap = lua::ComputeLuaMapSourceMapSha256(generated);
            if (!sourceMap.Succeeded()) {
                result.diagnostics.insert(result.diagnostics.end(), sourceMap.diagnostics.begin(), sourceMap.diagnostics.end());
                Fail("LUA_PIPELINE_GENERATED_CHANGED", "整图文本或来源范围在生成后变化。");
            }
            if (*sourceMap.value != generated.sourceMapSha256)
                Fail("LUA_PIPELINE_SOURCE_MAP_CHANGED", "整图来源范围身份在生成后变化。");
            if (timeout <= std::chrono::seconds::zero() || timeout > std::chrono::hours(24))
                Fail("LUA_PIPELINE_TIMEOUT_INVALID", "检查超时预算必须大于0且不超过24小时。");
            stage.Pass("completeTextSha256=" + generated.scriptSha256 + "\nsourceMapSha256=" + generated.sourceMapSha256);
        }
        const auto* identity = checker.Identity();
        if (!identity) Fail("LUA_CHECKER_UNBOUND", "没有捕获显式语法检查器。");
        result.checkerSha256 = identity->sha256;
        {
            Stage stage(result, "lua-verify-checker", identity->executable, onStage);
            VerifyChecker(*identity, result.diagnostics);
            stage.Pass("checkerSha256=" + identity->sha256 + "\nRuntime directories are named inputs; external DLL trees are not frozen.");
        }
        std::filesystem::path work;
        if (!Detail::NormalizeSourcePath(workDir, work) || work == work.root_path())
            Fail("LUA_PIPELINE_WORK_DIRECTORY_INVALID", "工作目录须为有界绝对新叶目录。", workDir);
        const auto scriptPath = work / "war3map.lua";
        const auto runnerPath = work / "syntax-runner.lua";
        const auto logPath = work / "lua-syntax.log";
        const auto runnerHash = Sha256Text(kRunner);
        if (!runnerHash.Succeeded()) Fail("LUA_PIPELINE_HASH_FAILED", "固定语法runner身份计算失败。");
        {
            Stage stage(result, "lua-stage-inputs", scriptPath, onStage);
            const auto parent = Detail::CaptureSourceBinding(work.parent_path(), Kind::Directory, true, 0);
            if (!parent.Succeeded()) {
                result.diagnostics.insert(result.diagnostics.end(), parent.diagnostics.begin(), parent.diagnostics.end());
                Fail("LUA_PIPELINE_WORK_DIRECTORY_INVALID", "工作目录父链不可用。", work.parent_path());
            }
            std::error_code error;
            if (!std::filesystem::create_directory(work, error))
                Fail("LUA_PIPELINE_WORK_DIRECTORY_EXISTS", "工作叶目录已存在或无法独占创建；保留既有文件。", work);
            const auto directory = Detail::CaptureSourceBinding(work, Kind::Directory, true, 0);
            if (!directory.Succeeded()) Fail("LUA_PIPELINE_WORK_DIRECTORY_INVALID", "新工作目录不能稳定绑定。", work);
            WriteNewFile(scriptPath, generated.text); WriteNewFile(runnerPath, kRunner);
            VerifyFile(scriptPath, generated.scriptSha256, generated.text.size(), kScriptBytes, "LUA_PIPELINE_SCRIPT_CHANGED", result.diagnostics);
            VerifyFile(runnerPath, *runnerHash.value, kRunner.size(), kScriptBytes, "LUA_PIPELINE_RUNNER_CHANGED", result.diagnostics);
            stage.Pass("scriptSha256=" + generated.scriptSha256 + "\nrunnerSha256=" + *runnerHash.value);
        }
        detail::ProcessOutcome outcome;
        {
            Stage stage(result, "lua-syntax-check", logPath, onStage);
            // 进度回调之后再次核验，不能让回调期间的输入替换进入解释器。
            VerifyChecker(*identity, result.diagnostics);
            VerifyFile(scriptPath, generated.scriptSha256, generated.text.size(), kScriptBytes, "LUA_PIPELINE_SCRIPT_CHANGED", result.diagnostics);
            VerifyFile(runnerPath, *runnerHash.value, kRunner.size(), kScriptBytes, "LUA_PIPELINE_RUNNER_CHANGED", result.diagnostics);
            detail::ProcessRequest request;
            request.executable = identity->executable;
            request.arguments = {L"-E", L"syntax-runner.lua", std::wstring(generated.scriptSha256.begin(), generated.scriptSha256.end())};
            request.workingDirectory = work; request.pathPrefix = identity->runtimeDirectories;
            request.outputFile = logPath; request.timeout = timeout; request.maxOutputBytes = 1024 * 1024;
            outcome = detail::RunProcess(request);
            stage.Value().exitCode = outcome.exitCode;
            stage.Value().log = ProcessLog(outcome, logPath);
        }
        {
            Stage stage(result, "lua-post-verify", scriptPath, onStage);
            VerifyChecker(*identity, result.diagnostics);
            VerifyFile(scriptPath, generated.scriptSha256, generated.text.size(), kScriptBytes, "LUA_PIPELINE_SCRIPT_CHANGED", result.diagnostics);
            VerifyFile(runnerPath, *runnerHash.value, kRunner.size(), kScriptBytes, "LUA_PIPELINE_RUNNER_CHANGED", result.diagnostics);
            stage.Pass("checker/input/runner byte identities unchanged after supervised process completion");
        }
        if (!outcome.started) Fail("LUA_CHECKER_LAUNCH_FAILED", outcome.error, identity->executable);
        if (outcome.timedOut) Fail("LUA_CHECKER_TIMEOUT", outcome.error, identity->executable);
        if (outcome.outputLimitExceeded || outcome.supervisionFailed) Fail("LUA_CHECKER_SUPERVISION_FAILED", outcome.error, identity->executable);
        if (outcome.exitCode != 0) Fail("LUA_CHECKER_REJECTED", "Lua语法或宿主前提未通过（实际退出码 " + std::to_string(outcome.exitCode) + "）。", logPath);
        auto normalizedOutput = outcome.output;
        std::erase(normalizedOutput, '\r');
        if (normalizedOutput != std::string(kMarker) + generated.scriptSha256 + "\n")
            Fail("LUA_CHECKER_SUCCESS_MARKER_MISSING", "退出0但未取得唯一且绑定本脚本SHA的语法成功标记。", logPath);
        {
            Stage stage(result, "lua-shared-syntax-check", scriptPath, onStage);
            Lua::SyntaxOptions options;
            options.maxSourceBytes = static_cast<std::size_t>(kScriptBytes);
            options.maxDuration = (std::min)(std::chrono::duration_cast<std::chrono::milliseconds>(timeout),
                std::chrono::milliseconds(30000));
            const auto checked = Lua::CheckSyntax(generated.text, "war3map.lua", options);
            for (const auto& diagnostic : checked.diagnostics)
                result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.LuaPipeline", diagnostic.code,
                    "war3map.lua:" + std::to_string(diagnostic.line) + ": " + diagnostic.message, {}});
            if (!checked.Succeeded())
                Fail("LUA_SHARED_CHECK_REJECTED", "完整脚本未通过与代码编辑器相同的 Lua 5.3 编译检查。", scriptPath);
            stage.Pass("Lua 5.3.6 text compilation only; no libraries or map chunk executed\npeakBytes="
                + std::to_string(checked.peakMemoryBytes) + "\nscriptSha256=" + generated.scriptSha256);
        }
        result.script = generated.text; result.scriptSha256 = generated.scriptSha256;
        result.targetSha256 = generated.targetSha256; result.sourceMapSha256 = generated.sourceMapSha256;
        result.checkedBy = "Lua 5.3 loadfile(text only); sha256=" + identity->sha256
            + "; shared embedded Lua 5.3.6 text compilation";
        result.syntaxChecked = true;
    } catch (const PipelineFailure& failure) {
        result.diagnostics.push_back(failure.diagnostic);
    } catch (const std::exception& failure) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.LuaPipeline", "LUA_PIPELINE_EXCEPTION",
            std::string("Lua语法检查异常，未返回可保存脚本：") + failure.what(), {}});
    }
    if (!result.Succeeded()) {
        result.script.clear(); result.scriptSha256.clear(); result.targetSha256.clear(); result.sourceMapSha256.clear();
        result.syntaxChecked = false;
    }
    return result;
}
} // namespace War3Map::Script
