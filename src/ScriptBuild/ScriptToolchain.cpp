#include "War3MapCore/Script/ScriptPipeline.h"

#include "ExternalProcess.h"
#include "ScriptJapiPlaceholder.h"
#include "ScriptSourceBinding.h"

#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Trigger/UiCallbackActions.h"
#include "War3MapCore/Jass/VJass.h"
#include "War3MapCore/Jass/IdentifierRewrite.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/VerifiedFile.h"
#include "../Jass/JassLexer.h"
#include "../Jass/JassCheckBudget.h"

#include <fstream>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

// 编译流水线：阶段文件依次写到工作目录（1_war3map.j … 6_vjass.j），便于与 YDWE 日志逐阶段对照。
// 内置实现（预处理、vJass 翻译、语法检查）在进程内完成；外部程序（Wave、jasshelper、pjass）可逐项选用或作为回退。
namespace War3Map::Script {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kLogLimit = 16 * 1024;

void Fail(PipelineResult& result, std::string code, std::string message) {
    result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Pipeline", std::move(code), std::move(message), {}});
}

void Warn(PipelineResult& result, std::string code, std::string message) {
    result.diagnostics.push_back({DiagnosticSeverity::Warning, "Script.Pipeline", std::move(code), std::move(message), {}});
}

bool HasError(const PipelineResult& result) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) return true;
    }
    return false;
}

bool VerifyLayout(PipelineResult& result, const ToolchainOptions& options) {
    auto verified = VerifyScriptToolchainLayout(*options.layout);
    bool succeeded = verified.Succeeded();
    for (auto& diagnostic : verified.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
    if (options.baseInputs) {
        auto bases = VerifyScriptBaseInputs(*options.baseInputs, *options.layout, options.basePolicy);
        succeeded = succeeded && bases.Succeeded();
        for (auto& diagnostic : bases.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
    }
    if (options.frameworkInputs) {
        auto framework = VerifyScriptFrameworkInputs(*options.frameworkInputs, *options.layout);
        succeeded = succeeded && framework.Succeeded();
        for (auto& diagnostic : framework.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (framework.Succeeded() && result.frameworkPreprocessSources) {
            auto includes = VerifyScriptPreprocessSources(*result.frameworkPreprocessSources,
                FrameworkSourceRequest(*options.frameworkInputs), result.frameworkPreprocessSources->request);
            succeeded = succeeded && includes.Succeeded();
            for (auto& diagnostic : includes.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        }
    }
    if (!succeeded) {
        result.script.clear();
        result.syntaxChecked = false;
    }
    return succeeded;
}

bool WriteFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

std::filesystem::path BasePath(const ToolchainOptions& options, bool common) {
    return options.workDir / (common ? "base_common.j" : "base_blizzard.j");
}

bool StageBases(PipelineResult& result, const ToolchainOptions& options) {
    for (const bool common : {true, false}) {
        const auto& base = common ? options.baseInputs->common : options.baseInputs->blizzard;
        const auto text = std::string_view(base.bytes.empty() ? "" : reinterpret_cast<const char*>(base.bytes.data()), base.bytes.size());
        if (!WriteFile(BasePath(options, common), text)) {
            Fail(result, "WORKDIR_WRITE", "无法写出本次冻结的基础脚本副本");
            return false;
        }
    }
    return true;
}

bool VerifyStagedBases(PipelineResult& result, const ToolchainOptions& options) {
    bool valid = true;
    for (const bool common : {true, false}) {
        const auto& base = common ? options.baseInputs->common : options.baseInputs->blizzard;
        auto read = ReadVerifiedFile(BasePath(options, common), base.sha256, 16ull * 1024 * 1024);
        valid = valid && read.Succeeded();
        for (auto& diagnostic : read.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
    }
    return valid;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return std::move(buffer).str();
}

std::string Tail(const std::string& text) {
    return text.size() <= kLogLimit ? text : "...\n" + text.substr(text.size() - kLogLimit);
}

// 路径末尾的分隔符会让引号被转义，统一去掉。
std::wstring PathArgument(const std::filesystem::path& path) {
    std::wstring text = path.lexically_normal().wstring();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return text;
}

std::wstring Widen(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
}

PipelineStage& BeginStage(PipelineResult& result, const ToolchainOptions& options, std::string name, std::filesystem::path output) {
    if (options.onStage) options.onStage(name);
    result.stages.push_back({std::move(name), std::move(output), {}, 0, {}});
    return result.stages.back();
}

bool RunWave(PipelineResult& result, const ToolchainOptions& options, const std::filesystem::path& input, const std::filesystem::path& output) {
    auto& stage = BeginStage(result, options, "wave", output);
    const auto started = Clock::now();
    if (!VerifyLayout(result, options)) return false;
    const auto& layout = *options.layout;
    if (layout.waveExecutable.empty()) {
        Fail(result, "WAVE_UNAVAILABLE", "绑定的工具链没有可用的 Wave.exe");
        return false;
    }
    detail::ProcessRequest request;
    request.executable = layout.waveExecutable;
    request.workingDirectory = options.workDir;
    request.outputFile = options.workDir / "wave.log";
    request.timeout = options.timeout;
    auto& args = request.arguments;
    args.push_back(L"--output=" + PathArgument(output));
    for (const auto& directory : layout.systemIncludes) args.push_back(L"--sysinclude=" + PathArgument(directory));
    for (const auto& directory : options.jassDirs) args.push_back(L"--include=" + PathArgument(directory));
    args.push_back(L"--define=WARCRAFT_VERSION=" + std::to_wstring(layout.versionMacro));
    args.push_back(L"--define=YDWE_VERSION_STRING=\"" + Widen(options.versionString) + L"\"");
    if (options.debug) args.push_back(L"--define=DEBUG=1");
    if (options.scriptInjection) args.push_back(L"--define=SCRIPT_INJECTION=1");
    std::error_code error;
    for (const auto& force : layout.forceIncludes) args.push_back(L"--forceinclude=" + PathArgument(force));
    for (const auto* flag : {L"--extended", L"--c99", L"--preserve=2", L"--line=0"}) args.push_back(flag);
    args.push_back(PathArgument(input));
    const auto outcome = detail::RunProcess(request);
    stage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    stage.exitCode = outcome.exitCode;
    stage.log = Tail(outcome.output);
    if (!outcome.started) {
        Fail(result, "WAVE_LAUNCH", outcome.error);
        return false;
    }
    if (outcome.timedOut) {
        Fail(result, "WAVE_TIMEOUT", "Wave 预处理超时");
        return false;
    }
    if (outcome.outputLimitExceeded || outcome.supervisionFailed) {
        Fail(result, "WAVE_PROCESS_LIMIT", outcome.error); return false;
    }
    if (!outcome.Succeeded() || !std::filesystem::exists(output, error)) {
        Fail(result, "WAVE_FAILED", "Wave 预处理失败（退出码 " + std::to_string(outcome.exitCode) + "）：\n" + Tail(outcome.output));
        return false;
    }
    return true;
}

// 内置预处理器：参数与 YDWE 调用 Wave 时相同（系统目录 compiler/include，用户目录为各函数库的 jass 目录）。
bool RunPreprocessor(PipelineResult& result, const ToolchainOptions& options, std::string_view input, std::string& output,
    const Jass::SourceLexicalView* inputView = nullptr,
    std::vector<Detail::JapiPlaceholderInclude>* placeholderIncludes = nullptr,
    Jass::SourceMap* placeholderSourceMap = nullptr) {
    auto& stage = BeginStage(result, options, placeholderIncludes ? "preprocess-owned-japi" : "preprocess",
        options.workDir / (placeholderIncludes ? "3_japi_sources.j" : "3_preprocess.j"));
    const auto started = Clock::now();
    if (options.frameworkInputs) {
        const auto& framework = *options.frameworkInputs;
        ScriptPreprocessSourceRequest request;
        request.mainText = input; request.mainDirectory = framework.profile.mainSourceDirectory;
        request.frameworkDirectivePolicy = FrameworkDirectivePolicy(framework);
        request.includeCandidatePolicy = ScriptIncludeCandidatePolicy::BoundRootsSearch;
        if (inputView) request.mainLexicalView = *inputView;
        request.defines = {{"WARCRAFT_VERSION", std::to_string(options.layout->versionMacro)},
            {"YDWE_VERSION_STRING", "\"" + options.versionString + "\""}};
        if (options.debug) request.defines.emplace_back("DEBUG", "1");
        if (options.scriptInjection) request.defines.emplace_back("SCRIPT_INJECTION", "1");
        auto captured = CaptureScriptPreprocessSources(framework.entryPlan, FrameworkSourceRequest(framework), request);
        for (auto& diagnostic : captured.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (!captured.Succeeded()) { stage.exitCode = 1; return false; }
        auto encoding = ValidateFrameworkIncludes(framework, *captured.value);
        for (auto& diagnostic : encoding.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (!encoding.Succeeded()) { stage.exitCode = 1; return false; }
        auto replay = ReplayScriptPreprocessSources(*captured.value);
        for (auto& diagnostic : replay.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (!replay.Succeeded()) { stage.exitCode = 1; return false; }
        if (replay.text != captured.value->preprocessed.text) {
            Fail(result, "SCRIPT_FRAMEWORK_REPLAY_MISMATCH", "owned包含回放与捕获结果不同；未使用磁盘回退。");
            stage.exitCode = 1; return false;
        }
        stage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        stage.log = "FrameworkText/owned replay；" + std::to_string(replay.filesIncluded) + " 次包含；profile=" +
            framework.entryPlan.profileIdentitySha256;
        output = std::move(replay.text);
        result.frameworkPreprocessSources = std::make_shared<const ScriptPreprocessSources>(std::move(*captured.value));
        if (!WriteFile(stage.output, output)) Fail(result, "WORKDIR_WRITE", "无法写入owned回放3_preprocess.j");
        return !HasError(result);
    }
    Jass::PreprocessOptions preprocess;
    preprocess.systemIncludes = options.layout->systemIncludes;
    preprocess.userIncludes = options.jassDirs;
    preprocess.defines.emplace_back("WARCRAFT_VERSION", std::to_string(options.layout->versionMacro));
    preprocess.defines.emplace_back("YDWE_VERSION_STRING", "\"" + options.versionString + "\"");
    if (options.debug) preprocess.defines.emplace_back("DEBUG", "1");
    if (options.scriptInjection) preprocess.defines.emplace_back("SCRIPT_INJECTION", "1");
    for (const auto& force : options.layout->forceIncludes) {
        const auto utf8 = force.u8string();
        preprocess.forceIncludes.emplace_back(utf8.begin(), utf8.end());
    }
    // 仅占位协调使用本次实际读到的包含快照；不改变 include 搜索顺序或扩大旧框架资格。
    std::map<std::filesystem::path, Jass::PreprocessIncludeContent> includeCache;
    std::size_t capturedBytes = 0;
    if (placeholderIncludes) {
        preprocess.includeProvider = [&](const std::filesystem::path& candidate) -> Result<Jass::PreprocessIncludeContent> {
            const auto path = candidate.lexically_normal();
            if (const auto found = includeCache.find(path); found != includeCache.end()) {
                return Result<Jass::PreprocessIncludeContent>::Success(found->second);
            }
            if (includeCache.size() >= 8192) {
                return Result<Jass::PreprocessIncludeContent>::Failure("Script.JapiPlaceholder",
                    "SCRIPT_JAPI_INCLUDE_BUDGET", "占位协调的包含候选数超过8192，未使用部分来源。");
            }
            auto snapshot = Detail::CaptureSourceBinding(path, ScriptToolchainBindingKind::RegularFile, false,
                16u * 1024u * 1024u);
            if (!snapshot.Succeeded()) {
                Result<Jass::PreprocessIncludeContent> failure;
                failure.diagnostics = std::move(snapshot.diagnostics);
                return failure;
            }
            Jass::PreprocessIncludeContent content;
            content.exists = snapshot.value->binding.kind == ScriptToolchainBindingKind::RegularFile;
            if (content.exists) {
                if (snapshot.value->bytes.size() > 64u * 1024u * 1024u - capturedBytes) {
                    return Result<Jass::PreprocessIncludeContent>::Failure("Script.JapiPlaceholder",
                        "SCRIPT_JAPI_INCLUDE_BUDGET", "占位协调的包含快照超过64MiB，未使用部分来源。");
                }
                capturedBytes += snapshot.value->bytes.size();
                const auto& bytes = snapshot.value->bytes;
                if (!bytes.empty()) content.text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                placeholderIncludes->push_back({snapshot.value->binding.path, content.text,
                    snapshot.value->binding.sha256});
            }
            includeCache.emplace(path, content);
            return Result<Jass::PreprocessIncludeContent>::Success(std::move(content));
        };
    }
    auto preprocessed = Jass::Preprocess(input, "war3map.j", options.workDir, preprocess, options.preprocessCache);
    stage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    stage.log = std::to_string(preprocessed.filesIncluded) + " 次包含（缓存命中 " + std::to_string(preprocessed.cacheHits) + "），" +
        std::to_string(preprocessed.macroExpansions) + " 次宏展开";
    const bool succeeded = preprocessed.Succeeded();
    for (auto& diagnostic : preprocessed.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
    if (!succeeded) {
        stage.exitCode = 1;
        return false;
    }
    output = std::move(preprocessed.text);
    if (placeholderSourceMap) *placeholderSourceMap = std::move(preprocessed.sourceMap);
    if (!WriteFile(stage.output, output)) Fail(result, "WORKDIR_WRITE", "无法写入 3_preprocess.j");
    return !HasError(result);
}

// 1.24 之前的版本允许 return bug（pjass +rb：只检查函数最后一条 return 的类型）。外部与内置检查共用这一判断。
bool AllowReturnBug(const ToolchainOptions& options) {
    if (options.frameworkInputs) return false; // 通用档案Strict；Pre124盘点不授予粗粒度+rb。
    return options.layout->allowReturnBug;
}

bool RunJassHelper(PipelineResult& result, const ToolchainOptions& options, const std::filesystem::path& input, const std::filesystem::path& output) {
    auto& stage = BeginStage(result, options, "jasshelper", output);
    const auto started = Clock::now();
    if (!VerifyLayout(result, options) || !StageBases(result, options)) return false;
    const auto& layout = *options.layout;
    if (layout.jassHelperExecutable.empty() || layout.pjassExecutable.empty()) {
        Fail(result, "JASSHELPER_UNAVAILABLE", "绑定的工具链没有可用的 jasshelper / pjass");
        return false;
    }
    // 此版 jasshelper 总在自身 exe 目录前缀下解释配置路径，连绝对路径也会被拼接。
    // 从已绑定的两条绝对路径推导相对路径；不依赖默认工具名或当前工作目录。
    const std::string checker = AllowReturnBug(options) ? "+rb " : "";
    const auto checkerRelative = layout.pjassExecutable.lexically_relative(layout.jassHelperExecutable.parent_path());
    if (checkerRelative.empty() || checkerRelative.is_absolute()) {
        Fail(result, "JASSHELPER_CONFIG", "无法从绑定的 jasshelper 目录确定 pjass 相对路径");
        return false;
    }
    const auto checkerPathUtf8 = checkerRelative.u8string();
    const std::string checkerPath(checkerPathUtf8.begin(), checkerPathUtf8.end());
    if (checkerPath.find_first_of("\"\r\n") != std::string::npos) {
        Fail(result, "JASSHELPER_CONFIG", "pjass 路径不能安全写入 jasshelper 配置");
        return false;
    }
    if (!WriteFile(options.workDir / "jasshelper.conf",
            "[jasscompiler]\r\n\"" + checkerPath + "\"\r\n\"" + checker + "$COMMONJ $BLIZZARDJ $WAR3MAPJ\"\r\n")) {
        Fail(result, "JASSHELPER_CONFIG", "无法写入 jasshelper.conf");
        return false;
    }
    detail::ProcessRequest request;
    request.executable = layout.jassHelperExecutable;
    request.workingDirectory = options.workDir;
    request.pathPrefix = {options.toolRoot / "bin"};
    request.outputFile = options.workDir / "jasshelper.log";
    request.timeout = options.timeout;
    if (options.debug) request.arguments.push_back(L"--debug");
    if (!options.optimize) request.arguments.push_back(L"--nooptimize");
    request.arguments.push_back(L"--scriptonly");
    for (const auto& path : {BasePath(options, true), BasePath(options, false), input, output}) request.arguments.push_back(PathArgument(path));
    std::error_code error;
    std::filesystem::remove(output, error);
    const auto outcome = detail::RunProcess(request);
    stage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    stage.exitCode = outcome.exitCode;
    stage.log = Tail(outcome.output);
    if (!VerifyStagedBases(result, options)) return false;
    if (!outcome.started) {
        Fail(result, "JASSHELPER_LAUNCH", outcome.error);
        return false;
    }
    if (outcome.timedOut) {
        Fail(result, "JASSHELPER_TIMEOUT", "jasshelper 超时");
        return false;
    }
    if (outcome.outputLimitExceeded || outcome.supervisionFailed) {
        Fail(result, "JASSHELPER_PROCESS_LIMIT", outcome.error); return false;
    }
    const bool success = outcome.Succeeded() && outcome.output.find("Success!") != std::string::npos &&
        std::filesystem::exists(output, error);
    if (!success) {
        Fail(result, "JASSHELPER_FAILED", "jasshelper / pjass 检查未通过（退出码 " + std::to_string(outcome.exitCode) + "）：\n" + Tail(outcome.output));
        return false;
    }
    return true;
}

// 游戏脚本（common.j / blizzard.j）：内置 vJass 翻译与内置检查都需要。
struct GameScripts {
    std::string common;
    std::string blizzard;
};

std::optional<GameScripts> ReadGameScripts(PipelineResult& result, const ToolchainOptions& options) {
    if (!VerifyLayout(result, options)) return std::nullopt;
    const auto text = [](const ScriptBaseInput& base) {
        return std::string(base.bytes.empty() ? "" : reinterpret_cast<const char*>(base.bytes.data()), base.bytes.size());
    };
    return GameScripts{text(options.baseInputs->common), text(options.baseInputs->blizzard)};
}

bool JassHelperAvailable(const ToolchainOptions& options) {
    return !options.layout->jassHelperExecutable.empty() && !options.layout->pjassExecutable.empty();
}

// 内置 vJass 翻译。返回 true 表示得到输出；遇到不支持的特性或翻译出错且有 jasshelper 可用时置 fallback。
bool RunBuiltinVJass(PipelineResult& result, const ToolchainOptions& options, const GameScripts& scripts, std::string_view input,
    std::string& output, bool& fallback) {
    auto& stage = BeginStage(result, options, "vjass", options.workDir / "6_vjass.j");
    const auto started = Clock::now();
    Jass::VJassOptions vjass;
    vjass.debug = options.debug;
    vjass.baseScripts = {{"common.j", scripts.common}, {"blizzard.j", scripts.blizzard}};
    auto translated = Jass::TranslateVJass(input, "war3map.j", vjass);
    stage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    stage.log = std::to_string(translated.libraries) + " 个库，" + std::to_string(translated.structs) + " 个结构，" +
        std::to_string(translated.functions) + " 个函数";
    if (translated.Succeeded()) {
        for (auto& diagnostic : translated.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        output = std::move(translated.text);
        if (!WriteFile(stage.output, output)) {
            Fail(result, "WORKDIR_WRITE", "无法写入 6_vjass.j");
            return false;
        }
        return true;
    }
    stage.exitCode = 1;
    std::string reason;
    if (!translated.unsupported.empty()) {
        reason = "遇到尚未支持的特性（" + translated.unsupported + "）";
    } else {
        std::size_t errors = 0;
        for (const auto& diagnostic : translated.diagnostics) {
            if (diagnostic.severity != DiagnosticSeverity::Error) continue;
            if (errors++ == 0) reason = "出错：" + diagnostic.message;
        }
        if (errors > 1) reason += "（共 " + std::to_string(errors) + " 处）";
    }
    stage.log += "；" + reason;
    if (!translated.completed) {
        // 取消、超时和预算耗尽属于硬停止，不能借外部编译器继续消耗资源。
        for (auto& diagnostic : translated.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        return false;
    }
    if (JassHelperAvailable(options)) {
        fallback = true;
        Warn(result, "VJASS_FALLBACK", "内置 vJass 翻译" + reason + "，改用 jasshelper");
        return false;
    }
    for (auto& diagnostic : translated.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
    if (!translated.unsupported.empty()) Fail(result, "VJASS_UNSUPPORTED", "内置 vJass 翻译" + reason + "，且没有 compiler/jasshelper 可以改用");
    return false;
}

bool CheckFrameworkPragmas(PipelineResult& result, std::string_view text, const std::filesystem::path& path,
    const Jass::JassCheckOptions& options = {}) {
    try {
    Jass::detail::CheckBudget budget(options);
    budget.RegisterSource(text.size());
    auto tokens = Jass::detail::LexJassBounded(text, Jass::detail::LexOptions{}, options.maxTokens, &budget);
    if (!tokens.Succeeded() || !tokens.value) {
        result.diagnostics.insert(result.diagnostics.end(), tokens.diagnostics.begin(), tokens.diagnostics.end());
        Fail(result, "SCRIPT_FRAMEWORK_PRAGMA_CHECK_INCOMPLETE", "pragma检查已停止或超过词法预算，不能继续保存。");
        return false;
    }
    for (const auto& token : *tokens.value) {
        budget.Poll();
        if (token.kind != Jass::detail::TokenKind::Pragma) continue;
        std::istringstream words(std::string(token.text.substr(3)));
        for (std::string word; words >> word;) {
            if (word != "+rb" && word != "+nosemanticerror" && word != "+nosyntaxerror") continue;
            Fail(result, "SCRIPT_FRAMEWORK_CHECK_PRAGMA_UNSUPPORTED", "最终检查来源含放宽类型/语法的pragma；Strict档案未授予此例外。");
            result.diagnostics.back().origin = OriginSpan{path, token.offset, token.text.size(), {}};
            return false;
        }
    }
    return true;
    } catch (const Jass::detail::CheckAborted&) {
        Fail(result, "SCRIPT_FRAMEWORK_PRAGMA_CHECK_INCOMPLETE", "pragma检查已取消、超时或达到预算，不能继续保存。");
        return false;
    }
}

bool RunBuiltinCheck(PipelineResult& result, const ToolchainOptions& options, const GameScripts& scripts, std::string_view script) {
    auto& stage = BeginStage(result, options, "check", {});
    const auto started = Clock::now();
    Jass::JassCheckOptions check;
    check.deadline = started + std::chrono::seconds(30);
    check.returnBug = AllowReturnBug(options);
    Jass::SourceLexicalView finalView;
    if (options.frameworkInputs && result.frameworkIdentifierReport &&
        !result.frameworkIdentifierReport->lexicalView.opaque.empty()) {
        auto rebound = RebindFrameworkLexicalView(script, result.frameworkIdentifierReport->text,
            result.frameworkIdentifierReport->lexicalView);
        if (!rebound.Succeeded()) {
            result.diagnostics.insert(result.diagnostics.end(), rebound.diagnostics.begin(), rebound.diagnostics.end());
            stage.exitCode = 1; return false;
        }
        finalView = std::move(*rebound.value); script = finalView.text; // 仅检查视图，发布的war3map.j仍使用原字节。
    }
    const std::vector<Jass::JassSource> sources = {{"common.j", scripts.common}, {"blizzard.j", scripts.blizzard}, {"war3map.j", script}};
    if (options.frameworkInputs) {
        for (const auto& source : sources) {
            const auto path = source.name == "war3map.j" ? options.workDir / "6_vjass.j"
                : (source.name == "common.j" ? options.baseInputs->common.sourceRoot : options.baseInputs->blizzard.sourceRoot);
            if (!CheckFrameworkPragmas(result, source.text, path, check)) { stage.exitCode = 1; return false; }
        }
    }
    auto checked = Jass::CheckJass(sources, check);
    stage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    stage.log = std::to_string(checked.lines) + " 行，" + std::to_string(checked.functions) + " 个函数，" +
        std::to_string(checked.threadsUsed) + " 个线程";
    if (checked.errorsIgnored > 0) stage.log += "，按 //# 注释忽略 " + std::to_string(checked.errorsIgnored) + " 个错误";
    const bool succeeded = checked.Succeeded() && (!options.frameworkInputs || checked.errorsIgnored == 0);
    if (options.frameworkInputs && checked.errorsIgnored != 0)
        Fail(result, "SCRIPT_FRAMEWORK_CHECK_ERRORS_IGNORED", "Strict档案不接受由pragma忽略的最终JASS错误。");
    stage.exitCode = succeeded ? 0 : 1;
    const auto firstCheckDiagnostic = result.diagnostics.size();
    for (auto& diagnostic : checked.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
    if (!succeeded) {
        auto roots = options.jassDirs;
        if (options.layout) roots.push_back(options.layout->root / "jass");
        AppendMissingIncludeHints(result.diagnostics, firstCheckDiagnostic, roots);
    }
    return succeeded;
}

// 外部 pjass。report 为 false 时（对照模式）不把失败记为错误。无法运行时返回空。
std::optional<bool> RunPjass(PipelineResult& result, const ToolchainOptions& options, const std::filesystem::path& script, bool report) {
    auto& stage = BeginStage(result, options, "pjass", {});
    const auto started = Clock::now();
    if (!VerifyLayout(result, options) || !StageBases(result, options)) return std::nullopt;
    const auto& layout = *options.layout;
    if (layout.pjassExecutable.empty()) {
        if (report) Fail(result, "PJASS_UNAVAILABLE", "绑定的工具链没有可用的 pjass");
        else Warn(result, "PJASS_UNAVAILABLE", "绑定的工具链没有可用的 pjass，未执行外部对照");
        return std::nullopt;
    }
    detail::ProcessRequest request;
    request.executable = layout.pjassExecutable;
    request.workingDirectory = options.workDir;
    request.outputFile = options.workDir / "pjass.log";
    request.timeout = options.timeout;
    if (AllowReturnBug(options)) request.arguments.push_back(L"+rb");
    for (const auto& path : {BasePath(options, true), BasePath(options, false), script}) request.arguments.push_back(PathArgument(path));
    const auto outcome = detail::RunProcess(request);
    stage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    stage.exitCode = outcome.exitCode;
    stage.log = Tail(outcome.output);
    if (!VerifyStagedBases(result, options)) return std::nullopt;
    const auto problem = !outcome.started || outcome.outputLimitExceeded || outcome.supervisionFailed ? outcome.error
        : outcome.timedOut ? std::string("pjass 超时") : std::string{};
    if (!problem.empty()) {
        if (report) {
            Fail(result, "PJASS_LAUNCH", problem);
        } else {
            Warn(result, "PJASS_LAUNCH", problem);
        }
        return std::nullopt;
    }
    const bool passed = outcome.Succeeded();
    if (!passed && report) Fail(result, "PJASS_FAILED", "pjass 检查未通过（退出码 " + std::to_string(outcome.exitCode) + "）：\n" + Tail(outcome.output));
    return passed;
}

} // namespace

std::filesystem::path ResolveScriptToolRoot(const std::filesystem::path& uiRoot) {
    if (uiRoot.empty()) return uiRoot;
    std::error_code error;
    if (std::filesystem::is_directory(uiRoot / "compiler", error)) return uiRoot;
    error.clear();
    // 只认 ui 目录本身（含 config），不向上任意查找。
    if (uiRoot.has_parent_path() && std::filesystem::is_regular_file(uiRoot / "config", error)) {
        error.clear();
        const auto parent = uiRoot.parent_path();
        if (std::filesystem::is_directory(parent / "compiler", error)) return parent;
    }
    return uiRoot;
}

bool PipelineResult::Succeeded() const noexcept {
    return syntaxChecked && !HasError(*this);
}

PipelineResult CompileMapScript(std::string_view generated, const ToolchainOptions& requested, const TemplateContext& templates) {
    PipelineResult result;
    std::error_code error;
    auto options = requested;
    // 外部工具的 cwd 是 workDir；所有路径先按调用方 cwd 固定，避免相对目录被重复拼接。
    const auto absolute = [&](std::filesystem::path& path) {
        if (path.empty()) return true;
        auto resolved = std::filesystem::absolute(path, error);
        if (error) return false;
        path = resolved.lexically_normal(); return true;
    };
    if (options.workDir.empty() || !absolute(options.workDir) || !absolute(options.toolRoot) ||
        !absolute(options.uiRoot) || !std::all_of(options.jassDirs.begin(), options.jassDirs.end(), absolute)) {
        Fail(result, "WORKDIR_PATH", "无法确定编译工作目录或工具来源的绝对路径");
        return result;
    }
    if (!options.layout) {
        auto resolved = ResolveScriptToolchainLayout({options.toolRoot, options.warcraftVersion, options.actualVersionMacro});
        const bool succeeded = resolved.Succeeded() && resolved.value.has_value();
        for (auto& diagnostic : resolved.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (!succeeded) return result;
        options.layout = std::make_shared<const ScriptToolchainLayout>(std::move(*resolved.value));
        options.toolRoot = options.layout->root;
    } else if (options.toolRoot != options.layout->root ||
        options.layout->allowReturnBug != (options.warcraftVersion == 20) ||
        (options.warcraftVersion != 20 && options.warcraftVersion != 24) ||
        options.layout->versionMacro != options.actualVersionMacro.value_or(100 + options.warcraftVersion)) {
        Fail(result, "SCRIPT_LAYOUT_OPTIONS_MISMATCH", "编译请求与冻结的工具链根、版本宏或校验策略不一致");
        return result;
    }
    if (!VerifyLayout(result, options)) return result;
    if (!options.layout->semanticsSupported && !options.frameworkInputs) {
        Fail(result, "SCRIPT_TOOLCHAIN_UNSUPPORTED", "旧工具链重写和模板语义尚未适配，不能据路径识别成功重建脚本");
        return result;
    }
    if (options.frameworkInputs && (options.layout->executionPolicy != ScriptToolchainExecutionPolicy::FrameworkInputsRequired ||
        options.preprocessor != ScriptEngine::Builtin)) {
        Fail(result, "SCRIPT_FRAMEWORK_ENGINE_UNSUPPORTED", "正常框架子集必须使用owned内置回放；不回退外部Wave或借现代路径扩大资格。");
        return result;
    }
    if (options.frameworkInputs) result.frameworkProfileSha256 = options.frameworkInputs->entryPlan.profileIdentitySha256;
    const bool ownedFrameworkUi = options.frameworkInputs &&
        options.frameworkInputs->profile.uiRoot ==
            Trigger::UiCatalog::ResolveUiRoot(options.uiRoot.empty() ? options.toolRoot : options.uiRoot).lexically_normal();
    if (options.frameworkInputs && !ownedFrameworkUi) {
        Fail(result, "SCRIPT_FRAMEWORK_UI_MISMATCH", "实际UI入口与拥有的框架目录不同，不能借档案绕过独立UI检查。");
        return result;
    }
    if (!options.uiRoot.empty() && options.uiRoot != options.toolRoot && !ownedFrameworkUi) {
        auto uiStatus = CheckScriptUiToolchainSemantics({options.uiRoot, options.warcraftVersion, options.actualVersionMacro});
        const bool valid = uiStatus.Succeeded();
        for (auto& diagnostic : uiStatus.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (!valid) return result;
    }
    if (!options.baseInputs) {
        if (options.basePolicy != ScriptBasePolicy::BoundLayout) {
            Fail(result, "SCRIPT_BASE_INPUTS_MISSING", "地图/游戏优先策略需要从最终保存视图显式捕获基础库");
            return result;
        }
        auto captured = CaptureScriptBaseInputs({options.layout.get()});
        const bool valid = captured.Succeeded();
        for (auto& diagnostic : captured.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (!valid) return result;
        options.baseInputs = std::make_shared<const ScriptBaseInputs>(std::move(*captured.value));
    }
    if (!VerifyLayout(result, options)) return result;
    result.baseInputs = options.baseInputs;
    std::filesystem::create_directories(options.workDir, error);
    if (error) {
        Fail(result, "WORKDIR_CREATE", "无法创建编译工作目录：" + options.workDir.string());
        return result;
    }
    const auto& dir = options.workDir;
    if (!WriteFile(dir / "1_war3map.j", generated)) {
        Fail(result, "WORKDIR_WRITE", "无法写入工作目录：" + dir.string());
        return result;
    }
    std::string script(generated);
    Jass::SourceLexicalView frameworkView;
    if (options.scriptInjection) {
        auto& stage = BeginStage(result, options, "inject", dir / "2_inject.j");
        const auto started = Clock::now();
        std::vector<std::filesystem::path> injected;
        std::size_t entries = 0;
        if (options.frameworkInputs) {
            auto owned = InjectScriptFramework(script, *options.frameworkInputs, injected, &frameworkView);
            for (auto& diagnostic : owned.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
            if (!owned.Succeeded()) { stage.exitCode = 1; return result; }
            entries = options.frameworkInputs->injection.choices.size();
            script = std::move(*owned.value);
        } else {
            const auto table = ScanInjectTable(options.jassDirs, options.warcraftVersion == 24, result.diagnostics);
            entries = table.entries.size();
            script = InjectCode(std::move(script), table, result.diagnostics, &injected);
        }
        stage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        stage.log = std::to_string(entries) + " 个注入函数，追加 " + std::to_string(injected.size()) + " 个文件";
        if (!WriteFile(dir / "2_inject.j", script)) Fail(result, "WORKDIR_WRITE", "无法写入 2_inject.j");
        if (HasError(result)) return result;
    }
    if (options.frameworkInputs && !options.scriptInjection) {
        const auto valid = ValidateFrameworkSourceEncoding(script,
            options.frameworkInputs->profile.mainSourceDirectory / "war3map.j", options.frameworkInputs->profile.mainEncoding,
            FrameworkDirectivePolicy(*options.frameworkInputs));
        result.diagnostics.insert(result.diagnostics.end(), valid.diagnostics.begin(), valid.diagnostics.end());
        if (!valid.Succeeded()) return result;
        auto view = MakeFrameworkLexicalView(script, options.frameworkInputs->profile.mainSourceDirectory / "war3map.j",
            options.frameworkInputs->profile.mainEncoding);
        if (!view.Succeeded()) { result.diagnostics.insert(result.diagnostics.end(), view.diagnostics.begin(), view.diagnostics.end()); return result; }
        frameworkView = std::move(*view.value);
    }
    std::string preprocessed;
    std::vector<Detail::JapiPlaceholderInclude> placeholderIncludes;
    std::vector<Detail::JapiPlaceholderRetained> placeholderRetained;
    Jass::SourceMap placeholderSourceMap;
    const bool inspectPlaceholders = !options.frameworkInputs && options.preprocessor == ScriptEngine::Builtin &&
        generated.find("JapiPlaceHolder") != std::string_view::npos;
    if (options.preprocessor == ScriptEngine::Builtin) {
        if (!RunPreprocessor(result, options, script, preprocessed, options.frameworkInputs ? &frameworkView : nullptr,
                nullptr,
                inspectPlaceholders ? &placeholderSourceMap : nullptr)) return result;
        if (inspectPlaceholders && Detail::HasActiveJapiPlaceholders({generated, script, preprocessed,
                &placeholderSourceMap, "war3map.j", {}})) {
            // 先以原读取规则证明候选活跃；注释、字符串和非活跃分支不改变预处理行为。
            std::string ownedPreprocessed;
            Jass::SourceMap ownedSourceMap;
            if (!RunPreprocessor(result, options, script, ownedPreprocessed, nullptr,
                    &placeholderIncludes, &ownedSourceMap)) return result;
            if (ownedPreprocessed != preprocessed) {
                Fail(result, "JAPI_PLACEHOLDER_SOURCE_CHANGED",
                    "占位协调捕获的包含内容与原预处理结果不同，未发布部分编译产物。");
                return result;
            }
            auto reconciled = Detail::ReconcileJapiPlaceholders({generated, script, preprocessed,
                &ownedSourceMap, "war3map.j", placeholderIncludes});
            for (auto& diagnostic : reconciled.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
            if (!reconciled.Succeeded()) return result;
            placeholderRetained = std::move(reconciled.value->retained);
            preprocessed = std::move(reconciled.value->text);
            if (!placeholderRetained.empty()) {
                auto& stage = BeginStage(result, options, "japi-placeholder", dir / "3_japi_placeholder.j");
                stage.log = "严格协调生成占位 " + std::to_string(placeholderRetained.size()) + " 项；原WTG/WCT不改";
                if (!WriteFile(stage.output, preprocessed)) {
                    Fail(result, "WORKDIR_WRITE", "无法写入占位协调的编译副本"); return result;
                }
            }
        }
    } else {
        // Wave 要求文件以换行结束；与 YDWE 一样补一个空注释行。
        script += "/**/\r\n";
        const auto waveInput = dir / "2_wave_input.j";
        if (!WriteFile(waveInput, script) || !RunWave(result, options, waveInput, dir / "3_wave.j")) return result;
        auto waveText = ReadFile(dir / "3_wave.j");
        if (!waveText) {
            Fail(result, "WAVE_OUTPUT", "无法读取 Wave 输出");
            return result;
        }
        preprocessed = std::move(*waveText);
    }

    auto& templateStage = BeginStage(result, options, "template", dir / "4_template.j");
    const auto templateStarted = Clock::now();
    auto evaluated = EvaluateTemplates(preprocessed, templates);
    Jass::SourceLexicalView frameworkEvaluatedView;
    if (options.frameworkInputs && result.frameworkPreprocessSources) {
        const auto& owned = result.frameworkPreprocessSources->preprocessed;
        if (evaluated.text == owned.text) frameworkEvaluatedView = owned.lexicalView;
        else if (!owned.lexicalView.opaque.empty()) {
            auto rebound = RebindFrameworkLexicalView(evaluated.text, owned.text, owned.lexicalView);
            if (!rebound.Succeeded()) {
                result.diagnostics.insert(result.diagnostics.end(), rebound.diagnostics.begin(), rebound.diagnostics.end());
                Fail(result, "SCRIPT_FRAMEWORK_TEMPLATE_OPAQUE_PROVENANCE_UNSUPPORTED",
                    "模板结果的不透明原字节位置无法严格恢复；未发布部分产物。");
                templateStage.exitCode = 1; return result;
            }
            frameworkEvaluatedView = std::move(*rebound.value);
        } else frameworkEvaluatedView.text = evaluated.text;
    }
    if (options.frameworkInputs && std::any_of(evaluated.diagnostics.begin(), evaluated.diagnostics.end(),
        [](const Diagnostic& diagnostic) { return diagnostic.code == "ABILITY_ORDER_UNKNOWN"; })) {
        result.diagnostics.insert(result.diagnostics.end(), evaluated.diagnostics.begin(), evaluated.diagnostics.end());
        Fail(result, "SCRIPT_FRAMEWORK_ABILITY_ORDER_UNRESOLVED", "模板命令ID未绑定到实际物编；框架子集不把未知命令按0发布。");
        templateStage.exitCode = 1;
        return result;
    }
    if (evaluated.skippedBlocks > 0 && templates.wholeChunk && !options.frameworkInputs) {
        // 含内置不支持的语句块：与 YDWE 保存相同，由真实模板整份执行一次；不混用内置求值结果。
        if (options.onStage) options.onStage("lua-template");
        auto whole = templates.wholeChunk(preprocessed);
        templateStage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - templateStarted);
        for (auto& diagnostic : whole.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (!whole.Succeeded()) return result;
        evaluated.text = std::move(whole.value->script);
        result.objectMembers = std::move(whole.value->objectMembers);
        templateStage.log = "YDWE 模板整份执行；物编输出 " + std::to_string(result.objectMembers.size()) + " 个成员";
        for (const auto& message : whole.value->toolMessages) templateStage.log += '\n' + message;
    } else {
        templateStage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - templateStarted);
        templateStage.log = std::to_string(evaluated.expressions) + " 个表达式，跳过 " + std::to_string(evaluated.skippedBlocks) + " 个代码块";
        const bool templateOk = evaluated.Succeeded();
        for (auto& diagnostic : evaluated.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (!templateOk) { templateStage.exitCode = 1; return result; }
        if (!evaluated.imports.empty()) templateStage.log += "；import " + std::to_string(evaluated.imports.size()) + " 个文件";
        result.templateImports = std::move(evaluated.imports);
    }
    const auto placeholderVerified = Detail::VerifyJapiPlaceholderResult(evaluated.text, placeholderRetained);
    result.diagnostics.insert(result.diagnostics.end(), placeholderVerified.diagnostics.begin(), placeholderVerified.diagnostics.end());
    if (!placeholderVerified.Succeeded()) { templateStage.exitCode = 1; return result; }
    if (!WriteFile(dir / "4_template.j", evaluated.text)) {
        Fail(result, "WORKDIR_WRITE", "无法写入 4_template.j");
        return result;
    }
    if (options.frameworkInputs) {
        auto& stage = BeginStage(result, options, "identifier", dir / "5_identifier.j");
        const auto started = Clock::now();
        auto digest = Sha256Text(evaluated.text);
        if (!digest.Succeeded()) { result.diagnostics.insert(result.diagnostics.end(), digest.diagnostics.begin(), digest.diagnostics.end()); return result; }
        Jass::IdentifierRewriteRequest request;
        request.text = evaluated.text; request.encoding = Text::SourceEncoding::Utf8;
        const auto sourceName = (dir / "4_template.j").u8string();
        request.sourceName.assign(sourceName.begin(), sourceName.end());
        request.expectedInputSha256 = *digest.value; request.globalScopeIdentity = "map:root";
        if (result.frameworkPreprocessSources) request.lexicalView = &frameworkEvaluatedView;
        auto rewritten = Jass::RewriteUdgIdentifiers(request);
        for (auto& diagnostic : rewritten.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
        if (!rewritten.Succeeded()) { stage.exitCode = 1; return result; }
        result.frameworkIdentifierReport = std::make_shared<const Jass::IdentifierRewriteResult>(std::move(*rewritten.value));
        evaluated.text = result.frameworkIdentifierReport->text;
        stage.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        stage.log = "根udg词法改名 " + std::to_string(result.frameworkIdentifierReport->mappings.size()) +
            " 项；" + result.frameworkIdentifierReport->ruleVersion;
        if (!WriteFile(stage.output, evaluated.text)) {
            Fail(result, "WORKDIR_WRITE", "无法写出标识符阶段副本"); return result;
        }
        // 翻译器不必保留检查pragma；在翻译前检查已拥有的阶段文本，防止原例外被隐藏。
        if (!CheckFrameworkPragmas(result, evaluated.text, stage.output)) { stage.exitCode = 1; return result; }
    }

    std::optional<GameScripts> scripts;
    std::string finalScript;
    bool translated = false;
    if (options.vjass == ScriptEngine::Builtin) {
        scripts = ReadGameScripts(result, options);
        if (!scripts) return result;
        bool fallback = false;
        translated = RunBuiltinVJass(result, options, *scripts, evaluated.text, finalScript, fallback);
        if (!translated && !fallback) return result;
    }
    if (!translated) {
        // jasshelper 翻译并调用 pjass 检查
        if (!RunJassHelper(result, options, dir / (options.frameworkInputs ? "5_identifier.j" : "4_template.j"), dir / "6_vjass.j")) return result;
        auto output = ReadFile(dir / "6_vjass.j");
        if (!output) {
            Fail(result, "JASSHELPER_OUTPUT", "无法读取 jasshelper 输出");
            return result;
        }
        result.script = std::move(*output);
        if (options.frameworkInputs) {
            if (!scripts) scripts = ReadGameScripts(result, options);
            if (!scripts || !RunBuiltinCheck(result, options, *scripts, result.script)) {
                result.script.clear(); return result;
            }
        }
        result.syntaxChecked = true;
        result.checkedBy = "jasshelper / pjass";
        VerifyLayout(result, options);
        return result;
    }
    if (options.checker == ScriptEngine::Builtin) {
        const bool passed = RunBuiltinCheck(result, options, *scripts, finalScript);
        result.checkedBy = "内置 vJass 翻译与语法检查";
        if (options.crossCheck) {
            const auto external = RunPjass(result, options, dir / "6_vjass.j", false);
            if (external && *external != passed) {
                Warn(result, "CHECK_MISMATCH", std::string("内置检查") + (passed ? "通过" : "报错") + "，pjass " + (*external ? "通过" : "报错") +
                    "（见 pjass 阶段日志），请反馈此差异");
            } else if (external) {
                result.checkedBy += "（pjass 对照一致）";
            }
        }
        if (!passed) return result;
    } else {
        const auto passed = RunPjass(result, options, dir / "6_vjass.j", true);
        if (!passed || !*passed) return result;
        if (options.frameworkInputs && !RunBuiltinCheck(result, options, *scripts, finalScript)) return result;
        result.checkedBy = "内置 vJass 翻译 + pjass";
    }
    result.script = std::move(finalScript);
    result.syntaxChecked = true;
    VerifyLayout(result, options);
    return result;
}

namespace {
// 只为提示服务的轻量 #include 展开：按行识别 #include "x" / <x>，先相对包含者目录、再相对库根目录解析，
// 只跟随库根目录内的文件；有文件数、单文件大小和深度上限，超出时按已知部分给提示。
using IncludeClosure = std::set<std::filesystem::path>;
std::vector<std::filesystem::path> IncludedFiles(const std::filesystem::path& file, const std::filesystem::path& root) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size > 8 * 1024 * 1024) return result;
    std::ifstream stream(file, std::ios::binary);
    for (std::string line; std::getline(stream, line);) {
        auto at = line.find_first_not_of(" \t");
        if (at == std::string::npos || line[at] != '#') continue;
        at = line.find_first_not_of(" \t", at + 1);
        if (at == std::string::npos || line.compare(at, 7, "include") != 0) continue;
        const auto open = line.find_first_of("\"<", at + 7);
        if (open == std::string::npos) continue;
        const auto close = line.find(line[open] == '"' ? '"' : '>', open + 1);
        if (close == std::string::npos) continue;
        const auto raw = line.substr(open + 1, close - open - 1);
        const std::filesystem::path name(std::u8string(raw.begin(), raw.end()));
        for (const auto& base : {file.parent_path(), root}) {
            auto candidate = (base / name).lexically_normal();
            const auto relative = candidate.lexically_relative(root);
            if (relative.empty() || *relative.begin() == "..") continue;
            if (std::filesystem::is_regular_file(candidate, error)) { result.push_back(std::move(candidate)); break; }
        }
    }
    return result;
}
IncludeClosure EntryClosure(const std::filesystem::path& entry, const std::filesystem::path& root, std::size_t& budget) {
    IncludeClosure closure;
    std::vector<std::pair<std::filesystem::path, int>> pending{{entry.lexically_normal(), 0}};
    while (!pending.empty() && budget > 0) {
        auto [file, depth] = std::move(pending.back());
        pending.pop_back();
        if (!closure.insert(file).second) continue;
        --budget;
        if (depth >= 32) continue;
        for (auto& next : IncludedFiles(file, root)) pending.emplace_back(std::move(next), depth + 1);
    }
    return closure;
}
} // namespace

// 最终检查报“未声明的函数”而该函数在函数库 jass 目录的某个文件里有声明时，说明那个文件不会被自动注入
// （例如没有对应 .cfg 的 MemHack 库），提示作者在自定义脚本区 #include；只提示，不替作者添加。
void AppendMissingIncludeHints(std::vector<Diagnostic>& diagnostics, std::size_t first,
    const std::vector<std::filesystem::path>& jassDirs) {
    constexpr std::string_view prefix = "未声明的函数 “";
    constexpr std::string_view suffix = "”";
    std::set<std::string> missing;
    for (std::size_t index = first; index < diagnostics.size(); ++index) {
        const auto& message = diagnostics[index].message;
        // 消息带“文件:行: ”前缀，按片段查找函数名。
        const auto at = diagnostics[index].code == "JASS_UNDECLARED" ? message.find(prefix) : std::string::npos;
        if (at == std::string::npos) continue;
        const auto begin = at + prefix.size(), end = message.find(suffix, begin);
        if (end != std::string::npos) missing.insert(message.substr(begin, end - begin));
    }
    if (missing.empty()) return;
    std::vector<std::filesystem::path> roots;
    auto addRoot = [&roots](std::filesystem::path root) {
        root = root.lexically_normal();
        if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(std::move(root));
    };
    for (const auto& root : jassDirs) addRoot(root);
    const auto scan = Trigger::ScanUiJassRoots(roots);
    // 声明所在文件常由库根目录下的入口文件（如 memory_hack.j）间接包含，文档目录里的副本则无人包含；
    // 所以提示能包含到它的入口文件，而不是声明所在的文件本身。多个入口都能包含时取不被其他入口包含的最外层。
    std::map<std::filesystem::path, std::vector<std::string>> declaredIn;
    for (const auto& declaration : scan.declarations) {
        if (!missing.contains(declaration.name)) continue;
        auto& names = declaredIn[declaration.source.lexically_normal()];
        if (std::find(names.begin(), names.end(), declaration.name) == names.end()) names.push_back(declaration.name);
    }
    if (declaredIn.empty()) return;
    std::map<std::string, std::vector<std::string>> byInclude;
    std::set<std::string> hinted;
    std::size_t budget = 4096;
    for (const auto& root : roots) {
        std::vector<std::pair<std::filesystem::path, IncludeClosure>> entries;
        std::error_code error;
        for (std::filesystem::directory_iterator it(root, error), last; !error && it != last; it.increment(error)) {
            auto extension = it->path().extension().generic_u8string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](auto c) { return static_cast<decltype(c)>(std::tolower(static_cast<unsigned char>(c))); });
            if (!it->is_regular_file(error) || extension != u8".j") continue;
            entries.emplace_back(it->path().lexically_normal(), EntryClosure(it->path(), root, budget));
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [source, names] : declaredIn) {
            for (const auto& [entry, closure] : entries) {
                if (!closure.contains(source)) continue;
                const bool nested = std::any_of(entries.begin(), entries.end(), [&](const auto& other) {
                    return other.first != entry && other.second.contains(entry) && other.second.contains(source);
                });
                if (nested) continue;
                const auto text = entry.lexically_relative(root).generic_u8string();
                auto& list = byInclude[std::string(text.begin(), text.end())];
                for (const auto& name : names) {
                    if (hinted.insert(name).second) list.push_back(name);
                }
            }
        }
    }
    for (const auto& [include, names] : byInclude) {
        if (names.empty()) continue;
        std::string list;
        for (std::size_t index = 0; index < names.size() && index < 3; ++index) list += (index ? "、" : "") + names[index];
        if (names.size() > 3) list += " 等 " + std::to_string(names.size()) + " 个函数";
        diagnostics.push_back({DiagnosticSeverity::Warning, "Script.Pipeline", "JASS_MISSING_INCLUDE_HINT", list + " 由函数库文件 " + include +
            " 提供，但这次没有引入这个文件（不在自动注入列表里的库需要手动引入）。请在地图的自定义脚本区加入 #include \"" + include + "\"。", {}});
    }
}

} // namespace War3Map::Script
