#include "MainWindow.h"
#include "AppHelpers.h"
#include "AssetsModule.h"
#include "MapEntitiesModule.h"
#include "ObjectModule.h"
#include "TriggerModule.h"
#include "VariablesModule.h"
#include <QTemporaryDir>
#include "TriggerCommands.h"

#include "War3MapCore/CompatibilityProfile.h"
#include "War3MapCore/EditorSession.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Workspace.h"
#include "Icons/IconProvider.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QSortFilterProxyModel>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTextDocument>
#include <QIcon>
#include <QPixmap>
#include <QToolBar>
#include <QTreeView>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

// 本机资源设置：Warcraft III 安装目录、扩展图标目录与触发器定义（UI 数据根）。

using namespace AppDetail;

void MainWindow::LoadUiDataRootFromSettings() {
    QSettings settings;
    const QString saved = settings.value(QStringLiteral("ui/dataRoot")).toString();
    if (saved.isEmpty()) {
        return;
    }
    const std::filesystem::path root = NormalizeUiDataRoot(
        std::filesystem::path(saved.toStdWString()));
    if (!HasUiCatalogConfig(root)) {
        AddDiagnostic(
            tr("警告"),
            tr("UI"),
            tr("UI_ROOT_SETTING_INVALID"),
            tr("已保存的 UI 数据根当前不可用；项目层仍可正常载入：%1")
                .arg(QString::fromStdWString(root.wstring())));
        return;
    }
    project_->uiDataRoot_ = root;
}

bool MainWindow::UseUiDataRoot(const std::filesystem::path& path) {
    // 仅本次会话生效，不写入 QSettings；外部根只读使用。
    const std::filesystem::path root = NormalizeUiDataRoot(path);
    if (!HasUiCatalogConfig(root)) return false;
    project_->uiDataRoot_ = root;
    return true;
}

void MainWindow::LoadGameRootFromSettings() {
    QSettings settings;
    const QString saved = settings.value(QStringLiteral("war3/gameRoot")).toString();
    if (!saved.isEmpty()) {
        const std::filesystem::path root(saved.toStdWString());
        if (ApplyGameRoot(root, /*announce=*/false)) {
            // 额外图标目录（可选，YDWE 自带纹理回退）。
            const QString extraDir =
                settings.value(QStringLiteral("war3/extraIconDir")).toString();
            if (!extraDir.isEmpty()) {
                project_->extraIconDir_ = std::filesystem::path(extraDir.toStdWString());
                if (project_->iconProvider_ != nullptr) {
                    project_->iconProvider_->SetExtraIconDir(project_->extraIconDir_);
                }
            }
            return;
        }
    }
    // 首次启动或路径失效：提示（非强制，图标会优雅回退）。
    statusBar()->showMessage(
        tr("尚未设置 Warcraft III 路径：触发器将使用程序化图标。点“魔兽资源”可启用真实图标。"),
        10000);
}

bool MainWindow::ApplyGameRoot(const std::filesystem::path& war3Root, bool announce) {
    // 先校验。
    auto validation = War3Map::Game::GameDataSource::Validate(war3Root);
    if (!validation.Usable()) {
        if (announce) {
            QMessageBox::warning(
                this, tr("魔兽资源"),
                tr("所选目录不是有效的 Warcraft III 安装目录（需包含 war3.mpq 与 war3x.mpq）。\n\n%1")
                    .arg(QString::fromStdString(validation.Summary())));
        }
        return false;
    }
    if (project_->iconProvider_ == nullptr) {
        project_->iconProvider_ = std::make_unique<War3Map::QtApp::IconProvider>();
    }
    if (!project_->iconProvider_->SetGameRoot(war3Root)) {
        if (announce) {
            QMessageBox::warning(this, tr("魔兽资源"),
                                 tr("无法挂载游戏 MPQ。"));
        }
        return false;
    }
    project_->war3GameRoot_ = war3Root;
    project_->RefreshWorldEditStrings();
    objectsModule_->RefreshStandardCatalog();
    entitiesModule_->Refresh();

    // SetGameRoot 会清空映射：按当前 Catalog 实际启用的层重新合并，不预设第三方库。
    project_->RefreshUiIconMappings();

    // 持久化。
    QSettings settings;
    settings.setValue(QStringLiteral("war3/gameRoot"),
                      QString::fromStdWString(war3Root.wstring()));

    if (announce) {
        AddDiagnostic(tr("信息"), tr("游戏资源"), tr("GAME_ROOT_SET"),
                      tr("已挂载 Warcraft III 资源，分类图标映射 %1 项。")
                          .arg(project_->iconProvider_->CategoryCount()));
        statusBar()->showMessage(
            tr("已启用真实图标资源：%1")
                .arg(QString::fromStdWString(war3Root.wstring())), 8000);
        // 重载触发器以刷新图标。
        if (!project_->projectRoot_.empty() && project_->triggerDocument_ != nullptr) {
            triggersModule_->RefreshEditor();
        }
    }
    return true;
}

void MainWindow::SelectExtraIconDir() {
    const QString selected = QFileDialog::getExistingDirectory(
        this,
        tr("选择额外图标目录（如 YDWE 安装目录）"),
        project_->extraIconDir_.empty()
            ? QString()
            : QString::fromStdWString(project_->extraIconDir_.wstring()),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) {
        return;
    }
    project_->extraIconDir_ = std::filesystem::path(selected.toStdWString());
    if (project_->iconProvider_ != nullptr) {
        project_->iconProvider_->SetExtraIconDir(project_->extraIconDir_);
    }
    project_->RefreshWorldEditStrings();
    project_->RefreshUiIconMappings();
    QSettings settings;
    settings.setValue(QStringLiteral("war3/extraIconDir"), selected);
    AddDiagnostic(
        tr("信息"), tr("游戏资源"), tr("EXTRA_ICON_DIR_SET"),
        tr("已设置额外图标目录：%1").arg(selected));
    statusBar()->showMessage(tr("已设置额外图标目录：%1").arg(selected), 8000);
    if (!project_->projectRoot_.empty() && project_->triggerDocument_ != nullptr) {
        triggersModule_->RefreshEditor();
    }
}

void MainWindow::SelectGameRoot() {
    const QString start = project_->war3GameRoot_.empty()
        ? QString()
        : QString::fromStdWString(project_->war3GameRoot_.wstring());
    const QString selected = QFileDialog::getExistingDirectory(
        this, tr("选择 Warcraft III 安装目录"), start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) {
        return;
    }
    ApplyGameRoot(std::filesystem::path(selected.toStdWString()), /*announce=*/true);
}

void MainWindow::SelectUiDataRoot() {
    const std::filesystem::path suggested = project_->ResolveUiDataRoot();
    const QString selected = QFileDialog::getExistingDirectory(
        this,
        tr("选择 Warcraft/YDWE UI 数据根"),
        suggested.empty()
            ? QString::fromStdWString(project_->projectRoot_.parent_path().wstring())
            : QString::fromStdWString(suggested.wstring()),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) {
        return;
    }
    const std::filesystem::path externalRoot = NormalizeUiDataRoot(
        std::filesystem::path(selected.toStdWString()));
    if (!HasUiCatalogConfig(externalRoot)) {
        AddDiagnostic(
            tr("错误"), tr("UI"), tr("UI_NO_CONFIG"),
            tr("所选目录不包含 ui/config、share/mpq/config 或 config，已拒绝切换：%1")
                .arg(QString::fromStdWString(externalRoot.wstring())));
        QMessageBox::warning(
            this,
            tr("UI 数据根无效"),
            tr("所选目录不包含 ui/config、share/mpq/config 或 config。\n"
               "请直接选择一个完整的外部 UI 数据根。"));
        return;
    }

    // 在询问用户是否放弃当前修改之前先完整解析候选 Catalog。
    // 仅有 config 文件不代表层路径、编码与定义都可用。
    auto validatedCatalog = War3Map::Trigger::UiCatalog::Load(externalRoot);
    if (!validatedCatalog.Succeeded()) {
        for (const auto& diagnostic : validatedCatalog.diagnostics) {
            AddDiagnostic(
                DiagnosticLevel(diagnostic.severity),
                Utf8(diagnostic.subsystem),
                Utf8(diagnostic.code),
                Utf8(diagnostic.message));
        }
        QMessageBox::warning(
            this,
            tr("UI Catalog 验证失败"),
            tr("所选数据根无法完整解析，已保留当前项目与未导出修改。\n"
               "请查看底部诊断。"));
        return;
    }

    // 已打开项目时，切库前必须让候选 Catalog 真正解析当前 WTG/WCT。
    // config 自身可解析并不代表它包含地图正在引用的所有可选函数定义。
    // 读当前来源（保存后就是刚写出的地图），按切换后重载的同一函数库层序解析，
    // 已安装并启用的函数库（不写进 config）也算在内。
    if (!project_->projectRoot_.empty()) {
        const auto source = project_->ReadTriggerSource();
        if (source.Succeeded() && source.value && source.value->present) {
            auto parsed = project_->ParseTriggersWithUiRoot(externalRoot, source.value->wtg, source.value->wct);
            if (!parsed.Succeeded()) {
                for (const auto& diagnostic : parsed.diagnostics) {
                    AddDiagnostic(
                        DiagnosticLevel(diagnostic.severity),
                        Utf8(diagnostic.subsystem),
                        Utf8(diagnostic.code),
                        Utf8(diagnostic.message));
                }
                QMessageBox::warning(
                    this,
                    tr("UI Catalog 与当前地图不兼容"),
                    tr("候选数据根无法解析当前 WTG/WCT，已保留当前 Catalog、项目与未导出修改。\n"
                       "诊断会显示 WTG 中的原始函数名；请补回对应库后重试。"));
                return;
            }
        }
    }
    if (!ConfirmDiscardPendingEdits(tr("重载 UI 数据"))) {
        return;
    }

    // 只记录并直接使用用户选定的外部根；绝不复制或覆盖其中文件。
    project_->uiDataRoot_ = externalRoot;
    project_->activeUiDataRoot_ = externalRoot;
    QSettings settings;
    settings.setValue(
        QStringLiteral("ui/dataRoot"),
        QString::fromStdWString(externalRoot.wstring()));
    AddDiagnostic(
        tr("信息"), tr("UI"), tr("UI_ROOT_SET"),
        tr("已切换到外部 UI 数据根（仅读）：%1")
            .arg(QString::fromStdWString(externalRoot.wstring())));
    if (!project_->projectRoot_.empty()) {
        project_->LoadTriggers();
        UpdateExportAvailability();
        ActivateModule("triggers");
    } else {
        statusBar()->showMessage(tr("已保存外部 UI 数据根。"), 8000);
    }
}
