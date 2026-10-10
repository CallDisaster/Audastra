#pragma once

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPixmap>
#include <QSaveFile>
#include <QScreen>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace PopupCapture {

inline QJsonObject Rect(const QRect& value) {
    return {{QStringLiteral("x"), value.x()}, {QStringLiteral("y"), value.y()},
        {QStringLiteral("width"), value.width()}, {QStringLiteral("height"), value.height()}};
}

// 分离的 Qt 顶层弹窗不会出现在宿主 grab 中。本回执保存两个真实 grab，并按实际
// 全局逻辑坐标合成一张检查定位的图；明确不是桌面截图，也不伪造操作系统标题栏。
inline QString Save(QWidget& host, QWidget& popup, QWidget* anchor,
    const QString& directory, const QString& stem) {
    if (directory.isEmpty()) return {};
    if (!host.isVisible() || !popup.isVisible()) return QStringLiteral("宿主或弹层未实际显示：%1").arg(stem);
    if (!QDir().mkpath(directory)) return QStringLiteral("不能创建捕获目录");
    const QStringList suffixes{QStringLiteral("-host.png"), QStringLiteral("-popup.png"),
        QStringLiteral("-composite.png"), QStringLiteral("-geometry.json")};
    for (const auto& suffix : suffixes)
        if (QFileInfo::exists(QDir(directory).filePath(stem + suffix)))
            return QStringLiteral("保留旧捕获，拒绝覆盖：%1").arg(stem + suffix);
    const QRect hostRect(host.mapToGlobal(QPoint{}), host.size());
    const QRect popupRect(popup.mapToGlobal(QPoint{}), popup.size());
    const QRect bounds = hostRect.united(popupRect);
    const QPixmap hostPixels = host.grab();
    const QPixmap popupPixels = popup.grab();
    if (hostPixels.isNull() || popupPixels.isNull()) return QStringLiteral("Qt grab 返回空图");
    const qreal dpr = std::max(hostPixels.devicePixelRatio(), popupPixels.devicePixelRatio());
    QImage composite(QSize(static_cast<int>(std::ceil(bounds.width() * dpr)),
        static_cast<int>(std::ceil(bounds.height() * dpr))), QImage::Format_ARGB32_Premultiplied);
    composite.setDevicePixelRatio(dpr);
    composite.fill(Qt::transparent);
    QPainter painter(&composite);
    painter.drawPixmap(hostRect.topLeft() - bounds.topLeft(), hostPixels);
    painter.drawPixmap(popupRect.topLeft() - bounds.topLeft(), popupPixels);
    painter.end();
    const QDir output(directory);
    if (!hostPixels.save(output.filePath(stem + suffixes[0]))
        || !popupPixels.save(output.filePath(stem + suffixes[1]))
        || !composite.save(output.filePath(stem + suffixes[2]))) return QStringLiteral("PNG 写入失败");
    QJsonObject evidence{{QStringLiteral("fixture_only"), true},
        {QStringLiteral("capture_method"), QStringLiteral("two_actual_qwidget_grabs_composited_at_global_logical_coordinates")},
        {QStringLiteral("native_desktop_capture"), false}, {QStringLiteral("native_titlebar_in_image"), false},
        {QStringLiteral("platform"), QGuiApplication::platformName()},
        {QStringLiteral("host_class"), QString::fromLatin1(host.metaObject()->className())},
        {QStringLiteral("host_name"), host.objectName()}, {QStringLiteral("host_title"), host.windowTitle()},
        {QStringLiteral("popup_class"), QString::fromLatin1(popup.metaObject()->className())},
        {QStringLiteral("popup_name"), popup.objectName()},
        {QStringLiteral("host_geometry"), Rect(hostRect)}, {QStringLiteral("popup_geometry"), Rect(popupRect)},
        {QStringLiteral("composite_geometry"), Rect(bounds)}, {QStringLiteral("dpr"), dpr}};
    if (anchor) {
        evidence.insert(QStringLiteral("anchor_name"), anchor->objectName());
        evidence.insert(QStringLiteral("anchor_geometry"), Rect(QRect(anchor->mapToGlobal(QPoint{}), anchor->size())));
    }
    if (auto* screen = popup.screen()) {
        evidence.insert(QStringLiteral("available_screen_geometry"), Rect(screen->availableGeometry()));
        evidence.insert(QStringLiteral("popup_inside_available_screen"), screen->availableGeometry().contains(popupRect));
    }
    const QImage image = popupPixels.toImage();
    QJsonArray corners;
    for (const QPoint corner : {QPoint(0, 0), QPoint(image.width() - 1, 0),
             QPoint(0, image.height() - 1), QPoint(image.width() - 1, image.height() - 1)})
        corners.append(image.pixelColor(corner).alpha());
    evidence.insert(QStringLiteral("popup_outer_corner_alpha"), corners);
    QSaveFile json(output.filePath(stem + suffixes[3]));
    const auto bytes = QJsonDocument(evidence).toJson(QJsonDocument::Indented);
    if (!json.open(QIODevice::WriteOnly) || json.write(bytes) != bytes.size() || !json.commit())
        return QStringLiteral("浮层定位回执写入失败");
    return {};
}

} // namespace PopupCapture
