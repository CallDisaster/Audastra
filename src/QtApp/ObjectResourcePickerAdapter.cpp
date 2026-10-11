#include "ObjectResourcePickerAdapter.h"
#include "ObjectResourcePreview.h"
#include "EditorProject.h"
#include "ProjectQuerySources.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Services/ImportInventoryQuery.h"
#include <mutex>
#include <type_traits>

namespace War3Map::QtApp {
namespace {
using CatalogResult = Result<std::shared_ptr<const Services::ObjectResourceCatalog>>;
struct SourceStamp {
    std::uint64_t generation = 0, revision = 0, epoch = 0;
    const void* archive = nullptr;
    const void* directory = nullptr;
    const void* catalog = nullptr;
    const void* schema = nullptr;
    std::filesystem::path project, game;
    bool operator==(const SourceStamp&) const = default;
};
SourceStamp Stamp(const QtProject::EditorProject& p) {
    return {p.editRevision_.Generation(), p.editRevision_.Revision(), p.ContentCacheEpoch(),
        p.archiveSession_.get(), p.directorySnapshot_.get(), p.standardCatalog_.get(), p.objectFieldSchema_.get(),
        p.projectRoot_, p.war3GameRoot_};
}
struct Inputs {
    std::string revision;
    std::array<std::vector<ObjectData::ObjectRecord>, 7> records;
    std::array<Services::ProjectSearchObjectSource, 7> sources;
    std::shared_ptr<const ObjectData::StandardObjectCatalog> standard;
    std::shared_ptr<const ObjectData::ObjectFieldSchema> schema;
    std::optional<MapData::StringTableDocument> strings;
    std::optional<DirectoryWorkspace> directory;
    std::filesystem::path archivePath, game;
    Services::ImportInventoryBinding importBinding;
    Services::ImportInventoryOverlay overlay;
    Result<Modules::WorldTerrainSources> previewSources;
    std::vector<Diagnostic> objectSnapshotErrors;
    std::mutex mutex;
    std::shared_ptr<const Services::ObjectResourceCatalog> catalogs[3];
};
bool ObjectSnapshotFits(const std::array<Services::ProjectSearchObjectSource, 7>& sources) {
    std::size_t bytes = 0, records = 0, modifications = 0;
    const auto add = [&bytes](std::size_t amount) {
        constexpr std::size_t maximum = 64 * 1024 * 1024;
        if (amount > maximum - bytes) return false;
        bytes += amount; return true;
    };
    for (const auto& source : sources) {
        if (source.records.size() > 32768 - records) return false;
        records += source.records.size();
        for (const auto& record : source.records) {
            if (!add(sizeof(record)) || !add(record.key.rawcode.size()) ||
                (record.parentRawcode && !add(record.parentRawcode->size()))) return false;
            if (record.modifications.size() > 262144 - modifications) return false;
            modifications += record.modifications.size();
            for (const auto& field : record.modifications) {
                if (!add(sizeof(field)) || !add(field.key.fieldId.size()) || !add(field.sourceKey.size()) ||
                    !add(field.originalValueToken.size())) return false;
                if (field.origin && (!add(field.origin->fingerprint.size()) ||
                    !add(field.origin->source.native().size() * sizeof(std::filesystem::path::value_type)))) return false;
                const bool fits = std::visit([&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::string>) return add(value.size());
                    else if constexpr (std::is_same_v<T, ObjectData::UnknownBytesValue>) return add(value.bytes.size());
                    else if constexpr (std::is_same_v<T, ObjectData::RawcodeListValue>) {
                        if (value.values.size() > 262144 || !add(value.values.size() * sizeof(std::string))) return false;
                        for (const auto& part : value.values) if (!add(part.size())) return false;
                        return true;
                    } else if constexpr (requires { value.value; }) return add(value.value.size());
                    else return true;
                }, field.value);
                if (!fits) return false;
            }
        }
    }
    return true;
}
std::shared_ptr<Inputs> Freeze(QtProject::EditorProject& p, const SourceStamp& stamp) {
    auto result = std::make_shared<Inputs>();
    result->revision = "resources:" + std::to_string(stamp.generation) + ':' + std::to_string(stamp.revision)
        + ':' + std::to_string(stamp.epoch);
    result->sources = QtProject::CaptureProjectObjectSources(p);
    // UI 线程只复制有明确上限的地图覆盖，原版大目录以不可变快照共享。
    // 超限拒绝整个物体索引，不能截断后将未复制的覆盖误认成原版值。
    if (!ObjectSnapshotFits(result->sources)) {
        result->objectSnapshotErrors = {{DiagnosticSeverity::Error, "Object.Resource", "OBJECT_RESOURCE_SNAPSHOT_LIMIT",
            "地图物编覆盖超过资源选择器的安全快照上限；仍可使用游戏文件、导入目录或自定义路径。"}};
        for (auto& source : result->sources) source.records = {};
    } else {
        for (std::size_t index = 0; index < result->sources.size(); ++index) {
            result->records[index].assign(result->sources[index].records.begin(), result->sources[index].records.end());
            result->sources[index].records = result->records[index];
        }
    }
    result->standard = p.standardCatalog_;
    result->schema = p.objectFieldSchema_;
    if (p.mapEdits_) {
        auto map = p.mapEdits_->Snapshot(MapDataKind::Strings, 32 * 1024 * 1024);
        if (map.Succeeded()) result->strings = map.value->Strings();
        else result->objectSnapshotErrors.insert(result->objectSnapshotErrors.end(), map.diagnostics.begin(), map.diagnostics.end());
    } else if (p.mapDocument_ && p.mapDocument_->Strings()) {
        const auto& strings = *p.mapDocument_->Strings();
        std::size_t bytes = strings.originalBytes.size();
        for (const auto& entry : strings.entries) {
            if (bytes > 32 * 1024 * 1024 || entry.value.size() > 32 * 1024 * 1024 - bytes) { bytes = 32 * 1024 * 1024 + 1; break; }
            bytes += entry.value.size();
        }
        if (strings.entries.size() <= 262144 && bytes <= 32 * 1024 * 1024) result->strings = strings;
        else result->objectSnapshotErrors.push_back({DiagnosticSeverity::Error, "Object.Resource", "OBJECT_RESOURCE_STRINGS_LIMIT",
            "地图字符串表超过资源选择器的安全快照上限，物体索引未加载。"});
    }
    result->previewSources = Modules::CaptureWorldTerrainSources(p, 262144);
    result->game = p.war3GameRoot_;
    result->importBinding.documentId = "resource-picker";
    result->importBinding.generation = stamp.generation;
    result->importBinding.revision = result->revision;
    result->importBinding.assetRevision = result->revision;
    result->importBinding.contentScope = "effective-overlay";
    if (p.archiveSession_) {
        result->archivePath = p.archiveSession_->SourcePath();
        result->importBinding.sourceSha256 = p.archiveSession_->SourceSha256();
    } else if (p.directorySnapshot_) {
        result->directory = *p.directorySnapshot_;
        result->importBinding.sourceSha256 = p.directorySnapshot_->Snapshot().aggregateSha256;
    }
    for (const auto& edit : p.assetEdits_) result->overlay.pathEdits.push_back({edit.from.toStdString(), edit.to.toStdString()});
    for (const auto& entry : p.pendingImports_) result->overlay.pending.push_back({entry.archivePath.toStdString(), {}});
    result->overlay.includesUnsavedChanges = !result->overlay.pathEdits.empty() || !result->overlay.pending.empty();
    return result;
}
CatalogResult Load(Inputs& inputs, Services::ObjectResourceOrigin origin, const std::function<bool(std::size_t)>& progress) {
    // 各面板共享一份自有修订快照和索引；磁盘句柄只在本后台调用内持有。
    std::lock_guard lock(inputs.mutex);
    const auto index = static_cast<std::size_t>(origin);
    if (inputs.catalogs[index]) return CatalogResult::Success(inputs.catalogs[index]);
    Services::ObjectResourceCatalogLimits limits; limits.progress = progress;
    CatalogResult result;
    if (origin == Services::ObjectResourceOrigin::Object) {
        if (!inputs.objectSnapshotErrors.empty()) return {std::nullopt, inputs.objectSnapshotErrors};
        Services::ObjectResourceContext context;
        context.revision = inputs.revision; context.schema = inputs.schema.get();
        context.names.objects = inputs.sources; context.names.standardObjects = inputs.standard.get();
        context.names.strings = inputs.strings ? &*inputs.strings : nullptr;
        result = Services::CaptureObjectResourceCatalog(context, limits);
    } else if (origin == Services::ObjectResourceOrigin::Game) {
        auto game = Game::GameDataSource::MountStrict(inputs.game);
        if (!game.Succeeded()) return {std::nullopt, std::move(game.diagnostics)};
        result = Services::CaptureGameResourceCatalog(*game.value, inputs.revision, limits);
    } else {
        std::optional<MapArchiveSession> archive;
        Result<std::shared_ptr<const Services::ImportInventorySnapshot>> inventory;
        if (!inputs.archivePath.empty()) {
            auto opened = MapArchiveSession::Open(inputs.archivePath);
            if (!opened.Succeeded()) return {std::nullopt, std::move(opened.diagnostics)};
            archive = std::move(*opened.value);
            if (archive->SourceSha256() != inputs.importBinding.sourceSha256)
                return CatalogResult::Failure("Object.Resource", "OBJECT_RESOURCE_SOURCE_CHANGED", "地图来源已变化，导入目录未加载。");
            inventory = Services::CaptureImportInventory(*archive, inputs.importBinding, inputs.overlay);
        } else if (inputs.directory) inventory = Services::CaptureImportInventory(*inputs.directory, inputs.importBinding, inputs.overlay);
        else return CatalogResult::Failure("Object.Resource", "OBJECT_RESOURCE_IMPORT_SOURCE_MISSING", "当前工程没有可验证的地图导入来源。");
        if (!inventory.Succeeded()) return {std::nullopt, std::move(inventory.diagnostics)};
        result = Services::CaptureImportResourceCatalog(**inventory.value, inputs.importBinding, limits);
    }
    if (result.Succeeded()) inputs.catalogs[index] = *result.value;
    return result;
}
}

Modules::ObjectResourcePickerFactory MakeObjectResourcePickerFactory(QtProject::EditorProject& project) {
    struct Cache { std::optional<SourceStamp> stamp; std::shared_ptr<Inputs> inputs; };
    const QPointer<QtProject::EditorProject> owner(&project);
    auto cache = std::make_shared<Cache>();
    return [owner, cache] {
        Modules::ObjectResourcePickerServices services;
        if (!owner) return services;
        const auto stamp = Stamp(*owner);
        if (!cache->stamp || *cache->stamp != stamp) { cache->inputs = Freeze(*owner, stamp); cache->stamp = stamp; }
        const auto inputs = cache->inputs;
        services.revision = inputs->revision;
        services.isCurrent = [owner, stamp] { return owner && !owner->sourceChangeSuspected_ && !owner->sourceChangeConfirmed_ && Stamp(*owner) == stamp; };
        services.load = [inputs](auto origin, const auto& progress) { return Load(*inputs, origin, progress); };
        services.inspect = [inputs, current = services.isCurrent](std::string_view value, Services::ObjectResourceKind kind)
            -> Result<Services::ObjectResourceInfo> {
            using Return = Result<Services::ObjectResourceInfo>;
            if (!current()) return Return::Failure("Object.Resource", "OBJECT_RESOURCE_STALE", "资源来源已变化。");
            if (!inputs->previewSources.Succeeded()) return {{}, inputs->previewSources.diagnostics};
            auto reader = Modules::WorldResourceReader::Open(*inputs->previewSources.value);
            if (!reader.Succeeded()) return {{}, reader.diagnostics};
            const auto before = (*reader.value)->VerifyCurrentSource();
            if (!before.Succeeded()) return {{}, before.diagnostics};
            auto result = Services::InspectObjectResource((*reader.value)->Resolver(), value, kind);
            const auto after = (*reader.value)->VerifyCurrentSource();
            if (!after.Succeeded()) return {{}, after.diagnostics};
            if (!current()) return Return::Failure("Object.Resource", "OBJECT_RESOURCE_STALE", "读取期间资源来源已变化。");
            return result;
        };
        services.preview = [inputs, stamp, current = services.isCurrent](QWidget* parent) {
            return MakeObjectResourcePreview(parent, inputs->previewSources, inputs->revision, stamp.generation, stamp.revision, current);
        };
        return services;
    };
}
}
