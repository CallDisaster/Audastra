#include "ProjectMinimapPreview.h"
#include "EditorProject.h"
#include "PendingImportPayload.h"
#include "../QtCommon/DiagnosticTimings.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include <QThread>
#include <QUndoStack>
#include <algorithm>

namespace War3Map::QtProject {
ProjectMinimapPreview::ProjectMinimapPreview(EditorProject& project, QObject* parent)
    : QObject(parent), project_(project) {
    debounce_.setSingleShot(true); debounce_.setInterval(500);
    connect(&debounce_, &QTimer::timeout, this, &ProjectMinimapPreview::Start);
    const auto changed = [this] { Invalidate(); };
    connect(&project_, &EditorProject::MapDocumentChanged, this, changed);
    connect(&project_, &EditorProject::ImportsChanged, this, changed);
    connect(&project_, &EditorProject::SourceStateChanged, this, changed);
    connect(&project_, &EditorProject::WorldEditStringsChanged, this, changed);
    connect(&project_, &EditorProject::ObjectCatalogChanged, this, changed);
    connect(&project_, &EditorProject::ObjectDocumentsReplaced, this, changed);
    connect(&project_, &EditorProject::EditTrackingBaselineChanged, this, changed);
    connect(&project_, &EditorProject::PendingTerrainCommitStateChanged, this,
        [this](bool pending) { if (!pending) Invalidate(); else { if (control_) (void)control_->RequestCancellation(); ++serial_; result_.reset(); pending_ = requested_; emit Changed(); } });
    if (project_.mapDataUndoStack_) connect(project_.mapDataUndoStack_, &QUndoStack::indexChanged, this, changed);
    for(auto* stack:project_.objectUndoStacks_)if(stack)connect(stack,&QUndoStack::indexChanged,this,changed);
}
ProjectMinimapPreview::~ProjectMinimapPreview() {
    if (control_) (void)control_->RequestCancellation();
    if (worker_) { disconnect(worker_, nullptr, this, nullptr); worker_->wait(); delete worker_.data(); }
}
void ProjectMinimapPreview::Request(MinimapPreviewOptions options, bool force) {
    if (!force && requested_ && options == options_) return;
    if(force)result_.reset();
    options_ = options; requested_ = true; Invalidate();
}
void ProjectMinimapPreview::Invalidate() {
    if (control_) (void)control_->RequestCancellation();
    if(requested_&&!options_.current&&result_&&result_->Succeeded()&&!result_->value->current
        &&project_.archiveSession_&&result_->value->sourceSha256==project_.archiveSession_->SourceSha256()
        &&!project_.sourceChangeSuspected_&&!project_.sourceChangeConfirmed_){
        ++serial_;pending_=false;emit Changed();return;
    }
    ++serial_; result_.reset(); pending_ = requested_; emit Changed();
    if (requested_) debounce_.start();
}
void ProjectMinimapPreview::Start() {
    if (!requested_ || worker_ || project_.HasPendingTerrainCommit()) return;
    if (project_.savingMap_) { debounce_.start(); return; }
    const auto failure = [this](std::string code, std::string message) {
        result_ = std::make_shared<Result<Services::MinimapPreview>>(Result<Services::MinimapPreview>::Failure(
            "Minimap.Preview",std::move(code),std::move(message)));
        pending_ = false; timing_.reset(); project_.AddDiagnostics(result_->diagnostics); emit Changed();
    };
    if (!project_.archiveSession_) {
        if(!project_.mapEdits_){result_.reset();pending_=false;emit Changed();return;}
        failure("MINIMAP_CURRENT_SOURCE", "当前小地图需要已打开的地图归档。"); return;
    }
    if (project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) {
        failure("MINIMAP_PREVIEW_STALE", "地图文件已变化，请重新打开。"); return;
    }
    const auto archive = project_.archiveSession_;
    const auto path = project_.sourceArchivePath_;
    const auto game = project_.war3GameRoot_;
    const auto generation = project_.editRevision_.Generation(), revision = project_.editRevision_.Revision();
    const auto serial = serial_;
    const auto options = options_;
    Services::CurrentMinimapPreviewInput current;
    Services::MinimapPreviewInput saved; saved.sourceSha256 = archive->SourceSha256();
    timing_=std::make_unique<QtCommon::DiagnosticTimingScope>("minimap-total");
    if (options.current) {
        if (!project_.mapEdits_ || !project_.mapEdits_->Terrain()) {
            failure("MINIMAP_CURRENT_SOURCE", "当前地形未就绪。"); return;
        }
        QtCommon::DiagnosticTimingScope freezeTiming("minimap-freeze");
        constexpr std::size_t maximumOverlayBytes = 128U * 1024U * 1024U;
        std::size_t overlayBytes = 0;
        if (!project_.assetEdits_.empty()) {
            failure("MINIMAP_CURRENT_PENDING_ASSETS", "资源路径修改尚未冻结。");
            return;
        }
        const auto scopes = project_.editRevision_.Current();
        for (std::size_t index = 0; index < project_.pendingImports_.size(); ++index) {
            const auto& entry = project_.pendingImports_[index];
            if (entry.archivePath.endsWith(".ai", Qt::CaseInsensitive)
                || entry.diskPath.endsWith(".ai", Qt::CaseInsensitive)) {
                failure("MINIMAP_CURRENT_PENDING_ASSETS", "引导资源不能作为小地图覆盖。");
                return;
            }
            if (!entry.ownedPayload) {
                failure("MINIMAP_CURRENT_PENDING_ASSETS", "磁盘导入尚未冻结。");
                return;
            }
            const auto expected = scopes.find("imports/" + std::to_string(index));
            if (expected == scopes.end() || expected->second.size() < 64 || entry.size < 0
                || std::uint64_t(entry.size) > maximumOverlayBytes - overlayBytes) {
                failure("MINIMAP_PREVIEW_BUDGET", "资源覆盖缺少身份或超过预算。");
                return;
            }
            auto bytes = ReadFrozenPendingImport(entry, std::string_view(expected->second).substr(expected->second.size() - 64));
            if (!bytes.Succeeded()) {
                project_.AddDiagnostics(bytes.diagnostics);
                failure("MINIMAP_CURRENT_PENDING_ASSETS", "资源覆盖身份已变化。");
                return;
            }
            overlayBytes += bytes.value->size();
            current.replacements.push_back({entry.archivePath.toStdString(), std::move(*bytes.value)});
        }
        for (std::size_t index = 0; index < project_.objectDirty_.size(); ++index) {
            if (!project_.objectDirty_[index]) continue;
            const auto& baseline = project_.nativeObjectDocuments_[index];
            const auto& edited = project_.objectDocuments_[index];
            if (!baseline || !edited) {
                failure("MINIMAP_CURRENT_OBJECT_BASELINE", "物编基线未就绪。");
                return;
            }
            const auto type = baseline->Type();
            if (edited->Objects().empty() && !archive->HasMember(ObjectData::NativeFileName(type))) continue;
            auto native = *baseline;
            // 与保存使用同一回灌和序列化入口；仅修改本次私有副本。
            const auto applied = ObjectData::ApplyLniChangesToNative(native, *edited,
                project_.objectFieldSchema_.get(), project_.standardCatalog_.get());
            if (!applied.Succeeded()) {
                project_.AddDiagnostics(applied.diagnostics);
                failure("MINIMAP_CURRENT_OBJECT_FREEZE", "物编修改未能冻结。");
                return;
            }
            auto encoded = ObjectData::NativeObjectCodec(type).Serialize(native);
            if (!encoded.Succeeded()) {
                project_.AddDiagnostics(encoded.diagnostics);
                failure("MINIMAP_CURRENT_OBJECT_FREEZE", "物编修改未能序列化。");
                return;
            }
            if (encoded.value->size() > 8U * 1024U * 1024U
                || encoded.value->size() > maximumOverlayBytes - overlayBytes) {
                failure("MINIMAP_PREVIEW_BUDGET", "物编覆盖超过预算。");
                return;
            }
            overlayBytes += encoded.value->size();
            current.replacements.push_back({std::string(ObjectData::NativeFileName(type)), std::move(*encoded.value)});
        }
        auto frozen = project_.mapEdits_->FreezeForSave(128U*1024U*1024U);
        freezeTiming.Finish();
        if (!frozen.Succeeded()) {
            result_ = std::make_shared<Result<Services::MinimapPreview>>(Result<Services::MinimapPreview>{{},std::move(frozen.diagnostics)});
            pending_=false; timing_.reset(); project_.AddDiagnostics(result_->diagnostics); emit Changed(); return;
        }
        current.archive=archive; current.edits=*frozen.value; current.gameRoot=game; current.documentRevision=revision;
        current.layers=options.layers; current.units=options.units; current.starts=options.starts; current.icons=options.icons;
    } else {
        for (const auto& member:archive->Members()) {
            const auto key=Resources::CanonicalImportPath(member.name);
            if (!key.Succeeded() || *key.value!="WAR3MAPMAP.BLP") continue;
            if(member.data.size()>4U*1024U*1024U){failure("MINIMAP_PREVIEW_BUDGET","底图超过4MiB输入预算。");return;}
            saved.member=member.data; break;
        }
        if(!saved.member && archive->HasOpaqueMembers()) {
            failure("MINIMAP_PREVIEW_UNCERTAIN","无法从不完整成员清单确认底图缺失。");return;
        }
    }
    control_ = std::make_shared<ProjectSaveControl>();
    current.control = control_;
    auto result=std::make_shared<Result<Services::MinimapPreview>>();
    const auto sessionIdentity=current.edits?current.edits->SessionIdentity():0;
    const auto sessionRevision=current.edits?current.edits->Revision():0;
    worker_=QThread::create([current=std::move(current),saved=std::move(saved),options,result,revision] {
        QtCommon::DiagnosticTimingScope generationTiming("minimap-generate");
        try { *result=options.current?Services::GenerateCurrentMinimapPreview(current):Services::DecodeMinimapPreview(saved);
            if(result->value) result->value->documentRevision=revision;
        } catch(...) { *result=Result<Services::MinimapPreview>::Failure("Minimap.Preview","MINIMAP_PREVIEW_EXCEPTION","小地图预览未完成。"); }
    });
    auto* thread=worker_.data();
    connect(thread,&QThread::finished,this,[this,result,archive,path,game,generation,revision,serial,sessionIdentity,sessionRevision] {
        worker_=nullptr;
        timing_.reset();
        if(serial!=serial_ || generation!=project_.editRevision_.Generation() || revision!=project_.editRevision_.Revision()
            || archive!=project_.archiveSession_ || path!=project_.sourceArchivePath_ || game!=project_.war3GameRoot_
            || project_.HasPendingTerrainCommit() || project_.savingMap_
            || (sessionIdentity && (!project_.mapEdits_ || project_.mapEdits_->SessionIdentity()!=sessionIdentity || project_.mapEdits_->Revision()!=sessionRevision))
            || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) { debounce_.start();return; }
        result_=result;pending_=false;
        if(!result->Succeeded())project_.AddDiagnostics(result->diagnostics);
        emit Changed();
    });
    connect(thread,&QThread::finished,thread,&QObject::deleteLater);
    thread->start(QThread::LowPriority);
}
ProjectMinimapPreview& AgentMinimapPreview(EditorProject& project) {
    auto* value=project.findChild<ProjectMinimapPreview*>("agentMinimapPreview",Qt::FindDirectChildrenOnly);
    if(!value){value=new ProjectMinimapPreview(project,&project);value->setObjectName("agentMinimapPreview");}
    return *value;
}
} // namespace War3Map::QtProject
