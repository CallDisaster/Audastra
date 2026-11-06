#pragma once
#include "War3MapCore/Services/LuaFolder.h"

namespace War3Map::Services {
struct MapSaveTask;
struct MapTestRequest;
[[nodiscard]] Status PrepareLuaFolderTestInput(MapTestRequest& request,
    const std::filesystem::path& overrideFolder = {}, std::string deletionApproval = {});
// 只绑定作者Lua的删除集合及身份，不把无关正文修改当作删除批准。
[[nodiscard]] Result<std::string> LuaFolderDeletionIdentity(std::span<const LuaFolderFile> map,
    const LuaFolderSnapshot& folder);
[[nodiscard]] Status PrepareLuaFolderSave(MapSaveTask& task);
[[nodiscard]] Status VerifyLuaFolderSaved(const MapSaveTask& task, const MapArchiveSession& saved);
} // namespace War3Map::Services
