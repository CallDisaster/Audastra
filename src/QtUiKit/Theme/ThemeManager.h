#pragma once

#include "Theme/DesignTokens.generated.h"

#include <QColor>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QStringList>

class QWidget;

namespace War3Map::UiKit {

[[nodiscard]] QColor ToQColor(Tokens::Color color);
// QSS 颜色文本：不透明色为 #rrggbb，半透明色为 rgba(...)。
[[nodiscard]] QString ToCss(Tokens::Color color);

// 按设计令牌切换主题：同步更新 QPalette、Qt 样式表与 SVG 图标配色。
// 主题 ID：graphite / midnight / paper。
class ThemeManager final : public QObject {
    Q_OBJECT

public:
    explicit ThemeManager(QObject* parent = nullptr);

    [[nodiscard]] static QStringList ThemeIds();
    [[nodiscard]] static QString ThemeLabel(const QString& id);
    // 未知 ID 返回默认主题，保证调用方总能拿到完整令牌。
    [[nodiscard]] static const Tokens::ThemeTokens& Theme(const QString& id);
    // 由控件调色板找回当前主题令牌（PaletteFor 把 QPalette::Window 设为 bg）；
    // 未套主题的调色板按明暗退回浅色或默认主题。供需要调色板之外令牌（textBody、inlineCodeAccent 等）的控件使用。
    [[nodiscard]] static const Tokens::ThemeTokens& ThemeForPalette(const QPalette& palette);
    // 同上，但从控件向上找第一个不透明的窗口底色（样式表常把子控件底色设为透明）。
    [[nodiscard]] static const Tokens::ThemeTokens& ThemeForWidget(const QWidget* widget);
    // 按窗口底色识别主题：先精确匹配各主题底色，再匹配面板/浮起色，最后按明暗回退。
    [[nodiscard]] static const Tokens::ThemeTokens& ThemeForColor(const QColor& window);

    [[nodiscard]] QString CurrentThemeId() const;
    [[nodiscard]] const Tokens::ThemeTokens& Current() const;
    void ApplyTheme(const QString& id);

    // 纯函数，供测试和离屏渲染使用：不接触 QApplication。
    [[nodiscard]] static QPalette PaletteFor(const Tokens::ThemeTokens& theme);
    [[nodiscard]] static QString StyleSheetFor(const Tokens::ThemeTokens& theme);

signals:
    void ThemeChanged(const QString& id);

private:
    QString current_;
};

} // namespace War3Map::UiKit
