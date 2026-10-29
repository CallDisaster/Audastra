#include "War3MapCore/Services/MapTestService.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"

#include <algorithm>
#include <fstream>
#include <span>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace War3Map::Services {
namespace {
template<class T> Result<T> Fail(std::string code, std::string message) {
    return Result<T>::Failure("Map.Test", std::move(code), std::move(message));
}
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    });
    return value;
}
bool Within(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto a = Lower(path.generic_wstring()), b = Lower(root.generic_wstring());
    return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == L'/');
}
bool OrdinaryAncestors(const std::filesystem::path& path) {
    auto current = path;
    bool leaf = true;
    while (!current.empty()) {
#ifdef _WIN32
        const auto attrs = GetFileAttributesW(current.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && ((attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            || (!leaf && !(attrs & FILE_ATTRIBUTE_DIRECTORY)))) return false;
#else
        std::error_code ec;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(current, ec))) return false;
#endif
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
        leaf = false;
    }
    return true;
}
bool WriteNew(const std::filesystem::path& path, std::span<const std::byte> bytes) {
#ifdef _WIN32
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written != count) { ok = false; break; }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
#else
    if (std::filesystem::exists(path)) return false;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output.flush());
#endif
}
}

std::filesystem::path MapTestYdweInstallRoot(const std::filesystem::path& configuredUiRoot) {
    auto root = configuredUiRoot;
    if (!root.empty() && root.filename().empty() && root.has_relative_path()) root = root.parent_path();
    if (Lower(root.filename().wstring()) == L"ui") return root.parent_path();
    if (Lower(root.filename().wstring()) == L"mpq" && Lower(root.parent_path().filename().wstring()) == L"share")
        return root.parent_path().parent_path();
    return root;
}

Result<MapTestArtifact> PrepareMapTest(const MapTestRequest& request,
    const MapTestSlkConverter& converter, const MapTestProgress& progress) {
    if (request.enableLuaHotReload && !request.useOwnedLuaRuntime)
        return Fail<MapTestArtifact>("MAP_TEST_LUA_RELOAD_RUNTIME", "Lua 热重载只能用于显式选择的自有 Lua 测试运行时。");
    std::error_code ec;
    if (!request.inputMap.is_absolute() || !request.workspace.is_absolute() || !request.gameRoot.is_absolute()
        || (!request.sourceMap.empty() && !request.sourceMap.is_absolute())
        || (!request.useOwnedLuaRuntime && !request.runtimeRoot.empty() && !request.runtimeRoot.is_absolute()))
        return Fail<MapTestArtifact>("MAP_TEST_PATH", "测试输入、独占目录与游戏目录必须是绝对路径。");
    const auto input = std::filesystem::weakly_canonical(request.inputMap, ec);
    if (ec) return Fail<MapTestArtifact>("MAP_TEST_PATH", "无法核对测试输入路径。");
    const auto workspace = std::filesystem::weakly_canonical(request.workspace, ec);
    if (ec || !OrdinaryAncestors(request.workspace))
        return Fail<MapTestArtifact>("MAP_TEST_WORKSPACE", "测试目录不得经重解析点或无效父目录写出。");
    const auto game = std::filesystem::weakly_canonical(request.gameRoot, ec);
    if (ec) return Fail<MapTestArtifact>("MAP_TEST_PATH", "无法核对游戏目录。");
    auto runtime = request.useOwnedLuaRuntime ? std::filesystem::path{} : request.runtimeRoot;
    if (!runtime.empty()) {
        runtime = std::filesystem::weakly_canonical(runtime, ec);
        if (ec) return Fail<MapTestArtifact>("MAP_TEST_PATH", "无法核对独立运行时目录。");
    }
    const auto ydwe = !request.useOwnedLuaRuntime && runtime.empty() ? request.runtimeYdweRoot : std::filesystem::path{};
    // 未使用的旧 UI 设置不应阻止普通地图。此处仅保护输出目录；实际加载时再严格核对路径。
    for (const auto& configured : {MapTestYdweInstallRoot(ydwe), ydwe}) {
        if (configured.empty() || !configured.is_absolute()) continue;
        const auto resolved = std::filesystem::weakly_canonical(configured, ec);
        if (ec) continue;
        // UI 子目录可能重定向到共享数据，同时保护原设置的安装根与解析后的只读数据根。
        if (Within(workspace, resolved))
            return Fail<MapTestArtifact>("MAP_TEST_WORKSPACE", "独占测试目录不能位于本机 YDWE 安装或数据目录内。");
    }
    if (Within(workspace, game) || (!runtime.empty() && Within(workspace, runtime)))
        return Fail<MapTestArtifact>("MAP_TEST_WORKSPACE", "独占测试目录不能位于游戏或工具安装内。");
    const auto extension = Lower(input.extension().wstring());
    if (extension != L".w3x" && extension != L".w3m")
        return Fail<MapTestArtifact>("MAP_TEST_FORMAT", "测试输入必须是已冻结的w3x或w3m地图归档。");
    if (request.optimizeSlk && !converter)
        return Fail<MapTestArtifact>("MAP_TEST_SLK_UNAVAILABLE", "SLK测试优化适配器尚未就绪，测试已停止。");
    if (request.maxMapBytes == 0 || request.maxMapBytes > 1024ull * 1024 * 1024)
        return Fail<MapTestArtifact>("MAP_TEST_BUDGET", "测试地图读取预算超出支持范围。");
    if (progress) progress("copy");
    auto bytes = ReadVerifiedFile(input, request.expectedInputSha256, request.maxMapBytes);
    if (!bytes.Succeeded()) { Result<MapTestArtifact> result; result.diagnostics = std::move(bytes.diagnostics); return result; }
    // create_directory 只允许本次新建；既有目录即使为空也不得取得所有权。
    if (!std::filesystem::create_directory(workspace, ec) || ec)
        return Fail<MapTestArtifact>("MAP_TEST_WORKSPACE_EXISTS", "测试独占目录已存在或无法创建，请创建新的测试任务目录。");
    const auto copy = workspace / std::filesystem::path(L"input" + extension);
    const auto target = workspace / std::filesystem::path(L"test" + extension);
    if (!request.sourceMap.empty()) {
        const auto source = std::filesystem::weakly_canonical(request.sourceMap, ec);
        if (ec || source == copy || source == target || Within(source, workspace))
            return Fail<MapTestArtifact>("MAP_TEST_SOURCE_ALIAS", "原地图不能成为测试输出或位于测试独占目录。");
    }
    const auto copied = request.optimizeSlk ? copy : target;
    if (!WriteNew(copied, *bytes.value))
        return Fail<MapTestArtifact>("MAP_TEST_COPY", "独立测试副本写入失败，原地图保持不变。");
    bytes.value.reset();
    auto verified = ReadVerifiedFile(copied, request.expectedInputSha256, request.maxMapBytes);
    if (!verified.Succeeded()) { Result<MapTestArtifact> result; result.diagnostics = std::move(verified.diagnostics); return result; }
    verified.value.reset();
    // 转换摘要等提示随成功结果返回给宿主显示。
    std::vector<Diagnostic> slkNotes;
    if (request.optimizeSlk) {
        if (progress) progress("slk");
        const auto optimized = converter({copy, target, game, workspace, request.expectedInputSha256});
        if (!optimized.Succeeded()) { Result<MapTestArtifact> result; result.diagnostics = optimized.diagnostics; return result; }
        slkNotes = optimized.diagnostics;
    }
    if (progress) progress("verify");
    if (!OrdinaryAncestors(target) || !std::filesystem::is_regular_file(target, ec) || ec)
        return Fail<MapTestArtifact>("MAP_TEST_OUTPUT", "测试产物不是本次独占目录中的普通地图文件。");
    // 普通副本已经完整读取并验证输入SHA，不再为相同字节额外读取整张地图。
    auto outputSha = request.expectedInputSha256;
    std::transform(outputSha.begin(), outputSha.end(), outputSha.begin(), [](char c) {
        return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (request.optimizeSlk) {
        const auto sha = Sha256File(target);
        if (!sha.Succeeded()) { Result<MapTestArtifact> result; result.diagnostics = sha.diagnostics; return result; }
        outputSha = *sha.value;
    }
    // 进度回调或转换器执行后仍须再次核对冻结输入；不允许复用SHA掩盖后续变化。
    auto unchanged = ReadVerifiedFile(copied, request.expectedInputSha256, request.maxMapBytes);
    if (!unchanged.Succeeded()) { Result<MapTestArtifact> result; result.diagnostics = std::move(unchanged.diagnostics); return result; }
    auto prepared = Result<MapTestArtifact>::Success({target, workspace, game,
        request.expectedInputSha256, std::move(outputSha), request.optimizeSlk, request.sourceMap, input,
        runtime, request.runtimeBridge, request.runtimeBootstrap, ydwe});
    prepared.value->useOwnedLuaRuntime = request.useOwnedLuaRuntime;
    prepared.value->enableLuaHotReload = request.enableLuaHotReload;
    prepared.value->handwrittenLuaEngine = request.handwrittenLuaEngine;
    prepared.value->handwrittenLuaHotReload = request.handwrittenLuaHotReload;
    prepared.diagnostics.insert(prepared.diagnostics.end(), slkNotes.begin(), slkNotes.end());
    return prepared;
}

Result<MapTestLaunchPlan> BuildMapTestLaunchPlan(const MapTestArtifact& artifact) {
    if (artifact.enableLuaHotReload && !artifact.useOwnedLuaRuntime)
        return Fail<MapTestLaunchPlan>("MAP_TEST_LUA_RELOAD_RUNTIME", "Lua 热重载不能附加到普通 JASS 或外部扩展运行时。");
    if (!artifact.testMap.is_absolute() || !artifact.workspace.is_absolute() || !artifact.gameRoot.is_absolute())
        return Fail<MapTestLaunchPlan>("MAP_TEST_PATH", "测试地图、独占目录和游戏目录必须是绝对路径。");
    auto verified = ReadVerifiedFile(artifact.testMap, artifact.testMapSha256, 1024ull * 1024 * 1024);
    if (!verified.Succeeded()) { Result<MapTestLaunchPlan> result; result.diagnostics = std::move(verified.diagnostics); return result; }
    verified.value.reset();
    std::error_code ec;
    const auto testMap = std::filesystem::weakly_canonical(artifact.testMap, ec);
    if (ec) return Fail<MapTestLaunchPlan>("MAP_TEST_OUTPUT", "无法确认测试地图路径。");
    const auto workspace = std::filesystem::weakly_canonical(artifact.workspace, ec);
    if (ec || !Within(testMap, workspace) || !OrdinaryAncestors(artifact.testMap) || !OrdinaryAncestors(artifact.workspace))
        return Fail<MapTestLaunchPlan>("MAP_TEST_OUTPUT", "测试地图不属于独占目录或经重解析路径访问。");
    const auto game = std::filesystem::weakly_canonical(artifact.gameRoot, ec);
    if (ec || !OrdinaryAncestors(game))
        return Fail<MapTestLaunchPlan>("MAP_TEST_PATH", "游戏目录无法确认，或经过重解析路径。");
    for (const auto* name : {L"war3.exe", L"Game.dll", L"war3.mpq", L"war3patch.mpq", L"storm.dll"})
        if (!OrdinaryAncestors(game / name) || !std::filesystem::is_regular_file(game / name, ec) || ec)
            return Fail<MapTestLaunchPlan>("MAP_TEST_GAME_MISSING", "所选经典游戏目录缺少必需的游戏文件。");
    const auto extension = Lower(artifact.testMap.extension().wstring());
    if (extension != L".w3x" && extension != L".w3m")
        return Fail<MapTestLaunchPlan>("MAP_TEST_FORMAT", "测试副本必须是w3x或w3m地图。");
    // 经典客户端的-loadfile需要短相对路径；每次任务独占，绝不覆盖旧WorldEditTestMap。
    const auto identity = Sha256Text(workspace.generic_string() + "\n" + artifact.testMapSha256);
    if (!identity.Succeeded()) { Result<MapTestLaunchPlan> result; result.diagnostics = identity.diagnostics; return result; }
    const auto relative = std::filesystem::path(L"Maps") / L"Audastra"
        / std::filesystem::path(identity.value->substr(0, 16)) / std::filesystem::path(L"test" + extension);
    const auto copied = game / relative;
    if (!OrdinaryAncestors(copied) || std::filesystem::exists(copied.parent_path(), ec) || ec)
        return Fail<MapTestLaunchPlan>("MAP_TEST_COPY_LOCATION", "本次短路径测试目录已存在或无法确认，请创建新的测试任务。");
    for (const auto& protectedPath : {artifact.sourceMap, artifact.inputMap, artifact.testMap}) {
        if (protectedPath.empty()) continue;
        const auto canonical = std::filesystem::weakly_canonical(protectedPath, ec);
        if (ec || Lower(canonical.generic_wstring()) == Lower(copied.generic_wstring())
            || Within(canonical, copied.parent_path()))
            return Fail<MapTestLaunchPlan>("MAP_TEST_COPY_SOURCE_ALIAS", "短路径测试副本与原图或冻结输入冲突。");
    }
    MapTestLaunchPlan plan;
    plan.executable = game / L"war3.exe";
    plan.workingDirectory = game;
    plan.testMap = testMap;
    plan.testMapSha256 = artifact.testMapSha256;
    plan.gameMapCopy = copied;
    plan.useOwnedLuaRuntime = artifact.useOwnedLuaRuntime;
    plan.enableLuaHotReload = artifact.enableLuaHotReload;
    plan.hiddenDesktop = artifact.hiddenDesktop;
    const auto gameSha = Sha256File(plan.executable), moduleSha = Sha256File(game / L"Game.dll");
    if (!gameSha.Succeeded() || !moduleSha.Succeeded())
        return Fail<MapTestLaunchPlan>("MAP_TEST_GAME_IDENTITY", "无法核对所选游戏启动文件的身份。");
    plan.gameExecutableSha256 = *gameSha.value;
    plan.gameModuleSha256 = *moduleSha.value;
    if (artifact.cloakedWindow && plan.gameModuleSha256 != kMapTestGame127ModuleSha256)
        return Fail<MapTestLaunchPlan>("MAP_TEST_CLOAK_UNSUPPORTED", "所选游戏无法确认隐身后台运行能力；不会改用不可见桌面或前台输入。");
    plan.backgroundPlay = (artifact.backgroundPlay || artifact.cloakedWindow) && plan.gameModuleSha256 == kMapTestGame127ModuleSha256;
    // 隐身测试必须同时安装后台运行与输入接收器，不能静默切换桌面策略。
    plan.cloakedWindow = artifact.cloakedWindow && plan.backgroundPlay;
    if (plan.cloakedWindow) plan.hiddenDesktop = false;
    const bool silent = artifact.hiddenDesktop || plan.cloakedWindow;
    plan.arguments = {L"-window", L"-loadfile", relative.wstring()};
    if (artifact.useOwnedLuaRuntime || !artifact.runtimeRoot.empty() || plan.backgroundPlay) {
        // 裸模式：地图不需要扩展运行库，只为后台运行钩子经过启动器。
        const bool bare = !artifact.useOwnedLuaRuntime && artifact.runtimeRoot.empty();
        if (!artifact.useOwnedLuaRuntime && artifact.runtimeLayout != MapTestRuntimeLayout::Flat && artifact.runtimeLayout != MapTestRuntimeLayout::YdweTree)
            return Fail<MapTestLaunchPlan>("MAP_TEST_RUNTIME_LAYOUT", "扩展运行时目录布局无效，未启动游戏。");
        if (!artifact.runtimeBridge.is_absolute() || !artifact.runtimeBootstrap.is_absolute()
            || (!artifact.useOwnedLuaRuntime && !bare && (!artifact.runtimeRoot.is_absolute()
                || !OrdinaryAncestors(artifact.runtimeRoot) || !std::filesystem::is_directory(artifact.runtimeRoot, ec) || ec)))
            return Fail<MapTestLaunchPlan>("MAP_TEST_RUNTIME_PATH", "扩展运行时必须使用独立的绝对目录。");
        for (const auto& file : {artifact.runtimeBridge, artifact.runtimeBootstrap})
            if (!OrdinaryAncestors(file) || !std::filesystem::is_regular_file(file, ec) || ec)
                return Fail<MapTestLaunchPlan>("MAP_TEST_RUNTIME_HELPER", "Audastra独立测试启动组件缺失，请使用完整安装包。");
        plan.executable = artifact.runtimeBridge;
        plan.runtimeRoot = artifact.useOwnedLuaRuntime || bare ? std::filesystem::path{} : artifact.runtimeRoot;
        plan.runtimeLayout = artifact.useOwnedLuaRuntime || bare ? MapTestRuntimeLayout::Flat : artifact.runtimeLayout;
        plan.runtimeBootstrap = artifact.runtimeBootstrap;
        const auto bootstrapSha = Sha256File(plan.runtimeBootstrap);
        if (!bootstrapSha.Succeeded()) return {std::nullopt, bootstrapSha.diagnostics};
        plan.runtimeBootstrapSha256 = *bootstrapSha.value;
        // 小型终态报告保留在安装目录下，不混入需按冻结身份清理的地图工作区。
        plan.runtimeReceipt = artifact.runtimeBridge.parent_path() / L"test-receipts"
            / std::filesystem::path(identity.value->substr(0, 16) + ".json");
        if (!OrdinaryAncestors(plan.runtimeReceipt) || std::filesystem::exists(plan.runtimeReceipt, ec) || ec)
            return Fail<MapTestLaunchPlan>("MAP_TEST_RUNTIME_RECEIPT", "独立测试报告位置已存在或无法确认。");
        plan.arguments = {L"--game", (game / L"war3.exe").wstring(), L"--map", relative.wstring(),
            L"--bootstrap", artifact.runtimeBootstrap.wstring(),
            L"--receipt", plan.runtimeReceipt.wstring()};
        if (artifact.useOwnedLuaRuntime) {
            plan.arguments.insert(plan.arguments.end(), {L"--runtime-mode", L"owned-lua",
                L"--lua-map-sha256", std::wstring(plan.testMapSha256.begin(), plan.testMapSha256.end())});
            // 启动器只接受显式 enabled；省略选项才是其关闭合同。
            if (artifact.enableLuaHotReload)
                plan.arguments.insert(plan.arguments.end(), {L"--lua-hot-reload", L"enabled"});
        } else if (bare) {
            plan.arguments.insert(plan.arguments.end(), {L"--runtime-mode", L"bare"});
        } else {
            plan.arguments.insert(plan.arguments.end(), {L"--runtime", artifact.runtimeRoot.wstring()});
        }
        if (!artifact.useOwnedLuaRuntime && !bare && artifact.runtimeLayout == MapTestRuntimeLayout::YdweTree) {
            plan.arguments.push_back(L"--runtime-layout");
            plan.arguments.push_back(L"ydwe-tree");
        }
        if (plan.backgroundPlay) plan.arguments.insert(plan.arguments.end(), {L"--background-play", L"enabled"});
        if (plan.cloakedWindow) plan.arguments.insert(plan.arguments.end(), {L"--window-cloak", L"enabled"});
        // 看不见的后台测试不出声。经典 1.27a 不认 -nosound（会连带丢掉 -loadfile），
        // 静音只能经启动器在游戏进程内完成；直接启动的不可见桌面测试无法静音。
        if (silent) plan.arguments.insert(plan.arguments.end(), {L"--game-nosound", L"enabled"});
    }
    const auto executableSha = Sha256File(plan.executable);
    if (!executableSha.Succeeded()) { Result<MapTestLaunchPlan> result; result.diagnostics = executableSha.diagnostics; return result; }
    plan.executableSha256 = *executableSha.value;
    return Result<MapTestLaunchPlan>::Success(std::move(plan));
}
} // namespace War3Map::Services
