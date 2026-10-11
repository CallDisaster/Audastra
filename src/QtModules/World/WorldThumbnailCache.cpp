#include "WorldThumbnailCache.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QLibrary>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <deque>
#include <exception>
#include <list>

namespace War3Map::Modules {
namespace {
constexpr std::size_t kImageBytes = 16 * 1024 * 1024;
QString Issue(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return QObject::tr("模型缩略图不可用");
    return QString::fromStdString(diagnostics.front().code + ": " + diagnostics.front().message).left(1024);
}
QByteArray Path(const std::filesystem::path& value) {
    const auto text = value.generic_u8string();
    return QByteArray(reinterpret_cast<const char*>(text.data()), static_cast<qsizetype>(text.size()));
}
void Field(QByteArray& bytes, const QByteArray& value) {
    bytes += QByteArray::number(value.size()); bytes += ':'; bytes += value;
}
QString Digest(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
QString SourceKey(const WorldTerrainSources& sources) {
    QByteArray bytes;
    Field(bytes, Path(sources.archivePath)); Field(bytes, Path(sources.gameRoot));
    Field(bytes, QByteArray::fromStdString(sources.archiveSha));
    for (const auto& entry : sources.entries) {
        Field(bytes, QByteArray::fromStdString(entry.path)); Field(bytes, QByteArray::fromStdString(entry.archivePath));
        Field(bytes, Path(entry.diskPath));
    }
    return Digest(bytes);
}
QString VariantKey(const Render::ModelInstanceRequest& request) {
    QByteArray bytes;
    Field(bytes, QByteArray::fromStdString(Render::AnimationPathKey(request.modelPath)));
    Field(bytes, QByteArray::fromStdString(Render::AnimationPathKey(request.replaceableTexture)));
    Field(bytes, QByteArray::number(request.teamColor));
    return Digest(bytes);
}
struct Resident {
    const War3RenderPluginApiV2* api = nullptr;
    War3RendererHandle handle = nullptr;
    QString key;
    int pixels = 0;
    std::uint64_t viewRevision = 0, staticUploads = 0;
    std::vector<War3RenderCore::AssetSource> sources;
    std::shared_ptr<Render::ModelAnimationService> service;
    std::shared_ptr<const Render::AnimationFrame> frame;
    std::unique_ptr<Render::AnimationRenderer> renderer;
    ~Resident() { renderer.reset(); if (handle) api->destroyRenderer(handle); }
};
struct Worker {
    const War3RenderPluginApiV2* api = nullptr;
    QString pluginPath;
    bool loadProductionBackend = false, attempted = false;
    std::unique_ptr<QLibrary> library;
    QString source;
    bool changed = false;
    std::unique_ptr<WorldResourceReader> reader;
    std::list<std::unique_ptr<Resident>> residents;
    WorldThumbnailResult Capture(const WorldTerrainSources& sources, const QString& sourceKey,
        const Render::ModelInstanceRequest& request, int pixels, std::optional<WorldThumbnailResult> cached) {
        WorldThumbnailResult result;
        const auto fail = [&](QString issue) { result.issue = std::move(issue); return result; };
        if (!api && loadProductionBackend && !attempted) {
            attempted = true; library = std::make_unique<QLibrary>(pluginPath);
            library->setLoadHints(QLibrary::ResolveAllSymbolsHint);
            if (library->load()) {
                const auto getApi = reinterpret_cast<War3GetRenderPluginApiV2>(library->resolve("War3MapEditor_GetRenderPluginApiV2"));
                if (getApi) api = getApi(WAR3_RENDER_PLUGIN_API_V2);
            }
        }
        if (!Render::AnimationRenderer::Supported(api) || !api->createRenderer || !api->destroyRenderer
            || !api->resize || !api->setCamera || !api->submitMesh)
            return fail(QObject::tr("当前后端不支持真实模型完成帧"));
        if (source != sourceKey) { residents.clear(); reader.reset(); source = sourceKey; changed = false; }
        if (changed) return fail(QObject::tr("资源来源已变化，请重新打开地图后重试"));
        if (!reader) {
            auto opened = WorldResourceReader::Open(sources);
            if (!opened.Succeeded()) return fail(Issue(opened.diagnostics));
            reader = std::move(*opened.value);
        }
        auto valid = reader->VerifyCurrentSource();
        if (!valid.Succeeded()) { changed = true; residents.clear(); return fail(Issue(valid.diagnostics)); }
        if (cached) return std::move(*cached); // 缓存命中也经同一来源租约验证。
        const auto variant = VariantKey(request);
        auto found = std::find_if(residents.begin(), residents.end(), [&](const auto& value) { return value->key == variant; });
        if (found == residents.end()) {
            if (residents.size() == 4) residents.pop_back();
            auto resident = std::make_unique<Resident>(); resident->api = api; resident->key = variant;
            resident->viewRevision = Render::NextAnimationSerial();
            Render::AnimationInstanceSpec spec; spec.identity = {1, resident->viewRevision};
            spec.hostKey = variant.toStdString(); spec.model = request; spec.action = "Stand"; spec.elapsedMs = 0;
            auto limits = Render::InteractiveAnimationPreviewLimits();
            limits.maxInstances = 1; limits.maxAssetBytes = 16 * 1024 * 1024;
            limits.maxFrameBytes = 8 * 1024 * 1024; limits.geometry.maxModels = limits.geometry.maxInstances = 1;
            limits.geometry.maxVertices = 262144; limits.geometry.maxIndices = 786432;
            limits.geometry.maxTextures = 64; limits.geometry.maxTextureDimension = 256;
            limits.geometry.maxTotalTextureBytes = 16 * 1024 * 1024;
            auto service = Render::ModelAnimationService::Create({resident->viewRevision, resident->viewRevision, sourceKey.toStdString()},
                std::span(&spec, 1), [&](std::string_view path, std::uint64_t maximum) {
                    auto resource = reader->ReadAnimationResource(path, maximum);
                    if (resource.Succeeded()) {
                        War3RenderCore::AssetSource provenance{std::string(path), resource.value->sourceLayer, resource.value->digest};
                        if (std::find(resident->sources.begin(), resident->sources.end(), provenance) == resident->sources.end()) resident->sources.push_back(std::move(provenance));
                    }
                    return resource;
                }, limits);
            if (!service.Succeeded()) return fail(Issue(service.diagnostics));
            resident->service = std::move(*service.value);
            const auto scene = resident->service->Scene();
            if (scene->geometry.instances.empty() || scene->geometry.untexturedLayers != 0)
                return fail(QObject::tr("模型或材质资源缺失，未用图标或无贴图模型替代缩略图"));
            const War3RenderCreateInfoV2 info{sizeof(info), static_cast<std::uint32_t>(pixels), static_cast<std::uint32_t>(pixels), 0};
            resident->handle = api->createRenderer(&info);
            if (!resident->handle) return fail(QObject::tr("无法创建模型缩略图 renderer"));
            resident->pixels = pixels; resident->renderer = std::make_unique<Render::AnimationRenderer>(api, resident->handle);
            const War3RenderMeshV2 empty{sizeof(empty), 0, 0, resident->viewRevision, nullptr, nullptr};
            if (api->submitMesh(resident->handle, &empty) != 0) return fail(QObject::tr("后端拒绝缩略图视图身份"));
            residents.push_front(std::move(resident)); found = residents.begin();
        }
        residents.splice(residents.begin(), residents, found);
        auto& resident = *residents.front();
        if (resident.pixels != pixels) {
            if (api->resize(resident.handle, pixels, pixels) != 0) return fail(QObject::tr("后端拒绝缩略图尺寸"));
            resident.pixels = pixels;
        }
        const auto scene = resident.service->Scene();
        // 只复制包围盒与单实例变换取景，缩放/染色不复制或重传静态资产。
        Render::AnimatedModelScene framing;
        framing.geometry.modelBounds = scene->geometry.modelBounds;
        framing.geometry.instances = scene->geometry.instances;
        const auto transform = Render::ModelInstanceTransform(request);
        std::copy(transform.begin(), transform.end(), framing.geometry.instances.front().transform);
        auto camera = Render::FitAnimationSceneCamera(framing);
        if (api->setCamera(resident.handle, &camera) != 0) return fail(QObject::tr("后端拒绝缩略图相机"));
        Render::AnimationAdvance advance; advance.worldCamera = Render::AnimationWorldCamera(camera);
        Render::AnimationEdit edit; edit.identity = scene->specs.front().identity;
        edit.transform = Render::ModelInstanceTransform(request); edit.tint = request.tint;
        edit.elapsedMs = 0; edit.globalMs = 0; edit.playing = false; advance.edits.push_back(edit);
        auto prepared = resident.service->Prepare(resident.frame.get(), advance);
        if (!prepared.Succeeded()) return fail(Issue(prepared.diagnostics));
        auto frame = std::move(*prepared.value);
        if (frame->poses.empty() || frame->hiddenInstances || !frame->poses.front().animated)
            return fail(QObject::tr("Stand 姿势不可求值，未发布静态替代帧"));
        const auto submitted = resident.frame ? resident.renderer->Update(frame) : resident.renderer->Submit(scene, frame);
        if (!submitted.Succeeded()) return fail(Issue(submitted.diagnostics));
        // CPU接收后成为本renderer的目标；捕获失败也不把下次误当首次静态上传。
        resident.frame = frame;
        std::vector<std::byte> rgba(static_cast<std::size_t>(pixels) * pixels * 4);
        auto captured = resident.renderer->Capture(rgba, resident.viewRevision);
        if (!captured.Succeeded()) return fail(Issue(captured.diagnostics));
        const auto& actual = captured.value->frame;
        if (actual.capture.width != static_cast<std::uint32_t>(pixels) || actual.capture.height != static_cast<std::uint32_t>(pixels)
            || actual.camera.targetX != camera.targetX || actual.camera.targetY != camera.targetY || actual.camera.targetZ != camera.targetZ
            || actual.camera.distance != camera.distance || actual.camera.yawRadians != camera.yawRadians || actual.camera.pitchRadians != camera.pitchRadians)
            return fail(QObject::tr("完成帧尺寸或相机不属于缩略图请求"));
        if (resident.staticUploads && resident.staticUploads != captured.value->skin.staticUploadCount)
            return fail(QObject::tr("动态缩略图更新触发了静态资源重传"));
        resident.staticUploads = captured.value->skin.staticUploadCount;
        valid = reader->VerifyCurrentSource();
        if (!valid.Succeeded()) { changed = true; residents.clear(); return fail(Issue(valid.diagnostics)); }
        result.image = QImage(reinterpret_cast<const uchar*>(rgba.data()), pixels, pixels, pixels * 4, QImage::Format_RGBA8888).copy();
        result.imageSha256 = Digest(QByteArray(reinterpret_cast<const char*>(rgba.data()), static_cast<qsizetype>(rgba.size()))).toStdString();
        result.receipt = std::move(*captured.value); result.sources = resident.sources; result.poses = frame->poses;
        return result;
    }
};
} // namespace

struct WorldThumbnailCache::State {
    struct Job { QString key; Render::ModelInstanceRequest request; int pixels; std::optional<WorldThumbnailResult> cached; };
    struct Cached { QString key; WorldThumbnailResult result; };
    QThread thread;
    QObject* owner = nullptr;
    std::shared_ptr<Worker> worker;
    WorldAssetBinding binding;
    std::shared_ptr<const WorldTerrainSources> sources;
    QString sourceKey, issue;
    std::function<bool()> current;
    std::uint64_t serial = 0, activeSerial = 0;
    bool busy = false;
    QString activeKey;
    std::deque<Job> pending;
    mutable std::list<Cached> images;
    std::size_t bytes = 0;
    void Store(QString key, WorldThumbnailResult result) {
        bytes += static_cast<std::size_t>(result.image.sizeInBytes());
        images.push_front({std::move(key), std::move(result)});
        while (images.size() > 256 || bytes > kImageBytes) {
            bytes -= static_cast<std::size_t>(images.back().result.image.sizeInBytes()); images.pop_back();
        }
    }
};
WorldThumbnailCache::WorldThumbnailCache(const War3RenderPluginApiV2* backend, QObject* parent, bool loadProductionBackend)
    : QObject(parent), state_(std::make_unique<State>()) {
    state_->worker = std::make_shared<Worker>(); state_->worker->api = backend;
    state_->worker->loadProductionBackend = loadProductionBackend;
    state_->worker->pluginPath = QCoreApplication::applicationDirPath() + QStringLiteral("/war3-render-d3d11.dll");
    state_->owner = new QObject; state_->owner->moveToThread(&state_->thread);
    connect(&state_->thread, &QThread::finished, state_->owner, &QObject::deleteLater);
    state_->thread.start(QThread::LowPriority);
}
WorldThumbnailCache::~WorldThumbnailCache() {
    Clear(); auto worker = state_->worker;
    QMetaObject::invokeMethod(state_->owner, [worker] {
        worker->residents.clear(); worker->reader.reset(); worker->library.reset(); worker->api = nullptr;
    }, Qt::BlockingQueuedConnection);
    state_->thread.quit(); state_->thread.wait();
}
void WorldThumbnailCache::Clear() {
    ++state_->serial; state_->pending.clear(); state_->images.clear(); state_->bytes = 0;
    state_->sources.reset(); state_->current = {}; state_->sourceKey.clear(); state_->issue.clear();
    auto worker = state_->worker;
    QMetaObject::invokeMethod(state_->owner, [worker] { worker->residents.clear(); worker->reader.reset(); worker->source.clear(); worker->changed = false; }, Qt::QueuedConnection);
}
void WorldThumbnailCache::SetContext(WorldAssetBinding binding, Result<WorldTerrainSources> sources, std::function<bool()> current) {
    ++state_->serial; state_->pending.clear(); state_->images.clear(); state_->bytes = 0;
    state_->sources.reset(); state_->sourceKey.clear(); state_->issue.clear();
    state_->binding = std::move(binding); state_->current = std::move(current);
    if (!sources.Succeeded()) { state_->issue = Issue(sources.diagnostics); return; }
    std::size_t bytes = 0;
    for (const auto& entry : sources.value->entries) bytes += entry.path.size() + entry.archivePath.size() + Path(entry.diskPath).size();
    if (sources.value->entries.size() > 4096 || bytes > 1024 * 1024) {
        state_->issue = tr("地图资源来源超过完整索引预算，缩略图不可用（未截断后回退游戏资源）"); return;
    }
    state_->sourceKey = SourceKey(*sources.value) + ":" + QString::number(state_->binding.generation);
    state_->sources = std::make_shared<WorldTerrainSources>(std::move(*sources.value));
}
QString WorldThumbnailCache::Request(Render::ModelInstanceRequest request, int pixels) {
    request.position = {}; request.angle = 0; request.previewGhost = false;
    QByteArray identity; Field(identity, state_->binding.mapKey.toUtf8());
    Field(identity, QByteArray::number(state_->binding.generation)); Field(identity, QByteArray::number(state_->binding.revision));
    Field(identity, state_->sourceKey.toLatin1()); Field(identity, VariantKey(request).toLatin1());
    for (auto scale : request.scale) Field(identity, QByteArray::number(scale, 'g', 9));
    for (auto tint : request.tint) Field(identity, QByteArray::number(tint, 'g', 9));
    Field(identity, QByteArray::number(pixels)); Field(identity, "Stand:0:0"); const auto key = Digest(identity);
    if ((state_->activeKey == key && state_->activeSerial == state_->serial)
        || std::any_of(state_->pending.begin(), state_->pending.end(), [&](const auto& job) { return job.key == key; })) return key;
    QString issue = state_->issue;
    if (!state_->current || !state_->current()) issue = tr("资源浏览器绑定已过期");
    if ((pixels != 128 && pixels != 256) || request.modelPath.empty() || request.modelPath.size() > 4096
        || request.replaceableTexture.size() > 4096 || request.teamColor > 15
        || std::any_of(request.scale.begin(), request.scale.end(), [](float v) { return !std::isfinite(v) || v <= 0 || v > 1000; })
        || std::any_of(request.tint.begin(), request.tint.end(), [](float v) { return !std::isfinite(v) || v < 0 || v > 1; }))
        issue = tr("模型缩略图请求超出有界参数范围");
    State::Job job{key, std::move(request), pixels, {}};
    const auto old = std::find_if(state_->images.begin(), state_->images.end(), [&](const auto& value) { return value.key == key; });
    if (old != state_->images.end()) {
        state_->bytes -= static_cast<std::size_t>(old->result.image.sizeInBytes());
        job.cached = std::move(old->result); state_->images.erase(old);
    }
    if (state_->pending.size() >= 32) issue = tr("缩略图队列已满，请滚动或重新选择后重试");
    if (!issue.isEmpty() || !state_->sources) {
        WorldThumbnailResult result; result.issue = issue.isEmpty() ? tr("地图资源来源不可用") : issue;
        state_->Store(key, std::move(result)); QTimer::singleShot(0, this, [this, key] { emit Ready(key); }); return key;
    }
    state_->pending.push_back(std::move(job)); StartNext(); return key;
}
std::optional<WorldThumbnailResult> WorldThumbnailCache::Lookup(const QString& key) const {
    if (!state_->current || !state_->current()) return std::nullopt;
    const auto found = std::find_if(state_->images.begin(), state_->images.end(), [&](const auto& value) { return value.key == key; });
    if (found == state_->images.end()) return std::nullopt;
    state_->images.splice(state_->images.begin(), state_->images, found); return state_->images.front().result;
}
void WorldThumbnailCache::StartNext() {
    if (state_->busy || state_->pending.empty() || !state_->sources) return;
    auto job = std::move(state_->pending.front()); state_->pending.pop_front(); state_->busy = true; state_->activeKey = job.key;
    const auto serial = state_->serial; auto sources = state_->sources; auto sourceKey = state_->sourceKey;
    state_->activeSerial = serial;
    auto worker = state_->worker; QPointer<WorldThumbnailCache> self(this);
    QMetaObject::invokeMethod(state_->owner, [worker, self, serial, sources, sourceKey, job = std::move(job)]() mutable {
        WorldThumbnailResult result;
        try { result = worker->Capture(*sources, sourceKey, job.request, job.pixels, std::move(job.cached)); }
        catch (const std::exception& error) { result.issue = QString::fromUtf8(error.what()).left(1024); }
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, serial, key = job.key, result = std::move(result)]() mutable {
            if (!self) return;
            auto& state = *self->state_; state.busy = false; state.activeKey.clear();
            if (state.serial == serial && state.current && state.current()) { state.Store(key, std::move(result)); emit self->Ready(key); }
            self->StartNext();
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
} // namespace War3Map::Modules
