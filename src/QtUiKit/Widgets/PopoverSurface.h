#pragma once

#include <QFrame>
#include <QPointer>

class QHideEvent;
class QPropertyAnimation;
class QScreen;
class QShowEvent;

namespace War3Map::UiKit {

// 轻量浮层：内容只负责布局和交互，圆角、阴影与屏幕定位共用 UiKit 表面规则。
// Qt::Popup 保留原生的点击外部关闭；不用于需要输入法的命令面板。
class PopoverSurface final : public QFrame {
    Q_OBJECT
public:
    enum class PreferredSide { Below, Above };

    explicit PopoverSurface(QWidget* parent = nullptr);
    ~PopoverSurface() override;

    [[nodiscard]] QWidget* ContentWidget() const noexcept { return content_; }
    // 接管 content 的 QObject 所有权；替换时释放旧内容。
    void SetContentWidget(QWidget* content);
    void ShowAnchored(QWidget* anchor, PreferredSide side = PreferredSide::Below,
        Qt::Alignment alignment = Qt::AlignLeft);
    void ToggleAnchored(QWidget* anchor, PreferredSide side = PreferredSide::Below,
        Qt::Alignment alignment = Qt::AlignLeft);
    void SetReducedMotion(bool reduced);
    void Dismiss();

    // 同一定位逻辑供测试及其它表面复用，不依赖窗口是否已经显示。
    [[nodiscard]] static QRect AnchoredGeometry(const QRect& anchor, QSize size,
        const QRect& available, PreferredSide side = PreferredSide::Below,
        Qt::Alignment alignment = Qt::AlignLeft);

signals:
    void dismissed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void PositionAtAnchor();
    void BeginFade();

    QWidget* content_ = nullptr;
    QPointer<QWidget> anchor_;
    QPointer<QWidget> anchorWindow_;
    QPointer<QWidget> previousFocus_;
    QPropertyAnimation* fade_ = nullptr;
    PreferredSide side_ = PreferredSide::Below;
    Qt::Alignment alignment_ = Qt::AlignLeft;
    bool reducedMotion_ = false;
};

} // namespace War3Map::UiKit
