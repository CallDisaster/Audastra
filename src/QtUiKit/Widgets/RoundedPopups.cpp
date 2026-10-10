#include "Widgets/RoundedPopups.h"

#include "Theme/DesignTokens.generated.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QScreen>
#include <QSurfaceFormat>
#include <QWindow>

#include <algorithm>

namespace War3Map::UiKit {
namespace {
constexpr auto kConfigured = "forgeRoundedSurface";
constexpr auto kOwnPaint = "forgeSurfaceOwnPaint";
constexpr auto kInstaller = "forgeRoundedPopupInstaller";
constexpr int kQtDefaultComboRows = 10;
constexpr int kComboRows = 20;

bool IsComboContainer(const QWidget& widget) {
    return widget.inherits("QComboBoxPrivateContainer");
}

class RoundedPopupFilter final : public QObject {
public:
    explicit RoundedPopupFilter(QApplication& app) : QObject(&app) {
        setObjectName(QLatin1String(kInstaller));
        app.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget) return false;
        // 下拉默认只显示 10 行，长列表（动作、玩家、类型）要反复滚动；保留控件显式设置的行数。
        if (event->type() == QEvent::Polish)
            if (auto* combo = qobject_cast<QComboBox*>(widget); combo && combo->maxVisibleItems() == kQtDefaultComboRows)
                combo->setMaxVisibleItems(kComboRows);
        const bool nativePopup = qobject_cast<QMenu*>(widget) != nullptr || IsComboContainer(*widget);
        if (nativePopup && (event->type() == QEvent::Polish || event->type() == QEvent::Show)) {
            ConfigureRoundedPopup(*widget);
            if (IsComboContainer(*widget) && event->type() == QEvent::Show) {
                // Qt 按原列表高度放置下拉；共用阴影/内距不能挤掉原有行。
                const int padding = PopupShadowExtent() + PopupContentPadding();
                const QRect available = widget->screen()->availableGeometry();
                const int height = std::min(widget->height() + padding * 2, available.height());
                widget->resize(widget->width(), height);
                if (widget->frameGeometry().bottom() > available.bottom())
                    widget->move(widget->x(), available.bottom() - height + 1);
                if (widget->layout()) widget->layout()->activate();
            }
        }
        if (widget->inherits("QComboBoxPrivateScroller") && event->type() == QEvent::Show) {
            auto* container = widget->parentWidget();
            if (container && container->property(kConfigured).toBool() && container->layout())
                container->layout()->activate();
        }
        if (event->type() == QEvent::Paint && widget->property(kConfigured).toBool()
            && !widget->property(kOwnPaint).toBool()) {
            QPainter painter(widget);
            PaintRoundedSurface(painter, QRectF(widget->rect()), widget->palette(),
                widget->property("forgeSurfaceDialog").toBool());
            // 私有下拉容器只画原生边框；列表是独立子控件，继续正常绘制。
            // 让原容器的 paintEvent 再画 PE_PanelMenu 会把透明边角重新填满。
            if (IsComboContainer(*widget)) return true;
            // 正常 paintEvent 仍负责菜单项、焦点和选择；它的窗口底色已设为透明。
        }
        return false;
    }
};

} // namespace

int PopupShadowExtent() { return Tokens::Size::kShadowPopover; }
int PopupContentPadding() { return Tokens::Size::kPopoverPadding; }

void PaintRoundedSurface(QPainter& painter, const QRectF& outer, const QPalette& palette, bool dialog) {
    const qreal extent = dialog ? Tokens::Size::kShadowDialog : PopupShadowExtent();
    const qreal radius = dialog ? Tokens::Size::kRadiusXl : Tokens::Size::kRadiusLg;
    const QRectF body = outer.adjusted(extent, extent, -extent, -extent);
    if (body.width() <= 1 || body.height() <= 1) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    // 一个有界柔影：逐层衰减模拟模糊，不再请求 Windows/DWM 或 QGraphicsEffect 阴影。
    // 阴影始终留在窗口内部，窗口的四角和最外一像素完全透明。
    const int layers = static_cast<int>(extent);
    for (int layer = layers - 1; layer >= 1; --layer) {
        const qreal spread = static_cast<qreal>(layer);
        const qreal strength = 1.0 - spread / extent;
        const int alpha = qRound(12.0 * strength * strength);
        if (alpha <= 0) continue;
        const QRectF shadow = body.adjusted(-spread, -spread, spread, spread).translated(0, 1);
        painter.setBrush(QColor(0, 0, 0, alpha));
        painter.drawRoundedRect(shadow, radius + spread, radius + spread);
    }
    QColor border = palette.color(QPalette::Mid);
    border.setAlpha(std::min(border.alpha(), 190));
    painter.setBrush(palette.brush(QPalette::ToolTipBase));
    painter.setPen(QPen(border, 1.0));
    painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.restore();
}

void ConfigureRoundedPopup(QWidget& popup) {
    const int extent = popup.property("forgeSurfaceDialog").toBool()
        ? Tokens::Size::kShadowDialog : PopupShadowExtent();
    const int padding = extent + PopupContentPadding();
    const bool menu = qobject_cast<QMenu*>(&popup) != nullptr;
    const bool combo = IsComboContainer(popup);
    if (!popup.property(kConfigured).toBool()) {
        popup.setProperty(kConfigured, true);
        popup.setAttribute(Qt::WA_TranslucentBackground);
        popup.setAttribute(Qt::WA_NoSystemBackground);
        popup.setAttribute(Qt::WA_OpaquePaintEvent, false);
        popup.setAutoFillBackground(false);
        // 保留 Qt::Tool/Qt::Popup 的原输入/关闭协议，禁用系统影子以免和共用柔影重叠。
        popup.setWindowFlag(Qt::FramelessWindowHint, true);
        popup.setWindowFlag(Qt::NoDropShadowWindowHint, true);
        if (auto* frame = qobject_cast<QFrame*>(&popup)) frame->setFrameShape(QFrame::NoFrame);
        QString local;
        if (menu) {
            local = QStringLiteral(
                "QMenu[forgeRoundedSurface=\"true\"] { background-color:transparent; border:0; padding:%1px; }"
                "QMenu::item { min-height:%2px; padding:0px 8px; }"
                "QMenu::separator { margin:4px 8px; }")
                .arg(padding).arg(Tokens::Size::kMenuItemHeight);
        } else {
            local = QStringLiteral("QWidget[forgeRoundedSurface=\"true\"] { background-color:transparent; border:0; }");
            if (combo) local += QStringLiteral("QAbstractItemView { background-color:transparent; border:0; }");
        }
        popup.setStyleSheet(popup.styleSheet() + local);
    }
    // QComboBox::showPopup 先 create() 再 show()：容器若此前未 Polish，原生窗口创建时还没有透明通道。
    // 表面格式只在创建时生效（Qt 对已创建窗口不再改格式，QTBUG-85714），圆角外和柔影区会显示为黑框；
    // 在真正显示到屏幕前带透明通道重建一次原生窗口。
    if (QWindow* window = popup.windowHandle(); window && window->handle() && !window->isVisible()
        && !window->requestedFormat().hasAlpha()) {
        QSurfaceFormat format = window->requestedFormat();
        format.setAlphaBufferSize(8);
        window->destroy();
        window->setFormat(format);
        window->create();
    }
    if (!menu) {
        if (QLayout* layout = popup.layout()) {
            // 部分原生下拉在 Polish 以后才添加布局，Show 时补齐；同一布局只增加一次。
            if (!layout->property("forgeRoundedPadding").toBool()) {
                layout->setProperty("forgeRoundedPadding", true);
                const QMargins old = layout->contentsMargins();
                layout->setContentsMargins(old.left() + padding, old.top() + padding,
                    old.right() + padding, old.bottom() + padding);
            }
        }
        if (combo) {
            // QComboBox 的列表填充仅在内容区内，不能把透明四角再次涂成实色。
            for (auto* view : popup.findChildren<QAbstractItemView*>()) {
                view->setFrameShape(QFrame::NoFrame);
                view->setAutoFillBackground(false);
                view->viewport()->setAutoFillBackground(false);
            }
        }
    }
}

void InstallRoundedPopups(QApplication& app) {
    // Qt 的下拉/菜单展开动画先把弹窗截成不透明位图再滚出或淡入，透明圆角和柔影区在动画期间显示为黑色，
    // 展开完才换成真实窗口。共用圆角弹窗直接显示，不用这些动画（Windows 默认按系统设置开启）。
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
    QApplication::setEffectEnabled(Qt::UI_FadeMenu, false);
    if (app.findChild<QObject*>(QLatin1String(kInstaller), Qt::FindDirectChildrenOnly)) return;
    new RoundedPopupFilter(app);
}

} // namespace War3Map::UiKit
