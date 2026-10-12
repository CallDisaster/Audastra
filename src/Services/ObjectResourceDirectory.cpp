#include "War3MapCore/Services/ObjectResourceDirectory.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/VerifiedFile.h"

#include <map>
#include <set>

namespace War3Map::Services {
Result<ObjectResourceInfo> InspectDirectoryObjectResource(const DirectoryWorkspace& directory,
    const std::filesystem::path& gameRoot, const ImportInventoryBinding& binding,
    const ImportInventoryOverlay& overlay, std::string_view value, ObjectResourceKind kind) {
    using Return = Result<ObjectResourceInfo>;
    constexpr std::size_t maximum = 32 * 1024 * 1024;
    const auto inventory = CaptureImportInventory(directory, binding, overlay);
    if (!inventory.Succeeded()) return {{}, inventory.diagnostics};
    std::map<std::string, std::string> edits;
    std::set<std::string> pending;
    for (const auto& edit : overlay.pathEdits) {
        auto from = Resources::CanonicalImportPath(edit.from);
        if (!from.Succeeded()) return {{}, from.diagnostics};
        std::string to;
        if (!edit.to.empty()) {
            auto canonical = Resources::CanonicalImportPath(edit.to);
            if (!canonical.Succeeded()) return {{}, canonical.diagnostics};
            to = *canonical.value;
        }
        if (!edits.emplace(*from.value, std::move(to)).second)
            return Return::Failure("Object.Resource", "OBJECT_RESOURCE_OVERLAY_CONFLICT", "来源具有重复路径覆盖。");
    }
    for (const auto& item : overlay.pending) {
        auto path = Resources::CanonicalImportPath(item.archivePath);
        if (!path.Succeeded()) return {{}, path.diagnostics};
        pending.insert(*path.value);
    }
    std::map<std::string, std::vector<const WorkspaceFile*>> paths, originals;
    for (const auto& file : directory.Snapshot().files) {
        auto path = Resources::CanonicalImportPath(IO::GenericPathUtf8(file.relativePath));
        if (!path.Succeeded()) return {{}, path.diagnostics};
        originals[*path.value].push_back(&file);
        const auto edit = edits.find(*path.value);
        const auto& effective = edit == edits.end() ? *path.value : edit->second;
        if (!effective.empty()) paths[effective].push_back(&file);
    }
    Resources::ResourceResolver resolver;
    Resources::ResourceSource map;
    map.id = "map-directory"; map.layer = Resources::ResourceLayer::MapImport;
    map.priority = Resources::ResourcePriority::MapDirectory;
    map.probe = [&](std::string_view path) -> Result<Resources::ResourceProbe> {
        if (pending.contains(std::string(path)))
            return Result<Resources::ResourceProbe>::Failure("Object.Resource", "OBJECT_RESOURCE_PENDING_UNFROZEN", "新导入资源尚未冻结，不能读取或回退低层。");
        const auto found = paths.find(std::string(path));
        if (found == paths.end()) return Result<Resources::ResourceProbe>::Success({});
        if (found->second.size() != 1) return Result<Resources::ResourceProbe>::Success({Resources::ResourceProbeState::Conflict, std::string(path)});
        return Result<Resources::ResourceProbe>::Success({Resources::ResourceProbeState::Found, IO::GenericPathUtf8(found->second.front()->relativePath)});
    };
    map.read = [&](std::string_view path) -> Result<std::vector<std::byte>> {
        const auto canonical = Resources::CanonicalImportPath(path);
        if (!canonical.Succeeded()) return {{}, canonical.diagnostics};
        const auto found = originals.find(*canonical.value);
        if (found == originals.end() || found->second.size() != 1)
            return Result<std::vector<std::byte>>::Failure("Object.Resource", "OBJECT_RESOURCE_SOURCE_CHANGED", "目录资源身份不再唯一。");
        const auto& file = *found->second.front();
        return ReadVerifiedFile(directory.Snapshot().root / file.relativePath, file.sha256, maximum);
    };
    auto added = resolver.AddSource(std::move(map)); if (!added.Succeeded()) return {{}, added.diagnostics};
    std::optional<Game::GameDataSource> game;
    std::map<std::string, std::string> gameSources;
    if (!gameRoot.empty()) {
        auto mounted = Game::GameDataSource::MountStrict(gameRoot);
        if (!mounted.Succeeded()) return {{}, mounted.diagnostics};
        game = std::move(*mounted.value);
        Resources::ResourceSource source;
        source.id = "game"; source.layer = Resources::ResourceLayer::Game; source.priority = Resources::ResourcePriority::GameBase;
        source.probe = [&](std::string_view path) { return Result<Resources::ResourceProbe>::Success(
            {game->HasFile(std::string(path)) ? Resources::ResourceProbeState::Found : Resources::ResourceProbeState::Missing, std::string(path)}); };
        source.read = [&](std::string_view path) -> Result<std::vector<std::byte>> {
            auto read = game->ReadFileBoundedWithSource(std::string(path), maximum);
            if (!read.Succeeded()) return {{}, read.diagnostics};
            gameSources[std::string(path)] = read.value->archiveName;
            return Result<std::vector<std::byte>>::Success(std::move(read.value->bytes));
        };
        added = resolver.AddSource(std::move(source)); if (!added.Succeeded()) return {{}, added.diagnostics};
    }
    auto result = InspectObjectResource(resolver, value, kind, maximum);
    if (result.Succeeded() && result.value->source.sourceId == "game") {
        const auto found = gameSources.find(result.value->source.sourcePath);
        if (found == gameSources.end()) return Return::Failure("Object.Resource", "OBJECT_RESOURCE_SOURCE_UNKNOWN", "资源没有实际读取来源。");
        result.value->source.sourceId = found->second;
    }
    const auto current = directory.VerifyUnchanged();
    return current.Succeeded() ? result : Return{{}, current.diagnostics};
}
}
