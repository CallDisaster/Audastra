#pragma once
#include <QWidget>
namespace War3Map::UiKit {
class CodeView;
class CodeViewGutter final : public QWidget {
public:
    explicit CodeViewGutter(CodeView* view);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
private:
    CodeView* view_;
};
}
