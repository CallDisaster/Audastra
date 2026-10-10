#pragma once

#include "Theme/DesignTokens.generated.h"

#include <QColor>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QStringList>

namespace War3Map::UiKit {

[[nodiscard]] QColor ToQColor(Tokens::Color color);
// QSS 颜色文本：不透明色为 #rrggbb，半透明色为 rgba(...)。
[[nodiscard]] QString ToCss(Tokens::Color color);

// 按设计令牌切换主题：同步更新 QPalette、Qt 样式表与 SVG 图标配色。
// 主题 ID 与 Web 原型一致（graphite / midnight / paper）。
class ThemeManager final : public QObject {
    Q_OBJECT

public:
    explicit ThemeManager(QObject* parent = nullptr);

    [[nodiscard]] static QStringList ThemeIds();
    [[nodiscard]] static QString ThemeLabel(const QString& id);
    // 未知 ID 返回默认主题，保证调用方总能拿到完整令牌。
    [[nodiscard]] static const Tokens::ThemeTokens& Theme(const QString& id);

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
