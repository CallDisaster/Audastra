#include "EditorProject.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"

namespace War3Map::QtProject {

Status EditorProject::ConfigureLuaMapTarget(const std::filesystem::path& configuration) {
    if (savingMap_) return Status::Failure("LuaMap.Target", "LUA_MAP_SAVE_ACTIVE", "保存过程中不能更换脚本目标。");
    if (!archiveSession_ || !triggerDocument_ || !uiCatalog_)
        return Status::Failure("LuaMap.Target", "LUA_MAP_DOCUMENT_REQUIRED", "请先打开具有可编辑触发器的地图。");
    auto request = Services::LoadLuaMapTargetConfiguration(configuration);
    if (!request.Succeeded() || !request.value) return {request.diagnostics};
    const auto settings = archiveSession_->ReadMember("war3map.w3i");
    if (!settings.Succeeded() || !settings.value) return {settings.diagnostics};
    const auto verified = Services::PrepareMapScriptTargetSelection(*settings.value, *settings.value,
        *triggerDocument_, *uiCatalog_, *request.value, editRevision_.Revision());
    if (!verified.Succeeded()) return {verified.diagnostics};
    // 目标选择不会当场改W3I。只有完整生成、检查、归档回读成功才提交语言。
    luaMapTarget_ = std::move(*request.value);
    luaMapTargetSource_ = sourceArchivePath_;
    ++luaMapTargetGeneration_;
    return {verified.diagnostics};
}

Status EditorProject::ClearLuaMapTarget() {
    if (savingMap_) return Status::Failure("LuaMap.Target", "LUA_MAP_SAVE_ACTIVE", "保存过程中不能清除脚本目标。");
    luaMapTarget_.reset();
    luaMapTargetSource_.clear();
    ++luaMapTargetGeneration_;
    return Status::Ok();
}

} // namespace War3Map::QtProject
