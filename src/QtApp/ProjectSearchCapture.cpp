#include "ProjectSearchCapture.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Workspace.h"

#include <QObject>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace War3Map::QtApp {
namespace {
using SearchState = Services::ProjectSearchState;
bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Charge(std::size_t& total, std::size_t count, std::size_t limit) {
    if (count > limit - total) return false;
    total += count; return true;
}
void Tick(std::uint64_t& value) { if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value; }
bool Valid(const ProjectSearchCaptureOptions& o) {
    return o.maxAssetInputRows && o.maxAssetInputRows <= 32768 && o.maxAssetPathBytes && o.maxAssetPathBytes <= 1024 * 1024
        && o.maxCatalogSourceRows && o.maxCatalogSourceRows <= 1024 && o.maxCatalogDefinitions && o.maxCatalogDefinitions <= 65536
        && o.maxCatalogBytes && o.maxCatalogBytes <= 8 * 1024 * 1024 && o.maxMapBytes && o.maxMapBytes <= 64 * 1024 * 1024;
}
std::string SourceSha(const QtProject::EditorProject& p) {
    if (p.archiveSession_) return p.archiveSession_->SourceSha256();
    if (p.directorySnapshot_) return p.directorySnapshot_->Snapshot().aggregateSha256;
    return {};
}
bool Unsafe(const QtProject::EditorProject& p) { return p.sourceChangeSuspected_ || p.sourceChangeConfirmed_; }
std::array<const void*, 14> ObjectOwners(const QtProject::EditorProject& p) {
    std::array<const void*, 14> owners{};
    for (std::size_t i = 0; i < 7; ++i) { owners[i * 2] = p.objectDocuments_[i].get(); owners[i * 2 + 1] = p.nativeObjectDocuments_[i].get(); }
    return owners;
}
bool Identity(std::string_view value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
} // namespace

struct ProjectSearchCapture::State {
    QPointer<QtProject::EditorProject> owner;
    std::unique_ptr<QObject> receiver = std::make_unique<QObject>();
    std::string identity, revision;
    std::uint64_t generation = 0, actualGeneration = 0, serial = 0, acceptedSerial = 0;
    std::uint64_t assetEpoch = 0, typeEpoch = 0, objectEpoch = 0;
    std::uint64_t acceptedAssetEpoch = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t acceptedTypeEpoch = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t acceptedObjectEpoch = (std::numeric_limits<std::uint64_t>::max)();
    ProjectSearchCaptureOptions options;
    ProjectSearchCaptureStats stats;
    const Trigger::UiCatalog* catalog = nullptr;
    std::string catalogSha;
    std::vector<Trigger::UiSourceManifestEntry> manifest; // 完整有限清单的拥有值，不是前缀证明。
    std::unique_ptr<Trigger::TriggerTypeSystem> types;
    const MapArchiveSession* assetArchive = nullptr;
    const DirectoryWorkspace* assetDirectory = nullptr;
    std::string assetSourceSha;
    bool assetUnsafe = false, sourceAdmitted = false;
    std::size_t pendingCount = 0, editCount = 0;
    const void* pendingData = nullptr;
    const void* editData = nullptr;
    std::vector<QtProject::AssetEntry> assets;
    std::vector<std::string> fileIds, filePaths;
    std::vector<Services::ProjectSearchImportedFile> files;
    const MapDocument* mapOwner = nullptr;
    const MapData::MapDataEditSession* mapEdits = nullptr;
    const MapArchiveSession* mapArchive = nullptr;
    const DirectoryWorkspace* mapDirectory = nullptr;
    std::uint64_t mapSessionId = 0, mapRevision = 0;
    std::string mapSourceSha;
    bool mapUnsafe = false;
    std::unique_ptr<Services::MapDataQuerySnapshot> map;
    std::array<Services::ProjectSearchObjectSource, 7> objects{};
    std::array<const void*, 14> objectOwners{};
    const MapArchiveSession* missingArchive = nullptr;
    std::string missingSha;
    bool missingUnsafe = true;
    std::array<bool, 7> missing{};

    void Attach(const QtProject::EditorProject& p) {
        owner = const_cast<QtProject::EditorProject*>(&p);
        const auto changed = [this] { Tick(serial); };
        QObject::connect(&p, &QtProject::EditorProject::ImportsChanged, receiver.get(), [this] { Tick(serial); Tick(assetEpoch); });
        QObject::connect(&p, &QtProject::EditorProject::MapDocumentChanged, receiver.get(), changed);
        QObject::connect(&p, &QtProject::EditorProject::SourceStateChanged, receiver.get(), changed);
        QObject::connect(&p, &QtProject::EditorProject::ObjectDocumentsAboutToReplace, receiver.get(), [this] { Tick(serial); Tick(objectEpoch); });
        QObject::connect(&p, &QtProject::EditorProject::ObjectDocumentsReplaced, receiver.get(), [this] { Tick(serial); Tick(objectEpoch); });
        const auto catalogChanged = [this] { Tick(serial); Tick(typeEpoch); types.reset(); };
        QObject::connect(&p, &QtProject::EditorProject::TriggerDocumentAboutToChange, receiver.get(), catalogChanged);
        QObject::connect(&p, &QtProject::EditorProject::TriggerDocumentChanged, receiver.get(), changed);
        QObject::connect(&p, &QtProject::EditorProject::UiLibrariesChanged, receiver.get(), catalogChanged);
        QObject::connect(&p, &QtProject::EditorProject::WorldEditStringsChanged, receiver.get(), catalogChanged);
    }

    bool Current(const QtProject::EditorProject& p) const {
        return owner && owner.data() == &p && serial != (std::numeric_limits<std::uint64_t>::max)()
            && acceptedSerial == serial && !revision.empty()
            && actualGeneration == p.editRevision_.Generation() && catalog == p.uiCatalog_.get()
            && (!catalog || catalogSha == catalog->Sha256()) && acceptedAssetEpoch == assetEpoch
            && assetArchive == p.archiveSession_.get() && assetDirectory == p.directorySnapshot_.get()
            && assetSourceSha == SourceSha(p) && assetUnsafe == Unsafe(p)
            && pendingCount == p.pendingImports_.size() && editCount == p.assetEdits_.size()
            && pendingData == p.pendingImports_.data() && editData == p.assetEdits_.data()
            && objectOwners == ObjectOwners(p) && ObjectViewsCurrent(p) && mapOwner == p.mapDocument_.get() && mapEdits == p.mapEdits_.get()
            && mapSessionId == (p.mapEdits_ ? p.mapEdits_->SessionIdentity() : 0)
            && mapRevision == (p.mapEdits_ ? p.mapEdits_->Revision() : 0)
            && mapArchive == p.archiveSession_.get() && mapDirectory == p.directorySnapshot_.get()
            && mapSourceSha == SourceSha(p) && mapUnsafe == Unsafe(p);
    }

    bool ObjectViewsCurrent(const QtProject::EditorProject& p) const {
        for (std::size_t i = 0; i < 7; ++i) {
            const auto* lni = p.objectDocuments_[i].get();
            const auto* native = p.nativeObjectDocuments_[i].get();
            const auto records = lni ? lni->Objects() : native ? native->Objects() : std::span<const ObjectData::ObjectRecord>{};
            if (records.data() != objects[i].records.data() || records.size() != objects[i].records.size()) return false;
            if (lni || native) {
                const bool safe = lni ? lni->IsStructurallySafe() : native->IsStructurallySafe();
                if (objects[i].state != (safe ? SearchState::Ready : SearchState::Partial)) return false;
            }
        }
        return true;
    }

    void Types(const QtProject::EditorProject& p, bool force) {
        const auto* next = p.uiCatalog_.get();
        const auto sha = next ? next->Sha256() : std::string{};
        if (!force && catalog == next && catalogSha == sha && acceptedTypeEpoch == typeEpoch) { Tick(stats.typeCacheHits); return; }
        Tick(stats.typePreparations); catalog = next; catalogSha = sha; acceptedTypeEpoch = typeEpoch;
        types.reset(); manifest.clear(); stats.typesReady = false; stats.typeReason = "SEARCH_TYPES_UNAVAILABLE";
        stats.catalogSourceRows = stats.catalogDefinitions = stats.catalogBytes = 0; stats.catalogManifestSha256.clear();
        if (!next || !Sha(sha)) return;
        if (next->Manifest().size() > options.maxCatalogSourceRows || next->Documents().size() > options.maxCatalogSourceRows) {
            stats.typeReason = "SEARCH_TYPES_BUDGET"; return;
        }
        stats.catalogSourceRows = next->Manifest().size();
        if (!Charge(stats.catalogBytes, next->ConfigDocument().OriginalBytes().size(), options.maxCatalogBytes)) { stats.typeReason = "SEARCH_TYPES_BUDGET"; return; }
        for (const auto& document : next->Documents())
            if (!Charge(stats.catalogBytes, document.OriginalBytes().size(), options.maxCatalogBytes)) { stats.typeReason = "SEARCH_TYPES_BUDGET"; return; }
        std::string digest;
        for (const auto& item : next->Manifest()) {
            if (item.relativePath.native().size() > 2048 || item.layer.size() > 2048 || item.sha256.size() > 64
                || !Charge(stats.catalogDefinitions, item.definitionCount, options.maxCatalogDefinitions)) { stats.typeReason = "SEARCH_TYPES_BUDGET"; return; }
            const auto path = IO::GenericPathUtf8(item.relativePath);
            if (path.size() > 2048 || !Charge(stats.catalogBytes, item.layer.size() + path.size() + item.sha256.size() + 128, options.maxCatalogBytes)) {
                stats.typeReason = "SEARCH_TYPES_BUDGET"; return;
            }
            // expected包含五种可选txt；Loader明确记录未提供，不把可信Absent当成解析失败。
            if ((item.present && (!item.parsed || !Sha(item.sha256)))
                || (!item.present && (item.parsed || !item.sha256.empty() || item.size || item.definitionCount))) {
                stats.typeReason = "SEARCH_TYPES_MANIFEST_UNSAFE"; return;
            }
            const auto append = [&digest](std::string_view value) { digest += std::to_string(value.size()) + ":"; digest += value; };
            append(item.layer); append(path); append(item.sha256);
            append(std::to_string(static_cast<unsigned>(item.kind))); append(std::to_string(item.size));
            append(std::to_string(item.definitionCount)); append(item.expected ? "1" : "0");
            append(item.present ? "1" : "0"); append(item.parsed ? "1" : "0");
        }
        auto hashed = Sha256Text(digest);
        if (!hashed.Succeeded()) { stats.typeReason = "SEARCH_TYPES_MANIFEST_HASH"; return; }
        manifest = next->Manifest(); stats.catalogManifestSha256 = std::move(*hashed.value);
        types = std::make_unique<Trigger::TriggerTypeSystem>(*next);
        stats.typesReady = true; stats.typeReason.clear();
    }

    void Assets(const QtProject::EditorProject& p, bool force) {
        const auto sha = SourceSha(p);
        if (!force && acceptedAssetEpoch == assetEpoch && assetArchive == p.archiveSession_.get()
            && assetDirectory == p.directorySnapshot_.get() && assetSourceSha == sha && assetUnsafe == Unsafe(p)
            && pendingCount == p.pendingImports_.size() && editCount == p.assetEdits_.size()
            && pendingData == p.pendingImports_.data() && editData == p.assetEdits_.data()) { Tick(stats.assetCacheHits); return; }
        Tick(stats.assetPreparations); acceptedAssetEpoch = assetEpoch;
        assetArchive = p.archiveSession_.get(); assetDirectory = p.directorySnapshot_.get(); assetSourceSha = sha; assetUnsafe = Unsafe(p);
        pendingCount = p.pendingImports_.size(); editCount = p.assetEdits_.size(); pendingData = p.pendingImports_.data(); editData = p.assetEdits_.data();
        assets.clear(); files.clear(); fileIds.clear(); filePaths.clear(); sourceAdmitted = false;
        stats.assetState = SearchState::Unavailable; stats.assetReason = "SEARCH_ASSETS_UNAVAILABLE";
        stats.assetInputRows = stats.assetPathBytes = stats.ambiguousAssets = 0;
        if (Unsafe(p) || (!p.archiveSession_ && !p.directorySnapshot_ && p.pendingImports_.empty())) return;
        const auto sourceRows = p.archiveSession_ ? p.archiveSession_->Members().size()
            : p.directorySnapshot_ ? p.directorySnapshot_->Snapshot().files.size() : 0;
        if (!Charge(stats.assetInputRows, sourceRows, options.maxAssetInputRows)
            || !Charge(stats.assetInputRows, p.assetEdits_.size(), options.maxAssetInputRows)
            || !Charge(stats.assetInputRows, p.pendingImports_.size(), options.maxAssetInputRows)) { stats.assetReason = "SEARCH_ASSETS_BUDGET"; return; }
        const auto path = [this](std::string_view text) { return text.size() <= 2048 && Charge(stats.assetPathBytes, text.size(), options.maxAssetPathBytes); };
        const auto qpath = [&path](const QString& text) { return text.size() <= 2048 && path(text.toUtf8().toStdString()); };
        if (p.archiveSession_) {
            for (const auto& member : p.archiveSession_->Members()) if (!path(member.name)) { stats.assetReason = "SEARCH_ASSETS_BUDGET"; return; }
        } else if (p.directorySnapshot_) for (const auto& member : p.directorySnapshot_->Snapshot().files) {
            if (member.relativePath.native().size() > 2048 || !path(IO::GenericPathUtf8(member.relativePath))) { stats.assetReason = "SEARCH_ASSETS_BUDGET"; return; }
        }
        sourceAdmitted = p.archiveSession_ || p.directorySnapshot_; // 所有已捕获源路径已准入；overlay失败不冒完整有效序列。
        for (const auto& edit : p.assetEdits_) if (!qpath(edit.from) || !qpath(edit.to)) { stats.assetReason = "SEARCH_ASSETS_BUDGET"; return; }
        for (const auto& pending : p.pendingImports_) if (!qpath(pending.archivePath) || !qpath(pending.diskPath)) { stats.assetReason = "SEARCH_ASSETS_BUDGET"; return; }
        Tick(stats.assetMergeCalls); assets = p.AssetEntries(4097); // 唯一业务合并；只有完整全域准入后才调用。
        const auto count = (std::min)(assets.size(), std::size_t{4096});
        fileIds.reserve(count); filePaths.reserve(count); files.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& entry = assets[i];
            if (entry.ambiguous) { ++stats.ambiguousAssets; continue; }
            fileIds.push_back((entry.Pending() ? "pending/" + entry.path : entry.originalPath).toUtf8().toStdString());
            filePaths.push_back(entry.path.toUtf8().toStdString());
        }
        for (std::size_t i = 0; i < fileIds.size(); ++i) files.push_back({fileIds[i], filePaths[i]});
        stats.assetState = assets.size() > count || stats.ambiguousAssets || !p.archiveSession_
            || p.archiveSession_->HasOpaqueMembers()
            ? SearchState::Partial : SearchState::Ready;
        stats.assetReason = stats.assetState == SearchState::Ready ? "" : "SEARCH_ASSETS_PARTIAL";
    }

    void Objects(const QtProject::EditorProject& p, bool force) {
        const auto owners = ObjectOwners(p);
        const bool unsafe = Unsafe(p) || !sourceAdmitted;
        bool recordsChanged = false;
        for (std::size_t i = 0; i < 7; ++i) {
            const auto* lni = p.objectDocuments_[i].get();
            const auto* native = p.nativeObjectDocuments_[i].get();
            const auto records = lni ? lni->Objects() : native ? native->Objects() : std::span<const ObjectData::ObjectRecord>{};
            const auto state = lni ? (lni->IsStructurallySafe() ? SearchState::Ready : SearchState::Partial)
                : native ? (native->IsStructurallySafe() ? SearchState::Ready : SearchState::Partial)
                : missing[i] ? SearchState::Missing : SearchState::Unavailable;
            recordsChanged |= records.data() != objects[i].records.data() || records.size() != objects[i].records.size() || state != objects[i].state;
        }
        if (!force && acceptedObjectEpoch == objectEpoch && objectOwners == owners
            && missingArchive == p.archiveSession_.get() && missingSha == SourceSha(p) && missingUnsafe == unsafe && !recordsChanged) {
            Tick(stats.objectCacheHits); return;
        }
        Tick(stats.objectPreparations); objectOwners = owners; acceptedObjectEpoch = objectEpoch;
        if (force || missingArchive != p.archiveSession_.get() || missingSha != SourceSha(p) || missingUnsafe != unsafe) {
            missing.fill(false); missingArchive = p.archiveSession_.get(); missingSha = SourceSha(p); missingUnsafe = unsafe;
            if (!unsafe && p.archiveSession_ && !p.archiveSession_->HasOpaqueMembers()) for (std::size_t i = 0; i < 7; ++i) {
                const auto type = static_cast<ObjectData::ObjectType>(i);
                const std::string native(ObjectData::NativeFileName(type)), lni(ObjectData::LniFileName(type));
                missing[i] = !p.archiveSession_->HasMember(native) && !p.archiveSession_->HasMember("map/" + native)
                    && !p.archiveSession_->HasMember(lni) && !p.archiveSession_->HasMember("table/" + lni);
            }
        }
        for (std::size_t i = 0; i < 7; ++i) {
            auto& source = objects[i]; source = {}; source.type = static_cast<ObjectData::ObjectType>(i);
            if (const auto* document = p.objectDocuments_[i].get()) { source.records = document->Objects(); source.state = document->IsStructurallySafe() ? SearchState::Ready : SearchState::Partial; }
            else if (const auto* native = p.nativeObjectDocuments_[i].get()) { source.records = native->Objects(); source.origin = Services::ProjectSearchObjectOrigin::Native; source.state = native->IsStructurallySafe() ? SearchState::Ready : SearchState::Partial; }
            else if (missing[i]) source.state = SearchState::Missing;
        }
    }

    void Map(const QtProject::EditorProject& p, bool force) {
        const auto session = p.mapEdits_ ? p.mapEdits_->SessionIdentity() : 0;
        const auto rev = p.mapEdits_ ? p.mapEdits_->Revision() : 0;
        const auto sha = SourceSha(p);
        if (!force && mapOwner == p.mapDocument_.get() && mapEdits == p.mapEdits_.get() && mapSessionId == session
            && mapRevision == rev && mapArchive == p.archiveSession_.get() && mapDirectory == p.directorySnapshot_.get()
            && mapSourceSha == sha && mapUnsafe == Unsafe(p)) { Tick(stats.mapCacheHits); return; }
        Tick(stats.mapPreparations); mapOwner = p.mapDocument_.get(); mapEdits = p.mapEdits_.get(); mapSessionId = session; mapRevision = rev;
        mapArchive = p.archiveSession_.get(); mapDirectory = p.directorySnapshot_.get(); mapSourceSha = sha; mapUnsafe = Unsafe(p);
        map.reset(); stats.mapState = SearchState::Unavailable; stats.mapReason = "SEARCH_MAP_UNAVAILABLE";
        stats.mapSnapshotBytesCharged = stats.mapUniqueBytes = 0;
        if (!p.mapDocument_ || Unsafe(p)) return;
        std::vector<MapArchiveMember> merged;
        for (std::size_t i = 0; i < 7; ++i) {
            const auto kind = static_cast<MapDataKind>(i);
            if (p.mapEdits_) {
                Tick(stats.mapSnapshotCalls);
                auto captured = p.mapEdits_->Snapshot(kind, options.maxMapBytes - stats.mapSnapshotBytesCharged);
                if (!captured.Succeeded() || !captured.value) { stats.mapReason = captured.diagnostics.empty() ? "SEARCH_MAP_UNSAFE" : captured.diagnostics.front().code; return; }
                for (const auto& member : captured.value->Members()) for (const auto& source : member.sources)
                    if (!Charge(stats.mapSnapshotBytesCharged, source.data.size(), options.maxMapBytes)) { stats.mapReason = "SEARCH_MAP_BUDGET"; return; }
                const auto& selected = captured.value->Member(kind);
                if (selected.sources.size() > 1) { stats.mapReason = "SEARCH_MAP_UNSAFE"; return; }
                if (!selected.sources.empty()) { stats.mapUniqueBytes += selected.sources.front().data.size(); merged.push_back(selected.sources.front()); }
            } else {
                const auto& selected = p.mapDocument_->Member(kind);
                if (selected.sources.size() > 1 || (selected.sources.empty() && selected.state != MapDataState::Missing)) { stats.mapReason = "SEARCH_MAP_UNSAFE"; return; }
                if (!selected.sources.empty()) {
                    const auto& source = selected.sources.front();
                    if (!Charge(stats.mapSnapshotBytesCharged, source.data.size(), options.maxMapBytes)) { stats.mapReason = "SEARCH_MAP_BUDGET"; return; }
                    stats.mapUniqueBytes += source.data.size(); merged.push_back(source);
                }
            }
        }
        auto owned = std::make_shared<const MapDocument>(MapDocument::FromMembers(merged));
        std::string sourceSha = sha;
        if (!Sha(sourceSha)) {
            // 原创内存IR没有归档/目录身份时，仅绑定这份有界拥有文档；不冒地图磁盘身份。
            std::string digest;
            for (const auto& source : merged) {
                auto hashed = Sha256(source.data);
                if (!hashed.Succeeded()) { stats.mapReason = "SEARCH_MAP_HASH"; return; }
                digest += std::to_string(source.name.size()) + ":" + source.name + ":" + *hashed.value;
            }
            auto hashed = Sha256Text(digest);
            if (!hashed.Succeeded()) { stats.mapReason = "SEARCH_MAP_HASH"; return; }
            sourceSha = std::move(*hashed.value);
        }
        auto captured = Services::CaptureMapDataSnapshot(std::move(owned), {identity, generation, rev, std::move(sourceSha)},
            p.archiveSession_ && p.archiveSession_->HasOpaqueMembers());
        if (!captured.Succeeded() || !captured.value) { stats.mapReason = captured.diagnostics.empty() ? "SEARCH_MAP_CAPTURE" : captured.diagnostics.front().code; return; }
        map = std::make_unique<Services::MapDataQuerySnapshot>(std::move(*captured.value));
        stats.mapState = SearchState::Ready; stats.mapReason.clear();
    }
};

ProjectSearchCapture::ProjectSearchCapture() : state_(std::make_unique<State>()) {}
ProjectSearchCapture::~ProjectSearchCapture() = default;
void ProjectSearchCapture::Reset() { state_ = std::make_unique<State>(); }
const ProjectSearchCaptureStats& ProjectSearchCapture::Stats() const { return state_->stats; }
std::span<const QtProject::AssetEntry> ProjectSearchCapture::Assets() const {
    return state_->owner && state_->owner->thread() == QThread::currentThread() && state_->Current(*state_->owner)
        ? std::span<const QtProject::AssetEntry>(state_->assets) : std::span<const QtProject::AssetEntry>{};
}

Status ProjectSearchCapture::Prepare(const QtProject::EditorProject& project, std::string projectIdentity,
    std::uint64_t openedGeneration, std::string contextRevision, const ProjectSearchCaptureOptions& options) {
    if (!Valid(options) || !Identity(projectIdentity) || openedGeneration == 0 || contextRevision.empty()
        || contextRevision.size() > 1024 || contextRevision.find('\0') != std::string::npos)
        return Status::Failure("Qt.Search.Capture", "SEARCH_CAPTURE_OPTIONS", "搜索准备身份或预算不在允许范围内。");
    if (project.thread() != QThread::currentThread()) return Status::Failure("Qt.Search.Capture", "SEARCH_CAPTURE_THREAD", "搜索准备必须在文档所属线程同步进行。");
    const bool replaced = !state_->owner || state_->owner.data() != &project || state_->identity != projectIdentity
        || state_->generation != openedGeneration || state_->actualGeneration != project.editRevision_.Generation();
    if (replaced) { Reset(); state_->Attach(project); }
    auto& s = *state_; Tick(s.stats.prepareCalls);
    if (s.serial == (std::numeric_limits<std::uint64_t>::max)()) {
        s.revision.clear();
        return Status::Failure("Qt.Search.Capture", "SEARCH_CAPTURE_EPOCH_EXHAUSTED", "搜索准备修订已耗尽，不能沿用旧借用值。");
    }
    // 分配中止后不能将已设置的部分域键当作完整缓存命中。
    const bool force = replaced || s.options != options || s.revision.empty();
    s.identity = std::move(projectIdentity); s.generation = openedGeneration; s.actualGeneration = project.editRevision_.Generation(); s.options = options;
    s.revision.clear();
    try {
        s.Types(project, force); s.Assets(project, force); s.Objects(project, force); s.Map(project, force);
        s.acceptedSerial = s.serial; s.revision = std::move(contextRevision);
        return Status::Ok();
    } catch (const std::bad_alloc&) {
        return Status::Failure("Qt.Search.Capture", "SEARCH_CAPTURE_ALLOCATION", "无法分配有界搜索准备；未补读磁盘。");
    } catch (const std::length_error&) {
        return Status::Failure("Qt.Search.Capture", "SEARCH_CAPTURE_LENGTH", "有界搜索准备容器长度无效；未补读磁盘。");
    }
}
Result<Services::ProjectSearchContext> ProjectSearchCapture::Borrow(const QtProject::EditorProject& project,
    std::string_view expectedRevision) const {
    if (project.thread() != QThread::currentThread() || expectedRevision != state_->revision || !state_->Current(project))
        return Result<Services::ProjectSearchContext>::Failure("Qt.Search.Capture", "SEARCH_CAPTURE_STALE", "搜索准备已过期或尚未准备，未借用旧域数据。");
    Services::ProjectSearchContext context; context.revision = state_->revision;
    context.types = state_->types.get(); context.objects = state_->objects;
    context.currentFiles = state_->files; context.usesImportedFiles = true; context.importState = state_->stats.assetState;
    context.mapData = state_->map.get();
    if (context.mapData) {
        context.mapDataBinding = context.mapData->Binding();
        const auto document = context.mapData->OwnedDocument();
        if (document && document->Strings()) context.strings = &*document->Strings();
    }
    return Result<Services::ProjectSearchContext>::Success(std::move(context));
}
} // namespace War3Map::QtApp
