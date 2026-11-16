#pragma once

#include "War3MapCore/Services/MapTestService.h"

#include <QSettings>
#include <QString>

namespace War3Map::QtCommon {

// 作者看着的测试的显示设置（设置 → 地图测试）。认不出的值按默认处理：窗口化、Direct3D、不限制鼠标。
inline Services::MapTestDisplay ReadMapTestDisplay(const QSettings& settings) {
    Services::MapTestDisplay display;
    if (const auto mode = Services::ParseMapTestWindowMode(settings.value(QStringLiteral("mapTest/windowMode")).toString().toStdString()))
        display.windowMode = *mode;
    if (const auto renderer = Services::ParseMapTestRenderer(settings.value(QStringLiteral("mapTest/renderer")).toString().toStdString()))
        display.renderer = *renderer;
    const auto lock = settings.value(QStringLiteral("mapTest/cursorLock"));
    display.cursorLock = lock.metaType().id() == QMetaType::Bool ? lock.toBool()
        : lock.metaType().id() == QMetaType::QString && lock.toString() == QStringLiteral("true");
    return display;
}

} // namespace War3Map::QtCommon
