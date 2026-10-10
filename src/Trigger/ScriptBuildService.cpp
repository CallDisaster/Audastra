#include "War3MapCore/Trigger/ScriptBuildService.h"
#include "War3MapCore/MapDocument.h"

#include <algorithm>

namespace War3Map::Trigger {

bool ScriptBuildCheck::CanBuildGameMap() const noexcept {
    return triggerFragmentAvailable && triggerFragment.Succeeded() &&
        !triggerFragment.jassCode.empty() && mapSourceDataAvailable && mapInitializationAvailable &&
        externalCompilerValidated && std::none_of(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& item) { return item.severity == DiagnosticSeverity::Error; });
}

ScriptBuildCheck ScriptBuildService::CheckGameBuild(
    const TriggerDocument& document, const UiCatalog& catalog, ScriptBackend backend,
    const MapDocument* mapDocument) {
    ScriptBuildCheck result;
    result.backend = backend;
    if (backend == ScriptBackend::Jass || backend == ScriptBackend::VJass) {
        result.triggerFragment = CompileTriggersToJass(document, catalog);
        result.triggerFragmentAvailable = result.triggerFragment.Succeeded() &&
            !result.triggerFragment.jassCode.empty();
        result.diagnostics = result.triggerFragment.diagnostics;
    } else if (backend == ScriptBackend::Lua) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build",
            "script.lua-backend-unavailable", "Lua 代码生成后端尚未实现。"});
    } else {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build",
            "script.backend-invalid", "无法识别所选脚本后端。"});
    }
    if (mapDocument) {
        const auto sourceStatus = mapDocument->InitializationSourceStatus();
        result.mapSourceDataAvailable = sourceStatus.Succeeded();
        result.diagnostics.insert(result.diagnostics.end(), sourceStatus.diagnostics.begin(), sourceStatus.diagnostics.end());
    } else {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build",
            "script.map-document-missing", "尚未载入地图基础数据文档。"});
    }
    result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build",
        "script.map-initialization-unavailable",
        "尚未生成完整地图 config/main 及预放置对象、区域、镜头、声音初始化；触发器片段不能替代整图脚本。"});
    result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build",
        backend == ScriptBackend::VJass ? "script.vjass-toolchain-unavailable" :
            "script.external-compiler-unavailable",
        backend == ScriptBackend::VJass ? "尚未接入并验证 vJass 外部编译工具链。" :
            "尚未使用目标游戏的脚本库完成外部编译验证。"});
    return result;
}

} // namespace War3Map::Trigger
