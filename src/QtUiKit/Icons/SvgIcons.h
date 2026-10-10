#pragma once

#include "Theme/DesignTokens.generated.h"

#include <QColor>
#include <QIcon>
#include <QString>

class QToolButton;

namespace War3Map::UiKit {

// SVG 图标按状态着色：源文件用 currentColor，渲染时替换成这里的颜色。
struct IconPalette {
    QColor normal;   // 常规
    QColor active;   // 悬停 / 聚焦
    QColor selected; // 选中行
    QColor on;       // 勾选（切换按钮按下）
    QColor disabled; // 禁用
};

[[nodiscard]] IconPalette IconPaletteFor(const Tokens::ThemeTokens& theme);
// 切换主题时调用；已缓存的位图按新配色重新生成。
void SetIconPalette(const IconPalette& palette);
[[nodiscard]] IconPalette CurrentIconPalette();

// name 为 assets/icons 下的相对路径（不含 .svg），如 "lucide/search"。
// 未知名称返回空 QIcon，调用方可以据此回退。
[[nodiscard]] bool HasSvgIcon(const QString& name);
[[nodiscard]] QIcon SvgIcon(const QString& name);
// 固定颜色（不随状态变化），用于强调色或语义色图标。
[[nodiscard]] QIcon SvgIcon(const QString& name, const QColor& color);

// 保留可读动作名；资源缺失或无效时显示文字，避免仅图标按钮变成空白入口。
void SetToolButtonSvgIcon(QToolButton& button, const QString& name, const QString& text,
    Qt::ToolButtonStyle style = Qt::ToolButtonIconOnly);

// 系统（Windows 资源管理器）的文件夹图标。visible 为实际绘制尺寸，canvas 为行图标尺寸：
// 在 24 像素的物体行里画 16 像素的文件夹，文件夹不会比物体图标还大。取不到系统图标时回退到 SVG。
[[nodiscard]] QIcon SystemFolderIcon(int visible = 16, int canvas = 16);
// 系统的通用文件图标（资源管理器的未知类型文件）。
[[nodiscard]] QIcon SystemFileIcon();

} // namespace War3Map::UiKit
