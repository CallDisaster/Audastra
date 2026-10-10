#include "Theme/ThemeManager.h"

#include "Icons/SvgIcons.h"
#include "Widgets/RoundedPopups.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace War3Map::UiKit {
namespace {

QString FromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// 样式表可引用的令牌：颜色、尺寸（px）与字重。
QHash<QString, QString> TokenValues(const Tokens::ThemeTokens& theme) {
    const auto& c = theme.colors;
    QHash<QString, QString> values{
        {QStringLiteral("bg"), ToCss(c.bg)},
        {QStringLiteral("panel"), ToCss(c.panel)},
        {QStringLiteral("raised"), ToCss(c.raised)},
        {QStringLiteral("hover"), ToCss(c.hover)},
        {QStringLiteral("line"), ToCss(c.line)},
        {QStringLiteral("text"), ToCss(c.text)},
        {QStringLiteral("textBody"), ToCss(c.textBody)},
        {QStringLiteral("inlineCodeAccent"), ToCss(c.inlineCodeAccent)},
        {QStringLiteral("muted"), ToCss(c.muted)},
        {QStringLiteral("dim"), ToCss(c.dim)},
        {QStringLiteral("disabled"), ToCss(c.disabled)},
        {QStringLiteral("accent"), ToCss(c.accent)},
        {QStringLiteral("accentSoft"), ToCss(c.accentSoft)},
        {QStringLiteral("accentLine"), ToCss(c.accentLine)},
        {QStringLiteral("selection"), ToCss(c.selection)},
        {QStringLiteral("scrollbar"), ToCss(c.scrollbar)},
        {QStringLiteral("amber"), ToCss(c.amber)},
        {QStringLiteral("violet"), ToCss(c.violet)},
        {QStringLiteral("blue"), ToCss(c.blue)},
        {QStringLiteral("red"), ToCss(c.red)},
    };
    const auto px = [](int value) { return QStringLiteral("%1px").arg(value); };
    values.insert(QStringLiteral("fontTiny"), px(Tokens::Size::kFontTiny));
    values.insert(QStringLiteral("fontSmall"), px(Tokens::Size::kFontSmall));
    values.insert(QStringLiteral("fontBody"), px(Tokens::Size::kFontBody));
    values.insert(QStringLiteral("fontTitle"), px(Tokens::Size::kFontTitle));
    values.insert(QStringLiteral("fontHeading"), px(Tokens::Size::kFontHeading));
    values.insert(QStringLiteral("radiusSmall"), px(Tokens::Size::kRadiusSmall));
    values.insert(QStringLiteral("radius"), px(Tokens::Size::kRadius));
    values.insert(QStringLiteral("radiusLarge"), px(Tokens::Size::kRadiusLarge));
    values.insert(QStringLiteral("radiusNone"), px(Tokens::Size::kRadiusNone));
    values.insert(QStringLiteral("radiusXs"), px(Tokens::Size::kRadiusXs));
    values.insert(QStringLiteral("radiusSm"), px(Tokens::Size::kRadiusSm));
    values.insert(QStringLiteral("radiusMd"), px(Tokens::Size::kRadiusMd));
    values.insert(QStringLiteral("radiusLg"), px(Tokens::Size::kRadiusLg));
    values.insert(QStringLiteral("radiusXl"), px(Tokens::Size::kRadiusXl));
    values.insert(QStringLiteral("radiusPill"), px(Tokens::Size::kRadiusPill));
    values.insert(QStringLiteral("popoverPadding"), px(Tokens::Size::kPopoverPadding));
    values.insert(QStringLiteral("menuItemHeight"), px(Tokens::Size::kMenuItemHeight));
    values.insert(QStringLiteral("weightRegular"), QString::number(Tokens::Weight::kWeightRegular));
    values.insert(QStringLiteral("weightMedium"), QString::number(Tokens::Weight::kWeightMedium));
    values.insert(QStringLiteral("weightBold"), QString::number(Tokens::Weight::kWeightBold));
    return values;
}

QString StyleSheetTemplate() {
    QFile file(QStringLiteral(":/war3map/uikit/Workbench.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

QColor ToQColor(Tokens::Color color) {
    return QColor(color.r, color.g, color.b, color.a);
}

QString ToCss(Tokens::Color color) {
    if (color.a == 255) {
        return QColor(color.r, color.g, color.b).name(QColor::HexRgb);
    }
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.r).arg(color.g).arg(color.b).arg(color.a);
}

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent),
      current_(FromView(Tokens::kDefaultTheme)) {
}

QStringList ThemeManager::ThemeIds() {
    QStringList ids;
    for (const auto& theme : Tokens::kThemes) {
        ids.push_back(FromView(theme.id));
    }
    return ids;
}

QString ThemeManager::ThemeLabel(const QString& id) {
    return FromView(Theme(id).label);
}

const Tokens::ThemeTokens& ThemeManager::Theme(const QString& id) {
    const QByteArray key = id.toUtf8();
    for (const auto& theme : Tokens::kThemes) {
        if (theme.id == std::string_view(key.constData(), static_cast<std::size_t>(key.size()))) {
            return theme;
        }
    }
    for (const auto& theme : Tokens::kThemes) {
        if (theme.id == Tokens::kDefaultTheme) {
            return theme;
        }
    }
    return Tokens::kThemes.front();
}

QString ThemeManager::CurrentThemeId() const {
    return current_;
}

const Tokens::ThemeTokens& ThemeManager::Current() const {
    return Theme(current_);
}

void ThemeManager::ApplyTheme(const QString& id) {
    if (auto* application = qobject_cast<QApplication*>(QCoreApplication::instance())) InstallRoundedPopups(*application);
    const auto& theme = Theme(id);
    current_ = FromView(theme.id);
    SetIconPalette(IconPaletteFor(theme));
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        // Fusion 用应用调色板绘制原生箭头和复选标记，避免暗色下残留黑色图元。
        if (!app->style() || app->style()->objectName().compare(QStringLiteral("fusion"), Qt::CaseInsensitive) != 0) {
            if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
                app->setStyle(fusion);
            }
        }
        // 系统标题栏（主窗口与浮动窗口）跟随主题明暗。
        QGuiApplication::styleHints()->setColorScheme(theme.dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        app->setPalette(PaletteFor(theme));
        app->setStyleSheet(StyleSheetFor(theme));
    }
    emit ThemeChanged(current_);
}

QPalette ThemeManager::PaletteFor(const Tokens::ThemeTokens& theme) {
    const auto& c = theme.colors;
    QPalette palette;
    palette.setColor(QPalette::Window, ToQColor(c.bg));
    palette.setColor(QPalette::WindowText, ToQColor(c.text));
    palette.setColor(QPalette::Base, ToQColor(c.bg));
    palette.setColor(QPalette::AlternateBase, ToQColor(c.panel));
    palette.setColor(QPalette::Text, ToQColor(c.text));
    palette.setColor(QPalette::Button, ToQColor(c.raised));
    palette.setColor(QPalette::ButtonText, ToQColor(c.text));
    palette.setColor(QPalette::ToolTipBase, ToQColor(c.raised));
    palette.setColor(QPalette::ToolTipText, ToQColor(c.text));
    palette.setColor(QPalette::Highlight, ToQColor(c.selection));
    palette.setColor(QPalette::HighlightedText, ToQColor(c.text));
    palette.setColor(QPalette::Link, ToQColor(c.accent));
    palette.setColor(QPalette::LinkVisited, ToQColor(c.accent));
    palette.setColor(QPalette::PlaceholderText, ToQColor(c.muted));
    palette.setColor(QPalette::Light, ToQColor(c.hover));
    palette.setColor(QPalette::Midlight, ToQColor(c.raised));
    palette.setColor(QPalette::Mid, ToQColor(c.line));
    palette.setColor(QPalette::Dark, ToQColor(c.bg));
    palette.setColor(QPalette::Shadow, ToQColor(c.line));
    palette.setColor(QPalette::Accent, ToQColor(c.accent));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                            QPalette::PlaceholderText, QPalette::Link, QPalette::HighlightedText}) {
        palette.setColor(QPalette::Disabled, role, ToQColor(c.disabled));
    }
    palette.setColor(QPalette::Disabled, QPalette::Highlight, ToQColor(c.panel));
    return palette;
}

QString ThemeManager::StyleSheetFor(const Tokens::ThemeTokens& theme) {
    const QString source = StyleSheetTemplate();
    const auto values = TokenValues(theme);
    // 最长匹配：@accentLine 不会被 @accent 截断；未知名称原样保留，由测试发现。
    static const QRegularExpression token(QStringLiteral("@([A-Za-z][A-Za-z0-9]*)"));
    QString sheet;
    sheet.reserve(source.size() + source.size() / 4);
    qsizetype last = 0;
    for (auto it = token.globalMatch(source); it.hasNext();) {
        const auto match = it.next();
        sheet += QStringView(source).mid(last, match.capturedStart() - last);
        const auto found = values.constFind(match.captured(1));
        sheet += found == values.cend() ? match.captured(0) : *found;
        last = match.capturedEnd();
    }
    sheet += QStringView(source).mid(last);
    return sheet;
}

} // namespace War3Map::UiKit
