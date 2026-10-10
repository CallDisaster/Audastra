#include "Icons/SvgIcons.h"

#include "Theme/ThemeManager.h"

#include <QFile>
#include <QFileIconProvider>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QResource>
#include <QSvgRenderer>
#include <QToolButton>

#include <optional>

// 显式引用资源注册符号，让静态组件库的独立调用方也能取得同一套图标。
static void InitializeSvgIconResources() {
    static const bool initialized = [] { Q_INIT_RESOURCE(war3map_uikit_icons); return true; }();
    (void)initialized;
}

namespace War3Map::UiKit {
namespace {

QString ResourcePath(const QString& name) {
    return QStringLiteral(":/war3map/icons/%1.svg").arg(name);
}

struct PaletteState {
    IconPalette palette;
    int revision = 0;
};

PaletteState& State() {
    static PaletteState state{IconPaletteFor(ThemeManager::Theme({})), 0};
    return state;
}

// SVG 源码缓存：同一图标只读一次资源。
QByteArray SvgSource(const QString& name) {
    InitializeSvgIconResources();
    static QHash<QString, QByteArray> cache;
    const auto found = cache.constFind(name);
    if (found != cache.cend()) {
        return *found;
    }
    QFile file(ResourcePath(name));
    QByteArray bytes = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    // 非空损坏SVG也不具备可见图标资格；空返回供按钮使用文字回退。
    if (!bytes.isEmpty() && !QSvgRenderer(bytes).isValid()) bytes.clear();
    cache.insert(name, bytes);
    return bytes;
}

class SvgIconEngine final : public QIconEngine {
public:
    SvgIconEngine(QString name, std::optional<QColor> fixed)
        : name_(std::move(name)), fixed_(std::move(fixed)) {}

    QIconEngine* clone() const override { return new SvgIconEngine(name_, fixed_); }
    QString key() const override { return QStringLiteral("war3map-svg"); }
    QString iconName() override { return name_; }
    bool isNull() override { return SvgSource(name_).isEmpty(); }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal scale = painter->device() != nullptr ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        const QColor color = ColorFor(mode, state);
        const QSize device(qMax(1, qRound(size.width() * scale)), qMax(1, qRound(size.height() * scale)));
        const QString cacheKey = QStringLiteral("w3svg|%1|%2|%3x%4|%5")
            .arg(name_, color.name(QColor::HexArgb))
            .arg(device.width()).arg(device.height())
            .arg(State().revision);
        QPixmap pixmap;
        if (!QPixmapCache::find(cacheKey, &pixmap)) {
            pixmap = Render(device, color);
            QPixmapCache::insert(cacheKey, pixmap);
        }
        pixmap.setDevicePixelRatio(scale);
        return pixmap;
    }

private:
    QColor ColorFor(QIcon::Mode mode, QIcon::State state) const {
        if (fixed_.has_value()) {
            return mode == QIcon::Disabled ? State().palette.disabled : *fixed_;
        }
        const auto& palette = State().palette;
        switch (mode) {
        case QIcon::Disabled: return palette.disabled;
        case QIcon::Selected: return palette.selected;
        case QIcon::Active: return state == QIcon::On ? palette.on : palette.active;
        case QIcon::Normal: break;
        }
        return state == QIcon::On ? palette.on : palette.normal;
    }

    QPixmap Render(const QSize& device, const QColor& color) const {
        QByteArray source = SvgSource(name_);
        source.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
        QPixmap pixmap(device);
        pixmap.fill(Qt::transparent);
        QSvgRenderer renderer(source);
        if (renderer.isValid()) {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setOpacity(color.alphaF());
            renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(device)));
        }
        return pixmap;
    }

    QString name_;
    std::optional<QColor> fixed_;
};

} // namespace

IconPalette IconPaletteFor(const Tokens::ThemeTokens& theme) {
    const auto& c = theme.colors;
    return {ToQColor(c.muted), ToQColor(c.text), ToQColor(c.text), ToQColor(c.accent), ToQColor(c.disabled)};
}

void SetIconPalette(const IconPalette& palette) {
    auto& state = State();
    state.palette = palette;
    ++state.revision;
}

IconPalette CurrentIconPalette() {
    return State().palette;
}

bool HasSvgIcon(const QString& name) {
    return !SvgSource(name).isEmpty();
}

QIcon SvgIcon(const QString& name) {
    return HasSvgIcon(name) ? QIcon(new SvgIconEngine(name, std::nullopt)) : QIcon();
}

QIcon SvgIcon(const QString& name, const QColor& color) {
    return HasSvgIcon(name) ? QIcon(new SvgIconEngine(name, color)) : QIcon();
}

void SetToolButtonSvgIcon(QToolButton& button, const QString& name, const QString& text, Qt::ToolButtonStyle style) {
    const auto icon = SvgIcon(name);
    button.setText(text);
    button.setAccessibleName(text);
    button.setIcon(icon);
    button.setToolButtonStyle(icon.isNull() ? Qt::ToolButtonTextOnly : style);
}

QIcon SystemFolderIcon(int visible, int canvas) {
    static QHash<quint64, QIcon> cache;
    const quint64 key = (static_cast<quint64>(visible) << 32) | static_cast<quint32>(canvas);
    if (const auto found = cache.constFind(key); found != cache.constEnd()) return *found;
    const QIcon system = QFileIconProvider().icon(QAbstractFileIconProvider::Folder);
    if (system.isNull()) return SvgIcon(QStringLiteral("lucide/folder"));
    QIcon result;
    for (const int scale : {1, 2}) {
        QPixmap pixmap(canvas * scale, canvas * scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        const int size = visible * scale;
        const int offset = (canvas - visible) * scale / 2;
        painter.drawPixmap(offset, offset, system.pixmap(size, size));
        painter.end();
        result.addPixmap(pixmap);
    }
    cache.insert(key, result);
    return result;
}

QIcon SystemFileIcon() {
    static const QIcon icon = QFileIconProvider().icon(QAbstractFileIconProvider::File);
    return icon.isNull() ? SvgIcon(QStringLiteral("lucide/file")) : icon;
}

} // namespace War3Map::UiKit
