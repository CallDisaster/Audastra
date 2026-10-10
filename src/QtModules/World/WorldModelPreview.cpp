#include "WorldModelPreview.h"
#include "WorldAnimationPlayer.h"
#include <QComboBox>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStringList>
#include <QThread>
#include <QVariant>
#include <QVBoxLayout>
#include <exception>

namespace War3Map::Modules {
struct WorldModelPreview::Built {
    std::shared_ptr<Render::ModelAnimationService> service;
    std::shared_ptr<WorldResourceReader> source;
    std::shared_ptr<const Render::AnimationFrame> frame;
    War3RenderCameraV2 camera{};
    QString error;
};
WorldModelPreview::WorldModelPreview(QWidget* parent,const War3RenderPluginApiV2* backend) : QWidget(parent) {
    setObjectName(QStringLiteral("worldUnitModelPreview")); setProperty("worldCard",true);
    auto* column=new QVBoxLayout(this); column->setContentsMargins(0,4,0,0); column->setSpacing(5);
    title_=new QLabel(tr("预览近似"),this); title_->setObjectName(QStringLiteral("worldUnitPreviewTitle")); column->addWidget(title_);
    viewport_=new WorldViewport(this,backend); viewport_->setObjectName(QStringLiteral("worldUnitPreviewViewport"));
    viewport_->setFixedHeight(188); viewport_->setFocusPolicy(Qt::NoFocus); viewport_->hide(); column->addWidget(viewport_);
    player_=new WorldAnimationPlayer(*viewport_,this);
    message_=new QLabel(this); message_->setObjectName(QStringLiteral("worldUnitPreviewMessage")); message_->setWordWrap(true); column->addWidget(message_);
    auto* controls=new QHBoxLayout; controls->setSpacing(5);
    action_=new QComboBox(this); action_->setObjectName(QStringLiteral("worldUnitPreviewAction")); action_->setAccessibleName(tr("预览动作"));
    controls->addWidget(action_,1);
    playPause_=new QPushButton(tr("播放"),this); playPause_->setObjectName(QStringLiteral("worldUnitPreviewPlayPause")); controls->addWidget(playPause_);
    step_=new QPushButton(tr("单步"),this); step_->setObjectName(QStringLiteral("worldUnitPreviewStep")); controls->addWidget(step_);
    column->addLayout(controls);
    connect(playPause_,&QPushButton::clicked,this,[this]{player_->SetPlaying(!player_->Playing());});
    connect(step_,&QPushButton::clicked,player_,&WorldAnimationPlayer::Step);
    connect(action_,&QComboBox::currentIndexChanged,this,[this](int index) {
        if (index<0 || !player_->Ready() || !identity_) return;
        bool ok=false; const auto sequence=action_->itemData(index).toUInt(&ok); if (!ok) return;
        if (identity_->isCurrent && !identity_->isCurrent()) { ClearSelection(); return; }
        player_->SetPlaying(false);
        Render::AnimationEdit edit; edit.identity=player_->Frame()->states.front().identity; edit.sequenceIndex=sequence;
        if (player_->Scene()->particlesEnabled) { edit.elapsedMs=0; edit.globalMs=0; }
        player_->Edit(std::move(edit));
    });
    connect(player_,&WorldAnimationPlayer::Changed,this,[this] {
        const auto ready=player_->Ready(); playPause_->setEnabled(ready); step_->setEnabled(ready && !player_->Playing() && !player_->Busy());
        playPause_->setText(player_->Playing()?tr("暂停"):tr("播放"));
        if (ready && player_->Frame() && player_->Frame()->contentVersion != displayedContentVersion_) {
            displayedContentVersion_=player_->Frame()->contentVersion;
            const auto& state=player_->Frame()->states.front();
            const auto& names=player_->Scene()->sequences.front();
            if (state.sequenceIndex<names.size()) {
                title_->setText(tr("预览近似 · %1").arg(QString::fromUtf8(names[state.sequenceIndex].name)));
                QString text=tr("局部/全局 %1/%2 ms").arg(state.elapsedMs).arg(state.globalMs);
                if (!player_->Frame()->poses.empty()) text+=tr(" · 目标姿势 %1").arg(player_->Frame()->poses.front().poseVersion);
                for (const auto& particle:player_->Frame()->particleLinks) {
                    if (!particle.rejectionCode.empty())
                        text+=(particle.partial?tr(" · 粒子部分显示：%1"):tr(" · 粒子不可用：%1")).arg(QString::fromUtf8(particle.rejectionCode));
                    if (particle.simulated) text+=tr(" · %1 粒子 · %2 条带段").arg(particle.particleCount).arg(particle.ribbonSegmentCount);
                }
                const auto& capabilities=player_->Scene()->particleCapabilities;
                if (!player_->Scene()->particlesEnabled && !capabilities.empty() &&
                    (capabilities.front().particle2Count || capabilities.front().ribbonCount || capabilities.front().premCount))
                    text+=tr(" · 当前插件不支持粒子预览");
                SetMessage(text);
            }
        }
    });
    connect(player_,&WorldAnimationPlayer::Message,this,[this](const QString& text) {
        SetMessage(text); if (!player_->Ready()) { viewport_->hide(); action_->setEnabled(false); }
    });
    hide();
}
WorldModelPreview::~WorldModelPreview() {
    ++serial_; pending_.reset();
    // QObject 按创建顺序析构子对象；调度器必须先于它引用的视口销毁。
    delete player_; player_ = nullptr;
}
void WorldModelPreview::hideEvent(QHideEvent* event) { player_->SetPlaying(false); QWidget::hideEvent(event); }
void WorldModelPreview::SetMessage(QString text) { message_->setText(std::move(text)); message_->setToolTip(message_->text()); }
bool WorldModelPreview::Matches(const QString& key,std::uint64_t generation,std::uint64_t revision,
    const MapData::PlacementId& placement,const Render::ModelInstanceRequest& r) const {
    return identity_ && identity_->mapKey==key && identity_->generation==generation && identity_->editRevision==revision &&
        identity_->placement==placement && identity_->modelPath==r.modelPath && identity_->replaceableTexture==r.replaceableTexture &&
        identity_->teamColor==r.teamColor && identity_->tint==r.tint && identity_->animationProperties==r.animationProperties;
}
void WorldModelPreview::ClearSelection() {
    ++serial_; pending_.reset(); identity_.reset(); displayedContentVersion_=0; player_->Clear();
    { const QSignalBlocker blocker(action_); action_->clear(); }
    action_->hide(); playPause_->hide(); step_->hide();
    (void)viewport_->SetModelScene(nullptr); viewport_->ClearMesh(++viewportRevision_); viewport_->hide(); hide();
}
void WorldModelPreview::ShowUnavailable(QString key,std::uint64_t generation,std::uint64_t revision,
    MapData::PlacementId placement,Render::ModelInstanceRequest request,QString reason) {
    ClearSelection(); Identity id; id.mapKey=std::move(key); id.generation=generation; id.editRevision=revision;
    id.placement=std::move(placement); id.modelPath=request.modelPath; id.replaceableTexture=request.replaceableTexture;
    id.teamColor=request.teamColor; id.tint=request.tint; id.animationProperties=request.animationProperties;
    identity_=std::move(id); SetMessage(std::move(reason)); show();
}
void WorldModelPreview::ShowSelection(QString key,std::uint64_t generation,std::uint64_t revision,
    MapData::PlacementId placement,std::string rowKey,Render::ModelInstanceRequest request,
    Result<WorldTerrainSources> sources,std::function<bool()> current) {
    if (!sources.Succeeded()) {
        ShowUnavailable(std::move(key),generation,revision,std::move(placement),std::move(request),
            sources.diagnostics.empty()?tr("模型来源不可用"):QString::fromUtf8(sources.diagnostics.front().message)); return;
    }
    ClearSelection(); Identity id; id.mapKey=std::move(key); id.generation=generation; id.editRevision=revision;
    id.placement=std::move(placement); id.rowKey=std::move(rowKey); id.modelPath=request.modelPath;
    id.replaceableTexture=request.replaceableTexture; id.teamColor=request.teamColor; id.tint=request.tint; id.isCurrent=std::move(current);
    id.animationProperties=request.animationProperties;
    identity_=id; request.position={0,0,0}; request.angle=0; request.scale={1,1,1};
    pending_=Pending{std::move(id),std::move(request),std::move(*sources.value)};
    SetMessage(tr("正在准备模型动作…")); show(); BeginNext();
}
void WorldModelPreview::BeginNext() {
    if (worker_ || !pending_) return;
    auto input=std::move(*pending_); pending_.reset(); const auto serial=serial_;
    auto work=std::make_shared<Built>(); const auto epoch=Render::NextAnimationSerial();
    const bool particles=viewport_->HasParticleSupport();
    auto* worker=QThread::create([input=std::move(input),work,epoch,particles] {
        try {
            auto reader=WorldResourceReader::Open(input.sources);
            if (!reader.Succeeded()) { work->error=QString::fromUtf8(reader.diagnostics.front().message); return; }
            work->source=std::shared_ptr<WorldResourceReader>(std::move(*reader.value));
            Render::AnimationInstanceSpec spec; spec.identity={1,epoch}; spec.hostKey=input.identity.rowKey; spec.model=input.request;
            if (particles) { spec.elapsedMs=0; spec.globalMs=0; }
            Render::AnimationLimits limits; limits.geometry.maxModels=1; limits.geometry.maxInstances=1;
            limits.allowRigidStaticPreview=true;
            limits.enableParticles=particles;
            limits.geometry.maxVertices=262144; limits.geometry.maxIndices=786432;
            limits.geometry.maxTextureDimension=256; limits.geometry.maxTotalTextureBytes=32ull*1024*1024;
            auto service=Render::ModelAnimationService::Create({epoch,input.identity.editRevision,input.sources.archiveSha},std::span(&spec,1),
                [&](auto path,auto maximum){return work->source->ReadAnimationResource(path,maximum);},limits);
            if (!service.Succeeded()) { work->error=service.diagnostics.empty()?QObject::tr("模型动画不可用"):
                QString::fromUtf8(service.diagnostics.front().code+"："+service.diagnostics.front().message); return; }
            work->service=std::move(*service.value);
            if (work->service->Scene()->geometry.instances.empty()) {
                work->error=QObject::tr("纯粒子没有可靠的模型取景边界；请在世界视图中查看，或使用显式 camera 的捕获清单。"); return;
            }
            work->camera=Render::FitAnimationSceneCamera(*work->service->Scene());
            Render::AnimationAdvance first; first.worldCamera=Render::AnimationWorldCamera(work->camera);
            auto frame=work->service->Prepare(nullptr,first);
            if (!frame.Succeeded()) { work->error=QString::fromUtf8(frame.diagnostics.front().code+"："+frame.diagnostics.front().message); return; }
            const auto verify=work->source->VerifyCurrentSource();
            if (!verify.Succeeded()) { work->error=QString::fromUtf8(verify.diagnostics.front().message); return; }
            work->frame=std::move(*frame.value);
        } catch (const std::exception& e) { work->error=QString::fromUtf8(e.what()); }
    });
    worker_=worker;
    connect(worker,&QThread::finished,this,[this,work,serial] {
        worker_=nullptr;
        if (serial==serial_ && identity_) {
            if (identity_->isCurrent && !identity_->isCurrent()) ClearSelection(); else Present(*work);
        }
        BeginNext();
    });
    connect(worker,&QThread::finished,worker,&QObject::deleteLater); worker->start(QThread::LowPriority);
}
void WorldModelPreview::Present(const Built& built) {
    if (!built.frame) { SetMessage(built.error.isEmpty()?tr("模型动画不可用"):built.error); return; }
    viewport_->ClearMesh(++viewportRevision_);
    if (!viewport_->SetCamera(built.camera,viewportRevision_).Succeeded() ||
        !player_->Install(built.service,built.frame,built.source,[this] {return identity_ && (!identity_->isCurrent || identity_->isCurrent());})) {
        SetMessage(tr("模型上传失败：渲染插件拒绝动画场景")); viewport_->hide(); return;
    }
    {
        const QSignalBlocker blocker(action_); action_->clear();
        // 按模型里的原顺序列出全部动作（SEQS），附时长与是否循环；损坏的序列保留但不可选。
        const auto& sequences=built.service->Scene()->sequences.front(); int selected=-1;
        for (std::uint32_t index=0; index<sequences.size(); ++index) {
            const auto& sequence=sequences[index];
            const auto name=sequence.name.empty()?tr("（未命名 %1）").arg(index):QString::fromUtf8(sequence.name);
            if (!sequence.Usable()) {
                action_->addItem(tr("%1（序列损坏）").arg(name));
                if (auto* model=qobject_cast<QStandardItemModel*>(action_->model())) model->item(action_->count()-1)->setEnabled(false);
                continue;
            }
            const auto seconds=static_cast<double>(sequence.end-sequence.start)/1000.0;
            action_->addItem(tr("%1 · %2 秒%3").arg(name).arg(seconds,0,'f',2).arg(sequence.flags==1?tr(" · 不循环"):QString{}),
                QVariant::fromValue(index));
            if (index==built.frame->states.front().sequenceIndex) selected=action_->count()-1;
        }
        action_->setCurrentIndex(selected); action_->setEnabled(true);
        action_->setToolTip(built.service->Scene()->particlesEnabled?tr("切换动作从局部/全局 0/0 ms 开始。"):QString{});
    }
    QStringList diagnostics;
    if (built.service->Scene()->particlesEnabled)
        diagnostics<<tr("新建预览和切换动作从局部/全局 0/0 ms 开始。");
    for (const auto& d:built.service->Scene()->geometry.warnings) diagnostics<<QString::fromUtf8(d.message);
    message_->setToolTip(diagnostics.join(QLatin1Char('\n')));
    action_->show(); playPause_->show(); step_->show(); viewport_->show();
}
} // namespace War3Map::Modules
