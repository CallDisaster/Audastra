#include "MainWindow.h"
#include "AppHelpers.h"
#include "AssetsModule.h"
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

// 工作区生命周期：打开展开目录 / 地图归档、另存编辑副本、结构验证与旁路候选导出。

using namespace AppDetail;

namespace {
bool WithinOwnedTemporaryRoot(const std::filesystem::path& input, const QTemporaryDir* owner) {
    if (!owner || !owner->isValid()) return false;
    const auto root = QDir::cleanPath(QFileInfo(owner->path()).canonicalFilePath());
    const auto actual = QDir::cleanPath(QFileInfo(QString::fromStdWString(input.wstring())).canonicalFilePath());
    if (root.isEmpty() || actual.isEmpty() || root == QStringLiteral(".") || actual == QStringLiteral(".")) return false;
#ifdef _WIN32
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return actual.compare(root, sensitivity) == 0 || actual.startsWith(root + QLatin1Char('/'), sensitivity);
}
}

// 提取目录是编辑投影；模块收到数据就绪通知前，必须先绑定真实归档身份和基线。
struct MainWindow::ArchiveOpenBinding {
    std::filesystem::path sourcePath;
    std::unique_ptr<War3Map::MapArchiveSession> session;
    std::array<std::unique_ptr<War3Map::ObjectData::NativeObjectDocument>, 7> nativeBaselines;
    std::unique_ptr<QTemporaryDir> workspace;
};

void MainWindow::OpenWorkspace() {
    const QString selected = QFileDialog::getExistingDirectory(
        this,
        tr("选择展开地图目录"),
        project_->projectRoot_.empty()
            ? QString{}
            : QString::fromStdWString(project_->projectRoot_.wstring()),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) {
        return;
    }

    (void)OpenWorkspacePath(std::filesystem::path(selected.toStdWString()));
}

void MainWindow::OpenMapArchive() {
    const QString selected = QFileDialog::getOpenFileName(
        this,
        tr("打开 Warcraft III 地图"),
        project_->projectRoot_.empty()
            ? QString{}
            : QString::fromStdWString(project_->projectRoot_.wstring()),
        tr("Warcraft III 地图与战役 (*.w3x *.w3m *.w3n);;所有文件 (*.*)"));
    if (selected.isEmpty()) {
        return;
    }
    (void)OpenUserMapArchivePath(std::filesystem::path(selected.toStdWString()));
}

bool MainWindow::OpenUserMapArchivePath(const std::filesystem::path& archivePath) {
    if (QString::fromStdWString(archivePath.extension().wstring()).compare(".w3n", Qt::CaseInsensitive) == 0)
        return OpenCampaignPath(archivePath);
    const QPointer<MainWindow> liveWindow(this);
    const QPointer<War3Map::QtProject::EditorProject> liveProject(project_.get());
    const bool opened = OpenMapArchivePath(archivePath);
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return false;
    if (opened && startupMode_ == StartupMode::Normal) RecordUserMap(archivePath);
    return opened;
}

bool MainWindow::OpenMapArchivePath(const std::filesystem::path& archivePath) {
    if (QString::fromStdWString(archivePath.extension().wstring()).compare(".w3n", Qt::CaseInsensitive) == 0)
        return OpenCampaignPath(archivePath);
    if (project_->savingMap_ || changesChecking_) {
        statusBar()->showMessage(tr("保存或检查进行中，完成后再打开地图"), 5000);
        return false;
    }
    if (WithinOwnedTemporaryRoot(archivePath, project_->mapWorkspace_.get())
        || WithinOwnedTemporaryRoot(archivePath, project_->restoredFiles_.get())) {
        AddDiagnostic(tr("错误"), tr("地图"), tr("MAP_MANAGED_SOURCE_REOPEN"),
            tr("此文件属于当前会话的临时工作目录。请打开原地图或工程文件；当前编辑保持不变。"));
        return false;
    }
    setCursor(Qt::WaitCursor);
    auto snapshot = War3Map::MapArchiveSession::Open(archivePath);
    auto opened = War3Map::MpqWorkspace::OpenReadOnly(archivePath);
    if (!opened.Succeeded() || !opened.value.has_value()) {
        unsetCursor();
        for (const auto& diagnostic : opened.diagnostics) {
            AddDiagnostic(
                tr("错误"), Utf8(diagnostic.subsystem), Utf8(diagnostic.code),
                Utf8(diagnostic.message));
        }
        AddDiagnostic(
            tr("错误"), tr("地图"), tr("MPQ_OPEN_FAILED"),
            tr("无法解析地图归档，可能不是有效的 w3x/w3m。"));
        return false;
    }

    // 源归档只读：提取到临时工作目录后走标准目录管线。
    auto workspace = std::make_unique<QTemporaryDir>(
        QDir::temp().filePath(QStringLiteral("War3MapEditor64-map-XXXXXX")));
    if (!workspace->isValid()) {
        unsetCursor();
        AddDiagnostic(tr("错误"), tr("地图"), tr("MAP_WORKSPACE_CREATE_FAILED"),
            tr("无法创建独立地图工作目录，当前编辑保持不变：%1").arg(workspace->errorString()));
        return false;
    }
    const std::filesystem::path workDir(workspace->path().toStdWString());
    const War3Map::Status extractStatus = opened.value->ExtractMapData(workDir);
    unsetCursor();
    if (!extractStatus.Succeeded()) {
        for (const auto& diagnostic : extractStatus.diagnostics)
            AddDiagnostic(DiagnosticLevel(diagnostic.severity), Utf8(diagnostic.subsystem),
                Utf8(diagnostic.code), Utf8(diagnostic.message));
        return false;
    }

    // 将提取的 native 物体文件转换为 LNI 工作格式，供物体编辑器消费。
    // 同时保留 native 基线文档：另存为时把 LNI 编辑差异回灌写回。
    std::vector<War3Map::Diagnostic> importDiagnostics;
    std::array<std::unique_ptr<War3Map::ObjectData::NativeObjectDocument>, 7> nativeBaselines;
    for (std::size_t index = 0; index < kObjectTypes.size(); ++index) {
        const auto type = kObjectTypes[index];
        const std::filesystem::path nativePath =
            workDir / "map" / std::string(War3Map::ObjectData::NativeFileName(type));
        std::error_code fileError;
        if (!std::filesystem::is_regular_file(nativePath, fileError) || fileError) {
            continue;
        }
        const std::vector<std::byte> nativeBytes = ReadBytes(nativePath);
        // 解析 native 并保留基线文档。
        War3Map::ObjectData::NativeObjectCodec nativeCodec(type);
        auto nativeParsed = nativeCodec.Parse(nativeBytes);
        importDiagnostics.insert(
            importDiagnostics.end(),
            nativeParsed.diagnostics.begin(),
            nativeParsed.diagnostics.end());
        if (!nativeParsed.Succeeded() || nativeParsed.document == nullptr) {
            continue;
        }
        auto* nativeDoc = dynamic_cast<War3Map::ObjectData::NativeObjectDocument*>(
            nativeParsed.document.get());
        if (nativeDoc == nullptr) {
            continue;
        }
        nativeParsed.document.release();
        nativeBaselines[index].reset(nativeDoc);
        // 渲染为 LNI 写入工作区 table/。
        auto rendered = War3Map::ObjectData::RenderObjectRecordsToLni(
            type, nativeBaselines[index]->Objects());
        importDiagnostics.insert(
            importDiagnostics.end(),
            rendered.diagnostics.begin(),
            rendered.diagnostics.end());
        if (!rendered.Succeeded() || !rendered.value.has_value()) {
            continue;
        }
        const std::filesystem::path lniPath =
            workDir / "table" / std::string(War3Map::ObjectData::LniFileName(type));
        std::filesystem::create_directories(lniPath.parent_path(), fileError);
        std::ofstream lniOut(lniPath, std::ios::binary);
        if (lniOut) {
            lniOut.write(
                reinterpret_cast<const char*>(rendered.value->data()),
                static_cast<std::streamsize>(rendered.value->size()));
        }
    }

    ArchiveOpenBinding binding{archivePath, {}, std::move(nativeBaselines), std::move(workspace)};
    if (snapshot.Succeeded() && snapshot.value)
        binding.session = std::make_unique<War3Map::MapArchiveSession>(std::move(*snapshot.value));
    const QPointer<MainWindow> liveWindow(this);
    const QPointer<War3Map::QtProject::EditorProject> liveProject(project_.get());
    const bool openedOk = OpenWorkspacePathImpl(workDir, &binding);
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return false;
    if (openedOk) {
        SetProjectTitle(QString::fromStdWString(archivePath.filename().wstring()),
            QString::fromStdWString(archivePath.wstring()));
        objectsModule_->RefreshIcons();
        objectsModule_->UpdateObjectReferenceEntries();
        project_->NotifyImportsChanged();
        UpdateExportAvailability();
        RefreshProjectWatch();
        if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return false;
        ResetEditTracking();
        if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return false;
    }

    // OpenWorkspacePath 会清空诊断，因此提取/导入诊断在其后追加。
    if (openedOk && project_->archiveSession_ && project_->archiveSession_->HasOpaqueMembers())
        AddDiagnostic(tr("警告"), tr("地图归档"), tr("MPQ_OPAQUE_MEMBERS_PRESERVED"),
            tr("资源清单不完整：%1 个未命名成员已保留。此地图的候选仅能替换已有的具名成员。")
                .arg(project_->archiveSession_->OpaqueMemberCount()));
    for (const auto& diagnostic : snapshot.diagnostics)
        AddDiagnostic(DiagnosticLevel(diagnostic.severity), Utf8(diagnostic.subsystem), Utf8(diagnostic.code), Utf8(diagnostic.message));
    for (const auto& diagnostic : extractStatus.diagnostics) {
        const QString level =
            diagnostic.severity == War3Map::DiagnosticSeverity::Error
                ? tr("错误")
                : (diagnostic.severity == War3Map::DiagnosticSeverity::Warning
                       ? tr("警告")
                       : tr("信息"));
        AddDiagnostic(
            level, Utf8(diagnostic.subsystem), Utf8(diagnostic.code),
            Utf8(diagnostic.message));
    }
    for (const auto& diagnostic : importDiagnostics) {
        const QString level =
            diagnostic.severity == War3Map::DiagnosticSeverity::Error
                ? tr("错误")
                : (diagnostic.severity == War3Map::DiagnosticSeverity::Warning
                       ? tr("警告")
                       : tr("信息"));
        AddDiagnostic(
            level, Utf8(diagnostic.subsystem), Utf8(diagnostic.code),
            Utf8(diagnostic.message));
    }
    if (openedOk) {
        statusBar()->showMessage(
            tr("已从地图归档提取并打开：%1（工作目录 %2）")
                .arg(QString::fromStdWString(archivePath.filename().wstring()))
                .arg(QString::fromStdWString(workDir.wstring())),
            8000);
    }
    return openedOk;
}

void MainWindow::SaveAsMap() {
    if (project_->sourceArchivePath_.empty()) {
        QMessageBox::information(
            this, tr("另存为地图"),
            tr("当前工作区不是从 w3x/w3m 打开的，无法重打包。请先用“打开地图”打开一个地图归档。"));
        return;
    }
    (void)SaveAsMapArchive();
}

bool MainWindow::OpenWorkspacePath(const std::filesystem::path& path) {
    return OpenWorkspacePathImpl(path, nullptr);
}

bool MainWindow::OpenWorkspacePathImpl(const std::filesystem::path& path, ArchiveOpenBinding* archive) {
    const QPointer<MainWindow> liveWindow(this);
    const QPointer<War3Map::QtProject::EditorProject> liveProject(project_.get());
    if (project_->savingMap_ || changesChecking_) return false;
    if (WithinOwnedTemporaryRoot(path, project_->mapWorkspace_.get())
        || WithinOwnedTemporaryRoot(path, project_->restoredFiles_.get())) {
        AddDiagnostic(tr("错误"), tr("工程"), tr("WORKSPACE_MANAGED_SOURCE_REOPEN"),
            tr("此目录属于当前会话的临时工作目录。请打开原地图或工程文件；当前编辑保持不变。"));
        return false;
    }
    auto snapshot = War3Map::DirectoryWorkspace::Open(path);
    if (!snapshot.Succeeded() || !snapshot.value) {
        AddDiagnostic(tr("错误"), tr("工程"), tr("WORKSPACE_SNAPSHOT_FAILED"), tr("不能完整读取工作区，当前编辑保持不变"));
        return false;
    }
    if (!ConfirmDiscardPendingEdits(tr("打开其他项目"))) {
        return false;
    }
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return false;
    setCursor(Qt::WaitCursor);
    ResetProjectState();
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return false;
    if (archive) {
        project_->mapWorkspace_ = std::move(archive->workspace);
        project_->sourceArchivePath_ = archive->sourcePath;
        project_->archiveSession_ = std::move(archive->session);
        project_->nativeObjectDocuments_ = std::move(archive->nativeBaselines);
        project_->iconProvider_->SetMapArchive(project_->sourceArchivePath_);
    }
    project_->directorySnapshot_ = std::make_unique<War3Map::DirectoryWorkspace>(std::move(*snapshot.value));
    project_->diagnosticsModel_->removeRows(0, project_->diagnosticsModel_->rowCount());
    project_->projectRoot_ = path;
    project_->iconProvider_->SetMapResourceDirectory(project_->projectRoot_ / "resource");
    const QString selected = QString::fromStdWString(project_->projectRoot_.wstring());
    const War3Map::CompatibilityProfile profile = War3Map::CompatibilityProfile::Default();
    const War3Map::Status profileStatus = profile.Validate();
    if (!profileStatus.Succeeded()) {
        AddDiagnostic(
            tr("错误"), tr("配置"), tr("PROFILE_INVALID"), tr("内置兼容配置校验失败。"));
        SetProjectTitle(tr("打开失败"), {});
        unsetCursor();
        return false;
    }

    std::size_t objectCount = 0;
    std::size_t loadedTypes = 0;
    for (std::size_t index = 0; index < kObjectTypes.size(); ++index) {
        const auto type = kObjectTypes[index];
        const std::filesystem::path relative =
            std::filesystem::path("table") /
            std::string(War3Map::ObjectData::LniFileName(type));
        const std::filesystem::path absolute = project_->projectRoot_ / relative;
        std::error_code fileError;
        const auto fileStatus = std::filesystem::status(absolute, fileError);
        if (!std::filesystem::is_regular_file(fileStatus) || fileError) {
            // 原图没有该类覆盖时，缺少投影 LNI 属正常情况；不隐藏解析失败或来源不可信的诊断。
            if (fileStatus.type() == std::filesystem::file_type::not_found && project_->archiveSession_
                && !project_->archiveSession_->HasMember(War3Map::ObjectData::NativeFileName(type))) continue;
            AddDiagnostic(
                tr("警告"),
                tr("物体数据"),
                tr("LNI_FILE_MISSING"),
                tr("未找到 %1；该类型保持不可用。").arg(QString::fromStdWString(relative.wstring())));
            continue;
        }

        const std::vector<std::byte> bytes = ReadBytes(absolute);
        War3Map::ObjectData::LniObjectCodec codec(type);
        auto parsed = codec.Parse(bytes);
        for (const auto& diagnostic : parsed.diagnostics) {
            AddDiagnostic(
                parsed.Succeeded() ? tr("警告") : tr("错误"),
                Utf8(diagnostic.subsystem),
                Utf8(diagnostic.code),
                Utf8(diagnostic.message));
        }
        if (!parsed.Succeeded() || parsed.document == nullptr) {
            continue;
        }
        War3Map::CodecDocument* rawDocument = parsed.document.release();
        auto* typed = dynamic_cast<War3Map::ObjectData::LniObjectDocument*>(rawDocument);
        if (typed == nullptr) {
            delete rawDocument;
            AddDiagnostic(
                tr("错误"), tr("物体数据"), tr("DOCUMENT_TYPE_MISMATCH"),
                tr("%1 的解析结果类型不正确。").arg(QString::fromStdWString(relative.wstring())));
            continue;
        }
        project_->objectDocuments_[index].reset(typed);
        objectCount += typed->Objects().size();
        ++loadedTypes;
    }

    SetProjectTitle(QFileInfo(selected).fileName(), QDir::toNativeSeparators(selected));
    AddDiagnostic(
        tr("信息"),
        tr("项目"),
        tr("WORKSPACE_OPENED"),
        tr("已只读打开工作区；载入 %1/7 类 LNI，共 %2 个对象。")
            .arg(loadedTypes)
            .arg(objectCount));
    project_->LoadTriggers();
    UpdateExportAvailability();
    LoadMapData();
    ActivateModule(project_->triggerDocument_ != nullptr ? "triggers" : "objects");
    objectsModule_->SelectFirstType();
    objectsModule_->UpdateObjectReferenceEntries();
    RefreshVariablesPage();
    if (project_->triggerDocument_ != nullptr) {
        const War3Map::Trigger::TriggerInspection inspection =
            War3Map::Trigger::WtgWctCodec{}.Inspect(*project_->triggerDocument_);
        statusBar()->showMessage(
            tr("已打开 · %1 个触发器 · %2 个 ECA · %3 个对象 · 源目录只读")
                .arg(inspection.triggerCount)
                .arg(inspection.ecaCount)
                .arg(objectCount));
    } else {
        statusBar()->showMessage(
            tr("已打开 · %1 个对象 · 触发器未就绪 · 源目录只读").arg(objectCount));
    }
    unsetCursor();
    UpdateBreadcrumb();
    RefreshProjectWatch();
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return false;
    ResetEditTracking();
    if (!liveWindow || !liveProject || liveWindow->project_.get() != liveProject.data()) return false;
    UpdateStatusBar();
    UpdateExportAvailability();
    return true;
}

void MainWindow::ValidateWorkspace() {
    const auto summary = project_->ValidateDocuments();
    if (!summary.opened) {
        return;
    }
    if (summary.Passed()) {
        statusBar()->showMessage(summary.triggersValidated
            ? tr("LNI 与 WTG/WCT 结构验证通过") : tr("LNI 结构验证通过；触发器未就绪"));
    } else {
        statusBar()->showMessage(tr("结构验证失败，请查看诊断"));
    }
}

void MainWindow::ExportCandidate() {
    if (!project_->HasPendingCandidateEdits()) {
        AddDiagnostic(tr("警告"), tr("导出"), tr("NO_PENDING_EDIT"), tr("当前没有可导出的事务修改。"));
        return;
    }
    const QString parentDirectory = QFileDialog::getExistingDirectory(this, tr("选择候选输出的父目录"),
        QString::fromStdWString(project_->projectRoot_.parent_path().wstring()),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (parentDirectory.isEmpty()) {
        return;
    }
    bool accepted = false;
    const QString defaultName = QStringLiteral("war3-candidate-%1")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString candidateName = QInputDialog::getText(this, tr("候选目录名称"), tr("新目录名称："),
        QLineEdit::Normal, defaultName, &accepted).trimmed();
    if (!accepted || candidateName.isEmpty()) {
        return;
    }
    if (candidateName == QStringLiteral(".") || candidateName == QStringLiteral("..") ||
        candidateName.contains('/') || candidateName.contains('\\') || candidateName.contains(':')) {
        AddDiagnostic(tr("错误"), tr("导出"), tr("CANDIDATE_NAME_INVALID"),
            tr("候选名称必须是单个安全目录名，不能包含路径分隔符或冒号。"));
        return;
    }
    const std::filesystem::path outputPath =
        std::filesystem::path(parentDirectory.toStdWString()) / candidateName.toStdWString();
    const auto dirtyCount = std::count(project_->objectDirty_.begin(), project_->objectDirty_.end(), true);
    if (QMessageBox::question(this, tr("导出旁路候选"),
            tr("将导出：%1 类物编修改%2\n\n输出目录：\n%3\n\n源目录不会被覆盖。")
                .arg(dirtyCount)
                .arg(project_->triggerDirty_ ? tr("，以及 WTG/WCT 触发器修改") : QString{})
                .arg(QString::fromStdWString(outputPath.wstring()))) != QMessageBox::Yes) {
        return;
    }
    setCursor(Qt::WaitCursor);
    const bool exported = project_->ExportCandidate(outputPath);
    unsetCursor();
    UpdateExportAvailability();
    if (exported) {
        statusBar()->showMessage(tr("旁路候选导出完成 · 源目录未修改"));
    }
}
