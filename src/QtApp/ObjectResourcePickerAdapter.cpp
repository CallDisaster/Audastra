#include "ObjectResourcePickerAdapter.h"
#include "ObjectResourcePreview.h"
#include "EditorProject.h"
#include "ProjectQuerySources.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Services/ImportInventoryQuery.h"
#include "War3MapCore/Services/ObjectResourceDirectory.h"
#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace War3Map::QtApp {
namespace {
using CatalogResult = Result<std::shared_ptr<const Services::ObjectResourceCatalog>>;
// 一份索引按“内容身份”共享：游戏文件只看游戏目录，物体用过的资源只看物编与字符串内容，
// 导入目录仍按修订。编辑触发器、地形等不改这些内容时直接复用，不再每次修改后重建。
struct CatalogSlot {
    std::mutex build;
    std::shared_ptr<const Services::ObjectResourceCatalog> catalog;
    std::atomic_size_t examined{0};
};
struct CatalogStore {
    std::mutex mutex;
    std::array<std::deque<std::pair<std::string, std::shared_ptr<CatalogSlot>>>, 3> byOrigin; // 不叫 slots：那是 Qt 的宏。
    std::shared_ptr<CatalogSlot> Slot(std::size_t origin, const std::string& key) {
        std::lock_guard lock(mutex);
        auto& list = byOrigin[origin];
        for (const auto& [existing, slot] : list) if (existing == key) return slot;
        list.emplace_back(key, std::make_shared<CatalogSlot>());
        while (list.size() > 2) list.pop_front(); // 每类只留最近两份；正在用的由持有者保活。
        return list.back().second;
    }
};
struct Fingerprint {
    std::uint64_t value = 1469598103934665603ull;
    void Bytes(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) { value ^= bytes[i]; value *= 1099511628211ull; }
    }
    template <class T> void Number(T number) { Bytes(&number, sizeof(number)); }
    void Text(std::string_view text) { Number(text.size()); Bytes(text.data(), text.size()); }
};
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
    std::shared_ptr<CatalogStore> store;
    std::array<std::string, 3> keys; // 按 ObjectResourceOrigin 编号的内容身份。
};
// 物体索引只取决于原版目录、字段元数据、地图物编覆盖与字符串表；指纹覆盖全部这些输入。
std::string ObjectContentKey(const Inputs& inputs) {
    Fingerprint print;
    print.Number(reinterpret_cast<std::uintptr_t>(inputs.standard.get()));
    print.Number(reinterpret_cast<std::uintptr_t>(inputs.schema.get()));
    for (const auto& error : inputs.objectSnapshotErrors) print.Text(error.code);
    for (const auto& source : inputs.sources) {
        print.Number(static_cast<int>(source.type)); print.Number(static_cast<int>(source.state));
        print.Number(static_cast<int>(source.origin)); print.Number(source.records.size());
        for (const auto& record : source.records) {
            print.Text(record.key.rawcode); print.Number(record.parentRawcode.has_value());
            if (record.parentRawcode) print.Text(*record.parentRawcode);
            print.Number(record.modifications.size());
            for (const auto& modification : record.modifications) {
                print.Text(modification.key.fieldId);
                for (const auto& part : {modification.key.level, modification.key.dataIndex, modification.key.variation}) {
                    print.Number(part.has_value()); print.Number(part.value_or(0));
                }
                print.Text(modification.sourceKey); print.Number(modification.valueWasOpaque);
                print.Number(modification.value.index());
                if (const auto* opaque = std::get_if<ObjectData::UnknownBytesValue>(&modification.value))
                    print.Bytes(opaque->bytes.data(), opaque->bytes.size());
                else print.Text(ObjectData::ValueToDisplayString(modification.value));
            }
        }
    }
    print.Number(inputs.strings.has_value());
    if (inputs.strings) {
        print.Number(inputs.strings->entries.size());
        for (const auto& entry : inputs.strings->entries) { print.Number(entry.id); print.Text(entry.value); }
    }
    return "object:" + std::to_string(print.value);
}
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
std::shared_ptr<Inputs> Freeze(QtProject::EditorProject& p, const SourceStamp& stamp, std::shared_ptr<CatalogStore> store) {
    auto result = std::make_shared<Inputs>();
    result->store = std::move(store);
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
    const auto game = result->game.u8string();
    result->keys[static_cast<std::size_t>(Services::ObjectResourceOrigin::Object)] = ObjectContentKey(*result);
    result->keys[static_cast<std::size_t>(Services::ObjectResourceOrigin::Game)] = "game:" + std::string(game.begin(), game.end());
    result->keys[static_cast<std::size_t>(Services::ObjectResourceOrigin::Import)] = "import:" + result->revision;
    return result;
}
CatalogResult Load(Inputs& inputs, Services::ObjectResourceOrigin origin, const std::function<bool(std::size_t)>& progress) {
    // 各面板与后台预热共享同一份按内容身份缓存的索引；磁盘句柄只在本后台调用内持有。
    const auto index = static_cast<std::size_t>(origin);
    if (origin == Services::ObjectResourceOrigin::Object && !inputs.objectSnapshotErrors.empty())
        return {std::nullopt, inputs.objectSnapshotErrors};
    const auto slot = inputs.store->Slot(index, inputs.keys[index]);
    // 同一份索引正在别处（后台预热或另一个面板）建立时等它完成，期间照常回报进度，调用方可取消。
    std::unique_lock lock(slot->build, std::defer_lock);
    while (!lock.try_lock()) {
        if (progress && !progress(slot->examined.load()))
            return CatalogResult::Failure("Object.Resource", "OBJECT_RESOURCE_INDEX_CANCELLED", "资源索引已取消。");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (slot->catalog) return CatalogResult::Success(slot->catalog);
    Services::ObjectResourceCatalogLimits limits;
    limits.progress = [&slot, &progress](std::size_t examined) {
        slot->examined.store(examined);
        return !progress || progress(examined);
    };
    CatalogResult result;
    if (origin == Services::ObjectResourceOrigin::Object) {
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
    if (result.Succeeded()) slot->catalog = *result.value;
    return result;
}
}

Modules::ObjectResourcePickerFactory MakeObjectResourcePickerFactory(QtProject::EditorProject& project) {
    struct Cache {
        std::optional<SourceStamp> stamp;
        std::shared_ptr<Inputs> inputs;
        std::shared_ptr<CatalogStore> store = std::make_shared<CatalogStore>();
    };
    const QPointer<QtProject::EditorProject> owner(&project);
    auto cache = std::make_shared<Cache>();
    return [owner, cache] {
        Modules::ObjectResourcePickerServices services;
        if (!owner) return services;
        const auto stamp = Stamp(*owner);
        if (!cache->stamp || *cache->stamp != stamp) { cache->inputs = Freeze(*owner, stamp, cache->store); cache->stamp = stamp; }
        const auto inputs = cache->inputs;
        services.revision = inputs->revision;
        services.isCurrent = [owner, stamp] { return owner && !owner->sourceChangeSuspected_ && !owner->sourceChangeConfirmed_ && Stamp(*owner) == stamp; };
        services.load = [inputs](auto origin, const auto& progress) { return Load(*inputs, origin, progress); };
        services.inspect = [inputs](std::string_view value, Services::ObjectResourceKind kind)
            -> Result<Services::ObjectResourceInfo> {
            using Return = Result<Services::ObjectResourceInfo>;
            // 只读冻结输入和来源租约；QObject修订只由调用方在所属线程前后复核。
            if (inputs->directory) return Services::InspectDirectoryObjectResource(*inputs->directory,
                inputs->game, inputs->importBinding, inputs->overlay, value, kind);
            if (!inputs->previewSources.Succeeded()) return {{}, inputs->previewSources.diagnostics};
            auto reader = Modules::WorldResourceReader::Open(*inputs->previewSources.value);
            if (!reader.Succeeded()) return {{}, reader.diagnostics};
            const auto before = (*reader.value)->VerifyCurrentSource();
            if (!before.Succeeded()) return {{}, before.diagnostics};
            auto result = Services::InspectObjectResource((*reader.value)->Resolver(), value, kind);
            const auto after = (*reader.value)->VerifyCurrentSource();
            if (!after.Succeeded()) return {{}, after.diagnostics};
            return result;
        };
        services.preview = [inputs, stamp, current = services.isCurrent](QWidget* parent) {
            return MakeObjectResourcePreview(parent, inputs->previewSources, inputs->revision, stamp.generation, stamp.revision, current);
        };
        return services;
    };
}

namespace {
// 打开地图后在后台依次建游戏文件和物体两份索引，作者第一次打开模型选择器时不必再等。
// 同时只跑一轮；跑的过程中又有请求时，结束后按最新工程再跑一轮（内容没变的索引直接命中缓存）。
class ResourcePrewarm final : public std::enable_shared_from_this<ResourcePrewarm> {
public:
    ResourcePrewarm(Modules::ObjectResourcePickerFactory factory, QObject* context)
        : factory_(std::move(factory)), context_(context), alive_(std::make_shared<std::atomic_bool>(true)) {
        QObject::connect(context, &QObject::destroyed, [alive = alive_] { alive->store(false); });
    }
    void Request() {
        if (!context_) return;
        if (running_) { again_ = true; return; }
        const auto services = factory_();
        if (!services.load) return;
        running_ = true;
        auto* worker = QThread::create([load = services.load, alive = alive_] {
            const auto progress = [alive](std::size_t) { return alive->load(); };
            try {
                (void)load(Services::ObjectResourceOrigin::Game, progress);
                (void)load(Services::ObjectResourceOrigin::Object, progress);
            } catch (...) {} // 预热失败不影响作者打开选择器时按需重建并看到原因。
        });
        QObject::connect(worker, &QThread::finished, context_, [self = shared_from_this()] {
            self->running_ = false;
            if (std::exchange(self->again_, false)) self->Request();
        });
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start(QThread::LowestPriority);
    }
private:
    Modules::ObjectResourcePickerFactory factory_;
    QPointer<QObject> context_;
    std::shared_ptr<std::atomic_bool> alive_;
    bool running_ = false, again_ = false;
};
}

std::function<void()> MakeObjectResourcePrewarm(Modules::ObjectResourcePickerFactory factory, QObject* context) {
    auto prewarm = std::make_shared<ResourcePrewarm>(std::move(factory), context);
    return [prewarm] { prewarm->Request(); };
}
}
