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
    // 安装根：<根>/ui 或旧版 <根>/share/mpq 的上层，且已有 jass 目录。
    std::filesystem::path toolRoot;
    if (uiRoot.filename() == "ui") toolRoot = uiRoot.parent_path();
    else if (uiRoot.filename() == "mpq" && uiRoot.parent_path().filename() == "share") toolRoot = uiRoot.parent_path().parent_path();
    if (!toolRoot.empty() && std::filesystem::is_directory(toolRoot / "jass", error))
        check.injectionTarget = toolRoot / "jass" / std::string(kBundledUiLibrary);
    return check;
}

// 包内文件写到哪些位置：包目录一份；jass/ 下的注入文件在安装根 jass/<包名> 再一份。
std::vector<QString> Destinations(const BundledUiLibraryCheck& check, const QString& relative) {
    std::vector<QString> out{QDir(QString::fromStdWString(check.target.wstring())).filePath(relative)};
    const QString jass = QStringLiteral("jass/");
    if (!check.injectionTarget.empty() && relative.startsWith(jass))
        out.push_back(QDir(QString::fromStdWString(check.injectionTarget.wstring())).filePath(relative.mid(jass.size())));
    return out;
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
    for (const auto& [relative, bytes] : files) {
        for (const QString& path : Destinations(check, relative)) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.readAll() != bytes) {
                check.state = BundledUiLibraryState::Different;
                return check;
            }
        }
    }
    return check;
}

QString InstallBundledUiLibrary(const std::filesystem::path& uiDataRoot) {
    const auto check = CheckBundledUiLibrary(uiDataRoot);
    if (!check.error.isEmpty()) return check.error;
    if (check.state == BundledUiLibraryState::Same) return {};
    for (const auto& [relative, bytes] : BundledFiles()) {
        for (const QString& path : Destinations(check, relative)) {
            if (!QDir().mkpath(QFileInfo(path).absolutePath())) return QObject::tr("无法创建目录：%1").arg(QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                return QObject::tr("无法写入 %1：%2").arg(QDir::toNativeSeparators(path), file.errorString());
        }
    }
    return CheckBundledUiLibrary(uiDataRoot).state == BundledUiLibraryState::Same ? QString()
        : QObject::tr("写入后核对不一致：%1").arg(Native(check.target));
}

} // namespace War3Map::QtProject
