#include "Shell/StartupPage.h"

#include "Brand/ApplicationBrand.h"
#include "Theme/DesignTokens.generated.h"

#include <QAction>
#include <QActionEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace War3Map::Shell {
namespace {

class StartupActionButton final : public QToolButton {
public:
    using QToolButton::QToolButton;

protected:
    void actionEvent(QActionEvent* event) override {
        QToolButton::actionEvent(event);
        // QAction 销毁后，Qt 的 ActionRemoved 会清空默认动作并重置按钮状态。
        // 在原生处理之后同步禁用，不能只依赖更早的 destroyed 信号。
        if (!defaultAction()) setEnabled(false);
    }
};

} // namespace

StartupPage::StartupPage(QAction* openMapAction, QAction* createMapAction, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("startupPage"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // 常规尺寸下完整展示；极小窗口仍可滚动到操作，不向宿主施加固定最小尺寸。
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("startupScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll);
    auto* canvas = new QWidget(scroll);
    auto* vertical = new QVBoxLayout(canvas);
    vertical->setContentsMargins(24, 24, 24, 24);
    vertical->addStretch(1);
    auto* content = new QWidget(canvas);
    content->setObjectName(QStringLiteral("startupContent"));
    content->setMaximumWidth(400);
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    auto* logo = new QLabel(content);
    logo->setObjectName(QStringLiteral("startupLogo"));
    logo->setAccessibleName(tr("Audastra 标志"));
    logo->setFixedSize(80, 80);
    logo->setPixmap(UiKit::ApplicationBrandPixmap());
    logo->setScaledContents(true);
    column->addWidget(logo, 0, Qt::AlignHCenter);
    column->addSpacing(16);

    auto* name = new QLabel(QStringLiteral("Audastra"), content);
    name->setObjectName(QStringLiteral("startupProductName"));
    name->setTextFormat(Qt::PlainText);
    name->setAlignment(Qt::AlignCenter);
    name->setStyleSheet(QStringLiteral("font-size: %1px; font-weight: %2;")
        .arg(UiKit::Tokens::Size::kFontHeading + 8)
        .arg(UiKit::Tokens::Weight::kWeightMedium));
    column->addWidget(name);
    column->addSpacing(6);
    auto* tagline = new QLabel(QStringLiteral("Per Audacia Ad Astra"), content);
    tagline->setObjectName(QStringLiteral("startupTagline"));
    tagline->setTextFormat(Qt::PlainText);
    tagline->setAlignment(Qt::AlignCenter);
    tagline->setWordWrap(true);
    tagline->setStyleSheet(QStringLiteral("color: palette(link);"));
    column->addWidget(tagline);
    column->addSpacing(24);

    const auto addAction = [&](QAction* action, const QString& objectName, const QString& fallback) {
        auto* button = new StartupActionButton(content);
        button->setObjectName(objectName);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(UiKit::Tokens::Size::kMenuItemHeight);
        button->setFocusPolicy(Qt::StrongFocus);
        if (action) {
            button->setDefaultAction(action);
        } else {
            button->setText(fallback);
            button->setEnabled(false);
        }
        column->addWidget(button);
        return button;
    };
    auto* open = addAction(openMapAction, QStringLiteral("startupOpenMap"), tr("打开地图"));
    // 仅强调主操作；颜色取当前主题调色板，状态/焦点仍由共享 UIKit 样式负责。
    open->setStyleSheet(QStringLiteral(
        "QToolButton#startupOpenMap:enabled { background: palette(highlight); color: palette(link); border-color: palette(mid); }"
        "QToolButton#startupOpenMap:hover:enabled, QToolButton#startupOpenMap:focus:enabled { border-color: palette(link); }"));
    column->addSpacing(8);
    auto* create = addAction(createMapAction, QStringLiteral("startupCreateMap"), tr("新建地图"));
    setTabOrder(open, create);
    setFocusProxy(open);

    auto* centered = new QHBoxLayout;
    centered->setContentsMargins(0, 0, 0, 0);
    centered->addStretch();
    centered->addWidget(content, 1);
    centered->addStretch();
    vertical->addLayout(centered);
    vertical->addStretch(2);
    scroll->setWidget(canvas);
}

} // namespace War3Map::Shell
