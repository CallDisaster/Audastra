#include "ProjectMinimapPreview.h"
#include "EditorProject.h"
#include "../QtCommon/DiagnosticTimings.h"
#include "War3MapCore/Resources/ImportTable.h"
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
        [this](bool pending) { if (!pending) Invalidate(); else { ++serial_; result_.reset(); pending_ = requested_; emit Changed(); } });
    if (project_.mapDataUndoStack_) connect(project_.mapDataUndoStack_, &QUndoStack::indexChanged, this, changed);
    for(auto* stack:project_.objectUndoStacks_)if(stack)connect(stack,&QUndoStack::indexChanged,this,changed);
}
ProjectMinimapPreview::~ProjectMinimapPreview() {
    if (worker_) { disconnect(worker_, nullptr, this, nullptr); worker_->wait(); delete worker_.data(); }
}
void ProjectMinimapPreview::Request(MinimapPreviewOptions options, bool force) {
    if (!force && requested_ && options == options_) return;
    if(force)result_.reset();
    options_ = options; requested_ = true; Invalidate();
}
void ProjectMinimapPreview::Invalidate() {
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
        // 未冻结的磁盘导入不能冒充当前资源。保存后由统一来源规则重新捕获。
        if (project_.HasPendingAssetEdits()) {
            failure("MINIMAP_CURRENT_PENDING_ASSETS", "导入资源尚未保存，当前小地图暂不可用。"); return;
        }
        if(std::any_of(project_.objectDirty_.begin(),project_.objectDirty_.end(),[](bool dirty){return dirty;})){
            failure("MINIMAP_CURRENT_PENDING_OBJECTS","物编修改尚未保存，当前小地图暂不可用。");return;
        }
        QtCommon::DiagnosticTimingScope freezeTiming("minimap-freeze");
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
