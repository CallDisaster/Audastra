#include "AssetOwnedPayload.h"
#include "AssetsModule.h"
#include "EditorProject.h"
#include "TriggerLuaScripts.h"
#include "Widgets/CodeView.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Workspace.h"

#include <QDir>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <Windows.h>

#include <algorithm>

using namespace War3Map;
namespace {
QString Write(const QTemporaryDir& root, const QString& relative, const QByteArray& bytes) {
    const auto path = root.filePath(relative);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return {};
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size()) return {};
    return path;
}
bool OpenDirectory(const QTemporaryDir& root, QtProject::EditorProject& project) {
    auto directory = DirectoryWorkspace::Open(std::filesystem::path(root.path().toStdWString()));
    if (!directory.Succeeded()) return false;
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    project.projectRoot_ = project.directorySnapshot_->Snapshot().root; project.ResetEditTracking(); return true;
}
std::string DirectorySourceText(const QtProject::OwnedAssetSource& source) {
    return {reinterpret_cast<const char*>(source.bytes->data()), source.bytes->size()};
}
std::shared_ptr<const std::vector<std::byte>> Payload(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return std::make_shared<const std::vector<std::byte>>(first, first + text.size());
}
bool Code(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& issue) { return issue.code == code; });
}
bool Symlink(const QString& path, const QString& target, bool directory) {
    const auto flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    BOOL linked = CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(path.utf16()), reinterpret_cast<LPCWSTR>(target.utf16()), flags | 0x2);
    if (!linked && GetLastError() == ERROR_INVALID_PARAMETER)
        linked = CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(path.utf16()), reinterpret_cast<LPCWSTR>(target.utf16()), flags);
    return linked != FALSE;
}
}

class DirectoryAssetTests final : public QObject {
    Q_OBJECT
private slots:
    void LiteralMembersAppearWithoutInventedPrefixAliases() {
        QTemporaryDir root; QVERIFY(root.isValid());
        QVERIFY(!Write(root, "Scripts/one.lua", "return 1\n").isEmpty());
        QVERIFY(!Write(root, "map/Scripts/one.lua", "return 2\n").isEmpty());
        QVERIFY(!Write(root, "resource/Scripts/one.lua", "return 3\n").isEmpty());
        QVERIFY(!Write(root, "map/war3map.wtg", "managed").isEmpty());
        QVERIFY(!Write(root, "table/unit.ini", "managed").isEmpty());
        QtProject::EditorProject project; QVERIFY(OpenDirectory(root, project));
        const auto aggregate = project.directorySnapshot_->Snapshot().aggregateSha256;
        const auto entries = project.AssetEntries(); QCOMPARE(entries.size(), std::size_t{3});
        for (const auto& entry : entries) { QCOMPARE(entry.path, entry.originalPath); QVERIFY(!entry.Pending()); QVERIFY(!entry.ambiguous); }
        const auto ordinary = QtProject::ReadOwnedAsset(project, "scripts/ONE.lua"); QVERIFY(ordinary.Succeeded());
        const auto map = QtProject::ReadOwnedAsset(project, "map/Scripts/one.lua"); QVERIFY(map.Succeeded());
        const auto resource = QtProject::ReadOwnedAsset(project, "resource/Scripts/one.lua"); QVERIFY(resource.Succeeded());
        QCOMPARE(DirectorySourceText(*ordinary.value), std::string("return 1\n")); QCOMPARE(DirectorySourceText(*map.value), std::string("return 2\n")); QCOMPARE(DirectorySourceText(*resource.value), std::string("return 3\n"));
        QCOMPARE(resource.value->path, QStringLiteral("resource\\Scripts\\one.lua"));
        QCOMPARE(resource.value->sourceSha256, aggregate); QVERIFY(QtProject::VerifyOwnedAsset(project, *resource.value).Succeeded());
        QVERIFY(!QtProject::ReadOwnedAsset(project, "one.lua").Succeeded());
        const auto after = project.directorySnapshot_->Rescan(); QVERIFY(after.Succeeded()); QCOMPARE(after.value->aggregateSha256, aggregate);
        QCOMPARE(project.AssetUndoStack()->count(), 0);
    }
    void OwnedOverlayAndSharedUndoKeepOriginalDirectoryBytes() {
        QTemporaryDir root; QVERIFY(root.isValid()); const auto original = Write(root, "Lua/main.lua", "return 1\n"); QVERIFY(!original.isEmpty());
        QtProject::EditorProject project; QVERIFY(OpenDirectory(root, project));
        const auto source = QtProject::ReadOwnedAsset(project, "Lua/main.lua"); QVERIFY(source.Succeeded());
        QVERIFY(project.RecordAssetEdit("directory Lua overlay", [&](QtProject::AssetState& candidate) {
            return QtProject::PutOwnedAsset(project, candidate, "Lua\\main.lua", Payload("return 2\n"));
        }).isEmpty());
        const auto entries = project.AssetEntries(); QCOMPARE(entries.size(), std::size_t{1}); QVERIFY(entries.front().Replacement());
        QCOMPARE(entries.front().originalPath, QStringLiteral("Lua\\main.lua")); QVERIFY(entries.front().runtimeLuaDraft);
        const auto edited = QtProject::ReadOwnedAsset(project, "Lua/main.lua"); QVERIFY(edited.Succeeded()); QCOMPARE(DirectorySourceText(*edited.value), std::string("return 2\n"));
        QVERIFY(!QtProject::VerifyOwnedAsset(project, *source.value).Succeeded());
        project.AssetUndoStack()->undo(); const auto undone = QtProject::ReadOwnedAsset(project, "Lua/main.lua"); QVERIFY(undone.Succeeded());
        QCOMPARE(undone.value->sha256, source.value->sha256); QVERIFY(!undone.value->runtimeLuaDraft);
        project.AssetUndoStack()->redo(); const auto redone = QtProject::ReadOwnedAsset(project, "Lua/main.lua"); QVERIFY(redone.Succeeded()); QCOMPARE(redone.value->sha256, edited.value->sha256);
        QCOMPARE(Sha256File(std::filesystem::path(original.toStdWString())).value.value_or(""), source.value->sha256);
    }
    void SameSizeExternalChangeCannotBecomeTheNewBaseline() {
        QTemporaryDir root; QVERIFY(root.isValid()); const auto path = Write(root, "Lua/main.lua", "return 1\n"); QVERIFY(!path.isEmpty());
        QtProject::EditorProject project; QVERIFY(OpenDirectory(root, project)); const auto source = QtProject::ReadOwnedAsset(project, "Lua/main.lua"); QVERIFY(source.Succeeded());
        QFile changed(path); QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(changed.write("return 9\n"), qint64{9}); changed.close();
        const auto rejected = QtProject::ReadOwnedAsset(project, "Lua/main.lua"); QVERIFY(!rejected.Succeeded()); QVERIFY(Code(rejected.diagnostics, "ASSET_SOURCE_CHANGED"));
        QVERIFY(!QtProject::VerifyOwnedAsset(project, *source.value).Succeeded()); QCOMPARE(project.AssetUndoStack()->count(), 0);
    }
    void CaseDistinctSnapshotMembersRemainAmbiguous() {
        QTemporaryDir root; QVERIFY(root.isValid());
        HANDLE directory = CreateFileW(reinterpret_cast<LPCWSTR>(root.path().utf16()), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (directory == INVALID_HANDLE_VALUE) QSKIP("Owned case-sensitive directory cannot be opened on this host.");
        // FileCaseSensitiveInfo=23：用兼容结构避免旧SDK缺少该结构名。
        struct CaseInformation { ULONG flags = 1; } information;
        const bool enabled = SetFileInformationByHandle(directory, static_cast<FILE_INFO_BY_HANDLE_CLASS>(23), &information, sizeof(information)) != FALSE;
        CloseHandle(directory);
        if (!enabled) QSKIP("This host does not permit an owned NTFS case-sensitive directory fixture.");
        QVERIFY(!Write(root, "Main.lua", "return 1\n").isEmpty()); QVERIFY(!Write(root, "main.lua", "return 2\n").isEmpty());
        QtProject::EditorProject project; QVERIFY(OpenDirectory(root, project));
        const auto entries = project.AssetEntries(); QCOMPARE(entries.size(), std::size_t{1}); QVERIFY(entries.front().ambiguous);
        const auto source = QtProject::ReadOwnedAsset(project, "MAIN.lua"); QVERIFY(!source.Succeeded()); QVERIFY(Code(source.diagnostics, "ASSET_SOURCE_AMBIGUOUS"));
        const auto before = project.CaptureAssets();
        QVERIFY(!project.RecordAssetEdit("refuse ambiguous overlay", [&](QtProject::AssetState& candidate) {
            return QtProject::PutOwnedAsset(project, candidate, "Main.lua", Payload("return 3\n"));
        }).isEmpty()); QVERIFY(project.CaptureAssets() == before); QCOMPARE(project.AssetUndoStack()->count(), 0);
    }
    void LeafReparseCannotRedirectASelectedSnapshotFile() {
        QTemporaryDir root, outside; QVERIFY(root.isValid()); QVERIFY(outside.isValid());
        const auto path = Write(root, "main.lua", "return 1\n"); const auto target = Write(outside, "external.lua", "return 1\n"); QVERIFY(!path.isEmpty()); QVERIFY(!target.isEmpty());
        QtProject::EditorProject project; QVERIFY(OpenDirectory(root, project));
        const auto outsideSha = Sha256File(std::filesystem::path(target.toStdWString())); QVERIFY(outsideSha.Succeeded());
        QVERIFY(QFile::remove(path));
        if (!Symlink(path, target, false)) QSKIP("This host cannot create an owned file-symlink fixture.");
        const auto source = QtProject::ReadOwnedAsset(project, "main.lua"); QVERIFY(!source.Succeeded()); QVERIFY(Code(source.diagnostics, "SCRIPT_USER_SOURCE_REPARSE_REJECTED"));
        const auto unchanged = Sha256File(std::filesystem::path(target.toStdWString())); QVERIFY(unchanged.Succeeded());
        QCOMPARE(*unchanged.value, *outsideSha.value); QCOMPARE(project.AssetUndoStack()->count(), 0);
        QVERIFY(QFile::remove(path));
    }
    void AncestorReparseCannotRedirectASelectedSnapshotFile() {
        QTemporaryDir root, outside; QVERIFY(root.isValid()); QVERIFY(outside.isValid());
        QVERIFY(!Write(root, "Lua/main.lua", "return 1\n").isEmpty()); const auto target = Write(outside, "main.lua", "return 1\n"); QVERIFY(!target.isEmpty());
        QtProject::EditorProject project; QVERIFY(OpenDirectory(root, project));
        const auto outsideSha = Sha256File(std::filesystem::path(target.toStdWString())); QVERIFY(outsideSha.Succeeded());
        QVERIFY(QDir(root.path()).rename("Lua", "OriginalLua"));
        if (!Symlink(root.filePath("Lua"), outside.path(), true)) QSKIP("This host cannot create an owned directory-symlink fixture.");
        const auto source = QtProject::ReadOwnedAsset(project, "Lua/main.lua"); QVERIFY(!source.Succeeded()); QVERIFY(Code(source.diagnostics, "SCRIPT_USER_SOURCE_REPARSE_REJECTED"));
        const auto unchanged = Sha256File(std::filesystem::path(target.toStdWString())); QVERIFY(unchanged.Succeeded());
        QCOMPARE(*unchanged.value, *outsideSha.value); QCOMPARE(project.AssetUndoStack()->count(), 0);
        QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(root.filePath("Lua").utf16())) != FALSE);
    }
    void ReadBudgetsAndTraversalNeverReadOutsideSnapshot() {
        QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(!Write(root, "main.lua", "return 1\n").isEmpty());
        QtProject::EditorProject project; QVERIFY(OpenDirectory(root, project));
        const auto tooSmall = QtProject::ReadOwnedAsset(project, "main.lua", 8); QVERIFY(!tooSmall.Succeeded()); QVERIFY(Code(tooSmall.diagnostics, "ASSET_TEXT_BUDGET"));
        QVERIFY(!QtProject::ReadOwnedAsset(project, "../main.lua").Succeeded()); QVERIFY(!QtProject::ReadOwnedAsset(project, root.filePath("main.lua")).Succeeded());
        QVERIFY(!QtProject::ReadOwnedAsset(project, "main.lua:other").Succeeded()); QCOMPARE(project.AssetUndoStack()->count(), 0);
        const auto exact = QtProject::ReadOwnedAsset(project, "main.lua", 9); QVERIFY(exact.Succeeded()); QCOMPARE(DirectorySourceText(*exact.value), std::string("return 1\n"));
    }
    void DirectoryUiReportsPartialSnapshotAndKeepsNewPathGate() {
        QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(!Write(root, "Lua/main.lua", "return 1\n").isEmpty());
        QtProject::EditorProject project; QVERIFY(OpenDirectory(root, project));
        Modules::AssetsModule assets(project); auto* page = assets.Widget(); auto* summary = page->findChild<QLabel*>("assetSummary"); QVERIFY(summary);
        QVERIFY(summary->text().contains(QStringLiteral("可能不完整"))); QVERIFY(!summary->text().contains(QStringLiteral("全图")));
        Modules::TriggerLuaScripts scripts(project); QVERIFY(scripts.ScriptPaths().contains("Lua\\main.lua")); QVERIFY(scripts.SelectScript("Lua/main.lua"));
        QCOMPARE(scripts.Editor()->toPlainText(), QStringLiteral("return 1\n")); QVERIFY(!scripts.Editor()->isReadOnly());
        QVERIFY(!scripts.CreateScript("Lua/new.lua")); QVERIFY(scripts.StatusText().contains(QStringLiteral("不完整")));
        QCOMPARE(project.AssetUndoStack()->count(), 0); QVERIFY(!QFile::exists(root.filePath("Lua/new.lua")));
    }
    void ResourcePropertiesShowDirectorySnapshotAndLiteralOriginalPath() {
        QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(!Write(root, "resource/Scripts/main.lua", "return 1\n").isEmpty());
        QtProject::EditorProject project; QVERIFY(OpenDirectory(root, project));
        const auto aggregate = project.directorySnapshot_->Snapshot().aggregateSha256;
        Modules::AssetsModule assets(project); auto* page = assets.Widget(); page->show();
        QString sourceText;
        QObject observationScope;
        QTimer::singleShot(0, &observationScope, [&] {
            if (auto* dialog = page->findChild<QDialog*>("assetPropertiesDialog")) {
                if (auto* source = dialog->findChild<QLabel*>("assetSource")) sourceText = source->text();
                dialog->reject();
            }
        });
        assets.EditProperties("resource\\Scripts\\main.lua");
        QCOMPARE(sourceText, QStringLiteral("目录快照（原路径 resource\\Scripts\\main.lua）"));
        QCOMPARE(project.AssetUndoStack()->count(), 0);
        const auto after = project.directorySnapshot_->Rescan(); QVERIFY(after.Succeeded()); QCOMPARE(after.value->aggregateSha256, aggregate);
    }
};

QTEST_MAIN(DirectoryAssetTests)
#include "DirectoryAssetTests.moc"
