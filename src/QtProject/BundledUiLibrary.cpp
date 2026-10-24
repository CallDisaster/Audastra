#include "BundledUiLibrary.h"

#include "War3MapCore/Trigger/UiCatalog.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSaveFile>

#include <algorithm>
#include <system_error>
#include <utility>
#include <vector>

namespace War3Map::QtProject {
namespace {

QString ResourceRoot() { return QStringLiteral(":/audastra/ui-library/") + QString::fromLatin1(kBundledUiLibrary.data(), qsizetype(kBundledUiLibrary.size())); }

// 包内文件：相对路径（正斜杠）与内容，按路径排序。
std::vector<std::pair<QString, QByteArray>> BundledFiles() {
    std::vector<std::pair<QString, QByteArray>> files;
    const QString root = ResourceRoot();
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        files.emplace_back(path.mid(root.size() + 1), file.readAll());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return files;
}

BundledUiLibraryCheck Locate(const std::filesystem::path& uiDataRoot) {
    BundledUiLibraryCheck check;
    std::error_code error;
    const auto uiRoot = Trigger::UiCatalog::ResolveUiRoot(uiDataRoot);
    if (uiDataRoot.empty() || !std::filesystem::is_regular_file(uiRoot / "config", error)) {
        check.error = QObject::tr("没有设置有效的触发器数据根（找不到 ui\\config）。");
        return check;
    }
    check.target = uiRoot / std::string(kBundledUiLibrary);  // ASCII 包名
    return check;
}

QString Native(const std::filesystem::path& path) { return QDir::toNativeSeparators(QString::fromStdWString(path.wstring())); }

} // namespace

BundledUiLibraryCheck CheckBundledUiLibrary(const std::filesystem::path& uiDataRoot) {
    auto check = Locate(uiDataRoot);
    if (!check.error.isEmpty()) return check;
    const auto files = BundledFiles();
    if (files.empty()) {
        check.error = QObject::tr("编辑器里没有自带的 %1 函数库。").arg(QString::fromLatin1(kBundledUiLibrary.data(), qsizetype(kBundledUiLibrary.size())));
        return check;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(check.target, error)) {
        check.state = BundledUiLibraryState::Missing;
        return check;
    }
    check.state = BundledUiLibraryState::Same;
    const QDir target(QString::fromStdWString(check.target.wstring()));
    for (const auto& [relative, bytes] : files) {
        QFile file(target.filePath(relative));
        if (!file.open(QIODevice::ReadOnly) || file.readAll() != bytes) {
            check.state = BundledUiLibraryState::Different;
            break;
        }
    }
    return check;
}

QString InstallBundledUiLibrary(const std::filesystem::path& uiDataRoot) {
    const auto check = CheckBundledUiLibrary(uiDataRoot);
    if (!check.error.isEmpty()) return check.error;
    if (check.state == BundledUiLibraryState::Same) return {};
    const QDir target(QString::fromStdWString(check.target.wstring()));
    for (const auto& [relative, bytes] : BundledFiles()) {
        const QString path = target.filePath(relative);
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return QObject::tr("无法创建目录：%1").arg(QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return QObject::tr("无法写入 %1：%2").arg(QDir::toNativeSeparators(path), file.errorString());
    }
    return CheckBundledUiLibrary(uiDataRoot).state == BundledUiLibraryState::Same ? QString()
        : QObject::tr("写入后核对不一致：%1").arg(Native(check.target));
}

} // namespace War3Map::QtProject
