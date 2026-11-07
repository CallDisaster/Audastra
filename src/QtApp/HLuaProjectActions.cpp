#include "MainWindow.h"
#include "TriggerModule.h"
#include "TriggerLuaScripts.h"
#include "ProjectLuaFolder.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"
#include "War3MapCore/HLua/Build.h"
#include "War3MapCore/HLua/ProjectConfiguration.h"
#include "War3MapCore/Hash.h"
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QHeaderView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QThread>
#include <QUuid>
#include <QVBoxLayout>

namespace HLua=War3Map::HLua;
namespace {
std::filesystem::path Path(const QString& text){return std::filesystem::path(text.toStdWString());}
QString RunRoot() {
    const auto root=QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("hlua-builds");
    if(!QDir().mkpath(root))return {};
    return QDir(root).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
}
}
bool MainWindow::HasHLuaProject() const {
    const auto& folder=project_->LuaFolderSource();
    if(!folder.Active())return false;
    const auto recognized=HLua::RecognizeProject(folder.Root());
    return recognized.Succeeded() && *recognized.value;
}
bool MainWindow::BuildHLuaMap(const std::filesystem::path& target, HLua::BuildTarget mode, bool retainLink, bool idsOnly, bool showIds) {
    if(project_->savingMap_ || !HasHLuaProject() || !triggersModule_->CommitPendingEdits())return false;
    if(project_->luaMapTarget_) {
        project_->AddDiagnostic(tr("错误"),"HLua","HLUA_TARGET_CONFLICT",tr("当前地图还有 Lua 保存档案。请先清除该档案，再构建 h-lua 工程。"));
        return false;
    }
    const auto host=War3Map::QtProject::ValidateHLuaHostEdits(*project_);
    if(!host.Succeeded()){project_->AddDiagnostics(host.diagnostics);ShowStatusMessage(QString::fromStdString(host.diagnostics.front().message));return false;}
    if(settingsModule_->HasPendingDraft() || soundsModule_->HasPendingDraft()) {
        project_->AddDiagnostic(tr("错误"),"HLua","HLUA_HOST_EDITS_UNSUPPORTED",tr("地图设置或声音仍有未纳入h-lua源工程的草稿，未构建或放弃草稿。"));return false;
    }
    QSettings settings;
    HLua::BuildRequest request;
    request.project=project_->LuaFolderSource().Root();request.target=target;request.mode=mode;
    request.gameRoot=Path(settings.value("war3/gameRoot").toString());
    request.retainFolderLink=retainLink;request.idsOnly=idsOnly;
    const auto run=RunRoot();if(run.isEmpty())return false;
    // 工作目录必须由Core独占创建；正常输出及失败证据均留在这次目录旁。
    request.workspace=Path(run);
    const auto resolved=HLua::ResolveProjectFramework(request.project,{},
        {Path(settings.value("hlua/frameworkRoot").toString()),{},{}});
    if(!resolved.Succeeded()){project_->AddDiagnostics(resolved.diagnostics);ShowStatusMessage(tr("h-lua 框架目录不可用"));return false;}
    request.framework=*resolved.value;
    QProgressDialog progress(tr("正在构建 h-lua 工程…"),QString(),0,0,this);
    progress.setObjectName("hluaBuildProgress");progress.setWindowModality(Qt::WindowModal);progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);progress.setAutoClose(false);progress.setAutoReset(false);
    War3Map::Result<HLua::BuildReceipt> result;
    War3Map::Result<HLua::ProjectSnapshot> sources;
    QScopedValueRollback<bool> busy(project_->savingMap_,true);
    auto* worker=QThread::create([&] {
        try {
            sources=HLua::HLuaProjectScanner::Capture(request.project);
            if(!sources.Succeeded()){result.diagnostics=sources.diagnostics;return;}
            request.expectedProjectSha256=sources.value->source.sha256;
            if(!idsOnly && std::filesystem::exists(target)) {
                auto hash=War3Map::Sha256File(target);
                if(!hash.Succeeded()){result.diagnostics=hash.diagnostics;return;}
                request.expectedTargetSha256=*hash.value;
            }
            result=HLua::BuildProject(request,[&](std::string_view stage){
                const auto text=QString::fromUtf8(stage.data(),static_cast<qsizetype>(stage.size()));
                QMetaObject::invokeMethod(&progress,[&progress,text]{progress.setLabelText(text);},Qt::QueuedConnection);
            });
        }catch(...){result=War3Map::Result<HLua::BuildReceipt>::Failure("HLua","HLUA_BUILD_EXCEPTION","h-lua构建未完成，原图及源工程保留。");}
    });
    QEventLoop loop;connect(worker,&QThread::finished,&loop,&QEventLoop::quit);
    worker->start(QThread::LowPriority);progress.show();loop.exec();worker->wait();delete worker;progress.hide();
    project_->AddDiagnostics(result.diagnostics);
    if(!result.Succeeded()){ShowStatusMessage(result.diagnostics.empty()?tr("h-lua 构建失败"):QString::fromStdString(result.diagnostics.front().message));return false;}
    for(const auto& difference:result.value->differences)
        project_->AddDiagnostic(tr("信息"),"HLua","HLUA_SDK_DIFFERENCE",QString::fromStdString(difference));
    if(!result.value->console.empty())project_->AddDiagnostic(tr("信息"),"HLua","HLUA_BUILD_OUTPUT",QString::fromStdString(result.value->console));
    project_->AddDiagnostic(tr("信息"),"HLua","HLUA_IDS",
        tr("名称与 ID 表：%1 个物体，分配回放%2。%3").arg(result.value->objects.size()).arg(result.value->idReplayPassed?tr("通过"):tr("未通过"))
        .arg(QString::fromStdString(HLua::BuildReceiptJson(*result.value,0,128))));
    if(!idsOnly) {
        hluaLastOutputSha256_=result.value->outputSha256;
        hluaBuiltFrameworkSha256_=result.value->inputs.frameworkSha256;
        const auto plan=HLua::PrepareReload(*sources.value,"");
        if(plan.Succeeded())hluaBuiltHslkSha256_=plan.value->hslkSha256;
        hluaAuthorPaths_.clear();
        for(const auto index:sources.value->runtimeFiles)hluaAuthorPaths_.insert(
            War3Map::QtProject::EditorProject::AssetKey(QString::fromStdString(sources.value->source.files[index].path)).toStdString());
        ShowStatusMessage(tr("h-lua 构建完成：%1").arg(QString::fromStdWString(result.value->output.wstring())));
    } else if(showIds) {
        QDialog dialog(this);dialog.setWindowTitle(tr("名称与 ID 表"));dialog.setObjectName("hluaIdTable");dialog.resize(780,520);
        auto* layout=new QVBoxLayout(&dialog);auto* table=new QTableWidget(static_cast<int>(result.value->objects.size()),5,&dialog);
        table->setHorizontalHeaderLabels({tr("名称"),tr("ID"),tr("类型"),tr("父物体"),tr("来源")});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        for(int row=0;row<table->rowCount();++row){const auto& object=result.value->objects[row];
            const QStringList values{QString::fromStdString(object.name),QString::fromStdString(object.id),QString::fromStdString(object.objectClass),
                QString::fromStdString(object.parent),QString::fromStdString(object.source)};
            for(int col=0;col<values.size();++col)table->setItem(row,col,new QTableWidgetItem(values[col]));}
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);layout->addWidget(table);dialog.exec();
    } else ShowStatusMessage(tr("h-lua 检查通过：%1 个物体，ID 回放通过").arg(result.value->objects.size()));
    return true;
}
bool MainWindow::SaveHLuaMap(bool choosePath) {
    if(project_->savingMap_ || !HasHLuaProject())return false;
    auto target=hluaOutputPath_;
    if(choosePath || target.empty()) {
        const auto file=QFileDialog::getSaveFileName(this,tr("保存 h-lua 地图"),
            QString::fromStdWString((target.empty()?project_->sourceArchivePath_:target).wstring()),tr("Warcraft III 地图 (*.w3x)"));
        if(file.isEmpty())return false;
        target=Path(file);
    }
    const auto generation=project_->editRevision_.Generation(),revision=project_->editRevision_.Revision();
    if(!BuildHLuaMap(target,HLua::BuildTarget::Build,true))return false;
    const HLuaOutputAdoption adoption{std::filesystem::absolute(target).lexically_normal(),hluaLastOutputSha256_,generation,revision};
    QScopedValueRollback<std::optional<HLuaOutputAdoption>> reviewed(hluaOutputAdoption_,adoption);
    // 使用正常打开链建立新基线，包含外部源关联；不让旧底图继续充当已保存地图。
    const bool opened=OpenMapArchivePath(target);
    if(opened)hluaOutputPath_=target;
    return opened;
}
bool MainWindow::HLuaOutputAdoptionCurrent(const std::filesystem::path& output,std::string_view sha) const {
    if(!hluaOutputAdoption_ || hluaOutputAdoption_->path!=std::filesystem::absolute(output).lexically_normal()
        || hluaOutputAdoption_->sha256!=sha || hluaOutputAdoption_->generation!=project_->editRevision_.Generation()
        || hluaOutputAdoption_->revision!=project_->editRevision_.Revision())return false;
    return War3Map::QtProject::ValidateHLuaHostEdits(*project_).Succeeded();
}
void MainWindow::HLuaAction(const QString& action) {
    if(project_->savingMap_ || !HasHLuaProject())return;
    if(action=="framework") {
        const auto root=project_->LuaFolderSource().Root();
        const auto config=HLua::ReadProjectConfiguration(root);
        if(!config.Succeeded()){project_->AddDiagnostics(config.diagnostics);return;}
        const auto chosen=QFileDialog::getExistingDirectory(this,tr("选择工程 h-lua 框架目录"),QString::fromStdWString(config.value->frameworkOverride.wstring()));
        if(chosen.isEmpty())return;
        const auto result=HLua::ConfigureProjectFramework(root,Path(chosen),config.value->sha256);
        project_->AddDiagnostics(result.diagnostics);triggersModule_->LuaScripts()->RefreshHLuaToolbar();return;
    }
    if(action=="test"){(void)StartMapTest(true);return;}
    if(action=="inspect") {
        // 检查也走完整字段/ID校验，但不发布地图。
        (void)BuildHLuaMap({},HLua::BuildTarget::Test,false,true);return;
    }
    if(action=="ids"){(void)BuildHLuaMap({},HLua::BuildTarget::Test,false,true,true);return;}
    const auto file=QFileDialog::getSaveFileName(this,tr("构建 h-lua 地图"),{},tr("Warcraft III 地图 (*.w3x)"));
    if(file.isEmpty())return;
    const auto mode=action=="dist"?HLua::BuildTarget::Dist:action=="build"?HLua::BuildTarget::Build:HLua::BuildTarget::Test;
    (void)BuildHLuaMap(Path(file),mode,action=="build");
}
