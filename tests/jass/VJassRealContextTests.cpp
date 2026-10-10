#include "War3MapCore/Hash.h"
#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Jass/Preprocessor.h"
#include "War3MapCore/Jass/VJass.h"

#include <algorithm>
#include "War3MapCore/Script/ScriptBaseInputs.h"
#include "War3MapCore/Script/ScriptToolchainLayout.h"
#include "War3MapCore/IO/FileBytes.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Opt-in：只读冻结的最终7.8MB JASS → PP → vJass → 同一JASS检查器。
// 捕获的是已展开最终脚本，include数为0；不冒充原始GUI/include完整保存验收。
// 不打开/复制w3x、不生成候选、不启动任何编译器或游戏、不写用户安装。
namespace {
using namespace War3Map;

constexpr std::string_view kScriptSha = "df8dc9639222faaa70aed885119bb8beff096b9d402b0170ac45f9545bf59bf9";
constexpr std::string_view kCommonSha = "9cb04da1c75dd30025d96fec12b4302c2f6a1f1a8c864b66d6cf52c65ecde924";
constexpr std::string_view kBlizzardSha = "d290865bd673f8394f4e6fb7b4d6720412fed3a447877523cec9e54e65b8e62c";

void Require(bool value, std::string_view reason) {
    if (!value) throw std::runtime_error(std::string(reason));
}

template<class T> T Take(Result<T>&& result, std::string_view reason) {
    if (!result.Succeeded() || !result.value) {
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        throw std::runtime_error(std::string(reason));
    }
    return std::move(*result.value);
}

void Check(const Status& status, std::string_view reason) {
    if (!status.Succeeded()) {
        for (const auto& diagnostic : status.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        throw std::runtime_error(std::string(reason));
    }
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) {
        const auto error = GetLastError();
        Require(error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND,
            "real context environment is unreadable");
        return {};
    }
    std::wstring value(size, L'\0');
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(name, value.data(), size);
    if (!length) {
        const auto error = GetLastError();
        Require(error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND,
            "real context environment is unreadable");
        return {};
    }
    Require(length < size, "real context environment is unreadable");
    value.resize(length);
    return std::filesystem::path(value);
}

std::string_view View(std::span<const std::byte> bytes) {
    return {bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void Run(const std::filesystem::path& root, const std::filesystem::path& repository,
    const std::filesystem::path& tools) {
    Require(root.is_absolute() && tools.is_absolute(), "configured real context paths must be absolute");
    Require(std::filesystem::is_directory(root), "configured editor workspace is not a directory");
    if (!repository.empty()) {
        Require(repository.is_absolute() && std::filesystem::equivalent(root, repository),
            "real context root must match configured FORGE_REAL_REPO_ROOT");
    }
    const auto scriptPath = root / "artifacts/astra-rounds/nightly-maptest-20261006/audastra-war3map.j";
    const auto expectedCommon = tools / "compiler/jass/24/common.j";
    const auto expectedBlizzard = tools / "compiler/jass/24/blizzard.j";
    const auto scriptBefore = Take(Sha256File(scriptPath), "readonly actual script SHA");
    const auto commonBefore = Take(Sha256File(expectedCommon), "readonly actual common SHA");
    const auto blizzardBefore = Take(Sha256File(expectedBlizzard), "readonly actual blizzard SHA");
    Require(scriptBefore == kScriptSha && commonBefore == kCommonSha && blizzardBefore == kBlizzardSha,
        "script or actual save-chain base library identity changed");
    const auto script = Take(IO::ReadFileBytes(scriptPath, 32 * 1024 * 1024), "bounded captured final JASS read");
    Require(script.size() == 7877622 && Take(Sha256(script), "captured final JASS byte SHA") == kScriptSha,
        "actual captured final JASS bytes mismatch");

    const auto layout = Take(Script::ResolveScriptToolchainLayout({tools, 24, {}}), "actual save-chain layout resolution");
    Require(layout.kind == Script::ScriptToolchainLayoutKind::Modern && layout.commonScript == expectedCommon
        && layout.blizzardScript == expectedBlizzard, "actual save layout is not the known BoundLayout context");
    Script::ScriptBaseInputsRequest request;
    request.layout = &layout;
    request.policy = Script::ScriptBasePolicy::BoundLayout; // 同生产现代保存的默认策略，不伪造native声明。
    const auto bases = Take(Script::CaptureScriptBaseInputs(request), "capture actual common/blizzard via production API");
    Check(Script::VerifyScriptBaseInputs(bases, layout, request.policy), "production base snapshot identity verification");
    Require(bases.common.origin == Script::ScriptBaseOrigin::Layout && bases.blizzard.origin == Script::ScriptBaseOrigin::Layout
        && bases.common.sourceRoot == expectedCommon && bases.blizzard.sourceRoot == expectedBlizzard
        && bases.common.bytes.size() == 166690 && bases.blizzard.bytes.size() == 451847
        && bases.common.sha256 == kCommonSha && bases.blizzard.sha256 == kBlizzardSha,
        "production base capture returned unexpected identity or provenance");

    const auto verify = [&] {
        Require(scriptBefore == Take(Sha256File(scriptPath), "readonly final script SHA")
            && commonBefore == Take(Sha256File(expectedCommon), "readonly final common SHA")
            && blizzardBefore == Take(Sha256File(expectedBlizzard), "readonly final blizzard SHA"),
            "readonly transformation changed script or installation base bytes");
        Check(Script::VerifyScriptToolchainLayout(layout), "layout unchanged after transform");
        Check(Script::VerifyScriptBaseInputs(bases, layout, request.policy), "base input unchanged after transform");
    };
    std::cout << "script=" << IO::PathUtf8(scriptPath) << " bytes=" << script.size() << " SHA256=" << scriptBefore << '\n';
    std::cout << "common=" << IO::PathUtf8(bases.common.sourceRoot) << " bytes=" << bases.common.bytes.size()
        << " SHA256=" << bases.common.sha256 << " provenance=BoundLayout\n";
    std::cout << "blizzard=" << IO::PathUtf8(bases.blizzard.sourceRoot) << " bytes=" << bases.blizzard.bytes.size()
        << " SHA256=" << bases.blizzard.sha256 << " provenance=BoundLayout\n";
    try {
        Jass::JassCheckOptions limits;
        limits.returnBug = bases.allowReturnBug;
        Require(!limits.syntaxOnly && !limits.returnBug, "real modern context uses full strict default semantics");
        limits.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30); // 三阶段共用绝对截止。
        Jass::PreprocessOptions ppOptions;
        ppOptions.check = limits;
        ppOptions.check.maxSources = 1024;
        const auto started = std::chrono::steady_clock::now();
        const auto pp = Jass::Preprocess(View(script), "war3map.j", {}, ppOptions);
        const auto ppElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "actual_pp completed=" << pp.completed << " bytes=" << pp.text.size() << " tokens=" << pp.tokens
            << " expansionNodes=" << pp.expansionNodes << " includes=" << pp.filesIncluded << " cacheHits=" << pp.cacheHits
            << " macroExpansions=" << pp.macroExpansions << " mapSegments=" << pp.sourceMap.SegmentCount()
            << " elapsedSeconds=" << ppElapsed << '\n';
        for (const auto& diagnostic : pp.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        Require(pp.completed && pp.Succeeded() && pp.filesIncluded == 0 && pp.tokens > 1000,
            "captured already-expanded script failed PP; not a full original include gate");
        Jass::VJassOptions vOptions;
        vOptions.check = limits;
        vOptions.baseScripts = {{"common.j", View(bases.common.bytes)}, {"blizzard.j", View(bases.blizzard.bytes)}};
        const auto vStarted = std::chrono::steady_clock::now();
        const auto translated = Jass::TranslateVJass(pp.text, "war3map.j", vOptions);
        const auto vElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - vStarted).count();
        std::cout << "actual_vjass completed=" << translated.completed << " bytes=" << translated.text.size()
            << " tokens=" << translated.tokens << " ast=" << translated.astNodes << " depth=" << translated.maxAstDepthObserved
            << " libraries=" << translated.libraries << " structs=" << translated.structs << " functions=" << translated.functions
            << " mapSegments=" << translated.sourceMap.SegmentCount() << " byteOrigins=" << translated.byteOrigins.size()
            << " elapsedSeconds=" << vElapsed << '\n';
        for (const auto& diagnostic : translated.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        if (!translated.unsupported.empty()) std::cerr << "VJASS_UNSUPPORTED: " << translated.unsupported << '\n';
        Require(translated.completed && translated.Succeeded() && !translated.sourceMap.Empty(), "actual final script failed bounded vJass emitter");
        std::size_t covered = 0, exactBytes = 0, generatedBytes = 0;
        for (const auto& origin : translated.byteOrigins) {
            Require(origin.outputOffset == covered && origin.byteLength != 0 && covered <= translated.text.size()
                && origin.byteLength <= translated.text.size() - covered && origin.sourceOffset <= pp.text.size(),
                "actual byte provenance must cover output in sorted disjoint emitted fragments");
            if (origin.exact) {
                Require(origin.byteLength <= pp.text.size() - origin.sourceOffset
                    && std::string_view(translated.text).substr(origin.outputOffset, origin.byteLength)
                        == std::string_view(pp.text).substr(origin.sourceOffset, origin.byteLength), "exact emitted fragments changed captured source bytes");
                exactBytes += origin.byteLength;
            } else generatedBytes += origin.byteLength;
            covered += origin.byteLength;
        }
        Require(covered == translated.text.size() && exactBytes > 1000 && !translated.FindByteOrigin(covered),
            "actual full context byte origins incomplete or EOF spuriously mapped");
        std::cout << "actual_byte_origins covered=" << covered << " exactBytes=" << exactBytes
            << " generatedAnchoredBytes=" << generatedBytes << '\n';
        const auto lines = 1 + static_cast<std::uint32_t>(std::count(translated.text.begin(), translated.text.end(), '\n'));
        const auto map = translated.sourceMap.ComposeWith(pp.sourceMap, lines); // outer只有本次单一输入名。
        const auto originalLines = 1 + static_cast<std::uint32_t>(std::count(View(script).begin(), View(script).end(), '\n'));
        std::uint32_t outputLine = 1;
        std::size_t mappedLines = 0, at = 0;
        while (at < translated.text.size()) {
            const auto position = map.Find(outputLine);
            Require(position && position->file < map.Files().size() && map.Files()[position->file] == "war3map.j"
                && position->line > 0 && position->line <= originalLines,
                "actual emitted nonempty line has no bounded original-file provenance");
            ++mappedLines;
            const auto end = translated.text.find('\n', at);
            if (end == std::string::npos) break;
            at = end + 1;
            ++outputLine;
        }
        std::cout << "actual_composed_map mappedLines=" << mappedLines << " segments=" << map.SegmentCount()
            << " originalLines=" << originalLines << " files=" << map.Files().size()
            << " evidence=captured-final-script-with-zero-include-not-original-GUI-save\n";
        const std::vector<Jass::JassSource> sources{{"common.j", View(bases.common.bytes)},
            {"blizzard.j", View(bases.blizzard.bytes)}, {"war3map.j", translated.text}};
        const auto checkStarted = std::chrono::steady_clock::now();
        const auto result = Jass::CheckJass(sources, limits);
        const auto checkElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - checkStarted).count();
        std::cout << "actual_check completed=" << result.completed << " semanticChecked=" << result.semanticChecked
            << " functions=" << result.functions << " lines=" << result.lines << " tokens=" << result.tokens
            << " ast=" << result.astNodes << " maxDepth=" << result.maxAstDepthObserved
            << " sourceWorkers=" << result.sourceThreadsUsed << " bodyWorkers=" << result.threadsUsed
            << " diagnosticBytes=" << result.diagnosticBytes << " elapsedSeconds=" << checkElapsed << '\n';
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        verify();
        Require(result.completed && result.semanticChecked && result.Succeeded() && result.functions >= 834
            && result.tokens > 1000 && result.astNodes > 1000 && mappedLines > 1000
            && result.maxAstDepthObserved <= limits.maxAstDepth, "actual full default semantic gate after PP/vJass failed");
        std::cout << "Real PP/vJass context gate passed; three identities unchanged; zero map copies/candidates/compiler/game/installation writes\n";
    } catch (...) {
        verify(); // 失败也回读实际三文件身份，不把不完整阶段写成成功。
        throw;
    }
}

} // namespace

int main() {
    try {
        const auto root = EnvironmentPath(L"FORGE_VJASS_REAL_CONTEXT_ROOT");
        const auto tools = EnvironmentPath(L"FORGE_YDWE_ROOT");
        const auto repository = EnvironmentPath(L"FORGE_REAL_REPO_ROOT");
        if (root.empty() || tools.empty()) {
            std::cout << "SKIP: FORGE_VJASS_REAL_CONTEXT_ROOT and FORGE_YDWE_ROOT are required for the real PP/vJass context gate.\n";
            return 77;
        }
        Run(root, repository, tools);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Real PP/vJass context gate failed: " << error.what() << '\n';
        return 1;
    }
}
