#include "War3MapCore/Services/LuaFolderSave.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/MapTestService.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <map>

namespace War3Map::Services {
namespace {
std::string Key(std::string name) {
    for (auto& c : name) { if (c == '/') c = '\\'; else if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; }
    return name;
}
void Replace(MapSaveTask& task, std::string name, std::vector<std::byte> bytes) {
    const auto key = Key(name);
    std::erase_if(task.replacements, [&](const auto& item) { return Key(item.name) == key; });
    std::erase_if(task.removals, [&](const auto& item) { return Key(item) == key; });
    task.replacements.push_back({std::move(name), std::move(bytes)});
}
}
Result<std::string> LuaFolderDeletionIdentity(std::span<const LuaFolderFile> map, const LuaFolderSnapshot& folder) {
    const auto diff = CompareLuaFolder(map, folder);
    if (folder.missing || diff.mapOnly.empty()) return Result<std::string>::Success({});
    auto identity = folder.root.generic_u8string();
    identity += u8"\nlua-folder-deletions-v1\n";
    for (const auto& file : map) if (std::find(diff.mapOnly.begin(), diff.mapOnly.end(), file.path) != diff.mapOnly.end()) {
        const auto value = Key(file.path) + ':' + file.sha256 + '\n';
        identity.append(reinterpret_cast<const char8_t*>(value.data()), value.size());
    }
    return Sha256(std::as_bytes(std::span(identity)));
}

Status PrepareLuaFolderSave(MapSaveTask& task) {
    const auto link = ReadLuaFolderLink(*task.archive, task.replacements, task.removals);
    if (!link.Succeeded()) return {link.diagnostics};
    if (!*link.value && task.luaFolderOverride.empty()) return Status::Ok();
    const auto root = task.luaFolderOverride.empty() ? **link.value : task.luaFolderOverride;
    const auto captured = CaptureLuaFolder(root, true, true);
    if (!captured.Succeeded()) return {captured.diagnostics};
    task.luaFolderSnapshot = std::make_shared<LuaFolderSnapshot>(*captured.value);
    if (task.luaFolderSnapshot->missing) {
        task.diagnostics.push_back({DiagnosticSeverity::Warning, "LuaFolder", "LUA_FOLDER_MISSING",
            "外部文件夹不存在，使用地图内Lua；关联信息保留。"});
    } else {
        const auto map = CaptureMapLuaFiles(*task.archive, task.replacements, task.removals);
        if (!map.Succeeded()) return {map.diagnostics};
        const auto diff = CompareLuaFolder(*map.value, *task.luaFolderSnapshot);
        const auto approval = LuaFolderDeletionIdentity(*map.value, *task.luaFolderSnapshot);
        if (!approval.Succeeded()) return {approval.diagnostics};
        task.luaFolderDeletedFiles = diff.mapOnly;
        if (!diff.mapOnly.empty() && *approval.value != task.luaFolderDeletionApproval) {
            std::string message = "保存会删除地图中的Lua文件，须确认删除集合SHA=" + *approval.value + "：";
            for (const auto& path : diff.mapOnly) message += "\n" + path;
            return Status::Failure("LuaFolder", "LUA_FOLDER_DELETE_CONFIRM_REQUIRED", std::move(message));
        }
        Resources::ImportTable imports = Resources::ImportTable::CreateEmpty();
        const auto pending = std::find_if(task.replacements.begin(), task.replacements.end(), [](const auto& item) { return Key(item.name) == "war3map.imp"; });
        if (pending != task.replacements.end() || task.archive->HasMember("war3map.imp")) {
            auto bytes = pending != task.replacements.end() ? Result<std::vector<std::byte>>::Success(pending->data)
                : task.archive->ReadMember("war3map.imp");
            if (!bytes.Succeeded()) return {bytes.diagnostics};
            const auto parsed = Resources::ImportTable::Parse(*bytes.value);
            if (!parsed.Succeeded()) return {parsed.diagnostics};
            imports = *parsed.value;
        }
        const auto exists = [&](std::string_view name) {
            return task.archive->HasMember(name) || std::any_of(task.replacements.begin(), task.replacements.end(),
                [&](const auto& item) { return Key(item.name) == Key(std::string(name)); });
        };
        for (const auto& path : diff.mapOnly) {
            const auto removed = imports.Remove(path, exists);
            if (!removed.Succeeded()) return removed;
            const auto key = Key(path);
            std::erase_if(task.replacements, [&](const auto& item) { return Key(item.name) == key; });
            if (std::none_of(task.removals.begin(), task.removals.end(), [&](const auto& item) { return Key(item) == key; })) task.removals.push_back(path);
        }
        for (const auto& file : task.luaFolderSnapshot->files) {
            const auto added = imports.Upsert(file.path, exists);
            if (!added.Succeeded()) return added;
            Replace(task, file.path, *file.bytes);
        }
        const auto wire = imports.Serialize();
        if (!wire.Succeeded()) return {wire.diagnostics};
        Replace(task, "war3map.imp", *wire.value);
    }
    // --lua-folder仅覆盖本次内容来源，不暗中改写既有关联；另存仍重定位原关联。
    if (*link.value && !task.target.empty()) {
        const auto metadata = EncodeLuaFolderLink(**link.value, task.target);
        if (!metadata.Succeeded()) return {metadata.diagnostics};
        Replace(task, std::string(kLuaFolderMember), *metadata.value);
    }
    const auto previous = std::move(task.beforePublish);
    const auto snapshot = task.luaFolderSnapshot;
    task.beforePublish = [previous, snapshot] {
        if (previous) { const auto status = previous(); if (!status.Succeeded()) return status; }
        return VerifyLuaFolder(*snapshot);
    };
    return Status::Ok();
}

Status VerifyLuaFolderSaved(const MapSaveTask& task, const MapArchiveSession& saved) {
    if (!task.luaFolderSnapshot || task.luaFolderSnapshot->missing) return Status::Ok();
    for (const auto& file : task.luaFolderSnapshot->files) {
        const auto read = saved.ReadMember(file.path);
        if (!read.Succeeded()) return {read.diagnostics};
        const auto sha = Sha256(*read.value);
        if (!sha.Succeeded() || *sha.value != file.sha256 || *read.value != *file.bytes)
            return Status::Failure("LuaFolder", "LUA_FOLDER_READBACK", file.path + "：地图Lua回读与冻结快照不同。");
    }
    for (const auto& path : task.luaFolderDeletedFiles) if (saved.HasMember(path))
        return Status::Failure("LuaFolder", "LUA_FOLDER_READBACK", path + "：已确认删除的Lua仍在地图中。");
    return Status::Ok();
}

Status PrepareLuaFolderTestInput(MapTestRequest& request, const std::filesystem::path& overrideFolder,
    std::string deletionApproval) {
    const auto link = ReadLuaFolderLinkFile(request.inputMap);
    if (!link.Succeeded()) return {link.diagnostics};
    if (!*link.value && overrideFolder.empty()) return Status::Ok();
    const auto source = MapArchiveSession::Open(request.inputMap);
    if (!source.Succeeded()) return {source.diagnostics};
    if (source.value->SourceSha256() != request.expectedInputSha256)
        return Status::Failure("LuaFolder", "LUA_FOLDER_MAP_CHANGED", "测试输入地图SHA已改变，未启动。");
    MapSaveTask task;
    const auto frozen = task.FreezeArchive(*source.value);
    if (!frozen.Succeeded()) return frozen;
    task.target = request.workspace.parent_path() / "lua-folder-input.w3x";
    task.testCopyOnly = true;
    task.runtimePurpose = MapRuntimeSavePurpose::TestRun;
    task.luaFolderOverride = overrideFolder;
    task.luaFolderDeletionApproval = std::move(deletionApproval);
    MapSaveService::Run(task);
    if (!task.succeeded) return {task.diagnostics};
    request.inputMap = task.written;
    request.expectedInputSha256 = task.nextSession->SourceSha256();
    return {task.diagnostics};
}
} // namespace War3Map::Services
