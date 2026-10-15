#include "WorldBuildingSplatPresenter.h"
#include "WorldViewport.h"
#include "WorldDiagnosticJson.h"
#include "War3MapCore/Hash.h"

#include <QEvent>
#include <QVariant>
#include <QJsonArray>

#include <algorithm>
#include <exception>
#include <set>

namespace War3Map::Modules {
namespace {
bool SameSources(const WorldTerrainSources& a, const WorldTerrainSources& b) {
    return a.archivePath == b.archivePath && a.archiveSha == b.archiveSha && a.gameRoot == b.gameRoot
        && a.mainTileset == b.mainTileset && a.entries.size() == b.entries.size()
        && std::equal(a.entries.begin(), a.entries.end(), b.entries.begin(), [](const auto& x, const auto& y) {
            return x.path == y.path && x.archivePath == y.archivePath && x.diskPath == y.diskPath;
        });
}
QString FailureText(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.empty() ? QObject::tr("建筑贴画准备失败，原层保留；未提交新层")
        : QString::fromUtf8(diagnostics.front().code + "：" + diagnostics.front().message);
}
void IdentityPart(std::string& identity, std::string_view value) {
    identity += std::to_string(value.size()) + ":" + std::string(value);
}
std::size_t NotApplicableCount(const WorldBuildingSplatCapture* capture) {
    if (!capture) return 0;
    return static_cast<std::size_t>(std::count_if(capture->units.begin(), capture->units.end(), [](const auto& input) {
        return input.fields.configuration == Services::BuildingSplatConfiguration::NotApplicableToNonBuilding;
    }));
}
}
struct WorldBuildingSplatPresenter::Cache {
    std::shared_ptr<WorldResourceReader> reader;
    std::shared_ptr<const Services::BuildingSplatCatalog> catalog;
    std::map<std::string, Render::BuildingSplatPreviewTexture> textures;
    std::set<std::string> failedTextures, budgetTextures, assetPaths;
    std::vector<Diagnostic> resourceIssues;
    std::optional<Render::BuildingSplatPreviewAssets> assets;
    std::shared_ptr<const Services::WorldInstanceRegistry> fallbackRegistry;
    std::optional<Services::WorldInstanceBinding> fallbackBinding;
};
struct WorldBuildingSplatPresenter::Work {
    std::shared_ptr<Cache> cache;
    std::optional<Render::BuildingSplatPreviewFrame> prepared;
    std::vector<Diagnostic> diagnostics;
    bool sourceRejected = false;
    std::size_t hidden = 0;
};
WorldBuildingSplatPresenter::WorldBuildingSplatPresenter(WorldViewport& viewport, QObject* parent)
    : QObject(parent), viewport_(&viewport), layerId_(Render::NextAnimationSerial()) {
    viewport.installEventFilter(this);
    parentTimer_.setInterval(250);
    connect(&parentTimer_, &QTimer::timeout, this, &WorldBuildingSplatPresenter::CheckParent);
    connect(&viewport, &WorldViewport::StatusChanged, this, [this] {
        if (needsSubmit_) BeginNext();
    }, Qt::QueuedConnection);
}
WorldBuildingSplatPresenter::~WorldBuildingSplatPresenter() {
    parentTimer_.stop();
    if (worker_) disconnect(worker_, nullptr, this, nullptr);
    if (viewport_) { viewport_->removeEventFilter(this); (void)viewport_->RemoveModelLayer(layerId_); }
    // worker只持owned输入/独立reader，结束自删；不能等待在GUI线程。
}
void WorldBuildingSplatPresenter::Report(QString text) {
    if (lastMessage_ != text) { lastMessage_ = std::move(text); emit Message(lastMessage_); }
}
void WorldBuildingSplatPresenter::SetSource(WorldTerrainSources sources, Render::AnimationSession session,
    std::function<bool()> sourceCurrent) {
    if (sources_ && SameSources(*sources_, sources) && session_.sourceIdentity == session.sourceIdentity) {
        sourceCurrent_ = std::move(sourceCurrent); return;
    }
    Clear(); sources_ = std::move(sources); session_ = std::move(session);
    sourceCurrent_ = std::move(sourceCurrent);
    observedParent_ = viewport_ ? viewport_->MeasurementModelEpoch() : 0;
}
void WorldBuildingSplatPresenter::Show(WorldBuildingSplatCapture capture, std::vector<std::uint8_t> visible,
    std::shared_ptr<const MapDocument> document,
    std::shared_ptr<const Services::WorldInstanceRegistry> registry, Services::WorldInstanceBinding fallbackBinding,
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain, std::string terrainIdentity,
    std::uint64_t terrainRevision, std::function<bool()> current) {
    if (visible.size() != capture.units.size() || (!document && !registry) || !terrain || !current) {
        Report(tr("建筑贴画输入、显隐或显示地形身份不完整；未提交新层")); return;
    }
    std::string identity; IdentityPart(identity, capture.inputIdentity); IdentityPart(identity, terrainIdentity);
    IdentityPart(identity, std::to_string(terrainRevision));
    // registry来源代次与真实映射参与身份；fallback由worker独立Create，绝不冒称主registry。
    const auto& binding = registry ? registry->Binding() : fallbackBinding;
    IdentityPart(identity, std::to_string(binding.sourceGeneration)); IdentityPart(identity, std::to_string(binding.editSessionIdentity));
    IdentityPart(identity, std::to_string(binding.mapRevision)); IdentityPart(identity, std::to_string(binding.editRevision));
    IdentityPart(identity, binding.sourceIdentity);
    if (registry) for (const auto& input : capture.units) {
        const auto id = registry->Find(input.owner);
        IdentityPart(identity, id ? std::to_string(id->value) : "unbound");
        IdentityPart(identity, id ? std::to_string(id->generation) : "unbound");
    }
    auto geometryHash = Sha256Text(identity);
    if (!geometryHash.Succeeded()) { Report(FailureText(geometryHash.diagnostics)); return; }
    for (const auto value : visible) IdentityPart(identity, std::to_string(value));
    auto hashed = Sha256Text(identity);
    if (!hashed.Succeeded()) { Report(FailureText(hashed.diagnostics)); return; }
    current_ = std::move(current);
    if (desiredIdentity_ == *hashed.value && terrain_ == terrain) { UpdateTimer(); BeginNext(); return; }
    desiredIdentity_ = std::move(*hashed.value);
    geometryIdentity_ = std::move(*geometryHash.value);
    capture_ = std::make_shared<const WorldBuildingSplatCapture>(std::move(capture)); visible_ = std::move(visible);
    notApplicableCount_ = NotApplicableCount(capture_.get());
    document_ = std::move(document); registry_ = std::move(registry); fallbackBinding_ = std::move(fallbackBinding);
    terrain_ = std::move(terrain); terrainIdentity_ = std::move(terrainIdentity); terrainRevision_ = terrainRevision;
    ++requestSerial_; UpdateTimer(); BeginNext();
}
void WorldBuildingSplatPresenter::Clear() {
    ++sourceSerial_; ++requestSerial_; ++bindingSerial_; parentTimer_.stop(); RemoveLayer();
    sources_.reset(); session_ = {}; sourceCurrent_ = {}; current_ = {}; cache_.reset();
    capture_.reset(); notApplicableCount_ = 0; visible_.clear(); document_.reset(); registry_.reset(); terrain_.reset();
    terrainIdentity_.clear(); desiredIdentity_.clear(); geometryIdentity_.clear(); preparedGeometryIdentity_.clear();
    prepared_.reset(); failed_.reset(); lastMessage_.clear(); hiddenPrepared_ = 0;
    needsSubmit_ = false; preparedRequestSerial_ = 0; observedParent_ = 0;
}
void WorldBuildingSplatPresenter::RemoveLayer() {
    if (submissionDeferred_) { removalPending_ = true; return; }
    removalPending_ = false;
    if (viewport_ && submitted_) {
        const auto removed = viewport_->RemoveModelLayer(layerId_);
        if (!removed.Succeeded()) Report(FailureText(removed.diagnostics));
    }
    submitted_ = false; installed_.reset();
}
void WorldBuildingSplatPresenter::RebindParent() {
    ++bindingSerial_; observedParent_ = viewport_ ? viewport_->MeasurementModelEpoch() : 0;
    submitted_ = false; installed_.reset(); removalPending_ = false;
    needsSubmit_ = capture_ != nullptr; UpdateTimer(); BeginNext();
}
void WorldBuildingSplatPresenter::SetSubmissionDeferred(bool deferred) {
    if (submissionDeferred_ == deferred) return;
    submissionDeferred_ = deferred;
    if (deferred) { parentTimer_.stop(); return; }
    if (removalPending_) RemoveLayer();
    UpdateTimer(); BeginNext();
}
WorldBuildingSplatPresenter::Signature WorldBuildingSplatPresenter::CurrentSignature() const {
    return {sourceSerial_, requestSerial_, bindingSerial_, viewport_ ? viewport_->MeasurementModelEpoch() : 0};
}
void WorldBuildingSplatPresenter::UpdateTimer() {
    if (!submissionDeferred_ && viewport_ && viewport_->isVisible() && !viewport_->RendererFailed() && sources_ && capture_)
        parentTimer_.start();
    else parentTimer_.stop();
}
void WorldBuildingSplatPresenter::CheckParent() {
    if (!viewport_ || !viewport_->isVisible()) { parentTimer_.stop(); return; }
    if (sourceCurrent_ && !sourceCurrent_()) { Clear(); return; }
    if (viewport_->MeasurementModelEpoch() != observedParent_) { RebindParent(); return; }
    BeginNext(); // 同签名/失败签名立即返回，不读资源、不重算、不重提交。
}
bool WorldBuildingSplatPresenter::eventFilter(QObject* watched, QEvent* event) {
    if (watched == viewport_) {
        if (event->type() == QEvent::Hide) { parentTimer_.stop(); needsSubmit_ = capture_ != nullptr; RemoveLayer(); }
        else if (event->type() == QEvent::Show)
            QTimer::singleShot(0, this, [this] { if (viewport_ && viewport_->isVisible()) RebindParent(); });
    }
    return QObject::eventFilter(watched, event);
}
void WorldBuildingSplatPresenter::BeginNext() {
    if (submissionDeferred_ || worker_ || !viewport_ || !viewport_->isVisible() || !sources_ || !capture_
        || !sourceCurrent_ || !current_) return;
    if (!sourceCurrent_()) { Clear(); return; }
    if (!current_() || !viewport_->RendererReady()) return;
    if (!viewport_->MeasurementModelEpoch()) {
        Report(tr("建筑贴画待主模型场景身份就绪；未用parent 0提交辅助层")); return;
    }
    const auto signature = CurrentSignature();
    if (failed_ && *failed_ == signature) return;
    if (!viewport_->HasModelLayerSupport()) {
        failed_ = signature; Report(tr("渲染插件不支持建筑贴画所需的通用辅助层")); return;
    }
    if (prepared_ && preparedRequestSerial_ == requestSerial_) {
        if (needsSubmit_ || !installed_ || prepared_->frame != installed_->frame) SubmitPrepared(signature);
        return;
    }
    auto work = std::make_shared<Work>();
    // 独占worker对reader的实际读取；复制缓存容器，旧accepted帧/素材owner继续独立保活。
    work->cache = cache_ ? std::make_shared<Cache>(*cache_) : std::make_shared<Cache>();
    const auto sources = *sources_; const auto session = session_; const auto capture = capture_;
    const auto visible = visible_; const auto document = document_; const auto registry = registry_;
    const auto fallbackBinding = fallbackBinding_; const auto terrain = terrain_;
    const auto terrainIdentity = terrainIdentity_; const auto terrainRevision = terrainRevision_;
    const auto previous = prepared_; const auto content = Render::NextAnimationSerial();
    const auto geometryIdentity = geometryIdentity_;
    const bool visibilityOnly = previous && preparedGeometryIdentity_ == geometryIdentity;
    auto* worker = QThread::create([work, sources, session, capture, visible, document, registry, fallbackBinding,
        terrain, terrainIdentity, terrainRevision, previous, content, visibilityOnly] {
        try {
            auto& cache = *work->cache;
            const auto applyVisibility = [&](Render::BuildingSplatPreviewFrame& output) {
                auto frame = std::make_shared<Render::AnimationFrame>(*output.frame);
                frame->contentVersion = content;
                for (const auto& owner : output.owners) {
                    if (owner.inputIndex >= visible.size() || owner.renderedInstance >= frame->instances.size())
                        return Status::Failure("World.BuildingSplat", "BUILDING_SPLAT_OWNER_STALE", "贴画显隐owner与owned frame不符。");
                    const auto alpha = frame->instances[owner.renderedInstance].firstAlpha;
                    if (alpha >= frame->alphas.size())
                        return Status::Failure("World.BuildingSplat", "BUILDING_SPLAT_OWNER_STALE", "贴画显隐alpha片段无效。");
                    // 建筑贴画服务已确认建筑alpha255；隐藏只修改动态GEOA片段，模型/材质布局不变。
                    frame->alphas[alpha] = visible[static_cast<std::size_t>(owner.inputIndex)] ? 1.0f : 0.0f;
                    if (!visible[static_cast<std::size_t>(owner.inputIndex)]) ++work->hidden;
                }
                output.binding.version = capture->version; output.frame = std::move(frame);
                return Status::Ok();
            };
            if (visibilityOnly && previous && previous->frame && cache.reader) {
                const auto valid = cache.reader->VerifyCurrentSource();
                if (!valid.Succeeded()) { work->diagnostics = valid.diagnostics; work->sourceRejected = true; return; }
                auto frame = *previous;
                const auto visibility = applyVisibility(frame);
                if (!visibility.Succeeded()) { work->diagnostics = visibility.diagnostics; return; }
                work->prepared = std::move(frame); return;
            }
            if (!cache.reader) {
                auto reader = WorldResourceReader::Open(sources);
                if (!reader.Succeeded()) { work->diagnostics = std::move(reader.diagnostics); work->sourceRejected = true; return; }
                cache.reader = std::shared_ptr<WorldResourceReader>(std::move(*reader.value));
            }
            const auto valid = cache.reader->VerifyCurrentSource();
            if (!valid.Succeeded()) { work->diagnostics = valid.diagnostics; work->sourceRejected = true; return; }
            if (!cache.catalog) {
                constexpr auto path = "Splats\\UberSplatData.slk";
                auto location = cache.reader->Resolver().Locate(path);
                auto resource = cache.reader->ReadAnimationResource(path, 2ull * 1024 * 1024);
                if (!location.Succeeded() || !resource.Succeeded()) {
                    work->diagnostics = location.Succeeded() ? std::move(resource.diagnostics) : std::move(location.diagnostics); return;
                }
                // 原始byte owned且真实解析器命中来源；Game具体归档来源保留，不借旁边安装。
                location.value->sourceId = resource.value->sourceLayer;
                auto catalog = Services::ParseBuildingSplatCatalog({std::move(*location.value), std::move(resource.value->bytes)});
                if (!catalog.Succeeded()) { work->diagnostics = std::move(catalog.diagnostics); return; }
                cache.catalog = std::move(*catalog.value);
            }
            auto effectiveRegistry = registry;
            if (!effectiveRegistry) {
                if (!cache.fallbackRegistry || cache.fallbackBinding != fallbackBinding) {
                    auto created = Services::WorldInstanceRegistry::Create(*document, fallbackBinding, {}, {});
                    if (!created.Succeeded()) { work->diagnostics = std::move(created.diagnostics); return; }
                    cache.fallbackRegistry = std::move(*created.value); cache.fallbackBinding = fallbackBinding;
                }
                effectiveRegistry = cache.fallbackRegistry;
            }
            std::vector<Render::BuildingSplatPreviewInstance> instances;
            std::set<std::string> wantedPaths;
            std::optional<Services::BuildingSplatCursor> cursor;
            Services::BuildingSplatQueryContext query;
            query.units = capture->units; query.inputIdentity = capture->inputIdentity;
            query.version = capture->version;
            if (!capture->units.empty()) do {
                auto page = Services::QueryBuildingSplatFootprints(cache.catalog, query, query.version,
                    cache.catalog->identitySha256, {128, 1024 * 1024}, cursor);
                if (!page.Succeeded()) { work->diagnostics = std::move(page.diagnostics); return; }
                cursor = page.value->next;
                for (auto& row : page.value->rows) {
                    const auto at = static_cast<std::size_t>(row.inputIndex);
                    if (at >= visible.size()) { work->diagnostics = Status::Failure("World.BuildingSplat", "BUILDING_SPLAT_OWNER_STALE", "显隐输入与查询序列不符。").diagnostics; return; }
                    const auto identity = effectiveRegistry->Find(row.owner);
                    if (!identity) { work->diagnostics = Status::Failure("World.BuildingSplat", "BUILDING_SPLAT_OWNER_STALE", "贴画owner没有当前registry的真实身份；未按行号猜复用。").diagnostics; return; }
                    if (row.availability == Services::BuildingSplatAvailability::Available)
                        wantedPaths.insert(cache.catalog->definitions.at(row.splatKey).texturePath);
                    instances.push_back({std::move(row), identity->value});
                }
            } while (cursor);
            // 缓存仅保留当前使用的纹理；accepted旧scene自己保活旧image，不能让编辑过程无界累积。
            for (auto at = cache.textures.begin(); at != cache.textures.end();) {
                if (!wantedPaths.contains(at->first)) at = cache.textures.erase(at); else ++at;
            }
            if (cache.assetPaths != wantedPaths) cache.budgetTextures.clear();
            std::size_t textureBytes = 0;
            for (const auto& [path, texture] : cache.textures) {
                (void)path; if (texture.image) textureBytes += texture.image->rgba.size();
            }
            const auto noteResource = [&](const std::vector<Diagnostic>& diagnostics) {
                for (const auto& diagnostic : diagnostics) {
                    if (cache.resourceIssues.size() == 32) break;
                    auto bounded = diagnostic;
                    if (bounded.message.size() > 1024) bounded.message.resize(1024);
                    cache.resourceIssues.push_back(std::move(bounded));
                }
            };
            for (const auto& path : wantedPaths) {
                if (cache.textures.contains(path) || cache.failedTextures.contains(path) || cache.budgetTextures.contains(path)) continue;
                auto resource = cache.reader->ReadAnimationResource(path, 16ull * 1024 * 1024);
                if (!resource.Succeeded()) {
                    cache.failedTextures.insert(path); noteResource(resource.diagnostics); continue;
                }
                auto decoded = Render::DecodeModelTexture(path, resource.value->bytes, 2048);
                if (!decoded.Succeeded()) {
                    cache.failedTextures.insert(path); noteResource(decoded.diagnostics); continue;
                }
                constexpr std::size_t textureBudget = 64ull * 1024 * 1024;
                if (decoded.value->rgba.size() > textureBudget - std::min(textureBudget, textureBytes)) {
                    cache.budgetTextures.insert(path);
                    noteResource(Status::Failure("World.BuildingSplat", "BUILDING_SPLAT_TEXTURE_BUDGET", "当前贴画纹理实际RGBA总字节超过预算，未限制全图实例数。").diagnostics);
                    continue;
                }
                textureBytes += decoded.value->rgba.size();
                cache.textures.emplace(path, Render::BuildingSplatPreviewTexture{path, resource.value->sourceLayer,
                    resource.value->digest, std::make_shared<const Render::ModelTextureRgba>(std::move(*decoded.value))});
            }
            if (!cache.assets || cache.assetPaths != wantedPaths) {
                std::vector<Render::BuildingSplatPreviewTexture> textures;
                for (const auto& path : wantedPaths) if (const auto found = cache.textures.find(path); found != cache.textures.end()) textures.push_back(found->second);
                auto assets = Render::BuildBuildingSplatPreviewAssets(cache.catalog, textures, session);
                if (!assets.Succeeded()) { work->diagnostics = std::move(assets.diagnostics); return; }
                cache.assets = std::move(*assets.value); cache.assetPaths = wantedPaths;
            }
            Render::BuildingSplatPreviewBinding binding{query.version, cache.catalog->identitySha256, terrainIdentity, terrainRevision};
            auto prepared = Render::BuildBuildingSplatPreviewFrame(*cache.assets, instances, binding, terrain, content, {},
                previous ? &*previous : nullptr);
            if (!prepared.Succeeded()) { work->diagnostics = std::move(prepared.diagnostics); return; }
            const auto visibility = applyVisibility(*prepared.value);
            if (!visibility.Succeeded()) { work->diagnostics = visibility.diagnostics; return; }
            const auto stillCurrent = cache.reader->VerifyCurrentSource();
            if (!stillCurrent.Succeeded()) { work->diagnostics = stillCurrent.diagnostics; work->sourceRejected = true; return; }
            work->prepared = std::move(*prepared.value);
        } catch (const std::exception& error) {
            work->diagnostics.push_back({DiagnosticSeverity::Error, "World.BuildingSplat", "BUILDING_SPLAT_PREPARE_FAILED", error.what(), std::nullopt});
        }
    });
    worker_ = worker;
    connect(worker, &QThread::finished, this, [this, work, signature, geometryIdentity] {
        worker_ = nullptr;
        if (signature.source != sourceSerial_ || !viewport_ || !sourceCurrent_) { BeginNext(); return; }
        if (!sourceCurrent_()) { Clear(); return; }
        if (work->sourceRejected) { Report(FailureText(work->diagnostics)); Clear(); return; }
        cache_ = work->cache; // owned缓存只在当前来源代次接收；过期请求不能发布frame。
        if (signature.request != requestSerial_ || !current_ || !current_()) { BeginNext(); return; }
        if (!work->prepared) { failed_ = signature; Report(FailureText(work->diagnostics)); return; }
        prepared_ = std::move(work->prepared); preparedRequestSerial_ = signature.request;
        preparedGeometryIdentity_ = geometryIdentity; hiddenPrepared_ = work->hidden;
        if (submissionDeferred_ || !viewport_->isVisible()) return;
        if (signature.parent != viewport_->MeasurementModelEpoch() || signature.binding != bindingSerial_) { RebindParent(); return; }
        SubmitPrepared(signature);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}
void WorldBuildingSplatPresenter::SubmitPrepared(const Signature& signature) {
    if (submissionDeferred_ || !viewport_ || !prepared_ || !prepared_->frame || !prepared_->scene || !current_ || !current_()) return;
    if (prepared_->frame->instances.empty()) { RemoveLayer(); installed_ = prepared_; needsSubmit_ = false; }
    else {
        const bool submit = needsSubmit_ || !submitted_ || !installed_ || installed_->scene != prepared_->scene;
        const auto result = submit ? viewport_->SetModelLayer(layerId_, prepared_->scene, prepared_->frame,
            prepared_->renderLayerRole)
            : viewport_->UpdateModelLayer(layerId_, prepared_->frame);
        if (!result.Succeeded()) { failed_ = signature; needsSubmit_ = false; Report(FailureText(result.diagnostics)); return; }
        submitted_ = true; installed_ = prepared_; needsSubmit_ = false; failed_.reset();
    }
    QString text = tr("建筑贴画：预览近似；已请求 %1，已准备 %2，无贴画 %3，不可用 %4，预算省略 %5；等待实际完成帧核验")
        .arg(static_cast<qulonglong>(prepared_->requested)).arg(static_cast<qulonglong>(prepared_->displayed))
        .arg(static_cast<qulonglong>(prepared_->absent)).arg(static_cast<qulonglong>(prepared_->unavailable))
        .arg(static_cast<qulonglong>(prepared_->omittedBudget));
    text += tr("；无贴画中 %1 个不适用于非建筑（保留原uubs Absent）")
        .arg(static_cast<qulonglong>(notApplicableCount_));
    setProperty("buildingSplatLayerId", QVariant::fromValue(static_cast<qulonglong>(layerId_)));
    setProperty("buildingSplatSceneVersion", QVariant::fromValue(static_cast<qulonglong>(prepared_->scene->sceneVersion)));
    setProperty("buildingSplatContentVersion", QVariant::fromValue(static_cast<qulonglong>(prepared_->frame->contentVersion)));
    setProperty("buildingSplatPrepared", QVariant::fromValue(static_cast<qulonglong>(prepared_->displayed)));
    setProperty("buildingSplatHidden", QVariant::fromValue(static_cast<qulonglong>(hiddenPrepared_)));
    text += tr("；共面深度偏移为编辑器近似；GE4、原版bias、悬崖/水面投影及pass顺序待验证");
    text += tr("；显隐隐藏 %1（动态片段，静态资产未改）").arg(static_cast<qulonglong>(hiddenPrepared_));
    for (std::size_t i = 0; i < std::min<std::size_t>(4, prepared_->issues.size()); ++i)
        text += QStringLiteral("\n") + QString::fromUtf8(prepared_->issues[i].code + "：" + prepared_->issues[i].reason);
    if (cache_ && !cache_->resourceIssues.empty()) text += QStringLiteral("\n") + FailureText(cache_->resourceIssues);
    Report(std::move(text));
}
bool WorldBuildingSplatPresenter::DiagnosticRowUnavailable(Services::BuildingSplatRow& row) const {
    if (row.availability == Services::BuildingSplatAvailability::Absent) return false;
    if (row.availability == Services::BuildingSplatAvailability::Available) {
        const auto& path = cache_->catalog->definitions.at(row.splatKey).texturePath;
        if (!cache_->failedTextures.contains(path) && !cache_->budgetTextures.contains(path)) return false;
        row.reason = cache_->failedTextures.contains(path) ? "贴画实际纹理来源读取/解码失败，未替换另一来源。"
            : "贴画纹理超过当前预览预算，未把预算省略当作资源缺失。";
        row.reasonCode = cache_->failedTextures.contains(path) ? "BUILDING_SPLAT_TEXTURE_UNAVAILABLE" : "BUILDING_SPLAT_TEXTURE_BUDGET";
        row.availability = Services::BuildingSplatAvailability::Unavailable;
    }
    return true;
}

std::vector<Services::BuildingSplatRow> WorldBuildingSplatPresenter::DiagnosticRows() const {
    std::vector<Services::BuildingSplatRow> result;
    // worker修改自己的Cache副本。这里仅查询GUI已接收的cache，且不给旧输入授予当前资格。
    if (worker_ || !cache_ || !cache_->catalog || !capture_ || !current_ || !current_()
        || !sourceCurrent_ || !sourceCurrent_()) return result;
    Services::BuildingSplatQueryContext query{capture_->version, capture_->inputIdentity, capture_->units};
    std::optional<Services::BuildingSplatCursor> cursor;
    if (!query.units.empty()) do {
        auto page = Services::QueryBuildingSplatFootprints(cache_->catalog, query, query.version,
            cache_->catalog->identitySha256, {128, 1024 * 1024}, cursor);
        if (!page.Succeeded()) return {};
        cursor = page.value->next;
        for (auto& row : page.value->rows) {
            if (DiagnosticRowUnavailable(row)) result.push_back(std::move(row));
        }
    } while (cursor);
    return result;
}
QJsonObject WorldBuildingSplatPresenter::DiagnosticSnapshot() const {
    return DiagnosticSnapshotImpl(std::nullopt, 0);
}
QJsonObject WorldBuildingSplatPresenter::DiagnosticSnapshot(std::uint32_t offset, std::uint32_t limit) const {
    return DiagnosticSnapshotImpl(offset, std::clamp(limit, 1u, 16u));
}
QJsonObject WorldBuildingSplatPresenter::DiagnosticSnapshotImpl(std::optional<std::uint32_t> offset, std::uint32_t limit) const {
    const bool bounded = offset.has_value();
    QJsonObject result{{"current", !worker_ && capture_ && current_ && current_() && sourceCurrent_ && sourceCurrent_()},
        {"preparationRunning", static_cast<bool>(worker_)}, {"previewApproximate", true}};
    if (!cache_ || !cache_->catalog || worker_) return result;
    const auto& catalog = *cache_->catalog;
    QJsonObject catalogInfo{{"layer", static_cast<int>(catalog.source.layer)}, {"priority", catalog.source.priority},
        {"tableSha256", QString::fromStdString(catalog.tableSha256)},
        {"catalogIdentity", QString::fromStdString(catalog.identitySha256)},
        {"definitionCount", static_cast<double>(catalog.definitions.size())}};
    const auto text = [&](QJsonObject& object, const char* key, const std::string& value, std::size_t maximum = 2048) {
        if (bounded) WorldDiagnosticJson::Text(object, key, std::string_view(value), maximum);
        else object[key] = QString::fromStdString(value);
    };
    text(catalogInfo, "virtualPath", catalog.source.canonicalPath);
    text(catalogInfo, "sourceId", catalog.source.sourceId, 256); text(catalogInfo, "sourcePath", catalog.source.sourcePath);
    result["catalog"] = catalogInfo;
    std::vector<Services::BuildingSplatRow> capturedRows;
    if (bounded) {
        if (!result["current"].toBool()) { result["rowsState"] = "pending-or-stale"; return result; }
        Services::BuildingSplatQueryContext query{capture_->version, capture_->inputIdentity, capture_->units};
        result["inputCount"] = static_cast<double>(query.units.size());
        result["pageBinding"] = QJsonObject{{"documentId", QString::fromStdString(query.version.document.documentId)},
            {"sourceSha256", QString::fromStdString(query.version.document.sourceSha256)},
            {"generation", QString::number(query.version.document.generation)}, {"revision", QString::number(query.version.document.revision)},
            {"catalogRevision", QString::number(query.version.catalogRevision)}, {"editorRevision", QString::number(query.version.editorRevision)},
            {"inputIdentity", QString::fromStdString(query.inputIdentity)}, {"catalogIdentity", QString::fromStdString(catalog.identitySha256)}};
        if (*offset < query.units.size()) {
            const Services::BuildingSplatCursor cursor{query.version, catalog.identitySha256, query.inputIdentity, *offset};
            auto page = Services::QueryBuildingSplatFootprints(cache_->catalog, query, query.version, catalog.identitySha256,
                {limit, 256 * 1024}, cursor);
            if (!page.Succeeded()) {
                result["rowsState"] = "query-rejected";
                result["diagnostics"] = WorldDiagnosticJson::Diagnostics(page.diagnostics, 4); return result;
            }
            capturedRows = std::move(page.value->rows);
        }
        result["inputPage"] = WorldDiagnosticJson::Page(query.units.size(), *offset, capturedRows.size(), limit);
        result["unavailableRowsScope"] = "returned-input-page";
        for (auto& row : capturedRows) (void)DiagnosticRowUnavailable(row);
    } else capturedRows = DiagnosticRows();
    QJsonArray rows;
    for (const auto& row : capturedRows) {
        QJsonArray origins;
        for (std::size_t i = 0; i < row.origins.size() && (!bounded || i < 4); ++i) {
            const auto& origin = row.origins[i];
            QJsonObject value{{"byteOffset", origin.byteOffset ? QJsonValue(QString::number(*origin.byteOffset)) : QJsonValue::Null}};
            text(value, "field", origin.field, 128); text(value, "objectRawcode", origin.objectRawcode, 32);
            text(value, "sourceId", origin.sourceId, 256); text(value, "sourcePath", origin.sourcePath, 1024);
            origins.push_back(std::move(value));
        }
        const auto definition = catalog.definitions.find(row.splatKey);
        QJsonObject value{{"file", row.owner.file == MapData::PlacementFile::Units ? "war3mapUnits.doo" : "war3map.doo"},
            {"index", static_cast<double>(row.owner.index)}, {"creationId", row.owner.creationId},
            {"rawcode", QString::fromStdString(row.owner.rawcode)}, {"origins", origins}};
        text(value, "splatKey", row.splatKey, 128); text(value, "reason", row.reason); text(value, "reasonCode", row.reasonCode, 256);
        if (definition == catalog.definitions.end()) value["texturePath"] = QJsonValue::Null;
        else text(value, "texturePath", definition->second.texturePath);
        if (bounded) {
            value["availability"] = row.availability == Services::BuildingSplatAvailability::Available ? "available"
                : row.availability == Services::BuildingSplatAvailability::Absent ? "absent" : "unavailable";
            value["inputIndex"] = QString::number(row.inputIndex);
            value["originsPage"] = WorldDiagnosticJson::Page(row.origins.size(), 0, origins.size(), 4);
        }
        rows.push_back(std::move(value));
    }
    if (bounded) {
        result["inputRows"] = rows; result["rowsState"] = "current";
        QJsonArray unavailable;
        for (const auto& row : rows) if (row.toObject()["availability"] == "unavailable") unavailable.append(row);
        result["unavailableRows"] = unavailable;
    } else result["unavailableRows"] = rows;
    if (prepared_) result["counts"] = QJsonObject{{"requested", static_cast<double>(prepared_->requested)},
        {"prepared", static_cast<double>(prepared_->displayed)}, {"absent", static_cast<double>(prepared_->absent)},
        {"unavailable", static_cast<double>(prepared_->unavailable)}, {"omittedBudget", static_cast<double>(prepared_->omittedBudget)},
        {"notApplicable", static_cast<double>(notApplicableCount_)}};
    return result;
}
} // namespace War3Map::Modules
