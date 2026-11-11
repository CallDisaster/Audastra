#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include "War3MapCore/ApplicationPaths.h"

namespace War3Map::QtCommon {

inline QSettings OpenEditorSettings() {
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope,
        QString::fromLatin1(kEditorSettingsOrganization), QString::fromLatin1(kEditorSettingsApplication));
}

// 便携包的可写产品目录是 root/bin 的父目录；开发构建只使用 EXE 自己的目录。
inline QString InstallationRoot() {
    return QString::fromStdWString(InstallationRootFromExecutableDirectory(
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString())).wstring());
}

} // namespace War3Map::QtCommon
