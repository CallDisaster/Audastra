#include "AssetsModule.h"
#include "EditorProject.h"
#include "MapSaveJob.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "Modules/ModuleRegistry.h"
#include "Shell/ShellWindow.h"

#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUndoStack>

#include <algorithm>
#include <memory>

using namespace War3Map;

namespace {

QString WriteFile(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
    return path;
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const char c : text) bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

std::vector<std::byte> MemberBytes(const MapArchiveSession& archive, std::string_view name) {
    const auto member = archive.ReadMember(name);
    return member.value.value_or(std::vector<std::byte>{});
}

// 原创合成归档：地图数据 + 两个按钮图标（一个有暂停图标）+ 一个模型。
bool OpenSyntheticMap(const QTemporaryDir& dir, QtProject::EditorProject& project) {
    Image::DecodedImage icon;
    icon.width = 2;
    icon.height = 2;
    icon.rgba.assign(16, std::byte{200});
    const auto blp = Image::EncodeBlpPaletted(icon);
    if (!blp.Succeeded()) return false;
    const std::filesystem::path path = dir.filePath(QStringLiteral("assets.w3x")).toStdWString();
    const std::vector<Mpq::MpqBuildFile> files{
        {"war3map.w3i", Bytes("synthetic"), false},
        {"war3mapImported\\BTNHero.blp", *blp.value, false},
        {"ReplaceableTextures\\CommandButtons\\BTNOk.blp", *blp.value, false},
        {"ReplaceableTextures\\CommandButtonsDisabled\\DISBTNOk.blp", *blp.value, false},
        {"Units\\Foo.mdx", Bytes("MDLX"), false},
    };
    if (!Mpq::WriteMpqArchive(path, files).Succeeded()) return false;
    auto session = MapArchiveSession::Open(path);
    if (!session.Succeeded()) return false;
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*session.value));
    return true;
}


QByteArray ReadDiskFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

struct AssetDropResult {
    Qt::DropAction enterProposed = Qt::IgnoreAction;
    Qt::DropAction dropProposed = Qt::IgnoreAction;
    Qt::DropAction enterAction = Qt::IgnoreAction;
    Qt::DropAction dropAction = Qt::IgnoreAction;
    bool enterAccepted = false;
    bool dropAccepted = false;
};

AssetDropResult SendAssetDrop(QTreeView& view, const QPoint& position, const QMimeData& mime,
    Qt::DropActions possible, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QDragEnterEvent enter(position, possible, &mime, Qt::LeftButton, modifiers);
    const auto enterProposed = enter.proposedAction();
    QApplication::sendEvent(view.viewport(), &enter);
    QDropEvent drop(QPointF(position), possible, &mime, Qt::LeftButton, modifiers);
    const auto dropProposed = drop.proposedAction();
    QApplication::sendEvent(view.viewport(), &drop);
    return {enterProposed, dropProposed, enter.dropAction(), drop.dropAction(),
        enter.isAccepted(), drop.isAccepted()};
}

} // namespace

class AssetsModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void QueueDeduplicatesAndSuggestsPaths();
    void ExternalResourceDropUsesCopyQueueAndJointUndo();
    void InternalResourceDropKeepsMoveContract();
    void ExplorerFoldersMoveRenameDeleteWithUndo();
    void PropertiesDialogValidatesCustomPath();
    void MovesInShellKeepingWidget();
    void PendingReplacementHasOneEffectiveIdentityAndJointUndo();
    void ReplacementMoveAndReturnUseExistingSavePreparation();
    void SharedStackRejectsPartialAndStaleAssetEdits();
    void DuplicatePendingSourcesStayAmbiguous();
    void MovedSourceThenReimportedOriginalPathPreservesBothLayers();
    void NavigationRestoresExactPathAndFilterWithoutEditing();
    void NavigationRefusesMissingPathAndFolderWithoutFallback();
};

void AssetsModuleTests::NavigationRestoresExactPathAndFilterWithoutEditing() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QtProject::EditorProject project; QVERIFY(OpenSyntheticMap(dir, project));
    Modules::AssetsModule module(project); auto* page = module.Widget();
    auto* search = page->findChild<QLineEdit*>("assetSearch"); QVERIFY(search);
    QVERIFY(module.Reveal({"assets", "file", "Units\\Foo.mdx", {}}));
    search->setText("Foo");
    const auto bookmark = module.CaptureNavigationBookmark(); QVERIFY(bookmark.has_value());
    const auto assets = project.CaptureAssets(); const auto undoCount = module.UndoStack()->count();
    QVERIFY(module.Reveal({"assets", "file", "ReplaceableTextures\\CommandButtons\\BTNOk.blp", {}}));
    search->setText("BTN");
    QCOMPARE(module.RestoreNavigationBookmark(*bookmark), Shell::NavigationRestoreResult::Restored);
    QCOMPARE(module.CurrentFolder(), QStringLiteral("Units")); QCOMPARE(search->text(), QStringLiteral("Foo"));
    QCOMPARE(module.FileList()->currentIndex().data(Qt::UserRole + 1).toString(), QStringLiteral("Units\\Foo.mdx"));
    QCOMPARE(module.FileList()->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(project.CaptureAssets(), assets); QCOMPARE(module.UndoStack()->count(), undoCount);
}

void AssetsModuleTests::NavigationRefusesMissingPathAndFolderWithoutFallback() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QtProject::EditorProject project; QVERIFY(OpenSyntheticMap(dir, project));
    Modules::AssetsModule module(project); module.Widget();
    QVERIFY(module.Reveal({"assets", "file", "Units\\Foo.mdx", {}}));
    const auto bookmark = module.CaptureNavigationBookmark(); QVERIFY(bookmark.has_value());
    QVERIFY(module.DeleteAssets({"Units\\Foo.mdx"}).isEmpty());
    module.OpenFolder("ReplaceableTextures\\CommandButtons");
    const auto view = module.SaveViewState(); const auto assets = project.CaptureAssets();
    QCOMPARE(module.RestoreNavigationBookmark(*bookmark), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), view); QCOMPARE(project.CaptureAssets(), assets);
    auto malformed = *bookmark; malformed["folder"] = "MissingFolder";
    QCOMPARE(module.RestoreNavigationBookmark(malformed), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.CurrentFolder(), QStringLiteral("ReplaceableTextures\\CommandButtons"));
    malformed = *bookmark; malformed["filter"] = 7;
    QCOMPARE(module.RestoreNavigationBookmark(malformed), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), view);
}

void AssetsModuleTests::PendingReplacementHasOneEffectiveIdentityAndJointUndo() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QtProject::EditorProject project; QVERIFY(OpenSyntheticMap(dir, project));
    const auto sourceSha = Sha256File(project.archiveSession_->SourcePath()); QVERIFY(sourceSha.Succeeded());
    const auto newFile = WriteFile(dir, "Foo.mdx", "replacement-model"); QVERIFY(!newFile.isEmpty());
    Modules::AssetsModule module(project); module.Widget();
    module.AddFiles({newFile}, "Units");
    QCOMPARE(module.UndoStack(), project.AssetUndoStack());
    const auto before = project.CaptureAssets();
    const auto entries = project.AssetEntries(); QCOMPARE(entries.size(), std::size_t{4});
    const auto replacement = std::find_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.path == "Units\\Foo.mdx"; });
    QVERIFY(replacement != entries.end()); QVERIFY(replacement->Pending() && replacement->Replacement());
    QCOMPARE(replacement->originalPath, QStringLiteral("Units\\Foo.mdx")); QCOMPARE(replacement->size, qint64{17});
    module.OpenFolder("Units"); QCOMPARE(module.ListedRows().size(), 1);
    QVERIFY(module.ListedRows().front().contains(QStringLiteral("待替换")));
    QSignalSpy changed(&project, &QtProject::EditorProject::ImportsChanged);
    QVERIFY(module.DeleteAssets({"units/Foo.mdx"}).isEmpty());
    QCOMPARE(changed.count(), 1); QVERIFY(project.pendingImports_.empty()); QCOMPARE(project.AssetEntries().size(), std::size_t{3});
    QCOMPARE(project.assetEdits_.size(), std::size_t{1}); QVERIFY(project.assetEdits_.front().to.isEmpty());
    module.UndoStack()->undo(); QVERIFY(project.CaptureAssets() == before);
    module.UndoStack()->redo(); QVERIFY(project.pendingImports_.empty()); QCOMPARE(project.AssetEntries().size(), std::size_t{3});
    module.UndoStack()->undo(); QVERIFY(project.CaptureAssets() == before);
    QCOMPARE(MemberBytes(*project.archiveSession_, "Units\\Foo.mdx"), Bytes("MDLX"));
    QCOMPARE(Sha256File(project.archiveSession_->SourcePath()).value.value_or(""), *sourceSha.value);
    QFile pending(newFile); QVERIFY(pending.open(QIODevice::ReadOnly)); QCOMPARE(pending.readAll(), QByteArray("replacement-model"));
}

void AssetsModuleTests::ReplacementMoveAndReturnUseExistingSavePreparation() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QtProject::EditorProject project; QVERIFY(OpenSyntheticMap(dir, project));
    project.sourceArchivePath_ = project.archiveSession_->SourcePath(); project.projectRoot_ = dir.path().toStdWString(); project.ResetEditTracking();
    const auto sourceSha = Sha256File(project.sourceArchivePath_); QVERIFY(sourceSha.Succeeded());
    const auto newFile = WriteFile(dir, "Foo.mdx", "replacement-model"); QVERIFY(!newFile.isEmpty());
    Modules::AssetsModule module(project); module.Widget(); module.AddFiles({newFile}, "Units");
    const auto beforeMove = project.CaptureAssets();
    QVERIFY(module.RenameAsset("Units\\Foo.mdx", "Models\\New.mdx").isEmpty());
    QCOMPARE(project.pendingImports_.front().archivePath, QStringLiteral("Models\\New.mdx"));
    QCOMPARE(project.assetEdits_.size(), std::size_t{1}); QVERIFY(project.assetEdits_.front().to.isEmpty());
    const auto savedState = project.CaptureAssets();
    const auto output = std::filesystem::path(dir.filePath("replacement-candidate.w3x").toStdWString());
    auto job = project.PrepareMapSave(output, false); QVERIFY(job);
    QVERIFY(job->archive->PreflightSave(job->replacements, job->removals, output).Succeeded());
    for (const auto& removed : job->removals) for (const auto& replacement : job->replacements)
        QVERIFY(QtProject::EditorProject::AssetKey(QString::fromStdString(removed)) != QtProject::EditorProject::AssetKey(QString::fromStdString(replacement.name)));
    const auto written = job->archive->SaveCandidate(output, job->replacements, job->removals); QVERIFY(written.Succeeded());
    const auto reopened = MapArchiveSession::Open(output); QVERIFY(reopened.Succeeded());
    QVERIFY(!reopened.value->HasMember("Units\\Foo.mdx"));
    QCOMPARE(MemberBytes(*reopened.value, "Models\\New.mdx"), Bytes("replacement-model"));
    const auto imp = Resources::ImportTable::Parse(MemberBytes(*reopened.value, "war3map.imp")); QVERIFY(imp.Succeeded());
    QVERIFY(std::any_of(imp.value->Entries().begin(), imp.value->Entries().end(), [](const auto& entry) { return entry.path == "Models\\New.mdx"; }));
    QVERIFY(project.CaptureAssets() == savedState);
    module.UndoStack()->undo(); QVERIFY(project.CaptureAssets() == beforeMove);
    module.UndoStack()->redo(); QVERIFY(project.CaptureAssets() == savedState);
    QVERIFY(module.RenameAsset("Models\\New.mdx", "units/Foo.mdx").isEmpty());
    QVERIFY(project.assetEdits_.empty());
    const auto returnedOutput = std::filesystem::path(dir.filePath("returned-candidate.w3x").toStdWString());
    auto returned = project.PrepareMapSave(returnedOutput, false); QVERIFY(returned);
    QVERIFY(returned->archive->PreflightSave(returned->replacements, returned->removals, returnedOutput).Succeeded());
    QVERIFY(returned->archive->SaveCandidate(returnedOutput, returned->replacements, returned->removals).Succeeded());
    const auto back = MapArchiveSession::Open(returnedOutput); QVERIFY(back.Succeeded());
    QCOMPARE(MemberBytes(*back.value, "Units\\Foo.mdx"), Bytes("replacement-model"));
    QCOMPARE(Sha256File(project.sourceArchivePath_).value.value_or(""), *sourceSha.value);
}

void AssetsModuleTests::SharedStackRejectsPartialAndStaleAssetEdits() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QtProject::EditorProject project; QVERIFY(OpenSyntheticMap(dir, project));
    Modules::AssetsModule first(project), second(project); first.Widget(); second.Widget();
    QCOMPARE(first.UndoStack(), second.UndoStack());
    const auto before = project.CaptureAssets(); const auto index = first.UndoStack()->index();
    QSignalSpy changed(&project, &QtProject::EditorProject::ImportsChanged);
    QVERIFY(!first.DeleteAssets({"Units\\Foo.mdx", "Missing.mdx"}).isEmpty());
    QVERIFY(project.CaptureAssets() == before); QCOMPARE(first.UndoStack()->index(), index); QCOMPARE(changed.count(), 0);
    QVERIFY(first.RenameAsset("Units\\Foo.mdx", "Models\\Foo.mdx").isEmpty());
    QVERIFY(second.RenameAsset("Models\\Foo.mdx", "Models\\Bar.mdx").isEmpty());
    QCOMPARE(first.UndoStack()->count(), 2);
    first.UndoStack()->undo(); QCOMPARE(project.assetEdits_.front().to, QStringLiteral("Models\\Foo.mdx"));
    second.UndoStack()->undo(); QVERIFY(project.CaptureAssets() == before);
    first.UndoStack()->redo();
    QVERIFY(project.MoveAsset("Models\\Foo.mdx", "Models\\External.mdx").isEmpty());
    const auto external = project.CaptureAssets(); const int signalCount = changed.count();
    first.UndoStack()->undo(); QVERIFY(project.CaptureAssets() == external); QCOMPARE(changed.count(), signalCount);
    project.NotifyImportsChanged(); QCOMPARE(first.UndoStack()->count(), 0);
}

void AssetsModuleTests::DuplicatePendingSourcesStayAmbiguous() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QtProject::EditorProject project; QVERIFY(OpenSyntheticMap(dir, project));
    const auto a = WriteFile(dir, "a.mdx", "first"); const auto b = WriteFile(dir, "b.mdx", "second");
    project.pendingImports_ = {{"Units\\Foo.mdx", a, 5}, {"units/foo.mdx", b, 6}};
    const auto before = project.CaptureAssets();
    const auto entries = project.AssetEntries(); QCOMPARE(entries.size(), std::size_t{4});
    QVERIFY(std::any_of(entries.begin(), entries.end(), [](const auto& entry) { return entry.ambiguous; }));
    Modules::AssetsModule module(project); module.Widget();
    QVERIFY(!module.DeleteAssets({"Units\\Foo.mdx"}).isEmpty());
    QVERIFY(!module.RenameAsset("Units\\Foo.mdx", "Models\\Foo.mdx").isEmpty());
    QVERIFY(project.CaptureAssets() == before); QCOMPARE(module.UndoStack()->count(), 0);
}

void AssetsModuleTests::MovedSourceThenReimportedOriginalPathPreservesBothLayers() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QtProject::EditorProject project; QVERIFY(OpenSyntheticMap(dir, project));
    project.sourceArchivePath_ = project.archiveSession_->SourcePath(); project.projectRoot_ = dir.path().toStdWString(); project.ResetEditTracking();
    const auto sourceSha = Sha256File(project.sourceArchivePath_); QVERIFY(sourceSha.Succeeded());
    Modules::AssetsModule module(project); module.Widget();
    QVERIFY(module.RenameAsset("Units\\Foo.mdx", "Models\\Old.mdx").isEmpty());
    const auto movedOnly = project.CaptureAssets();
    const auto newFile = WriteFile(dir, "Foo.mdx", "replacement-model"); QVERIFY(!newFile.isEmpty()); module.AddFiles({newFile}, "Units");
    QCOMPARE(project.assetEdits_.front().to, QStringLiteral("Models\\Old.mdx"));
    const auto bothLayers = project.CaptureAssets();
    const auto output = std::filesystem::path(dir.filePath("moved-and-reimported.w3x").toStdWString());
    auto job = project.PrepareMapSave(output, false); QVERIFY(job);
    QVERIFY(job->archive->PreflightSave(job->replacements, job->removals, output).Succeeded());
    QVERIFY(job->archive->SaveCandidate(output, job->replacements, job->removals).Succeeded());
    const auto two = MapArchiveSession::Open(output); QVERIFY(two.Succeeded());
    QCOMPARE(MemberBytes(*two.value, "Models\\Old.mdx"), Bytes("MDLX"));
    QCOMPARE(MemberBytes(*two.value, "Units\\Foo.mdx"), Bytes("replacement-model"));
    const auto imports = Resources::ImportTable::Parse(MemberBytes(*two.value, "war3map.imp")); QVERIFY(imports.Succeeded());
    for (const auto path : {"Models\\Old.mdx", "Units\\Foo.mdx"})
        QVERIFY(std::any_of(imports.value->Entries().begin(), imports.value->Entries().end(), [&](const auto& entry) { return entry.path == path; }));
    QVERIFY(module.DeleteAssets({"Models\\Old.mdx"}).isEmpty());
    const auto deletedOld = project.CaptureAssets();
    const auto deletedOutput = std::filesystem::path(dir.filePath("reimported-only.w3x").toStdWString());
    auto deleted = project.PrepareMapSave(deletedOutput, false); QVERIFY(deleted);
    QVERIFY(deleted->archive->PreflightSave(deleted->replacements, deleted->removals, deletedOutput).Succeeded());
    QVERIFY(deleted->archive->SaveCandidate(deletedOutput, deleted->replacements, deleted->removals).Succeeded());
    const auto one = MapArchiveSession::Open(deletedOutput); QVERIFY(one.Succeeded());
    QVERIFY(!one.value->HasMember("Models\\Old.mdx"));
    QCOMPARE(MemberBytes(*one.value, "Units\\Foo.mdx"), Bytes("replacement-model"));
    module.UndoStack()->undo(); QVERIFY(project.CaptureAssets() == bothLayers);
    module.UndoStack()->redo(); QVERIFY(project.CaptureAssets() == deletedOld);
    module.UndoStack()->undo(); module.UndoStack()->undo(); QVERIFY(project.CaptureAssets() == movedOnly);
    module.UndoStack()->redo(); QVERIFY(project.CaptureAssets() == bothLayers);
    QCOMPARE(Sha256File(project.sourceArchivePath_).value.value_or(""), *sourceSha.value);
    QFile pending(newFile); QVERIFY(pending.open(QIODevice::ReadOnly)); QCOMPARE(pending.readAll(), QByteArray("replacement-model"));
}


void AssetsModuleTests::ExternalResourceDropUsesCopyQueueAndJointUndo() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QtProject::EditorProject project; QVERIFY(OpenSyntheticMap(dir, project));
    Modules::AssetsModule module(project);
    auto* page = module.Widget(); page->resize(1100, 600); page->show();
    module.OpenFolder(QStringLiteral("Units"));
    QApplication::processEvents();
    auto* files = page->findChild<QTreeView*>("assetFileList"); QVERIFY(files);
    auto* folders = page->findChild<QTreeView*>("assetFolderTree"); QVERIFY(folders);
    const auto sourceArchive = dir.filePath("assets.w3x");
    const auto sourceArchiveBytes = ReadDiskFile(sourceArchive); QVERIFY(!sourceArchiveBytes.isEmpty());
    const auto disk = WriteFile(dir, "Drop.wav", QByteArray("original-resource-bytes\r\n")); QVERIFY(!disk.isEmpty());
    const auto diskBytes = ReadDiskFile(disk); QVERIFY(!diskBytes.isEmpty());
    const auto before = project.CaptureAssets();
    auto* undo = module.UndoStack(); QVERIFY(undo); QCOMPARE(undo, project.AssetUndoStack());
    const auto initialCount = undo->count();
    QMimeData external; external.setUrls({QUrl::fromLocalFile(disk)});
    const QPoint blank(8, files->viewport()->height() - 8);
    QVERIFY(!files->indexAt(blank).isValid());
    const auto copied = SendAssetDrop(*files, blank, external, Qt::CopyAction | Qt::MoveAction, Qt::ShiftModifier);
    // 先证明构造的反例确实建议Move，再核入口主动改成Copy，避免仅测试默认Copy。
    QCOMPARE(copied.enterProposed, Qt::MoveAction); QCOMPARE(copied.dropProposed, Qt::MoveAction);
    QVERIFY(copied.enterAccepted && copied.dropAccepted);
    QCOMPARE(copied.enterAction, Qt::CopyAction); QCOMPARE(copied.dropAction, Qt::CopyAction);
    QCOMPARE(project.pendingImports_.size(), std::size_t{1});
    QCOMPARE(project.pendingImports_.front().archivePath, QStringLiteral("Units\\Drop.wav"));
    QCOMPARE(undo->count(), initialCount + 1);
    const auto first = project.CaptureAssets();
    undo->undo(); QVERIFY(project.CaptureAssets() == before);
    undo->redo(); QVERIFY(project.CaptureAssets() == first);
    QCOMPARE(ReadDiskFile(disk), diskBytes); QCOMPARE(ReadDiskFile(sourceArchive), sourceArchiveBytes);

    const auto second = WriteFile(dir, "Second.txt", QByteArray("second-resource\n")); QVERIFY(!second.isEmpty());
    const auto secondBytes = ReadDiskFile(second);
    folders->expandAll(); QApplication::processEvents();
    const auto matches = folders->model()->match(folders->model()->index(0, 0), Qt::DisplayRole,
        QStringLiteral("war3mapImported"), 1, Qt::MatchExactly | Qt::MatchRecursive);
    QVERIFY(!matches.isEmpty()); folders->scrollTo(matches.front()); QApplication::processEvents();
    const auto targetRect = folders->visualRect(matches.front()); QVERIFY(!targetRect.isEmpty());
    QMimeData toFolder; toFolder.setUrls({QUrl::fromLocalFile(second)});
    const auto inFolder = SendAssetDrop(*folders, targetRect.center(), toFolder, Qt::CopyAction);
    QVERIFY(inFolder.enterAccepted && inFolder.dropAccepted); QCOMPARE(inFolder.dropAction, Qt::CopyAction);
    QCOMPARE(project.pendingImports_.size(), std::size_t{2});
    QCOMPARE(project.pendingImports_.back().archivePath, QStringLiteral("war3mapImported\\Second.txt"));
    const auto both = project.CaptureAssets();
    undo->undo(); QVERIFY(project.CaptureAssets() == first);
    undo->redo(); QVERIFY(project.CaptureAssets() == both);

    const auto moveOnly = WriteFile(dir, "MoveOnly.txt", QByteArray("do-not-move\n")); QVERIFY(!moveOnly.isEmpty());
    const auto moveBytes = ReadDiskFile(moveOnly);
    QMimeData move; move.setUrls({QUrl::fromLocalFile(moveOnly)});
    const auto beforeRefusal = project.CaptureAssets();
    const auto countBeforeRefusal = undo->count(); const auto indexBeforeRefusal = undo->index();
    const auto rejected = SendAssetDrop(*files, blank, move, Qt::MoveAction, Qt::ShiftModifier);
    QCOMPARE(rejected.enterProposed, Qt::MoveAction); QCOMPARE(rejected.dropProposed, Qt::MoveAction);
    QVERIFY(!rejected.enterAccepted && !rejected.dropAccepted);
    QVERIFY(project.CaptureAssets() == beforeRefusal);
    QCOMPARE(undo->count(), countBeforeRefusal); QCOMPARE(undo->index(), indexBeforeRefusal);
    QCOMPARE(ReadDiskFile(moveOnly), moveBytes);
    QMimeData remote; remote.setUrls({QUrl(QStringLiteral("https://example.invalid/asset.wav"))});
    const auto remoteRejected = SendAssetDrop(*files, blank, remote, Qt::CopyAction);
    QVERIFY(!remoteRejected.enterAccepted && !remoteRejected.dropAccepted);
    QVERIFY(project.CaptureAssets() == beforeRefusal);
    QCOMPARE(undo->count(), countBeforeRefusal); QCOMPARE(undo->index(), indexBeforeRefusal);
    QCOMPARE(ReadDiskFile(disk), diskBytes); QCOMPARE(ReadDiskFile(second), secondBytes);
    QCOMPARE(ReadDiskFile(moveOnly), moveBytes); QCOMPARE(ReadDiskFile(sourceArchive), sourceArchiveBytes);
}

void AssetsModuleTests::InternalResourceDropKeepsMoveContract() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QtProject::EditorProject project; QVERIFY(OpenSyntheticMap(dir, project));
    Modules::AssetsModule module(project);
    auto* page = module.Widget(); page->resize(1100, 600); page->show();
    module.OpenFolder(QStringLiteral("Units")); QApplication::processEvents();
    auto* files = page->findChild<QTreeView*>("assetFileList"); QVERIFY(files);
    auto* folders = page->findChild<QTreeView*>("assetFolderTree"); QVERIFY(folders);
    const auto entry = files->model()->index(0, 0); QVERIFY(entry.isValid());
    QCOMPARE(entry.data(Qt::DisplayRole).toString(), QStringLiteral("Foo.mdx"));
    // 使用实际列表模型生成的MIME，避免手写内部路径替代真实拖出合同。
    std::unique_ptr<QMimeData> internal(files->model()->mimeData({entry})); QVERIFY(internal);
    folders->expandAll(); QApplication::processEvents();
    const auto matches = folders->model()->match(folders->model()->index(0, 0), Qt::DisplayRole,
        QStringLiteral("war3mapImported"), 1, Qt::MatchExactly | Qt::MatchRecursive);
    QVERIFY(!matches.isEmpty()); folders->scrollTo(matches.front()); QApplication::processEvents();
    const auto targetRect = folders->visualRect(matches.front()); QVERIFY(!targetRect.isEmpty());
    const auto before = project.CaptureAssets();
    const auto source = dir.filePath("assets.w3x"); const auto sourceBytes = ReadDiskFile(source);
    const auto moved = SendAssetDrop(*folders, targetRect.center(), *internal, Qt::MoveAction, Qt::ShiftModifier);
    QCOMPARE(moved.enterProposed, Qt::MoveAction); QCOMPARE(moved.dropProposed, Qt::MoveAction);
    QVERIFY(moved.enterAccepted && moved.dropAccepted); QCOMPARE(moved.dropAction, Qt::MoveAction);
    QVERIFY(project.pendingImports_.empty()); QCOMPARE(project.assetEdits_.size(), std::size_t{1});
    QCOMPARE(project.assetEdits_.front().from, QStringLiteral("Units\\Foo.mdx"));
    QCOMPARE(project.assetEdits_.front().to, QStringLiteral("war3mapImported\\Foo.mdx"));
    const auto after = project.CaptureAssets();
    module.UndoStack()->undo(); QVERIFY(project.CaptureAssets() == before);
    module.UndoStack()->redo(); QVERIFY(project.CaptureAssets() == after);
    QCOMPARE(ReadDiskFile(source), sourceBytes);
}

void AssetsModuleTests::QueueDeduplicatesAndSuggestsPaths() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString wave = WriteFile(dir, "hit.wav", "RIFF");
    const QString texture = WriteFile(dir, "Icon.BLP", "BLP1");
    QVERIFY(!wave.isEmpty() && !texture.isEmpty());
    QtProject::EditorProject project;
    QSignalSpy edited(&project, &QtProject::EditorProject::ImportsEdited);
    QSignalSpy changed(&project, &QtProject::EditorProject::ImportsChanged);

    QCOMPARE(project.AddImportFiles({wave, wave, texture, dir.filePath("missing.mdx")}), 3);
    QCOMPARE(project.pendingImports_.size(), std::size_t{2});
    QCOMPARE(project.pendingImports_[0].archivePath, QStringLiteral("Sounds\\hit.wav"));
    QCOMPARE(project.pendingImports_[1].archivePath, QStringLiteral("Textures\\Icon.BLP"));
    QCOMPARE(project.pendingImports_[0].size, qint64{4});
    QCOMPARE(edited.count(), 1);
    QCOMPARE(changed.count(), 1);
    project.NotifyImportsChanged();
    QCOMPARE(edited.count(), 1);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(QtProject::EditorProject::SuggestImportPath("a/b/Unit.mdx"), QStringLiteral("Unit.mdx"));
    QCOMPARE(QtProject::EditorProject::SuggestImportPath("x.j"), QStringLiteral("Scripts\\x.j"));
    // 按钮图标进暴雪标准文件夹（游戏按 CommandButtonsDisabled\DIS + 文件名 找暂停图标）。
    QCOMPARE(QtProject::EditorProject::SuggestImportPath("d:/icons/BTNHero.blp"), QStringLiteral("ReplaceableTextures\\CommandButtons\\BTNHero.blp"));
    QCOMPARE(QtProject::EditorProject::SuggestImportPath("DISBTNHero.blp"), QStringLiteral("ReplaceableTextures\\CommandButtonsDisabled\\DISBTNHero.blp"));
    QCOMPARE(QtProject::EditorProject::SuggestImportPath("PASAura.tga"), QStringLiteral("ReplaceableTextures\\PassiveButtons\\PASAura.tga"));
    QCOMPARE(QtProject::EditorProject::SuggestImportPath("DISPASAura.blp"), QStringLiteral("ReplaceableTextures\\CommandButtonsDisabled\\DISPASAura.blp"));
    QCOMPARE(QtProject::EditorProject::DisabledIconPath("war3mapImported\\PASAura.blp"),
        QStringLiteral("ReplaceableTextures\\CommandButtonsDisabled\\DISPASAura.blp"));
    QVERIFY(QtProject::EditorProject::DisabledIconPath("Units\\Foo.mdx").isEmpty());
}

void AssetsModuleTests::ExplorerFoldersMoveRenameDeleteWithUndo() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QtProject::EditorProject project;
    QVERIFY(OpenSyntheticMap(dir, project));
    Modules::AssetsModule module(project);
    QWidget* page = module.Widget();
    page->resize(1100, 600);
    page->show();

    // 虚拟文件夹树：按归档路径组成；地图自身的数据文件不列出。
    const QStringList folders = module.FolderPaths();
    for (const char* folder : {"", "Units", "war3mapImported", "ReplaceableTextures", "ReplaceableTextures\\CommandButtons"}) {
        QVERIFY2(folders.contains(QString::fromLatin1(folder)), qPrintable(folders.join('|')));
    }
    QCOMPARE(module.AssetCount(), 4);
    // 根目录：只有文件夹（在前）。
    QVERIFY(module.ListedRows().front().startsWith(QStringLiteral("ReplaceableTextures|文件夹")));

    // 缺少 DISBTN 的按钮图标显示感叹号说明；已有暂停图标的不提示。
    module.OpenFolder(QStringLiteral("war3mapImported"));
    QCOMPARE(module.ListedRows().size(), 1);
    QVERIFY2(module.ListedRows().front().contains(QStringLiteral("缺少暂停图标")), qPrintable(module.ListedRows().front()));
    module.OpenFolder(QStringLiteral("ReplaceableTextures\\CommandButtons"));
    QVERIFY(!module.ListedRows().join('\n').contains(QStringLiteral("缺少暂停图标")));

    // 移动到标准文件夹（与拖到文件夹相同的操作）；撤销 / 重做。
    QUndoStack* undo = module.UndoStack();
    QVERIFY(module.MoveAssets({QStringLiteral("war3mapImported\\BTNHero.blp")}, QStringLiteral("ReplaceableTextures\\CommandButtons")).isEmpty());
    QCOMPARE(project.assetEdits_.size(), std::size_t{1});
    QCOMPARE(project.assetEdits_.front().to, QStringLiteral("ReplaceableTextures\\CommandButtons\\BTNHero.blp"));
    QVERIFY(!module.FolderPaths().contains(QStringLiteral("war3mapImported")));
    QVERIFY(module.ListedRows().join('\n').contains(QStringLiteral("已移动")));
    undo->undo();
    QVERIFY(project.assetEdits_.empty());
    QVERIFY(module.FolderPaths().contains(QStringLiteral("war3mapImported")));
    undo->redo();
    QCOMPARE(project.assetEdits_.size(), std::size_t{1});

    // 非法名称（中文）被拒绝，不入撤销栈。
    const int before = undo->count();
    QVERIFY(!module.RenameAsset(QStringLiteral("Units\\Foo.mdx"), QStringLiteral("Units\\英雄.mdx")).isEmpty());
    QCOMPARE(undo->count(), before);

    // 重命名文件夹：其中全部资源一起移动。
    QVERIFY(module.RenameFolder(QStringLiteral("Units"), QStringLiteral("Models")).isEmpty());
    QVERIFY(module.FolderPaths().contains(QStringLiteral("Models")));
    QVERIFY(!module.FolderPaths().contains(QStringLiteral("Units")));

    // 删除（保存时生效）与撤销。
    QVERIFY(module.DeleteAssets({QStringLiteral("Models\\Foo.mdx")}).isEmpty());
    QCOMPARE(module.AssetCount(), 3);
    undo->undo();
    QCOMPARE(module.AssetCount(), 4);

    // 新建的空文件夹在树中出现，导入到当前文件夹。
    module.OpenFolder(QString{});
    module.NewFolder(QStringLiteral("Sounds"));
    QVERIFY(module.FolderPaths().contains(QStringLiteral("Sounds")));
    const QString wave = WriteFile(dir, "hit.wav", "RIFF");
    module.AddFiles({wave}, QStringLiteral("Sounds"));
    QCOMPARE(module.PendingPaths(), QStringList{QStringLiteral("Sounds\\hit.wav")});
    module.OpenFolder(QStringLiteral("Sounds"));
    QVERIFY(module.ListedRows().front().contains(QStringLiteral("待导入")));
    // 待导入文件也可以改名。
    QVERIFY(module.RenameAsset(QStringLiteral("Sounds\\hit.wav"), QStringLiteral("Sounds\\Hit2.wav")).isEmpty());
    QCOMPARE(module.PendingPaths(), QStringList{QStringLiteral("Sounds\\Hit2.wav")});

    const QString deep = QStringLiteral("Textures\\VeryLongDirectoryNameForImportedHudAssets\\CommandButtons\\VariantPreviewResources");
    QVERIFY(module.RenameAsset(QStringLiteral("Sounds\\Hit2.wav"), deep + QStringLiteral("\\Hit2.wav")).isEmpty());
    module.OpenFolder(deep);
    auto* search = page->findChild<QLineEdit*>("assetSearch");
    search->setText(QStringLiteral("Hit2"));
    auto* files = module.FileList();
    files->setCurrentIndex(files->model()->index(0, 0));
    files->setColumnWidth(0, 230);
    const QModelIndex selected = files->currentIndex();
    const int editIndex = undo->index();
    for (const int width : {1100, 480, 640, 1100, 480}) {
        search->setFocus();
        page->resize(width, 600);
        QApplication::processEvents();
        QCOMPARE(page->width(), width);
        QVERIFY(search->hasFocus());
        QCOMPARE(search->text(), QStringLiteral("Hit2"));
        if (width == 480) QVERIFY(page->findChild<QToolButton*>("assetMoreActions")->isVisible() && page->findChild<QPushButton*>("addImportButton")->isVisible());
        QCOMPARE(files->currentIndex(), selected);
        QCOMPARE(files->columnWidth(0), 230);
        QCOMPARE(module.CurrentFolder(), deep);
        QCOMPARE(undo->index(), editIndex);
        const auto shots = qEnvironmentVariable("WAR3_TEST_SCREENSHOT_DIR");
        if (!shots.isEmpty()) QVERIFY(page->grab().save(QDir(shots).filePath(QStringLiteral("assets-%1.png").arg(width))));
    }
    auto* scroll = page->findChild<QScrollArea*>("assetBreadcrumbScroll");
    const auto crumbs = scroll->findChildren<QToolButton*>();
    QVERIFY(scroll->viewport()->rect().intersects(QRect(crumbs.back()->mapTo(scroll->viewport(), QPoint()), crumbs.back()->size())));
    QCOMPARE(crumbs.back()->toolTip(), deep);
    crumbs.front()->setFocus();
    QTest::keyClick(crumbs.front(), Qt::Key_Space);
    QCOMPARE(module.CurrentFolder(), QString{});
    QCOMPARE(undo->index(), editIndex);
}

void AssetsModuleTests::PropertiesDialogValidatesCustomPath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QtProject::EditorProject project;
    QVERIFY(OpenSyntheticMap(dir, project));
    Modules::AssetsModule module(project);
    module.Widget()->show();
    bool checked = false;
    QTimer::singleShot(100, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr && dialog->objectName() == QStringLiteral("assetPropertiesDialog"));
        auto* custom = dialog->findChild<QCheckBox*>(QStringLiteral("assetCustomPath"));
        auto* path = dialog->findChild<QLineEdit*>(QStringLiteral("assetPathEdit"));
        auto* ok = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        // war3mapImported\ 下的文件：默认不是自定义路径，路径不可编辑。
        QVERIFY(!custom->isChecked() && !path->isEnabled());
        QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("assetDisabledIconState"))->text().contains(QStringLiteral("DISBTNHero")));
        custom->setChecked(true);
        path->setText(QStringLiteral("ReplaceableTextures\\CommandButtons\\BTNOk.blp")); // 重名
        QVERIFY(!ok->isEnabled());
        path->setText(QStringLiteral("图标\\BTNHero.blp")); // 非 ASCII
        QVERIFY(!ok->isEnabled());
        path->setText(QStringLiteral("ReplaceableTextures\\CommandButtons\\BTNHero.blp"));
        QVERIFY(ok->isEnabled());
        checked = true;
        ok->click();
    });
    module.EditProperties(QStringLiteral("war3mapImported\\BTNHero.blp"));
    QVERIFY(checked);
    QCOMPARE(project.assetEdits_.size(), std::size_t{1});
    QCOMPARE(project.assetEdits_.front().to, QStringLiteral("ReplaceableTextures\\CommandButtons\\BTNHero.blp"));
}

void AssetsModuleTests::MovesInShellKeepingWidget() {
    QTemporaryDir dir;
    const QString a = WriteFile(dir, "a.wav", "a");
    QtProject::EditorProject project;
    Shell::ModuleRegistry registry;
    auto owned = std::make_unique<Modules::AssetsModule>(project);
    auto* module = owned.get();
    QVERIFY(registry.Register(std::move(owned)));
    QPointer<QWidget> widget;
    {
        Shell::ShellWindow shell(registry, nullptr);
        shell.show();
        shell.ActivateModule("assets");
        widget = module->Widget();
        module->AddFiles({a});
        module->OpenFolder(QStringLiteral("Sounds"));
        shell.PlaceModule("assets", Shell::Placement::Float(40, 40));
        shell.PlaceModule("assets", Shell::Placement::Side());
        shell.PlaceModule("assets", Shell::Placement::Main());
        QCOMPARE(module->Widget(), widget.data());
        QCOMPARE(module->CurrentFolder(), QStringLiteral("Sounds"));
        const QJsonObject state = module->SaveViewState();
        QCOMPARE(state.value("folder").toString(), QStringLiteral("Sounds"));
    }
    QVERIFY(widget);
    QCOMPARE(widget->parentWidget(), nullptr);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    UiKit::ThemeManager theme;
    theme.ApplyTheme(QStringLiteral("graphite"));
    AssetsModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "AssetsModuleTests.moc"
