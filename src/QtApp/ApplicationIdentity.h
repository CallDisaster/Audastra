#pragma once

#include <QCoreApplication>
#include <QGuiApplication>
#include "Brand/ApplicationBrand.h"
#include "AudastraVersion.generated.h"

namespace War3Map::QtApp {

// 产品身份决定 Qt 的设置与数据目录；本地开发版改名不迁移或删除旧目录。
inline void ConfigureApplicationIdentity() {
    QCoreApplication::setApplicationName(QStringLiteral("Audastra"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Audastra"));
    QGuiApplication::setWindowIcon(UiKit::ApplicationIcon());
    QCoreApplication::setOrganizationName(QStringLiteral("CallDisaster"));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(War3Map::BuildInfo::Version.data(),
        static_cast<qsizetype>(War3Map::BuildInfo::Version.size())));
}

} // namespace War3Map::QtApp
