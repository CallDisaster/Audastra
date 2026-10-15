#include "War3MapCore/Services/MapObjectReferenceCapture.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"

#include <algorithm>
#include <array>

namespace War3Map::Services {
namespace {

using SnapshotResult = Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>>;
using SourceResult = Result<MapObjectReferenceSource>;
constexpr std::size_t kMemberBytes = 128U * 1024U * 1024U;
const std::array<std::string, 2> kClassificationNames{"Units/UnitData.slk", "Units/ItemData.slk"};
using SourceScope = Trigger::MapObjectReferenceSourceScope;

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

MapObjectReferenceCapture::Reader ClassificationReader(const ObjectCatalogSourceSnapshot& snapshot,
    const ObjectCatalogSources& sources) {
    const auto missingPrefix = sources.gameRoot.empty() ? std::string("unconfigured:")
        : PathText(sources.gameRoot.lexically_normal()) + "/<absent>/";
    return [snapshot, missingPrefix](std::string_view name) -> SourceResult {
        auto read = snapshot.Read(name);
        if (!read.Succeeded() || !read.value) return {{}, std::move(read.diagnostics)};
        if (!*read.value) return SourceResult::Success({missingPrefix + std::string(name), {}});
        auto& file = **read.value;
        MapObjectReferenceSource result;
        // 身份字段用长度前缀编码，路径含分隔符也不会混淆层、容器与成员。
        result.source = "object-catalog:";
        const auto append = [&](std::string_view value) {
            result.source += std::to_string(value.size()) + ':';
            result.source += value;
        };
        append(std::to_string(static_cast<int>(file.location.layer)));
        append(std::to_string(file.location.priority));
        append(file.containerIdentity);
        append(file.location.sourceId);
        append(file.location.sourcePath);
        append(file.location.canonicalPath);
        if (file.location.layer == Resources::ResourceLayer::MapImport
            || file.location.layer == Resources::ResourceLayer::SessionOverride) result.scope = SourceScope::Map;
        else if (file.location.layer == Resources::ResourceLayer::Campaign) result.scope = SourceScope::Campaign;
        result.bytes = std::move(file.bytes);
        return SourceResult::Success(std::move(result));
    };
}

} // namespace

SnapshotResult CaptureDirectoryMapObjectReferences(const std::filesystem::path& root,
    const std::filesystem::path& gameRoot, const ObjectCatalogSources* classificationSources) {
    auto workspace = DirectoryWorkspace::Open(root);
    if (!workspace.Succeeded() || !workspace.value) return {{}, std::move(workspace.diagnostics)};
    auto map = MapDocument::FromDirectory(*workspace.value);
    if (!map.Succeeded() || !map.value) return {{}, std::move(map.diagnostics)};
    ObjectCatalogSources sources;
    sources.directory = &*workspace.value;
    sources.gameRoot = gameRoot;
    if (classificationSources) sources = *classificationSources;
    auto standard = CaptureObjectCatalogSources(sources, kClassificationNames);
    if (!standard.Succeeded() || !standard.value) return {{}, std::move(standard.diagnostics)};
    auto captured = MapObjectReferenceCapture::Capture(*map.value,
        [&](auto member) { return ReadDirectoryMember(*workspace.value, member); }, ClassificationReader(*standard.value, sources));
    if (!captured.Succeeded()) return captured;
    const auto classified = standard.value->Verify();
    if (!classified.Succeeded()) return {{}, classified.diagnostics};
    const auto verified = workspace.value->VerifyUnchanged();
    if (!verified.Succeeded()) return {{}, verified.diagnostics};
    return captured;
}

SnapshotResult CaptureArchiveMapObjectReferences(const MapArchiveSession& archive,
    const std::filesystem::path& gameRoot, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals, const ObjectCatalogSources* classificationSources) {
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
    ObjectCatalogSources sources;
    sources.archive = &archive;
    sources.gameRoot = gameRoot;
    sources.replacements = replacements;
    sources.removals = removals;
    sources.overlayIdentity = archive.SourceSha256();
    if (classificationSources) sources = *classificationSources;
    auto standard = CaptureObjectCatalogSources(sources, kClassificationNames);
    if (!standard.Succeeded() || !standard.value) return {{}, std::move(standard.diagnostics)};
    auto result = MapObjectReferenceCapture::Capture(map, reader, ClassificationReader(*standard.value, sources));
    if (!result.Succeeded()) return result;
    const auto verified = standard.value->Verify();
    return verified.Succeeded() ? result : SnapshotResult{{}, verified.diagnostics};
}

Status VerifyMapObjectReferenceContents(const Trigger::MapObjectReferenceSnapshot& expected,
    const Trigger::MapObjectReferenceSnapshot& actual) {
    const auto reject = [] { return Status::Failure("MapObjectReference", "MAP_OBJECT_REFERENCE_SOURCE_CHANGED",
        "预放对象的成员、分类或遮蔽来源已改变；须重新查询并验证触发器引用。"); };
    if (expected.Inputs().size() != actual.Inputs().size() || expected.References().size() != actual.References().size()) return reject();
    for (std::size_t i = 0; i < expected.Inputs().size(); ++i) {
        const auto& a = expected.Inputs()[i]; const auto& b = actual.Inputs()[i];
        if (a.role != b.role || a.present != b.present || a.bytes != b.bytes || a.sha256 != b.sha256
            || a.scope != b.scope
            || (a.role.starts_with("standard-") && a.scope != SourceScope::Map && a.source != b.source)) return reject();
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
    ObjectCatalogSources sources;
    sources.gameRoot = gameRoot;
    return VerifyMapObjectReferenceClassificationSources(expected, sources);
}

Status VerifyMapObjectReferenceClassificationSources(const Trigger::MapObjectReferenceSnapshot& expected,
    const ObjectCatalogSources& sources) {
    auto snapshot = CaptureObjectCatalogSources(sources, kClassificationNames);
    if (!snapshot.Succeeded() || !snapshot.value) return {snapshot.diagnostics};
    const auto reader = ClassificationReader(*snapshot.value, sources);
    for (const auto& input : expected.Inputs()) {
        if (!input.role.starts_with("standard-")) continue;
        const auto name = input.role == "standard-unit" ? "Units/UnitData.slk" : "Units/ItemData.slk";
        auto current = reader(name);
        if (!current.Succeeded() || !current.value) return {current.diagnostics};
        if (input.present != current.value->bytes.has_value() || input.scope != current.value->scope
            || input.source != current.value->source
            || (input.present && input.bytes != *current.value->bytes))
            return Status::Failure("MapObjectReference", "MAP_OBJECT_REFERENCE_SOURCE_CHANGED", "预放引用的有效分类表已改变、换层或被遮蔽。");
    }
    return snapshot.value->Verify();
}

Status VerifyDirectoryMapObjectReferenceClassificationSources(const Trigger::MapObjectReferenceSnapshot& expected,
    const std::filesystem::path& root, const std::filesystem::path& gameRoot) {
    auto directory = DirectoryWorkspace::Open(root);
    if (!directory.Succeeded() || !directory.value) return {directory.diagnostics};
    ObjectCatalogSources sources;
    sources.directory = &*directory.value;
    sources.gameRoot = gameRoot;
    const auto verified = VerifyMapObjectReferenceClassificationSources(expected, sources);
    return verified.Succeeded() ? directory.value->VerifyUnchanged() : verified;
}

} // namespace War3Map::Services
