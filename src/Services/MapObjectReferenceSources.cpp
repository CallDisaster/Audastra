#include "War3MapCore/Services/MapObjectReferenceCapture.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <algorithm>
#include <array>

namespace War3Map::Services {
namespace {

using SnapshotResult = Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>>;
using SourceResult = Result<MapObjectReferenceSource>;
constexpr std::size_t kMemberBytes = 128U * 1024U * 1024U;

std::string PathText(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::string Canonical(std::string name) {
    for (char& c : name) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return name;
}
SourceResult ReadDirectoryMember(const DirectoryWorkspace& workspace, std::string_view member) {
    const auto key = Canonical(std::string(member));
    const WorkspaceFile* selected = nullptr;
    for (const auto& file : workspace.Snapshot().files) {
        const auto relative = Canonical(PathText(file.relativePath));
        if ((key == "war3map.w3u" && relative == "table/unit.ini")
            || (key == "war3map.w3t" && relative == "table/item.ini"))
            return SourceResult::Failure("MapObjectReference", "MAP_OBJECT_LNI_CLASSIFICATION_UNSUPPORTED",
                "当前目录含LNI物编覆盖，尚未捕获其类别来源，不能借原生或标准表猜测预放引用。");
        if (relative != key && relative != "map/" + key) continue;
        if (selected) return SourceResult::Failure("MapObjectReference", "MAP_OBJECT_SOURCE_AMBIGUOUS",
            "预放引用的分类成员有多个同名来源：" + std::string(member));
        selected = &file;
    }
    MapObjectReferenceSource result;
    result.source = PathText(workspace.Snapshot().root / std::filesystem::path(member));
    if (!selected) return SourceResult::Success(std::move(result));
    const auto path = workspace.Snapshot().root / selected->relativePath;
    result.source = PathText(path);
    auto bytes = IO::ReadFileBytes(path, kMemberBytes);
    if (!bytes.Succeeded() || !bytes.value) return {{}, std::move(bytes.diagnostics)};
    const auto hash = Sha256(*bytes.value);
    if (!hash.Succeeded() || !hash.value || *hash.value != selected->sha256)
        return SourceResult::Failure("MapObjectReference", "MAP_OBJECT_SOURCE_STALE", "分类来源在捕获时改变。");
    result.bytes = std::move(*bytes.value);
    return SourceResult::Success(std::move(result));
}

Result<MapObjectReferenceCapture::Reader> StandardReader(const std::filesystem::path& root) {
    using Output = Result<MapObjectReferenceCapture::Reader>;
    if (root.empty()) return Output::Success({});
    auto mounted = Game::GameDataSource::MountStrict(root);
    if (!mounted.Succeeded() || !mounted.value) return {{}, std::move(mounted.diagnostics)};
    auto game = std::make_shared<Game::GameDataSource>(std::move(*mounted.value));
    return Output::Success([game](std::string_view name) -> SourceResult {
        const auto logical = std::string(name);
        if (!game->HasFile(logical)) return SourceResult::Success({PathText(game->Root()) + "/<absent>/" + logical, {}});
        auto read = game->ReadFileBoundedWithSource(logical, 32U * 1024U * 1024U);
        if (!read.Succeeded() || !read.value) return {{}, std::move(read.diagnostics)};
        return SourceResult::Success({PathText(game->Root() / read.value->archiveName) + "/" + logical,
            std::move(read.value->bytes)});
    });
}

} // namespace

SnapshotResult CaptureDirectoryMapObjectReferences(const std::filesystem::path& root,
    const std::filesystem::path& gameRoot) {
    auto workspace = DirectoryWorkspace::Open(root);
    if (!workspace.Succeeded() || !workspace.value) return {{}, std::move(workspace.diagnostics)};
    auto map = MapDocument::FromDirectory(*workspace.value);
    if (!map.Succeeded() || !map.value) return {{}, std::move(map.diagnostics)};
    auto standard = StandardReader(gameRoot);
    if (!standard.Succeeded() || !standard.value) return {{}, std::move(standard.diagnostics)};
    auto captured = MapObjectReferenceCapture::Capture(*map.value,
        [&](auto member) { return ReadDirectoryMember(*workspace.value, member); }, *standard.value);
    if (!captured.Succeeded()) return captured;
    const auto verified = workspace.value->VerifyUnchanged();
    if (!verified.Succeeded()) return {{}, verified.diagnostics};
    return captured;
}

SnapshotResult CaptureArchiveMapObjectReferences(const MapArchiveSession& archive,
    const std::filesystem::path& gameRoot, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals) {
    // 一次归档身份校验捕获所有未覆盖输入，避免参数面板为每个小成员重哈希整张大地图。
    std::vector<std::string> requested;
    for (const auto name : {"war3map.w3i", "war3mapUnits.doo", "war3map.w3u", "war3map.w3t"}) {
        const auto key = Canonical(name);
        if (std::none_of(replacements.begin(), replacements.end(), [&](const auto& entry) { return Canonical(entry.name) == key; })
            && std::none_of(removals.begin(), removals.end(), [&](const auto& entry) { return Canonical(entry) == key; }))
            requested.emplace_back(name);
    }
    ArchiveExactMemberLimits limits;
    // 复用明确成员读取的既有64MiB硬预算，只缩小成员数量，不提高全局读取上限。
    limits.memberCount = 4;
    auto captured = archive.CaptureExactMembers(requested, limits);
    if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
    const auto reader = [&](std::string_view member) -> SourceResult {
        const auto key = Canonical(std::string(member));
        const MemberReplacement* selected = nullptr;
        for (const auto& replacement : replacements) {
            if (Canonical(replacement.name) != key) continue;
            if (selected) return SourceResult::Failure("MapObjectReference", "MAP_OBJECT_SOURCE_AMBIGUOUS", "最终分类成员存在多个覆盖。");
            selected = &replacement;
        }
        const bool removed = std::any_of(removals.begin(), removals.end(), [&](const auto& name) { return Canonical(name) == key; });
        if (removed && selected) return SourceResult::Failure("MapObjectReference", "MAP_OBJECT_SOURCE_AMBIGUOUS", "最终分类成员同时被删除和覆盖。");
        MapObjectReferenceSource source{PathText(archive.SourcePath()) + "/" + std::string(member), {}};
        if (selected) { source.bytes = selected->data; return SourceResult::Success(std::move(source)); }
        if (removed) return SourceResult::Success(std::move(source));
        const auto at = std::find_if(requested.begin(), requested.end(), [&](const auto& name) { return Canonical(name) == key; });
        if (at == requested.end()) return SourceResult::Failure("MapObjectReference", "MAP_OBJECT_CAPTURE_INPUT_MISSING", "来源读取超出本次实际捕获的成员。");
        const auto& input = (*captured.value)[static_cast<std::size_t>(at - requested.begin())];
        if (input.present) source.bytes = input.bytes;
        return SourceResult::Success(std::move(source));
    };
    std::vector<MapArchiveMember> members;
    for (const auto name : {"war3map.w3i", "war3mapUnits.doo"}) {
        auto source = reader(name);
        if (!source.Succeeded() || !source.value) return {{}, std::move(source.diagnostics)};
        if (source.value->bytes) members.push_back({name, std::move(*source.value->bytes)});
    }
    const auto map = MapDocument::FromMembers(members, archive.SourcePath());
    auto standard = StandardReader(gameRoot);
    if (!standard.Succeeded() || !standard.value) return {{}, std::move(standard.diagnostics)};
    return MapObjectReferenceCapture::Capture(map, reader, *standard.value);
}

Status VerifyMapObjectReferenceContents(const Trigger::MapObjectReferenceSnapshot& expected,
    const Trigger::MapObjectReferenceSnapshot& actual) {
    const auto reject = [] { return Status::Failure("MapObjectReference", "MAP_OBJECT_REFERENCE_SOURCE_CHANGED",
        "预放对象的成员、分类或遮蔽来源已改变；须重新查询并验证触发器引用。"); };
    if (expected.Inputs().size() != actual.Inputs().size() || expected.References().size() != actual.References().size()) return reject();
    for (std::size_t i = 0; i < expected.Inputs().size(); ++i) {
        const auto& a = expected.Inputs()[i]; const auto& b = actual.Inputs()[i];
        if (a.role != b.role || a.present != b.present || a.bytes != b.bytes || a.sha256 != b.sha256
            || (a.role.starts_with("standard-") && a.source != b.source)) return reject();
    }
    for (std::size_t i = 0; i < expected.References().size(); ++i) {
        const auto& a = expected.References()[i]; const auto& b = actual.References()[i];
        if (a.symbol != b.symbol || a.type != b.type || a.rawcode != b.rawcode || a.creationId != b.creationId
            || a.sourceIndex != b.sourceIndex || a.memberSha256 != b.memberSha256) return reject();
    }
    return Status::Ok();
}

Status VerifyMapObjectReferenceStandardSources(const Trigger::MapObjectReferenceSnapshot& expected,
    const std::filesystem::path& gameRoot) {
    auto reader = StandardReader(gameRoot);
    if (!reader.Succeeded() || !reader.value) return {reader.diagnostics};
    for (const auto& input : expected.Inputs()) {
        if (!input.role.starts_with("standard-")) continue;
        const auto name = input.role == "standard-unit" ? "Units/UnitData.slk" : "Units/ItemData.slk";
        if (!*reader.value) {
            if (input.present || input.source != "unconfigured:" + std::string(name))
                return Status::Failure("MapObjectReference", "MAP_OBJECT_REFERENCE_SOURCE_CHANGED", "预放引用分类的游戏来源已移除。");
            continue;
        }
        auto current = (*reader.value)(name);
        if (!current.Succeeded() || !current.value) return {current.diagnostics};
        if (input.present != current.value->bytes.has_value() || input.source != current.value->source
            || (input.present && input.bytes != *current.value->bytes))
            return Status::Failure("MapObjectReference", "MAP_OBJECT_REFERENCE_SOURCE_CHANGED", "预放引用的游戏分类来源已改变或被遮蔽。");
    }
    return Status::Ok();
}

} // namespace War3Map::Services
