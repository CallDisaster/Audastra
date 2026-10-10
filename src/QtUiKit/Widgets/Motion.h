#pragma once

#include <QFrame>
#include <QPointer>

class QPropertyAnimation;

namespace War3Map::UiKit {

// 界面动效的统一入口。系统关闭“显示动画”（Windows 客户区动画）时一律直接跳到终态，
// 所有动效只改变呈现，不参与业务状态。
[[nodiscard]] bool AnimationsEnabled();

// 淡入：透明度从 from 过渡到 1，结束后移除效果，控件不常驻离屏绘制。
// 不能用于含原生子窗口（如 D3D 视口）的控件。重复调用会接着当前动画重新开始。
void FadeIn(QWidget* widget, int durationMs = 160, qreal from = 0.0);

// 分段选择器的滑动底块：放在一组按钮的共同父控件里，垫在按钮下方并随当前按钮移动。
// 样式由 objectName 在样式表中指定；目标控件移动或改变大小时自动跟随。
class SlidingHighlight final : public QFrame {
    Q_OBJECT
public:
    explicit SlidingHighlight(QWidget* parent);
    // target 为空时隐藏底块。animate 为 false 或系统关闭动画时直接跳到目标位置。
    void Track(QWidget* target, bool animate = true);
    [[nodiscard]] QWidget* Target() const noexcept { return target_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void Follow(bool animate);
    QPointer<QWidget> target_;
    QPropertyAnimation* animation_ = nullptr;
};

} // namespace War3Map::UiKit
