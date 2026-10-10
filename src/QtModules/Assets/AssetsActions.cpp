#include "AssetsModule.h"

#include "AssetPropertiesDialog.h"
#include "AssetsDetail.h"
#include "EditorProject.h"
#include "Icons/SvgIcons.h"
#include "Modules/ModuleHost.h"

#include <QApplication>
#include <QClipboard>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QScrollArea>
#include <QTreeView>
#include <QUndoCommand>
#include <QUndoStack>

// 资源操作：导入、移动、改名、删除、新建文件夹，全部经 Record 登记撤销；拖放与右键菜单。
namespace War3Map::Modules {

using namespace AssetsDetail;

QString AssetsModule::Record(const QString& text, const std::function<QString(QtProject::AssetState&)>& edit) {
    const auto problem = project_.RecordAssetEdit(text, edit);
    if (!problem.isEmpty()) Status(problem);
    return problem;
}

void AssetsModule::AddFiles(const QStringList& diskPaths, const QString& folder) {
    (void)Widget();
    int added = 0;
    (void)Record(tr("导入文件"), [&](QtProject::AssetState& state) {
        added = project_.AddImportFiles(state, diskPaths, Normalized(folder));
        return added > 0 ? QString{} : tr("没有可以导入的文件。");
    });
    if (added > 0) Status(tr("已添加 %1 个文件到导入列表，保存地图时写入。").arg(added));
}

QString AssetsModule::MoveAssets(const QStringList& paths, const QString& folder) {
    const QString target = Normalized(folder);
    return Record(tr("移动资源"), [&](QtProject::AssetState& state) -> QString {
        for (const QString& item : paths) {
            if (item.startsWith(QLatin1String("folder:"))) {
                const QString source = item.mid(7);
                if (InFolder(target, source)) return tr("不能把文件夹移到它自己里面。");
                const QString destination = JoinPath(target, FileName(source));
                for (const auto& entry : project_.AssetEntries(state)) {
                    const QString path = Normalized(entry.path);
                    if (!InFolder(path, source)) continue;
                    const QString moved = destination + path.mid(source.size());
                    if (const QString failed = project_.MoveAsset(state, path, moved); !failed.isEmpty()) return failed;
                }
                continue;
            }
            if (SameFolder(ParentFolder(item), target)) continue;
            if (const QString failed = project_.MoveAsset(state, item, JoinPath(target, FileName(item))); !failed.isEmpty()) return failed;
        }
        return {};
    });
}

QString AssetsModule::RenameAsset(const QString& path, const QString& newPath) {
    return Record(tr("重命名资源"), [&](QtProject::AssetState& state) { return project_.MoveAsset(state, path, Normalized(newPath)); });
}

QString AssetsModule::RenameFolder(const QString& folder, const QString& newName) {
    if (newName.isEmpty()) return {};
    const QString source = Normalized(folder);
    const QString destination = JoinPath(ParentFolder(source), newName);
    if (const QString problem = QtProject::EditorProject::CheckAssetPath(destination + QStringLiteral("\\x")); !problem.isEmpty()) {
        Status(problem);
        return problem;
    }
    const QString result = Record(tr("重命名文件夹"), [&](QtProject::AssetState& state) -> QString {
        for (const auto& entry : project_.AssetEntries(state)) {
            const QString path = Normalized(entry.path);
            if (!InFolder(path, source)) continue;
            if (const QString failed = project_.MoveAsset(state, path, destination + path.mid(source.size())); !failed.isEmpty()) return failed;
        }
        return {};
    });
    if (result.isEmpty()) {
        if (emptyFolders_.remove(source)) emptyFolders_.insert(destination);
        if (InFolder(folder_, source)) folder_ = destination + folder_.mid(source.size());
        Refresh();
    }
    return result;
}

QString AssetsModule::DeleteAssets(const QStringList& paths) {
    if (paths.isEmpty()) return {};
    const QString result = Record(tr("删除资源"), [&](QtProject::AssetState& state) -> QString {
        for (const QString& path : paths) {
            if (const QString failed = project_.RemoveAsset(state, path); !failed.isEmpty()) return failed;
        }
        return {};
    });
    if (result.isEmpty()) Status(tr("已删除 %1 个资源（保存地图时生效，可撤销）。").arg(paths.size()));
    return result;
}

void AssetsModule::NewFolder(const QString& name) {
    (void)Widget();
    QString folderName = name;
    if (folderName.isEmpty()) {
        bool accepted = false;
        folderName = QInputDialog::getText(page_, tr("新建文件夹"), tr("文件夹名（英文字母、数字与常见符号）："),
            QLineEdit::Normal, QStringLiteral("NewFolder"), &accepted).trimmed();
        if (!accepted || folderName.isEmpty()) return;
    }
    const QString path = JoinPath(folder_, folderName);
    if (const QString problem = QtProject::EditorProject::CheckAssetPath(path + QStringLiteral("\\x")); !problem.isEmpty()) {
        Status(problem);
        return;
    }
    emptyFolders_.insert(path);
    RebuildTree();
    RebuildList();
}

void AssetsModule::EditProperties(const QString& path) {
    const auto entries = project_.AssetEntries();
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return SameFolder(entry.path, path); });
    if (found == entries.end()) return;
    QString iconState;
    if (const QString disabled = QtProject::EditorProject::DisabledIconPath(found->path); !disabled.isEmpty()) {
        const bool missing = project_.MissingDisabledIcons().contains(found->path);
        iconState = missing ? tr("⚠ 缺少 %1，保存地图时自动生成（去色压暗）。").arg(disabled) : tr("已有 %1").arg(disabled);
    }
    const QString original = found->path;
    AssetPropertiesDialog dialog(*found, [this, original](const QString& candidate) -> QString {
        if (const QString problem = QtProject::EditorProject::CheckAssetPath(candidate); !problem.isEmpty()) return problem;
        const QString key = QtProject::EditorProject::AssetKey(candidate);
        for (const auto& entry : project_.AssetEntries()) {
            if (QtProject::EditorProject::AssetKey(entry.path) == key && !SameFolder(entry.path, original)) return tr("已有同名资源。");
        }
        return {};
    }, iconState, page_, project_.directorySnapshot_ != nullptr && project_.archiveSession_ == nullptr);
    if (dialog.exec() == QDialog::Accepted && dialog.Path() != Normalized(original)) {
        (void)RenameAsset(original, dialog.Path());
    }
}

QString AssetsModule::FolderAt(const QModelIndex& index) const {
    const QModelIndex name = index.siblingAtColumn(0);
    if (!name.isValid()) return folder_;
    return name.data(kKindRole).toInt() == 0 ? name.data(kPathRole).toString() : ParentFolder(name.data(kPathRole).toString());
}

void AssetsModule::ShowContextMenu(const QPoint& pos) {
    auto* view = qobject_cast<QTreeView*>(sender());
    if (view == nullptr) return;
    const QModelIndex index = view->indexAt(pos);
    const bool onTree = view == tree_;
    const bool onFolder = index.isValid() && index.siblingAtColumn(0).data(kKindRole).toInt() == 0;
    const QString folder = index.isValid() ? FolderAt(index) : folder_;
    QMenu menu(page_);
    if (!onTree && index.isValid() && !onFolder) {
        const QString path = index.siblingAtColumn(0).data(kPathRole).toString();
        menu.addAction(tr("属性…"), this, [this, path]() { EditProperties(path); })->setShortcut(QKeySequence(Qt::Key_F2));
        menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/copy")), tr("复制完整路径"), this, [path]() {
            QApplication::clipboard()->setText(path);
        });
        menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/trash-2")), tr("删除"), this, [this]() { (void)DeleteAssets(SelectedPaths()); })
            ->setShortcut(QKeySequence::Delete);
        menu.addSeparator();
    }
    if (onFolder && !folder.isEmpty()) {
        menu.addAction(tr("打开"), this, [this, folder]() { OpenFolder(folder); });
        menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/pencil")), tr("重命名文件夹…"), this, [this, folder]() {
            bool accepted = false;
            const QString name = QInputDialog::getText(page_, tr("重命名文件夹"), tr("新名称："), QLineEdit::Normal, FileName(folder), &accepted).trimmed();
            if (accepted) (void)RenameFolder(folder, name);
        });
        menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/trash-2")), tr("删除文件夹及其中的资源"), this, [this, folder]() {
            QStringList paths;
            for (const auto& entry : project_.AssetEntries()) {
                if (InFolder(entry.path, folder)) paths << Normalized(entry.path);
            }
            emptyFolders_.remove(folder);
            if (paths.isEmpty()) Refresh();
            else (void)DeleteAssets(paths);
        });
        menu.addSeparator();
    }
    menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/folder-plus")), tr("新建文件夹…"), this, [this, folder]() {
        folder_ = folder;
        NewFolder({});
    });
    menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/file-plus-2")), tr("导入文件到这里…"), this, [this, folder]() {
        const QStringList chosen = QFileDialog::getOpenFileNames(page_, tr("选择要导入的文件"), QString{}, tr("所有文件 (*.*)"));
        if (!chosen.isEmpty()) AddFiles(chosen, folder);
    });
    menu.exec(view->viewport()->mapToGlobal(pos));
}

bool AssetsModule::HandleDrop(QObject* watched, QEvent* event) {
    QTreeView* view = watched == tree_->viewport() ? tree_.data() : watched == files_->viewport() ? files_.data() : nullptr;
    if (view == nullptr) return false;
    auto* drag = static_cast<QDropEvent*>(event);
    const QMimeData* data = drag->mimeData();
    const bool internal = data->hasFormat(QString::fromLatin1(kMime));
    if (!internal && !data->hasUrls()) return false;
    QStringList externalFiles;
    if (!internal) {
        for (const QUrl& url : data->urls()) {
            if (url.isLocalFile()) externalFiles << url.toLocalFile();
        }
        // 导入只引用磁盘来源，不向外部拖放源回报移动；Move-only 也不能入队。
        if (externalFiles.isEmpty() || !(drag->possibleActions() & Qt::CopyAction)) {
            drag->ignore();
            return true;
        }
    }
    const auto accept = [&] {
        if (internal) drag->acceptProposedAction();
        else {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
        }
    };
    const QModelIndex index = view->indexAt(drag->position().toPoint());
    // 放在文件夹上 = 放进该文件夹；列表空白处 = 当前文件夹。
    const bool onFolder = index.isValid() && index.siblingAtColumn(0).data(kKindRole).toInt() == 0;
    const QString target = onFolder ? index.siblingAtColumn(0).data(kPathRole).toString() : (view == tree_ && index.isValid() ? FolderAt(index) : folder_);
    if (event->type() != QEvent::Drop) {
        const bool useful = !internal || view == tree_ || onFolder;
        if (useful) accept();
        else drag->ignore();
        return true;
    }
    accept();
    if (internal) {
        const QStringList paths = QString::fromUtf8(data->data(QString::fromLatin1(kMime))).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        (void)MoveAssets(paths, target);
    } else {
        AddFiles(externalFiles, target);
    }
    return true;
}

bool AssetsModule::eventFilter(QObject* watched, QEvent* event) {
    if (!rebuilding_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
        event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        (watched == page_ || watched == toolbar_ ||
            (breadcrumbScroll_ && watched == breadcrumbScroll_->viewport()))) {
        QueueResponsiveLayout();
    }
    if (page_ && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop)) {
        if (HandleDrop(watched, event)) return true;
    }
    if (page_ && watched == files_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_F2) {
            const QStringList selected = SelectedPaths();
            if (!selected.isEmpty()) EditProperties(selected.front());
            return true;
        }
        if (key->matches(QKeySequence::Delete)) {
            (void)DeleteAssets(SelectedPaths());
            return true;
        }
        if (key->key() == Qt::Key_Backspace) {
            OpenFolder(ParentFolder(folder_));
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace War3Map::Modules
