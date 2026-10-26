#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include "War3MapCore/ApplicationPaths.h"

namespace War3Map::QtCommon {

// 便携包的可写产品目录是 root/bin 的父目录；开发构建只使用 EXE 自己的目录。
inline QString InstallationRoot() {
    return QString::fromStdWString(InstallationRootFromExecutableDirectory(
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString())).wstring());
}

} // namespace War3Map::QtCommon
