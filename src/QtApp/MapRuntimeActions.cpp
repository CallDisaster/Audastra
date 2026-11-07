#include "MainWindow.h"
#include "Commands/TriggerCommands.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "Widgets/ProblemList.h"
#include <QStandardItemModel>
#include <QThread>
#include <QTimer>
#include <QStatusBar>
#include <QMessageBox>
#include <QPushButton>

namespace {
constexpr auto kSubsystem = "地图运行库";
}

void MainWindow::RefreshRuntimeCompatibility() {
    const auto serial = ++runtimeCompatibilitySerial_;
    if (runtimeCompatibilityRunning_) return;
    runtimeCompatibility_ = {};
    runtimeCompatibilityLockedIdentity_.clear();
    project_->ReplaceDiagnostics(tr(kSubsystem), {});
    if (!project_->uiCatalog_ || project_->savingMap_) return;
    const auto state = project_->QueryMapRuntimeState();
    if (!state.Succeeded() || state.value->settings.packageSha256.empty()
        || state.value->settings.mode == War3Map::Services::MapRuntimeMode::Disabled) return;
    const auto generation = state.value->generation;
    const auto settings = state.value->settings;
    const auto sources = project_->runtimePackageSources_;
    const auto tool = project_->compilerDataRoot_.empty() ? project_->UiToolRoot() : project_->compilerDataRoot_;
    const auto ui = project_->compilerDataRoot_.empty() ? project_->ResolveUiDataRoot() : project_->compilerDataRoot_;
    const auto directory = project_->sourceArchivePath_.empty() ? project_->projectRoot_ : project_->sourceArchivePath_.parent_path();
    const auto catalog = *project_->uiCatalog_;
    using Result = War3Map::Result<War3Map::Services::MapRuntimeCompatibility>;
    auto result = std::make_shared<Result>();
    auto* worker = QThread::create([=] {
        try { *result = War3Map::Services::QueryMapRuntimeLibraryCompatibility(settings, sources, tool, ui, directory, catalog); }
        catch (...) { *result = Result::Failure("MapRuntime", "RUNTIME_LIBRARY_QUERY_FAILED", "无法核对当前函数库与地图运行库；保存时仍会重新检查。"); }
    });
    runtimeCompatibilityRunning_ = true;
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, result, settings, generation, serial] {
        runtimeCompatibilityRunning_ = false;
        if (runtimeCompatibilitySerial_ != serial || project_->editRevision_.Generation() != generation) {
            QTimer::singleShot(0, this, &MainWindow::RefreshRuntimeCompatibility); return;
        }
        const auto current = project_->QueryMapRuntimeSettings();
        if (!current.Succeeded() || *current.value != settings) return;
        runtimeCompatibilityLockedIdentity_ = settings.packageSha256;
        runtimeCompatibilityGeneration_ = generation;
        if (result->Succeeded()) runtimeCompatibility_ = *result->value;
        project_->ReplaceDiagnostics(tr(kSubsystem), result->Succeeded() ? result->value->diagnostics : result->diagnostics);
        if (runtimeCompatibility_.action == War3Map::Services::MapRuntimeCompatibilityAction::SwitchRuntime) {
            for (int row = 0; row < project_->diagnosticsModel_->rowCount(); ++row) {
                if (project_->diagnosticsModel_->index(row, 1).data().toString() != tr(kSubsystem)) continue;
                project_->diagnosticsModel_->item(row, 3)->setData(
                    tr("切换到 %1 并保存").arg(QString::fromStdString(runtimeCompatibility_.libraryVersion)),
                    War3Map::UiKit::ProblemList::ActionRole);
            }
        }
        if (runtimeCompatibilityReady_) std::exchange(runtimeCompatibilityReady_, {})();
    });
    worker->start(QThread::LowPriority);
}

void MainWindow::ConfigureRuntimeUpgradeNotice(QMessageBox* notice, std::function<void()> save) {
    const QPointer<QMessageBox> box(notice);
    const auto configure = [this, box, save = std::move(save)] {
        if (!box || runtimeCompatibility_.action != War3Map::Services::MapRuntimeCompatibilityAction::SwitchRuntime) return;
        auto* action = box->addButton(tr("切换到 %1 并保存").arg(QString::fromStdString(runtimeCompatibility_.libraryVersion)), QMessageBox::ActionRole);
        action->setObjectName("runtimeUpgradeAndSave");
        connect(action, &QPushButton::clicked, this, [this, box, save] {
            if (box) box->close();
            QTimer::singleShot(0, this, [this, save] { ApplyRuntimeUpgrade(save); });
        });
    };
    if (runtimeCompatibility_.action == War3Map::Services::MapRuntimeCompatibilityAction::SwitchRuntime) configure();
    else { runtimeCompatibilityReady_ = configure; RefreshRuntimeCompatibility(); }
}

bool MainWindow::ApplyRuntimeUpgrade(const std::function<void()>& save) {
    using namespace War3Map;
    if (project_->savingMap_ || runtimeCompatibility_.action != Services::MapRuntimeCompatibilityAction::SwitchRuntime) return false;
    const auto state = project_->QueryMapRuntimeState();
    if (!state.Succeeded() || state.value->generation != runtimeCompatibilityGeneration_
        || state.value->settings.packageSha256 != runtimeCompatibilityLockedIdentity_) {
        RefreshRuntimeCompatibility(); return false;
    }
    const auto target = runtimeCompatibility_.suggestedPackageSha256;
    const auto approved = QtProject::PublishMapRuntimeSettings(*project_, {state.value->settings.mode, target}, tr("升级地图运行库"));
    if (!approved.Succeeded()) {
        project_->AddDiagnostics(approved.diagnostics);
        statusBar()->showMessage(tr("运行库未切换，请查看错误列表")); return false;
    }
    save();
    return true;
}
