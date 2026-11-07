#include "War3MapCore/HLua/Hslk.h"
#include "War3MapCore/Lua/LuaModuleScan.h"
#include "HslkSandbox.h"
#include <lauxlib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <exception>

namespace War3Map::HLua {
namespace {
constexpr std::string_view constants[]{"abilityTarget", "attribute", "cache", "damageSource", "event", "hero",
    "hotKey", "item", "monitor", "target", "unit", "orderStr"};
constexpr std::string_view foundations[]{"color", "array", "math", "string", "mbstring", "table", "description"};
Status RunFile(Detail::HslkSandbox& host, const Services::LuaFolderSnapshot& source, std::string_view path) {
    const auto* file = FindLuaFile(source, path);
    if (!file) return Status::Failure("HLua", "HLUA_FRAMEWORK_FILE_MISSING", "快照缺少文件：" + std::string(path));
    return host.Run(LuaText(*file), path);
}
Status Prelude(Detail::HslkSandbox& host, const FrameworkSnapshot& framework) {
    auto status = RunFile(host, framework.runtime, "builtIn/console/go.lua");
    if (!status.Succeeded()) return status;
    for (const auto name : constants) {
        status = RunFile(host, framework.runtime, "const/" + std::string(name) + ".lua");
        if (!status.Succeeded()) return status;
    }
    for (const auto name : foundations) {
        status = RunFile(host, framework.runtime, "foundation/" + std::string(name) + ".lua");
        if (!status.Succeeded()) return status;
    }
    return RunFile(host, framework.runtime, "slk/pilot.lua");
}
Status ProjectFiles(Detail::HslkSandbox& host, const ProjectSnapshot& project) {
    for (const auto index : project.hslkFiles) {
        if (index >= project.source.files.size()) return Status::Failure("HLua", "HLUA_PROJECT_INVALID", "hslk 文件索引失效。");
        const auto& file = project.source.files[index];
        auto status = host.Run(LuaText(file), file.path);
        if (!status.Succeeded()) return status;
    }
    return Status::Ok();
}
std::string IdTable(std::span<const std::string> ids) {
    std::string output = "{";
    for (const auto& id : ids) { if (output.size() > 1) output += ','; output += nlohmann::json(id).dump(); }
    return output + "}";
}
Result<std::string> Inject(const FrameworkSnapshot& framework, std::span<const std::string> existing,
    std::span<const std::string> generated) {
    const auto* file = FindLuaFile(framework.runtime, "slk/slk.lua");
    if (!file) return Result<std::string>::Failure("HLua", "HLUA_FRAMEWORK_FILE_MISSING", "没有运行期 slk/slk.lua。");
    const auto original = LuaText(*file);
    auto first = Lua::FindEmptyTableInitializers(original, "hslk_cli_ids");
    auto second = Lua::FindEmptyTableInitializers(original, "HSLK_CLI_H_IDS");
    if (first.size() != 1 || second.size() != 1)
        return Result<std::string>::Failure("HLua", "HLUA_ID_INJECTION_SITE", "运行期两份 ID 空表初始化必须各有一个词法位置。");
    std::vector<std::pair<Lua::EmptyTableInitializer, std::string>> edits{
        {first.front(), IdTable(existing)}, {second.front(), IdTable(generated)}};
    std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.first.begin > b.first.begin; });
    std::string output(original);
    for (const auto& [span, text] : edits) output.replace(span.begin, span.end - span.begin, text);
    return Result<std::string>::Success(std::move(output));
}
}

Result<HslkOutput> EvaluateHslk(const FrameworkSnapshot& framework, const ProjectSnapshot& project,
    std::span<const std::string> existingIds, const HslkLimits& limits) {
    try {
        Detail::HslkSandbox host(limits, existingIds);
        auto status = host.Initialize();
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        status = Prelude(host, framework);
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        for (const auto name : {"json.lua", "string.lua", "table.lua", "go.lua"}) {
            status = RunFile(host, framework.build, name);
            if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        }
        status = host.InstallAllocator();
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        for (const auto name : {"setter.lua", "slk.lua"}) {
            status = RunFile(host, framework.build, name);
            if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        }
        status = host.CallText("SLK_GO_INI", nlohmann::json(existingIds).dump());
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        status = RunFile(host, framework.build, "system.lua");
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        status = ProjectFiles(host, project);
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        HslkOutput output;
        status = host.ReadObjects(output.objects);
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        output.generatedIds.assign(host.ids.Allocated().begin(), host.ids.Allocated().end());
        if (output.objects.size() != output.generatedIds.size())
            return Result<HslkOutput>::Failure("HLua", "HLUA_BUILD_ID_MISMATCH", "登记物体数与 ID 分配次数不一致。");
        for (std::size_t i = 0; i < output.objects.size(); ++i)
            if (output.objects[i].id != output.generatedIds[i])
                return Result<HslkOutput>::Failure("HLua", "HLUA_BUILD_ID_MISMATCH", "登记物体 ID 顺序与分配顺序不一致。");
        auto injected = Inject(framework, existingIds, output.generatedIds);
        if (!injected.Succeeded()) return {std::nullopt, std::move(injected.diagnostics)};
        output.injectedSlk = std::move(*injected.value);
        output.console = host.console; output.peakMemoryBytes = host.peak; output.instructions = host.instructions;

        // 第二个独立状态加载游戏内版本；不把构建期函数或缓存带进回放。
        Detail::HslkSandbox replay(limits, {});
        status = replay.Initialize();
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        status = Prelude(replay, framework);
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        status = RunFile(replay, framework.runtime, "slk/setter.lua");
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        status = replay.Run(output.injectedSlk, "h-lua-slk/slk.lua");
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        status = RunFile(replay, framework.runtime, "slk/system.lua");
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        status = ProjectFiles(replay, project);
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        status = replay.Run("assert(HSLK_CLI_H_IDI == " + std::to_string(output.generatedIds.size() + 1)
            + ", 'HLUA_REPLAY_ID_MISMATCH: 游戏内 ID 领取数与构建期不同')", "=id-replay-count");
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        output.replayPassed = true;
        output.peakMemoryBytes = std::max(output.peakMemoryBytes, replay.peak);
        output.instructions += replay.instructions;
        return Result<HslkOutput>::Success(std::move(output));
    } catch (const std::exception& e) {
        return Result<HslkOutput>::Failure("HLua", "HLUA_BUILD_HOST", e.what());
    }
}
} // namespace War3Map::HLua
