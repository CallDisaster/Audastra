#include "MainWindow.h"
#include "EditorProject.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QStatusBar>

void MainWindow::SelectLuaMapTarget() {
    if (project_->savingMap_) return;
    const auto file = QFileDialog::getOpenFileName(this, tr("选择当前地图的 Lua 保存档案"), {},
        tr("Lua 保存档案 (*.forge-lua *.tsv);;所有文件 (*)"));
    if (file.isEmpty()) return;
    const auto selected = project_->ConfigureLuaMapTarget(std::filesystem::path(file.toStdWString()));
    project_->AddDiagnostics(selected.diagnostics);
    if (!selected.Succeeded()) {
        QStringList reasons;
        for (const auto& diagnostic : selected.diagnostics)
            if (diagnostic.severity == War3Map::DiagnosticSeverity::Error)
                reasons << QString::fromStdString(diagnostic.message);
        QMessageBox::warning(this, tr("Lua 保存档案不可用"), reasons.join(QLatin1Char('\n')));
        return;
    }
    statusBar()->showMessage(tr("已选择 Lua 保存档案"), 10000);
}

void MainWindow::ClearLuaMapTarget() {
    const auto cleared = project_->ClearLuaMapTarget();
    project_->AddDiagnostics(cleared.diagnostics);
    if (cleared.Succeeded()) statusBar()->showMessage(tr("已清除当前 Lua 保存档案；地图文件未改动。"), 6000);
}
