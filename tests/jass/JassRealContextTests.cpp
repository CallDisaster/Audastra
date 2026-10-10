#include "War3MapCore/Hash.h"
#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Script/ScriptBaseInputs.h"
#include "War3MapCore/Script/ScriptToolchainLayout.h"
#include "War3MapCore/IO/FileBytes.h"

#include <Windows.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Opt-in：只读冻结的最终7.8MiB JASS，基础库从实际保存链同一BoundLayout捕获。
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

    Jass::JassCheckOptions options;
    options.returnBug = bases.allowReturnBug;
    Require(!options.syntaxOnly && !options.returnBug, "real modern save gate uses full strict default semantics");
    const std::vector<Jass::JassSource> sources{{"common.j", View(bases.common.bytes)},
        {"blizzard.j", View(bases.blizzard.bytes)}, {"war3map.j", View(script)}};
#ifdef FORGE_SYMBOL_COLLECTION_REAL_GATE
    const auto baseline = Jass::CheckJass(sources, options);
    options.collectSymbols = true;
#endif
    const auto started = std::chrono::steady_clock::now();
    const auto result = Jass::CheckJass(sources, options);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    // 无论成功/失败均先输出真实指标；parent据actualexit作门禁，不从预期值推导通过。
    std::cout << "script=" << IO::PathUtf8(scriptPath) << " bytes=" << script.size() << " SHA256=" << scriptBefore << '\n';
    std::cout << "common=" << IO::PathUtf8(bases.common.sourceRoot) << " bytes=" << bases.common.bytes.size()
        << " SHA256=" << bases.common.sha256 << " provenance=BoundLayout\n";
    std::cout << "blizzard=" << IO::PathUtf8(bases.blizzard.sourceRoot) << " bytes=" << bases.blizzard.bytes.size()
        << " SHA256=" << bases.blizzard.sha256 << " provenance=BoundLayout\n";
    std::cout << "actual_check completed=" << result.completed << " semanticChecked=" << result.semanticChecked
        << " functions=" << result.functions << " lines=" << result.lines << " tokens=" << result.tokens
        << " ast=" << result.astNodes << " maxDepth=" << result.maxAstDepthObserved
        << " sourceWorkers=" << result.sourceThreadsUsed << " bodyWorkers=" << result.threadsUsed
        << " diagnosticBytes=" << result.diagnosticBytes << " elapsedSeconds=" << elapsed << '\n';
    for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';

    const auto scriptAfter = Take(Sha256File(scriptPath), "readonly final script SHA");
    const auto commonAfter = Take(Sha256File(expectedCommon), "readonly final common SHA");
    const auto blizzardAfter = Take(Sha256File(expectedBlizzard), "readonly final blizzard SHA");
    Require(scriptBefore == scriptAfter && commonBefore == commonAfter && blizzardBefore == blizzardAfter,
        "readonly gate changed script or installation base bytes");
    Check(Script::VerifyScriptToolchainLayout(layout), "layout unchanged after check");
    Check(Script::VerifyScriptBaseInputs(bases, layout, request.policy), "base input unchanged after check");
    Require(result.completed && result.semanticChecked
#ifndef FORGE_SYMBOL_COLLECTION_REAL_GATE
        && result.Succeeded()
#endif
        && result.functions >= 834
        && result.tokens > 1000 && result.astNodes > 1000 && result.sourceThreadsUsed <= options.maxSourceWorkers
        && result.threadsUsed <= options.maxBodyWorkers && result.maxAstDepthObserved <= options.maxAstDepth,
        "actual full real-map context failed the bounded semantic gate");
#ifdef FORGE_SYMBOL_COLLECTION_REAL_GATE
    Require(baseline.completed && baseline.semanticChecked && baseline.Succeeded() == result.Succeeded()
        && baseline.diagnostics.size() == result.diagnostics.size(), "Optional symbol collection changed real save-check outcome");
    for (std::size_t i = 0; i < baseline.diagnostics.size(); ++i)
        Require(baseline.diagnostics[i].code == result.diagnostics[i].code && baseline.diagnostics[i].message == result.diagnostics[i].message,
            "Optional symbol collection changed real diagnostics");
    Require(result.symbols.definitions.size() > 1000 && result.symbols.references.size() > 1000
        && result.symbols.definitions.size() <= options.maxSymbolDefinitions
        && result.symbols.references.size() <= options.maxSymbolReferences, "Real semantic collection empty or exceeded its budget");
    std::cout << "actual_symbol_collection definitions=" << result.symbols.definitions.size()
        << " references=" << result.symbols.references.size() << " complete=" << result.symbols.completed
        << " map_script_passed=" << result.Succeeded() << " baseline_diagnostics=" << baseline.diagnostics.size()
        << " ignored_errors=" << result.errorsIgnored << " unknown_targets="
        << std::count_if(result.symbols.references.begin(), result.symbols.references.end(), [](const auto& reference) { return !reference.target; })
        << " elapsedSeconds=" << elapsed << "\nBounded real symbol collection preserved save-check verdict; this is not map validity/gameplay proof\n";
#else
    std::cout << "Real JASS context gate passed; all original captured/script/library hashes unchanged; no candidate/compiler/game/installation writes\n";
#endif
}

} // namespace

int main() {
    try {
        const auto root = EnvironmentPath(L"FORGE_JASS_REAL_CONTEXT_ROOT");
        const auto tools = EnvironmentPath(L"FORGE_YDWE_ROOT");
        const auto repository = EnvironmentPath(L"FORGE_REAL_REPO_ROOT");
        if (root.empty() || tools.empty()) {
            std::cout << "SKIP: FORGE_JASS_REAL_CONTEXT_ROOT and FORGE_YDWE_ROOT are required for the real JASS context gate.\n";
            return 77;
        }
        Run(root, repository, tools);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Real JASS context gate failed: " << error.what() << '\n';
        return 1;
    }
}
