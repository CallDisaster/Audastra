#include "MainWindow.h"

// 工程层只维护文档修订；主窗口负责刷新可见状态，后续各模块复用同一服务。
std::optional<War3Map::EditScopeSnapshot> MainWindow::CaptureEditScopes() const {
    return project_->CaptureEditScopes();
}

void MainWindow::ResetEditTracking() {
    const QPointer<MainWindow> liveWindow(this);
    const QPointer<War3Map::QtProject::EditorProject> liveProject(project_.get());
    if (!liveProject) return;
    liveProject->ResetEditTracking();
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return;
    liveWindow->UpdateEditDirtyFlags();
}

bool MainWindow::ObserveEditState(const std::string& description) {
    const bool succeeded = project_->ObserveEditState(description);
    if (succeeded) UpdateEditDirtyFlags();
    return succeeded;
}

void MainWindow::ObserveObjectEdit(int typeIndex) {
    project_->ObserveObjectEdit(typeIndex);
    UpdateEditDirtyFlags();
}

void MainWindow::ObserveTriggerEdit() {
    project_->ObserveTriggerEdit();
    UpdateEditDirtyFlags();
}

void MainWindow::UpdateEditDirtyFlags() {
    project_->UpdateEditDirtyFlags();
    if (!project_->editTrackingReady_ || project_->editTrackingFailed_) return;
    UpdateExportAvailability();
    UpdateStatusBar();
}

void MainWindow::CommitMapEditBaseline() {
    project_->CommitMapEditBaseline();
    UpdateEditDirtyFlags();
}
