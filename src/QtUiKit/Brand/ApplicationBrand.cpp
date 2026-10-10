#include "Brand/ApplicationBrand.h"

#include <QPixmapCache>
#include <QResource>

// Q_INIT_RESOURCE 必须在全局命名空间引用生成符号，以支持静态组件库的资源链接。
static void InitializeBrandResources() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(audastra_brand);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace War3Map::UiKit {

QIcon ApplicationIcon() {
    InitializeBrandResources();
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        icon.addFile(QStringLiteral(":/audastra/brand/icon-%1.png").arg(size), QSize(size, size));
    }
    return icon;
}

QPixmap ApplicationBrandPixmap() {
    InitializeBrandResources();
    const QString key = QStringLiteral("audastra/approved-brand-512");
    QPixmap pixmap;
    if (!QPixmapCache::find(key, &pixmap)) {
        pixmap.load(QStringLiteral(":/audastra/brand/audastra-512.png"));
        QPixmapCache::insert(key, pixmap);
    }
    return pixmap;
}

} // namespace War3Map::UiKit
