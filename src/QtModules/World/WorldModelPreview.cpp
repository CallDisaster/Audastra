#include "WorldModelPreview.h"
#include "WorldAnimationPlayer.h"
#include "War3MapCore/Render/ModelGameLimits.h"
#include <QAbstractItemView>
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
#include <algorithm>
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
    player_->SetLoopFinishedActions(true); // 预览卡循环播放；特效动作播完常淡出成空白。
    // 帧行逐帧刷新：单行、不换行、由主题画不透明底。旁边是原生 D3D 子窗口，透明标签反复改字会留下旧字迹。
    frame_=new QLabel(this); frame_->setObjectName(QStringLiteral("worldUnitPreviewFrame"));
    frame_->setTextFormat(Qt::PlainText); frame_->setWordWrap(false); frame_->setAttribute(Qt::WA_StyledBackground);
    frame_->hide(); column->addWidget(frame_);
    message_=new QLabel(this); message_->setObjectName(QStringLiteral("worldUnitPreviewMessage")); message_->setWordWrap(true); column->addWidget(message_);
    auto* controls=new QHBoxLayout; controls->setSpacing(5);
    action_=new QComboBox(this); action_->setObjectName(QStringLiteral("worldUnitPreviewAction")); action_->setAccessibleName(tr("预览动作"));
    // 卡片很窄：下拉框本身按可用宽度省略，展开的列表按最长动作名加宽，不再显示成“D…环”。
    action_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); action_->setMinimumContentsLength(6);
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
        // 切换动作保持原来的播放/暂停状态；先停下让编辑从 0 ms 落定，再按原状态恢复。
        const bool resume=player_->Playing();
        player_->SetPlaying(false);
        Render::AnimationEdit edit; edit.identity=player_->Frame()->states.front().identity; edit.sequenceIndex=sequence;
        // 动作选择始终从头开始，不能由当前插件是否支持粒子决定起点。
        edit.elapsedMs=0; edit.globalMs=0;
        player_->Edit(std::move(edit));
        if (resume) player_->SetPlaying(true);
    });
    connect(player_,&WorldAnimationPlayer::Changed,this,[this] {
        const auto ready=player_->Ready(); playPause_->setEnabled(ready); step_->setEnabled(ready && !player_->Playing() && !player_->Busy());
        playPause_->setText(player_->Playing()?tr("暂停"):tr("播放"));
        if (ready && player_->Frame() && player_->Frame()->contentVersion != displayedContentVersion_) {
            displayedContentVersion_=player_->Frame()->contentVersion;
            const auto& state=player_->Frame()->states.front();
            const auto& names=player_->Scene()->sequences.front();
            if (state.sequenceIndex<names.size()) {
                const auto& sequence=names[state.sequenceIndex];
                title_->setText(tr("预览近似 · %1").arg(QString::fromUtf8(sequence.name)));
                // 显示当前动作在模型时间轴上的帧号（与模型编辑器里的关键帧对应）和该动作的帧区间。
                const std::uint64_t length=sequence.end>sequence.start?static_cast<std::uint64_t>(sequence.end-sequence.start):0;
                std::uint64_t local=state.elapsedMs;
                if (length && local>length) local=sequence.flags==1?length:local%length;
                QString frame=tr("第 %1 帧 · 区间 %2–%3").arg(static_cast<std::uint64_t>(sequence.start)+local).arg(sequence.start).arg(sequence.end);
                QStringList notes;
                for (const auto& particle:player_->Frame()->particleLinks) {
                    if (!particle.rejectionCode.empty())
                        notes<<(particle.partial?tr("粒子部分显示：%1"):tr("粒子不可用：%1")).arg(QString::fromUtf8(particle.rejectionCode));
                    if (particle.simulated && particle.particleCount) frame+=tr(" · %1 粒子").arg(particle.particleCount);
                }
                frame_->setText(frame); frame_->setToolTip(frame); frame_->show();
                const auto& capabilities=player_->Scene()->particleCapabilities;
                if (!player_->Scene()->particlesEnabled && !capabilities.empty() &&
                    (capabilities.front().particle2Count || capabilities.front().ribbonCount || capabilities.front().premCount))
                    notes<<tr("当前插件不支持粒子预览");
                if (!omissions_.isEmpty()) notes<<tr("近似：%1").arg(omissions_);
                notes.removeDuplicates();
                // 说明行只在内容变化时改，不随帧闪动。
                if (const auto text=notes.join(tr(" · ")); text!=message_->text()) SetMessage(text);
            }
        }
    });
    connect(player_,&WorldAnimationPlayer::Message,this,[this](const QString& text) {
        SetMessage(text); if (!player_->Ready()) { viewport_->hide(); frame_->hide(); action_->setEnabled(false); }
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
    ++serial_; pending_.reset(); identity_.reset(); displayedContentVersion_=0; omissions_.clear(); player_->Clear();
    { const QSignalBlocker blocker(action_); action_->clear(); }
    action_->hide(); playPause_->hide(); step_->hide(); frame_->clear(); frame_->hide();
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
    // 面板刚创建时视口还没显示、插件未载入；不先载入就会把第一次预览的粒子误判为不支持。
    const bool particles=viewport_->LoadRenderPlugin() && viewport_->HasParticleSupport();
    auto* worker=QThread::create([input=std::move(input),work,epoch,particles] {
        try {
            auto reader=WorldResourceReader::Open(input.sources);
            if (!reader.Succeeded()) { work->error=QString::fromUtf8(reader.diagnostics.front().message); return; }
            work->source=std::shared_ptr<WorldResourceReader>(std::move(*reader.value));
            Render::AnimationInstanceSpec spec; spec.identity={1,epoch}; spec.hostKey=input.identity.rowKey; spec.model=input.request;
            spec.elapsedMs=0; spec.globalMs=0;
            const auto limits=Render::SingleModelPreviewLimits(particles);
            auto service=Render::ModelAnimationService::Create({epoch,input.identity.editRevision,input.sources.archiveSha},std::span(&spec,1),
                [&](auto path,auto maximum){return work->source->ReadAnimationResource(path,maximum);},limits);
            if (!service.Succeeded()) { work->error=service.diagnostics.empty()?QObject::tr("模型动画不可用"):
                QString::fromUtf8(service.diagnostics.front().code+"："+service.diagnostics.front().message); return; }
            work->service=std::move(*service.value);
            auto camera=Render::SingleModelPreviewCamera(*work->service->Scene(),
                [&](auto path,auto maximum){return work->source->ReadAnimationResource(path,maximum);},spec.model.modelPath);
            if (!camera.Succeeded()) {
                work->error=QObject::tr("模型无法预览：%1").arg(QString::fromUtf8(camera.diagnostics.front().code)); return;
            }
            work->camera=*camera.value;
            Render::AnimationAdvance first; first.worldCamera=Render::AnimationWorldCamera(work->camera);
            auto frame=work->service->Prepare(nullptr,first);
            if (!frame.Succeeded()) { work->error=QString::fromUtf8(frame.diagnostics.front().code+"："+frame.diagnostics.front().message); return; }
            const auto verify=work->source->VerifyCurrentSource();
            if (!verify.Succeeded()) { work->error=QString::fromUtf8(verify.diagnostics.front().message); return; }
            // 按首帧实际可见的网格重新取景：肖像背景板、建造脚手架等平时隐藏的大面片不再把模型挤成一个点。
            work->camera=Render::FitVisibleMeshCamera(*work->service->Scene(),**frame.value,work->camera);
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
    {
        // 预览近似省略的部分逐项写进状态行，不把近似画面当成完整模型。
        QStringList omitted; const auto& scene=*built.service->Scene();
        for (const auto& d:scene.geometry.warnings) {
            if (d.code=="ANIMATION_FIRST_SEQUENCE") omitted<<tr("无 Stand，播放首个动作");
            else if (d.code=="PARTICLE_PREM_UNSUPPORTED") omitted<<tr("PREM 子模型未显示");
            else if (d.code=="ANIMATION_NODE_FLAGS_MASKED") omitted<<tr("忽略未知节点标志");
            else if (d.code=="ANIMATION_GEOA_FLAGS_MASKED") omitted<<tr("忽略 GEOA 未用标志");
            else if (d.code=="ANIMATION_ZERO_ROTATION_PREVIEW") omitted<<tr("零长度旋转按不旋转");
            else if (d.code=="ANIMATION_GLOBAL_KEY_ORDER_PREVIEW") omitted<<tr("全局关键帧倒序按原版查找");
            else if (d.code=="PARTICLE_RIBBON_MATERIAL_APPROXIMATE") omitted<<tr("条带材质按首层近似");
            else if (d.code==Render::kGeosetOverGameLimitCode) omitted<<tr("几何体超出游戏上限，游戏里可能不显示");
        }
        std::size_t skipped=0; QStringList reasons;
        for (const auto& capability:scene.particleCapabilities) if (capability.skippedEmitters) {
            skipped+=capability.skippedEmitters->size();
            for (const auto& issue:*capability.skippedEmitters) reasons<<QString::fromUtf8(issue.code);
        }
        reasons.removeDuplicates();
        if (skipped) omitted<<tr("跳过 %1 个粒子发射器（%2）").arg(skipped).arg(reasons.join(QLatin1Char(',')));
        if (scene.geometry.untexturedLayers) omitted<<tr("%1 层缺贴图").arg(scene.geometry.untexturedLayers);
        omitted.removeDuplicates(); omissions_=omitted.join(tr("、"));
    }
    viewport_->ClearMesh(++viewportRevision_);
    if (!viewport_->SetCamera(built.camera,viewportRevision_).Succeeded() ||
        !player_->Install(built.service,built.frame,built.source,[this] {return identity_ && (!identity_->isCurrent || identity_->isCurrent());})) {
        SetMessage(tr("模型上传失败：渲染插件拒绝动画场景")); viewport_->hide(); return;
    }
    // 每个新模型打开即循环播放：特效和动作在 0 ms 常常不可见。上一个模型被清除时隐藏会撤销播放意图，
    // 这里按新模型重新请求；纯静态模型不请求，免得“只有静态模型”的提示盖掉状态行。
    if (const auto& scene=*built.service->Scene(); scene.animatedInstances || scene.particleInstanceCount)
        player_->SetPlaying(true);
    {
        const QSignalBlocker blocker(action_); action_->clear();
        // 按模型里的原顺序列出全部动作（SEQS）。卡片窄，下拉框只写动作名；当前动作的帧区间在帧行，
        // 各动作的帧区间、时长与是否循环在悬停详情。损坏的序列保留但不可选。
        const auto& sequences=built.service->Scene()->sequences.front(); int selected=-1;
        for (std::uint32_t index=0; index<sequences.size(); ++index) {
            const auto& sequence=sequences[index];
            const auto name=sequence.name.empty()?tr("（未命名 %1）").arg(index):QString::fromUtf8(sequence.name);
            if (!sequence.Usable()) {
                action_->addItem(tr("%1（序列损坏）").arg(name));
                if (auto* model=qobject_cast<QStandardItemModel*>(action_->model())) model->item(action_->count()-1)->setEnabled(false);
                continue;
            }
            const auto loops=sequence.flags==1?tr("，不循环"):QString{};
            action_->addItem(name, QVariant::fromValue(index));
            const auto seconds=static_cast<double>(sequence.end-sequence.start)/1000.0;
            action_->setItemData(action_->count()-1, tr("%1\n帧区间 %2–%3，时长 %4 秒%5").arg(name).arg(sequence.start).arg(sequence.end)
                .arg(seconds,0,'f',2).arg(loops), Qt::ToolTipRole);
            if (index==built.frame->states.front().sequenceIndex) selected=action_->count()-1;
        }
        action_->setCurrentIndex(selected); action_->setEnabled(true);
        action_->setToolTip(tr("切换动作从该动作第一帧开始，保持当前播放状态。"));
        action_->view()->setMinimumWidth(std::max(action_->width(), action_->view()->sizeHintForColumn(0)+32));
    }
    QStringList diagnostics;
    diagnostics<<tr("新建预览和切换动作从该动作第一帧开始并自动播放。");
    for (const auto& d:built.service->Scene()->geometry.warnings) diagnostics<<QString::fromUtf8(d.message);
    message_->setToolTip(diagnostics.join(QLatin1Char('\n')));
    action_->show(); playPause_->show(); step_->show(); viewport_->show();
}
} // namespace War3Map::Modules
