#pragma once

#include <QLabel>

namespace War3Map::UiKit {

// 单行标签：放不下时在末尾显示省略号，悬停显示全文。text() 始终是完整文本，
// 适合状态栏等宽度受限、但内容长短不定的位置。
class ElidedLabel final : public QLabel {
    Q_OBJECT
public:
    explicit ElidedLabel(QWidget* parent = nullptr);
    explicit ElidedLabel(const QString& text, QWidget* parent = nullptr);
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] bool IsElided() const;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
};

} // namespace War3Map::UiKit
