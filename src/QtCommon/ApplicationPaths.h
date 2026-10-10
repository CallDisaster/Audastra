#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace War3Map::QtCommon {

// 便携包的可写产品目录是 root/bin 的父目录；开发构建只使用 EXE 自己的目录。
inline QString InstallationRoot() {
    const QDir directory(QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
    if (directory.dirName() == QStringLiteral("bin")
        && QFileInfo::exists(directory.filePath(QStringLiteral("../manifest.json"))))
        return QFileInfo(directory.filePath(QStringLiteral(".."))).canonicalFilePath();
    return directory.absolutePath();
}

} // namespace War3Map::QtCommon
