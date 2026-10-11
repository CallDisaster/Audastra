#include "EditorProject.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QFileSystemWatcher>
#include <QTimer>

#include <filesystem>
#include <map>

// 函数库热重载（架构 §4.6）：只读监视，不执行包内任何程序；变化判定以内容指纹为准（时间戳抖动不算）。
namespace War3Map::QtProject {
namespace {

QString PathText(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

bool IsDefinitionText(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    for (auto& character : extension) character = static_cast<wchar_t>(towlower(character));
    return extension == L".txt" || extension == L".j" || extension == L".cfg";
}

} // namespace

std::vector<std::string> EditorProject::DetectUiLibraryChanges() const {
    std::vector<std::string> changed;
    if (activeUiDataRoot_.empty()) {
        return changed;
    }
    auto discovered = UiLibrary::DiscoverPackages(activeUiDataRoot_);
    std::map<std::string, std::string> before;
    for (const auto& package : uiLibraryPackages_) before[package.id] = package.fingerprint;
    std::map<std::string, std::string> after;
    if (discovered.value) {
        for (const auto& package : *discovered.value) after[package.id] = package.fingerprint;
    }
    for (const auto& [id, fingerprint] : after) {
        const auto found = before.find(id);
        if (found == before.end() || found->second != fingerprint) changed.push_back(id);
    }
    for (const auto& [id, fingerprint] : before) {
        if (!after.contains(id)) changed.push_back(id);
    }
    return changed;
}

void EditorProject::WatchUiLibraries() {
    if (uiLibraryWatcher_ == nullptr) {
        uiLibraryWatcher_ = new QFileSystemWatcher(this);
        uiLibraryDebounce_ = new QTimer(this);
        uiLibraryDebounce_->setSingleShot(true);
        uiLibraryDebounce_->setInterval(400); // 编辑器保存常是“写临时文件 + 替换”，合并成一次检查
        const auto schedule = [this](const QString&) { uiLibraryDebounce_->start(); };
        connect(uiLibraryWatcher_, &QFileSystemWatcher::fileChanged, this, schedule);
        connect(uiLibraryWatcher_, &QFileSystemWatcher::directoryChanged, this, schedule);
        connect(uiLibraryDebounce_, &QTimer::timeout, this, [this]() {
            if (savingMap_) {
                uiLibraryDebounce_->start(); // 保存期间不重新载入触发器，保存结束后再检查
                return;
            }
            const auto changed = DetectUiLibraryChanges();
            if (changed.empty()) {
                WatchUiLibraries(); // 替换式保存会让文件脱离监视，重新挂上
                return;
            }
            QStringList ids;
            for (const auto& id : changed) ids << QString::fromStdString(id);
            bool reloaded = false;
            if (!triggerDirty_) {
                // 重新解析定义并重新绑定已打开的触发器；LoadTriggers 会重新扫描包并刷新指纹。
                reloaded = LoadTriggers();
                AddDiagnostic(tr("信息"), tr("函数库"), tr("UI_LIBRARY_RELOADED"),
                    tr("函数库 %1 已在磁盘上变化，已按新定义重新载入触发器。").arg(ids.join(QStringLiteral("、"))));
            } else {
                AddDiagnostic(tr("警告"), tr("函数库"), tr("UI_LIBRARY_CHANGED"),
                    tr("函数库 %1 已在磁盘上变化；触发器有未导出的修改，未自动重新载入。").arg(ids.join(QStringLiteral("、"))));
            }
            WatchUiLibraries();
            emit UiLibrariesChanged(ids, reloaded);
        });
    }
    const QStringList watched = uiLibraryWatcher_->files() + uiLibraryWatcher_->directories();
    if (!watched.isEmpty()) uiLibraryWatcher_->removePaths(watched);
    if (activeUiDataRoot_.empty()) {
        return;
    }
    const std::filesystem::path uiRoot = Trigger::UiCatalog::ResolveUiRoot(activeUiDataRoot_);
    std::error_code error;
    if (!std::filesystem::is_directory(uiRoot, error)) {
        return;
    }
    QStringList paths{PathText(uiRoot)};
    if (std::filesystem::is_regular_file(uiRoot / "config", error)) paths << PathText(uiRoot / "config");
    for (const auto& package : uiLibraryPackages_) {
        const std::filesystem::path directory = uiRoot / IO::PathFromUtf8(package.id);
        if (!std::filesystem::is_directory(directory, error)) continue;
        paths << PathText(directory);
        for (std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            if (it->is_directory(error)) {
                paths << PathText(it->path());
            } else if (IsDefinitionText(it->path())) {
                paths << PathText(it->path());
            }
        }
    }
    uiLibraryWatcher_->addPaths(paths);
}

} // namespace War3Map::QtProject
