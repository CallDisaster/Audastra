#include "War3MapCore/Services/ObjectCatalogSources.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Services/CampaignService.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Workspace.h"

#include <Windows.h>
#include <algorithm>
#include <map>
#include <set>

namespace War3Map::Services {
namespace {
using namespace ObjectData;
using namespace Resources;
using FileResult = Result<std::optional<EffectiveCatalogFile>>;
constexpr std::size_t kTotalBytes = 64U * 1024U * 1024U;

Status Fail(std::string code, std::string message) {
    return Status::Failure("Object.Catalog", std::move(code), std::move(message));
}

std::string PathText(const std::filesystem::path& path) { return IO::GenericPathUtf8(path); }

// 固定路径逐组件查证：权限失败不能作为 Missing，大小写同名也不随枚举顺序挑选。
Result<std::optional<std::filesystem::path>> FindDirectoryInput(
    const std::filesystem::path& root, std::string_view canonical) {
    using Output = Result<std::optional<std::filesystem::path>>;
    auto current = root;
    std::size_t start = 0, examined = 0;
    while (start < canonical.size()) {
        const auto separator = canonical.find('\\', start);
        const auto part = canonical.substr(start, separator == std::string_view::npos ? canonical.size() - start : separator - start);
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DIRECTORY_UNVERIFIED", "物编来源目录无法唯一确认。");
        std::error_code error;
        std::filesystem::directory_iterator it(current, error), end;
        if (error) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DIRECTORY_UNREADABLE", "无法查证物编来源目录。");
        std::optional<std::filesystem::path> matched;
        while (it != end) {
            if (++examined > 65536) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DIRECTORY_BUDGET", "物编固定路径查询超过目录项预算。");
            const auto name = CanonicalImportPath(PathText(it->path().filename()));
            if (name.Succeeded() && *name.value == part) {
                if (matched) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DIRECTORY_CONFLICT", "物编来源存在大小写同名冲突。");
                matched = it->path();
            }
            it.increment(error);
            if (error) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DIRECTORY_UNREADABLE", "目录查询未完整完成，不能判断物编表缺失。");
        }
        if (!matched) return Output::Success(std::nullopt);
        current = *matched;
        const DWORD selected = GetFileAttributesW(current.c_str());
        if (selected == INVALID_FILE_ATTRIBUTES || (selected & FILE_ATTRIBUTE_REPARSE_POINT))
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DIRECTORY_UNVERIFIED", "物编来源路径发生变化或含重解析点。");
        if (separator == std::string_view::npos) {
            if (selected & FILE_ATTRIBUTE_DIRECTORY) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_NOT_FILE", "物编表路径指向目录。");
            return Output::Success(current);
        }
        start = separator + 1;
    }
    return Output::Failure("Object.Catalog", "OBJECT_CATALOG_PATH_INVALID", "物编表路径为空。");
}

Result<std::vector<ArchiveExactMember>> DirectoryInputs(const DirectoryWorkspace& directory,
    std::span<const std::string> names, std::size_t maximum, Render::AnimationSourceGuard& guard,
    std::map<std::string, std::string, std::less<>>& actualPaths) {
    using Output = Result<std::vector<ArchiveExactMember>>;
    std::map<std::string, const WorkspaceFile*, std::less<>> known;
    std::set<std::string> relevant;
    for (const auto& name : names) { relevant.insert(name); relevant.insert("MAP\\" + name); }
    for (const auto& file : directory.Snapshot().files) {
        const auto key = CanonicalImportPath(PathText(file.relativePath));
        if (!key.Succeeded() || !relevant.contains(*key.value)) continue;
        if (!known.emplace(*key.value, &file).second)
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DIRECTORY_CONFLICT", "目录快照中存在大小写同名路径。");
    }
    std::vector<ArchiveExactMember> output;
    std::size_t total = 0;
    for (const auto& name : names) {
        std::optional<std::filesystem::path> selected;
        const WorkspaceFile* expected = nullptr;
        for (const auto& physicalName : {name, "MAP\\" + name}) {
            auto found = FindDirectoryInput(directory.Snapshot().root, physicalName);
            if (!found.Succeeded()) return {{}, std::move(found.diagnostics)};
            const auto prior = known.find(physicalName);
            if (found.value->has_value() != (prior != known.end()))
                return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_CHANGED", "物编表与目录快照的存在性不一致：" + physicalName);
            const auto watchPath = *found.value ? **found.value : directory.Snapshot().root / std::filesystem::path(physicalName);
            if (const auto watched = guard.Watch(watchPath, !*found.value); !watched.Succeeded()) return {{}, watched.diagnostics};
            if (*found.value) {
                if (selected) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DIRECTORY_CONFLICT", "物编表同时存在 root 与 map 来源：" + name);
                selected = **found.value; expected = prior->second;
                actualPaths[name] = PathText(expected->relativePath);
            }
        }
        ArchiveExactMember member{name, false, {}};
        if (selected) {
            auto read = ReadVerifiedFile(*selected, expected->sha256, maximum);
            if (!read.Succeeded()) return {{}, std::move(read.diagnostics)};
            if (read.value->size() > kTotalBytes - total)
                return Output::Failure("Object.Catalog", "OBJECT_CATALOG_TOTAL_BUDGET", "物编表目录来源超过 64 MiB 累积预算。");
            total += read.value->size();
            member.present = true; member.bytes = std::move(*read.value);
        }
        output.push_back(std::move(member));
    }
    return Output::Success(std::move(output));
}
} // namespace

struct ObjectCatalogSourceSnapshot::State {
    std::map<std::string, std::optional<EffectiveCatalogFile>, std::less<>> files;
    // 复用渲染资源的只读文件租约，不引入第二套文件身份比较。此处不创建图形设备。
    Render::AnimationSourceGuard guard;
    ObjectCatalogSourceVerifier overlayVerifier;
};

FileResult ObjectCatalogSourceSnapshot::Read(std::string_view name) const {
    const auto key = CanonicalImportPath(name);
    if (!key.Succeeded()) return {{}, key.diagnostics};
    if (!state_) return FileResult::Failure("Object.Catalog", "OBJECT_CATALOG_SNAPSHOT_EMPTY", "物编来源尚未捕获。");
    const auto found = state_->files.find(*key.value);
    if (found == state_->files.end()) return FileResult::Failure("Object.Catalog", "OBJECT_CATALOG_UNCAPTURED", "请求的物编表不在本次固定捕获范围。");
    return FileResult::Success(found->second);
}

Status ObjectCatalogSourceSnapshot::Verify() const {
    if (!state_) return Fail("OBJECT_CATALOG_SNAPSHOT_EMPTY", "物编来源尚未捕获。");
    if (const auto status = state_->guard.Verify(); !status.Succeeded()) return status;
    try { return state_->overlayVerifier ? state_->overlayVerifier() : Status::Ok(); }
    catch (...) { return Fail("OBJECT_CATALOG_SOURCE_CHANGED", "物编会话来源复核未能完成。"); }
}

Result<ObjectCatalogSourceSnapshot> CaptureObjectCatalogSources(const ObjectCatalogSources& request,
    std::span<const std::string> requested, std::size_t maximum) {
    using Output = Result<ObjectCatalogSourceSnapshot>;
    if (int(request.archive != nullptr) + int(request.readOnlyArchive != nullptr) + int(request.directory != nullptr) > 1)
        return Output::Failure("Object.Catalog", "OBJECT_CATALOG_MAP_SOURCE_CONFLICT", "当前地图有多个候选来源，不能猜测生效顺序。");
    if (requested.size() > 256 || maximum > 32U * 1024U * 1024U)
        return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_BUDGET", "物编表请求超过固定读取预算。");
    std::set<std::string> domain;
    std::size_t pathBytes = 0;
    for (const auto& name : requested) {
        if (name.size() > 1024 || name.size() > 8192 - pathBytes)
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_BUDGET", "物编表固定名字超过路径预算。");
        pathBytes += name.size();
        const auto key = CanonicalImportPath(name);
        if (!key.Succeeded()) return {{}, key.diagnostics};
        if (!domain.insert(*key.value).second)
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DUPLICATE_INPUT", "物编固定表请求存在重复路径。");
    }
    std::set<std::string> gameOnly;
    for (const auto& name : request.gameOnlyNames) {
        const auto key = CanonicalImportPath(name);
        if (!key.Succeeded()) return {{}, key.diagnostics};
        if (!domain.contains(*key.value)) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_UNCAPTURED",
            "受信任来源名字不在固定捕获范围内。");
        gameOnly.insert(*key.value);
    }
    ObjectCatalogSourceSnapshot snapshot;
    snapshot.state_ = std::make_shared<ObjectCatalogSourceSnapshot::State>();
    auto& state = *snapshot.state_;
    state.overlayVerifier = request.overlayVerifier;
    if (const auto status = snapshot.Verify(); !status.Succeeded()) return {{}, status.diagnostics};
    std::map<std::string, const MemberReplacement*, std::less<>> overrides;
    std::set<std::string> removed;
    for (const auto& input : request.replacements) {
        const auto key = CanonicalImportPath(input.name);
        if (!key.Succeeded()) return {{}, key.diagnostics};
        if (!domain.contains(*key.value) || gameOnly.contains(*key.value)) continue;
        if (!overrides.emplace(*key.value, &input).second)
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_OVERLAY_CONFLICT", "物编会话表存在重复覆盖。");
        if (input.data.size() > maximum) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_BUDGET", "物编会话表超过单文件预算。");
    }
    for (const auto& name : request.removals) {
        const auto key = CanonicalImportPath(name);
        if (!key.Succeeded()) return {{}, key.diagnostics};
        if (!domain.contains(*key.value) || gameOnly.contains(*key.value)) continue;
        if (overrides.contains(*key.value) || !removed.insert(*key.value).second)
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_OVERLAY_CONFLICT", "物编会话表同时删除和覆盖，或重复删除。");
    }
    std::vector<std::string> mapNames;
    for (const auto& name : domain) if (!gameOnly.contains(name)
        && !overrides.contains(name) && !removed.contains(name)) mapNames.push_back(name);
    std::vector<ArchiveExactMember> mapFiles;
    std::map<std::string, std::string, std::less<>> mapPaths;
    std::string mapIdentity;
    ArchiveExactMemberLimits limits; limits.memberBytes = maximum; limits.totalBytes = kTotalBytes;
    if (request.archive) {
        if (const auto status = state.guard.Watch(request.archive->SourcePath()); !status.Succeeded()) return {{}, status.diagnostics};
        auto captured = request.archive->CaptureExactMembers(mapNames, limits);
        if (!captured.Succeeded()) return {{}, std::move(captured.diagnostics)};
        mapFiles = std::move(*captured.value);
        mapIdentity = PathText(request.archive->SourcePath()) + '|' + request.archive->SourceSha256();
    } else if (request.readOnlyArchive) {
        if (const auto status = state.guard.Watch(request.readOnlyArchive->SourcePath()); !status.Succeeded()) return {{}, status.diagnostics};
        if (const auto status = request.readOnlyArchive->VerifyUnchanged(); !status.Succeeded()) return {{}, status.diagnostics};
        std::size_t retained = 0;
        for (const auto& name : mapNames) {
            ArchiveExactMember input{name, request.readOnlyArchive->Archive().HasFile(name), {}};
            if (input.present) {
                auto read = request.readOnlyArchive->ReadMember(name, maximum);
                if (!read.Succeeded()) return {{}, std::move(read.diagnostics)};
                if (read.value->size() > kTotalBytes - retained) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_TOTAL_BUDGET", "只读地图表超过累积预算。");
                retained += read.value->size(); input.bytes = std::move(*read.value);
            }
            mapFiles.push_back(std::move(input));
        }
        mapIdentity = PathText(request.readOnlyArchive->SourcePath()) + '|' + request.readOnlyArchive->SourceSha256();
    } else if (request.directory) {
        auto captured = DirectoryInputs(*request.directory, mapNames, maximum, state.guard, mapPaths);
        if (!captured.Succeeded()) return {{}, std::move(captured.diagnostics)};
        mapFiles = std::move(*captured.value);
        mapIdentity = PathText(request.directory->Snapshot().root) + '|' + request.directory->Snapshot().aggregateSha256;
    }

    ResourceResolver resolver;
    ResourceSource session;
    session.id = "object-session"; session.layer = ResourceLayer::SessionOverride; session.priority = ResourcePriority::SessionOverride;
    session.probe = [&](std::string_view name) { return Result<ResourceProbe>::Success(overrides.contains(name)
        ? ResourceProbe{ResourceProbeState::Found, std::string(name)} : ResourceProbe{}); };
    session.read = [&](std::string_view name) { return Result<std::vector<std::byte>>::Success(overrides.at(std::string(name))->data); };
    if (const auto added = resolver.AddSource(std::move(session)); !added.Succeeded()) return {{}, added.diagnostics};
    std::map<std::string, const ArchiveExactMember*, std::less<>> map, physicalMap;
    for (const auto& input : mapFiles) {
        map.emplace(input.name, &input);
        const auto [path, inserted] = mapPaths.try_emplace(input.name, input.name);
        (void)inserted;
        if (input.present) physicalMap.emplace(path->second, &input);
    }
    ResourceSource mapSource;
    mapSource.id = "object-map"; mapSource.layer = ResourceLayer::MapImport;
    mapSource.priority = request.directory ? ResourcePriority::MapDirectory : ResourcePriority::MapArchive;
    mapSource.probe = [&](std::string_view name) { const auto found = map.find(name); return Result<ResourceProbe>::Success(
        found != map.end() && found->second->present ? ResourceProbe{ResourceProbeState::Found, mapPaths.at(std::string(name))} : ResourceProbe{}); };
    mapSource.read = [&](std::string_view name) { return Result<std::vector<std::byte>>::Success(physicalMap.at(std::string(name))->bytes); };
    if (const auto added = resolver.AddSource(std::move(mapSource)); !added.Succeeded()) return {{}, added.diagnostics};

    std::vector<ArchiveExactMember> campaignFiles;
    std::map<std::string, const ArchiveExactMember*, std::less<>> campaign;
    std::string campaignIdentity;
    if (request.campaign) {
        if (!request.campaign->SourcePath().empty()) {
            const auto status = state.guard.Watch(request.campaign->SourcePath());
            if (!status.Succeeded()) return {{}, status.diagnostics};
        }
        const auto revision = request.campaign->Revision();
        // 只捕获当前地图/会话未命中的表，避免无关坏低层影响有效高层。
        std::vector<std::string> names;
        for (const auto& name : domain) if (!gameOnly.contains(name) && !overrides.contains(name)
            && (!map.contains(name) || !map.at(name)->present)) names.push_back(name);
        auto captured = request.campaign->CaptureExactMembers(revision, names, limits);
        if (!captured.Succeeded()) return {{}, std::move(captured.diagnostics)};
        campaignFiles = std::move(*captured.value);
        for (const auto& input : campaignFiles) campaign.emplace(input.name, &input);
        campaignIdentity = PathText(request.campaign->SourcePath()) + '|' + std::to_string(revision.document) + ':' + std::to_string(revision.revision);
        ResourceSource source;
        source.id = "object-campaign"; source.layer = ResourceLayer::Campaign; source.priority = ResourcePriority::Campaign;
        source.probe = [&](std::string_view name) { const auto found = campaign.find(name); return Result<ResourceProbe>::Success(
            found != campaign.end() && found->second->present ? ResourceProbe{ResourceProbeState::Found, std::string(name)} : ResourceProbe{}); };
        source.read = [&](std::string_view name) { return Result<std::vector<std::byte>>::Success(campaign.at(std::string(name))->bytes); };
        if (const auto added = resolver.AddSource(std::move(source)); !added.Succeeded()) return {{}, added.diagnostics};
    }
    std::optional<Game::GameDataSource> game;
    std::map<std::string, std::string, std::less<>> gameArchives;
    if (!request.gameRoot.empty()) {
        ResourceSource source;
        source.id = "object-game"; source.layer = ResourceLayer::Game; source.priority = ResourcePriority::GameBase;
        source.probe = [&](std::string_view name) -> Result<ResourceProbe> {
            if (!game) {
                if (const auto status = state.guard.WatchGameRoot(request.gameRoot); !status.Succeeded()) return {{}, status.diagnostics};
                auto mounted = Game::GameDataSource::MountStrict(request.gameRoot);
                if (!mounted.Succeeded()) return {{}, std::move(mounted.diagnostics)};
                game = std::move(*mounted.value);
            }
            return Result<ResourceProbe>::Success(game->HasFile(std::string(name))
                ? ResourceProbe{ResourceProbeState::Found, std::string(name)} : ResourceProbe{});
        };
        source.read = [&](std::string_view name) -> Result<std::vector<std::byte>> {
            auto read = game->ReadFileBoundedWithSource(std::string(name), maximum);
            if (!read.Succeeded()) return {{}, std::move(read.diagnostics)};
            gameArchives[std::string(name)] = read.value->archiveName;
            return Result<std::vector<std::byte>>::Success(std::move(read.value->bytes));
        };
        if (const auto added = resolver.AddSource(std::move(source)); !added.Succeeded()) return {{}, added.diagnostics};
    }
    std::size_t retained = 0;
    for (const auto& name : domain) {
        auto read = resolver.Read(name);
        if (!read.Succeeded()) {
            if (read.diagnostics.size() == 1 && read.diagnostics.front().code == "RESOURCE_NOT_FOUND") {
                state.files.emplace(name, std::nullopt); continue;
            }
            return {{}, std::move(read.diagnostics)};
        }
        if (read.value->bytes.size() > kTotalBytes - retained)
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_TOTAL_BUDGET", "有效物编表超过 64 MiB 累积来源预算。");
        retained += read.value->bytes.size();
        std::string identity;
        switch (read.value->location.layer) {
        case ResourceLayer::SessionOverride: identity = "session:" + mapIdentity + '|' + request.overlayIdentity; break;
        case ResourceLayer::MapImport: identity = mapIdentity; break;
        case ResourceLayer::Campaign: identity = campaignIdentity; break;
        case ResourceLayer::Game:
            read.value->location.sourceId = gameArchives.at(name);
            identity = PathText(request.gameRoot) + '|' + read.value->location.sourceId;
            break;
        default: return Output::Failure("Object.Catalog", "OBJECT_CATALOG_LAYER_INVALID", "物编表来自未注册层。");
        }
        state.files.emplace(name, EffectiveCatalogFile{std::move(read.value->location), std::move(identity), std::move(read.value->bytes)});
    }
    if (const auto status = snapshot.Verify(); !status.Succeeded()) return {{}, status.diagnostics};
    return Output::Success(std::move(snapshot));
}

Result<EffectiveObjectCatalog> LoadEffectiveObjectCatalog(const ObjectCatalogSources& sources,
    const CatalogLoadOptions& options) {
    const auto names = EffectiveObjectCatalog::InputNames(options);
    if (!names.Succeeded()) return {{}, names.diagnostics};
    auto request = sources;
    request.gameOnlyNames = options.stringFiles;
    auto captured = CaptureObjectCatalogSources(request, *names.value, options.maximumFileBytes);
    if (!captured.Succeeded()) return {{}, std::move(captured.diagnostics)};
    const auto snapshot = std::move(*captured.value);
    return EffectiveObjectCatalog::Capture([snapshot](std::string_view name) { return snapshot.Read(name); }, options,
        [snapshot] { return snapshot.Verify(); });
}

Status VerifyEffectiveObjectCatalog(const EffectiveObjectCatalog& expected, const ObjectCatalogSources& sources) {
    if (const auto status = expected.VerifyCapturedSources(); !status.Succeeded()) return status;
    std::vector<std::string> names;
    std::vector<std::string> gameOnly;
    for (const auto& input : expected.Inputs()) {
        names.push_back(input.logicalPath);
        if (input.gameOnly) gameOnly.push_back(input.logicalPath);
    }
    auto request = sources; request.gameOnlyNames = gameOnly;
    auto captured = CaptureObjectCatalogSources(request, names);
    if (!captured.Succeeded()) return {captured.diagnostics};
    auto verified = expected.Verify([&](std::string_view name) { return captured.value->Read(name); });
    return verified.Succeeded() ? captured.value->Verify() : verified;
}
} // namespace War3Map::Services
