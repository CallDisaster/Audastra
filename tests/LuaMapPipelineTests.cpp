#include "War3MapCore/Script/LuaMapPipeline.h"
#include "War3MapCore/Hash.h"
#include "../src/ScriptBuild/LuaMapScriptInternal.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Script;
namespace {
void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
std::string Codes(const auto& result) {
    std::string value;
    for (const auto& diagnostic : result.diagnostics) value += diagnostic.code + ": " + diagnostic.message + "\n";
    return value;
}
std::string Logs(const LuaMapPipelineResult& result) {
    std::string value;
    for (const auto& stage : result.stages)
        value += stage.name + " exit=" + std::to_string(stage.exitCode) + "\n" + stage.log + "\n";
    return value;
}
bool Code(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
void Write(const std::filesystem::path& path, std::string_view text, bool append = false) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    file << text; Require(static_cast<bool>(file), "write failed: " + path.string());
}
std::optional<std::wstring> Environment(const wchar_t* name) {
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    const auto written = GetEnvironmentVariableW(name, result.data(), size);
    Require(written < size, "environment read raced"); result.resize(written); return result;
}
class ScopedEnvironment {
public:
    ScopedEnvironment(const wchar_t* name, const std::wstring& value) : name_(name), old_(Environment(name)) {
        Require(SetEnvironmentVariableW(name, value.c_str()) != FALSE, "set test-only process environment");
    }
    ~ScopedEnvironment() { SetEnvironmentVariableW(name_.c_str(), old_ ? old_->c_str() : nullptr); }
private:
    std::wstring name_;
    std::optional<std::wstring> old_;
};
LuaMapScriptResult Generated(const std::filesystem::path& root) {
    MapData::MapSettings settings; settings.formatVersion = 31; settings.scriptLanguage = 1;
    settings.tileset = 'L'; settings.custom.emplace();
    MapScriptInputs inputs; inputs.settings = &settings;
    const auto plan = BuildMapInitializationPlan(inputs); Require(plan.Succeeded(), Codes(plan));
    LuaTargetProfile target; target.identity = "pipeline-test/explicit-lua53-host";
    target.handleTypes = {{"placement", "handle"}};
    const auto api = [&](const char* name, const char* returns, std::initializer_list<const char*> parameters) {
        LuaApiSignature entry{name, returns, {}};
        for (const auto* parameter : parameters) entry.parameters.emplace_back(parameter);
        target.apis.push_back(std::move(entry));
    };
    api("GetCameraMargin", "real", {"integer"});
    api("SetCameraBounds", "nothing", {"real", "real", "real", "real", "real", "real", "real", "real"});
    api("SetDayNightModels", "nothing", {"string", "string"});
    for (const auto* name : {"NewSoundEnvironment", "SetAmbientDaySound", "SetAmbientNightSound", "SetMapName", "SetMapDescription"})
        api(name, "nothing", {"string"});
    api("SetMapMusic", "nothing", {"string", "boolean", "integer"});
    api("InitBlizzard", "nothing", {}); api("InitGenericPlayerSlots", "nothing", {});
    api("SetPlayers", "nothing", {"integer"}); api("SetTeams", "nothing", {"integer"});
    api("SetGamePlacement", "nothing", {"placement"});
    target.constants = {{"CAMERA_MARGIN_LEFT", "integer"}, {"CAMERA_MARGIN_RIGHT", "integer"},
        {"CAMERA_MARGIN_BOTTOM", "integer"}, {"CAMERA_MARGIN_TOP", "integer"}, {"MAP_PLACEMENT_USE_MAP_SETTINGS", "placement"}};
    Write(root / "ui/config", "base\n"); Write(root / "ui/base/define.txt", "[TriggerTypes]\n");
    const auto catalog = Trigger::UiCatalog::Load(root); Require(catalog.Succeeded(), Codes(catalog));
    auto result = GenerateLuaMapScript(*plan.value, {}, *catalog.value, target);
    Require(result.Succeeded(), Codes(result)); return result;
}
void Reidentify(LuaMapScriptResult& source) {
    const auto textHash = Sha256Text(source.text); Require(textHash.Succeeded(), "hash modified syntax fixture");
    source.scriptSha256 = *textHash.value;
    const auto sourceHash = lua::ComputeLuaMapSourceMapSha256(source);
    Require(sourceHash.Succeeded(), Codes(sourceHash)); source.sourceMapSha256 = *sourceHash.value;
}
void Rejected(const LuaMapPipelineResult& result, const char* code) {
    Require(!result.Succeeded() && result.script.empty() && !result.syntaxChecked && result.scriptSha256.empty()
        && result.targetSha256.empty() && result.sourceMapSha256.empty(), "rejection returned saveable script");
    Require(Code(result, code), Codes(result) + " expected " + code);
}
const PipelineStage& SyntaxStage(const LuaMapPipelineResult& result) {
    const auto found = std::find_if(result.stages.begin(), result.stages.end(), [](const auto& stage) { return stage.name == "lua-syntax-check"; });
    Require(found != result.stages.end(), "actual syntax stage absent"); return *found;
}
}

int main(int argc, char** argv) {
    // 自身作为独立负例工具：已捕获的可执行文件退出0仍不能当作Lua检查成功。
    if (argc > 1 && std::string_view(argv[1]) == "-E") {
        const auto mode = Environment(L"FORGE_LUA_PIPELINE_TEST_MODE");
        if (mode && *mode == L"timeout") Sleep(10000);
        if (mode && *mode == L"forged-success" && argc == 4) {
            std::cout << "FORGE_LUA_SYNTAX_OK_V1 " << argv[3] << '\n';
            return 0;
        }
        std::cout << "intentional test executable, no Lua success marker\n";
        return 0;
    }
    try {
        const auto root = argc > 1 ? std::filesystem::absolute(argv[1]) : std::filesystem::temp_directory_path()
            / ("forge-lua-pipeline-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(root);
        auto source = Generated(root / "catalog");
        const auto ydwe = Environment(L"FORGE_YDWE_ROOT"); Require(ydwe && !ydwe->empty(), "FORGE_YDWE_ROOT required; no skipped syntax execution");
        const auto executable = std::filesystem::path(*ydwe) / "bin/lua.exe";
        const auto executableHash = Sha256File(executable); Require(executableHash.Succeeded(), Codes(executableHash));
        const std::vector<std::filesystem::path> directories{executable.parent_path()};
        const auto captured = CaptureLuaSyntaxChecker(executable, *executableHash.value, directories);
        Require(captured.Succeeded() && captured.value->Identity(), Codes(captured));
        const auto& checker = *captured.value;
        Require(checker.Identity()->sha256 == *executableHash.value && checker.Identity()->bytes > 0
            && checker.Identity()->runtimeDirectories.size() == 1, "checker does not own identity");
        Require(Code(CaptureLuaSyntaxChecker(executable, std::string(64, '0'), directories), "LUA_CHECKER_SHA_MISMATCH"), "wrong checker SHA accepted");
        Require(Code(CaptureLuaSyntaxChecker(executable, "", directories), "LUA_CHECKER_EXPECTED_SHA_INVALID"), "missing checker SHA accepted");
        auto duplicate = directories; duplicate.push_back(directories.front());
        Require(Code(CaptureLuaSyntaxChecker(executable, *executableHash.value, duplicate), "LUA_CHECKER_RUNTIME_DIRECTORY_DUPLICATE"), "duplicate runtime paths accepted");
        const std::vector<std::filesystem::path> excessive(9, directories.front());
        Require(Code(CaptureLuaSyntaxChecker(executable, *executableHash.value, excessive), "LUA_CHECKER_RUNTIME_DIRECTORY_BUDGET"), "runtime path budget ignored");

        auto stale = source; stale.text += "-- source changed after generation\n";
        Rejected(CheckLuaMapScript(stale, checker, root / "stale-source"), "LUA_PIPELINE_GENERATED_CHANGED");
        stale = source; stale.sourceMapSha256 = std::string(64, '0');
        Rejected(CheckLuaMapScript(stale, checker, root / "stale-map"), "LUA_PIPELINE_SOURCE_MAP_CHANGED");
        stale = source; stale.sourceSpans.push_back({source.text.size() + 1, 1, 1, 1, LuaSourceSpanKind::Statement});
        Rejected(CheckLuaMapScript(stale, checker, root / "bad-span"), "LUA_PIPELINE_GENERATED_CHANGED");
        Require(!std::filesystem::exists(root / "stale-source") && !std::filesystem::exists(root / "stale-map"), "invalid input reached output staging");
        Rejected(CheckLuaMapScript(source, {}, root / "unbound"), "LUA_CHECKER_UNBOUND");

        const auto copiedExe = root / "bound-lua.exe";
        std::filesystem::copy_file(executable, copiedExe);
        const auto copied = CaptureLuaSyntaxChecker(copiedExe, *executableHash.value, directories);
        Require(copied.Succeeded(), Codes(copied));
        Write(copiedExe, "changed", true);
        Rejected(CheckLuaMapScript(source, *copied.value, root / "changed-checker"), "LUA_CHECKER_CHANGED");

        wchar_t windows[32768]{};
        const auto windowsSize = GetWindowsDirectoryW(windows, 32768);
        Require(windowsSize > 0 && windowsSize < 32768, "Windows directory lookup");
        ScopedEnvironment minimalPath(L"PATH", std::wstring(windows) + L"\\System32;" + windows);
        ScopedEnvironment blockedInit(L"LUA_INIT", L"error('LUA_INIT must not execute')");
        ScopedEnvironment blockedVersionInit(L"LUA_INIT_5_3", L"error('LUA_INIT_5_3 must not execute')");
        // 顶层、config/main都不得运行；不存在map-executed.flag且成功表明只是文本加载。
        source.text += "\nlocal flag = assert(io.open('map-executed.flag', 'wb'))\nflag:write('unexpected')\nflag:close()\nerror('map chunk must never execute')\n";
        Reidentify(source);
        const auto success = CheckLuaMapScript(source, checker, root / "valid-minimal-path");
        Require(success.Succeeded(), Codes(success) + Logs(success));
        Require(success.script == source.text && success.scriptSha256 == source.scriptSha256
            && success.sourceMapSha256 == source.sourceMapSha256 && success.checkerSha256 == *executableHash.value,
            "successful syntax result identity changed");
        Require(!std::filesystem::exists(root / "valid-minimal-path/map-executed.flag"), "map chunk executed");
        const auto sharedStage = std::find_if(success.stages.begin(), success.stages.end(),
            [](const auto& stage) { return stage.name == "lua-shared-syntax-check"; });
        Require(sharedStage != success.stages.end() && sharedStage->exitCode == 0
            && sharedStage->log.find("peakBytes=") != std::string::npos
            && success.checkedBy.find("shared embedded Lua 5.3.6") != std::string::npos,
            "Save did not use the code editor Lua syntax kernel");
        Require(SyntaxStage(success).exitCode == 0 && SyntaxStage(success).log.find("started=true") != std::string::npos
            && SyntaxStage(success).log.find("FORGE_LUA_SYNTAX_OK_V1 " + source.scriptSha256) != std::string::npos,
            "actual checker success evidence missing");
        const auto diskHash = Sha256File(root / "valid-minimal-path/war3map.lua");
        Require(diskHash.Succeeded() && *diskHash.value == source.scriptSha256, "staged full script differs");
        Rejected(CheckLuaMapScript(source, checker, root / "valid-minimal-path"), "LUA_PIPELINE_WORK_DIRECTORY_EXISTS");
        auto malformed = source; malformed.text += "\nfunction broken(\n"; Reidentify(malformed);
        const auto syntaxError = CheckLuaMapScript(malformed, checker, root / "syntax-error");
        Rejected(syntaxError, "LUA_CHECKER_REJECTED");
        Require(SyntaxStage(syntaxError).exitCode == 21 && SyntaxStage(syntaxError).log.find("LUA_MAP_SYNTAX:") != std::string::npos,
            "syntax failure lost real Lua exit/log");

        const auto changedInput = CheckLuaMapScript(source, checker, root / "input-replaced", std::chrono::seconds(20),
            [&](std::string_view stage) { if (stage == "lua-syntax-check") Write(root / "input-replaced/war3map.lua", "-- tamper\n", true); });
        Rejected(changedInput, "LUA_PIPELINE_SCRIPT_CHANGED");
        Require(SyntaxStage(changedInput).exitCode == -1, "changed input was launched");
        const auto changedRunner = CheckLuaMapScript(source, checker, root / "runner-replaced", std::chrono::seconds(20),
            [&](std::string_view stage) { if (stage == "lua-syntax-check") Write(root / "runner-replaced/syntax-runner.lua", "print('forged')\n"); });
        Rejected(changedRunner, "LUA_PIPELINE_RUNNER_CHANGED");
        const auto postInput = CheckLuaMapScript(source, checker, root / "post-input-replaced", std::chrono::seconds(20),
            [&](std::string_view stage) { if (stage == "lua-post-verify") Write(root / "post-input-replaced/war3map.lua", "-- after load\n", true); });
        Rejected(postInput, "LUA_PIPELINE_SCRIPT_CHANGED");
        Require(SyntaxStage(postInput).exitCode == 0, "post-verify failure replaced actual exit code");
        const auto postExe = root / "post-lua.exe"; std::filesystem::copy_file(executable, postExe);
        const auto postCapture = CaptureLuaSyntaxChecker(postExe, *executableHash.value, directories); Require(postCapture.Succeeded(), Codes(postCapture));
        const auto postChecker = CheckLuaMapScript(source, *postCapture.value, root / "post-checker-replaced", std::chrono::seconds(20),
            [&](std::string_view stage) { if (stage == "lua-post-verify") Write(postExe, "after-exit", true); });
        Rejected(postChecker, "LUA_CHECKER_CHANGED"); Require(SyntaxStage(postChecker).exitCode == 0, "post checker proof lost actual exit");

        wchar_t self[32768]{}; const auto selfSize = GetModuleFileNameW(nullptr, self, 32768);
        Require(selfSize > 0 && selfSize < 32768, "self checker path");
        const auto selfHash = Sha256File(self); Require(selfHash.Succeeded(), Codes(selfHash));
        const auto falseChecker = CaptureLuaSyntaxChecker(self, *selfHash.value); Require(falseChecker.Succeeded(), Codes(falseChecker));
        const auto noMarker = CheckLuaMapScript(source, *falseChecker.value, root / "exit-zero-no-marker");
        Rejected(noMarker, "LUA_CHECKER_SUCCESS_MARKER_MISSING");
        Require(SyntaxStage(noMarker).exitCode == 0, "zero-exit negative fixture did not actually exit zero");
        {
            ScopedEnvironment mode(L"FORGE_LUA_PIPELINE_TEST_MODE", L"forged-success");
            const auto forged = CheckLuaMapScript(malformed, *falseChecker.value, root / "forged-valid-marker");
            Rejected(forged, "LUA_SHARED_CHECK_REJECTED");
            Require(Code(forged, "LUA_SYNTAX_ERROR") && SyntaxStage(forged).exitCode == 0,
                "A trusted external marker bypassed the shared compiler syntax gate");
        }
        {
            ScopedEnvironment mode(L"FORGE_LUA_PIPELINE_TEST_MODE", L"timeout");
            const auto timeout = CheckLuaMapScript(source, *falseChecker.value, root / "timeout", std::chrono::seconds(1));
            Rejected(timeout, "LUA_CHECKER_TIMEOUT");
            Require(SyntaxStage(timeout).log.find("timedOut=true") != std::string::npos, "timeout process evidence missing");
        }
        const auto originalHash = Sha256File(executable);
        Require(originalHash.Succeeded() && *originalHash.value == *executableHash.value, "installed checker modified");
        std::cout << "Lua 5.3 syntax-only pipeline actual execution passed under Windows-only PATH; source/runner/tool identities and failure gates verified.\n"
            << "No map chunk, config/main/native or game execution performed. Artifacts: " << root.string() << '\n';
        return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
