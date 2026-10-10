#include "EditorProject.h"
#include "War3MapCore/Workspace.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSet>
#include <QSignalBlocker>

namespace War3Map::QtProject {
namespace {
constexpr qsizetype kMaximumWatchPaths = 2048;
QString PathText(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}
} // namespace

void EditorProject::ResetProjectWatch() {
    if (projectWatch_) {
        const QSignalBlocker block(projectWatch_);
        const auto files = projectWatch_->files();
        const auto directories = projectWatch_->directories();
        if (!files.isEmpty()) projectWatch_->removePaths(files);
        if (!directories.isEmpty()) projectWatch_->removePaths(directories);
    }
    sourceChangeSuspected_ = false;
    sourceChangeConfirmed_ = false;
    projectWatchLimited_ = false;
    projectNoticeDismissed_ = false;
    recoveryNoticePath_.clear();
    emit SourceStateChanged();
}

void EditorProject::RefreshProjectWatch() {
    if (!projectWatch_) return;
    {
        const QSignalBlocker block(projectWatch_);
        const auto files = projectWatch_->files();
        const auto directories = projectWatch_->directories();
        if (!files.isEmpty()) projectWatch_->removePaths(files);
        if (!directories.isEmpty()) projectWatch_->removePaths(directories);
        projectWatchLimited_ = false;
        QStringList paths;
        QSet<QString> keys;
        const auto append = [&](const std::filesystem::path& path) {
            if (path.empty()) return;
            const QFileInfo info(PathText(path));
            const auto normalized = QDir::cleanPath(info.absoluteFilePath());
            const auto key = normalized.toCaseFolded();
            if (keys.contains(key)) return;
            keys.insert(key);
            if (paths.size() >= kMaximumWatchPaths || !info.exists()) {
                projectWatchLimited_ = true;
                return;
            }
            paths.push_back(normalized);
        };
        const auto appendFile = [&](const std::filesystem::path& path) {
            if (path.empty()) return;
            // 父目录保留观察，捕获文件被删除后以同名重新创建的情形。
            append(path.parent_path());
            append(path);
        };
        appendFile(projectFilePath_);
        if (!sourceArchivePath_.empty()) {
            appendFile(sourceArchivePath_);
        } else if (directorySnapshot_) {
            append(directorySnapshot_->Snapshot().root.parent_path());
            append(directorySnapshot_->Snapshot().root);
            for (const auto& file : directorySnapshot_->Snapshot().files) {
                if (paths.size() >= kMaximumWatchPaths) {
                    projectWatchLimited_ = true;
                    break;
                }
                const auto path = directorySnapshot_->Snapshot().root / file.relativePath;
                appendFile(path);
            }
        }
        if (!paths.isEmpty() && !projectWatch_->addPaths(paths).isEmpty())
            projectWatchLimited_ = true;
    }
    emit SourceStateChanged();
}

} // namespace War3Map::QtProject
