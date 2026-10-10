#pragma once

#include <QMargins>
#include <QPalette>
#include <QRectF>

class QApplication;
class QPainter;
class QWidget;

namespace War3Map::UiKit {

// 尺寸来自生成的设计令牌，保持浮层和旧工具窗口的边界一致。
[[nodiscard]] int PopupShadowExtent();
[[nodiscard]] int PopupContentPadding();

// outer 是完整透明窗口范围，背景内缩阴影留白；只有此函数绘制表面与单层阴影。
void PaintRoundedSurface(QPainter& painter, const QRectF& outer, const QPalette& palette,
    bool dialog = false);

// 仅配置窗口与共用背景，不改变业务、焦点协议或窗口类型（命令面板仍为 Qt::Tool）。
// 此函数会将现有布局增加阴影/内容留白，重复调用无额外影响。
void ConfigureRoundedPopup(QWidget& popup);

// 自动处理 QMenu 与 QComboBox 的原生下拉容器。可重复调用，应用只保留一份过滤器。
void InstallRoundedPopups(QApplication& app);

} // namespace War3Map::UiKit
