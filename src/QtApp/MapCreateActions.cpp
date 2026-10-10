#include "MainWindow.h"
#include "AppHelpers.h"
#include "War3MapCore/Services/MapCreateService.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStatusBar>
#include <QThread>
#include <QUuid>
#include <QVBoxLayout>

using namespace AppDetail;

void MainWindow::CreateNewMap() {
    if (project_->savingMap_) return;
    const auto toolRoot = project_->UiToolRoot();
    if (toolRoot.empty() || project_->war3GameRoot_.empty()) {
        QMessageBox::information(this, tr("新建地图"), tr("请先在资源设置中选择 YDWE 与 Warcraft III 目录。"));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("createMapDialog");
    dialog.setWindowTitle(tr("新建地图"));
    dialog.setMinimumWidth(440);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(tr("新地图"), &dialog);
    name->setObjectName("createMapName");
    auto* author = new QLineEdit(&dialog);
    author->setObjectName("createMapAuthor");
    form->addRow(tr("名称"), name);
    form->addRow(tr("作者"), author);
    form->addRow(tr("地形"), new QLabel(tr("洛丹伦夏季 · 平地 · 无水"), &dialog));
    form->addRow(tr("尺寸"), new QLabel(tr("64 × 64（可玩范围 52 × 52）"), &dialog));
    form->addRow(tr("玩家"), new QLabel(tr("1 名人类玩家 · 中心开始点"), &dialog));
    layout->addLayout(form);
    auto* note = new QLabel(tr("空白触发器；未添加对战基地、胜负条件或玩家单位。"), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* error = new QLabel(&dialog);
    error->setWordWrap(true);
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("创建…"));
    layout->addWidget(buttons);
    const auto options = [name, author] {
        War3Map::Services::MapCreateOptions value;
        value.name = name->text().toUtf8().toStdString();
        value.author = author->text().toUtf8().toStdString();
        return value;
    };
    const auto validate = [options, error, buttons] {
        const auto result = War3Map::Services::ValidateMapCreateOptions(options());
        buttons->button(QDialogButtonBox::Ok)->setEnabled(result.Succeeded());
        error->setText(result.Succeeded() ? QString{} : Utf8(result.diagnostics.front().message));
    };
    connect(name, &QLineEdit::textChanged, &dialog, validate);
    connect(author, &QLineEdit::textChanged, &dialog, validate);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validate();
    name->selectAll();
    if (dialog.exec() != QDialog::Accepted) return;
    const auto output = QFileDialog::getSaveFileName(this, tr("创建地图文件"), {}, tr("Warcraft III 地图 (*.w3x)"));
    if (output.isEmpty()) return;
    War3Map::Services::MapCreateRequest request;
    request.options = options();
    request.target = std::filesystem::path(output.toStdWString());
    request.workspace = std::filesystem::temp_directory_path() /
        (L"War3MapEditor-create-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString());
    request.toolRoot = toolRoot;
    request.gameRoot = project_->war3GameRoot_;
    War3Map::Result<War3Map::Services::MapCreateReceipt> result;
    QProgressDialog progress(tr("正在生成地图并检查脚本…"), {}, 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    project_->savingMap_ = true;
    auto worker = std::unique_ptr<QThread>(QThread::create([&request, &result] {
        result = War3Map::Services::CreateMapCandidate(request);
    }));
    QEventLoop loop;
    connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    progress.show();
    loop.exec();
    worker->wait();
    progress.close();
    project_->savingMap_ = false;
    for (const auto& d : result.diagnostics)
        AddDiagnostic(DiagnosticLevel(d.severity), Utf8(d.subsystem), Utf8(d.code), Utf8(d.message));
    if (!result.Succeeded() || !result.value) {
        statusBar()->showMessage(tr("地图未创建 · 查看诊断"), 8000);
        return;
    }
    // 成功后才走既有打开入口；其未保存编辑确认仍有效，取消打开不删除已生成的文件。
    // 已关闭的栈上窗口由局部作用域销毁，避免打开通知同步删除主窗口时被父对象先销毁。
    dialog.setParent(nullptr);
    progress.setParent(nullptr);
    const QPointer<MainWindow> liveWindow(this);
    const QPointer<War3Map::QtProject::EditorProject> liveProject(project_.get());
    const bool opened = OpenMapArchivePath(result.value->path);
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return;
    if (!opened)
        statusBar()->showMessage(tr("地图已创建，未切换当前工程：%1").arg(output), 12000);
    else statusBar()->showMessage(tr("地图已创建 · 脚本检查通过"), 8000);
}
