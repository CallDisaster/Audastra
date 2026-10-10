#include "Widgets/CodeViewGutter.h"
#include "Widgets/CodeView.h"
namespace War3Map::UiKit {
CodeViewGutter::CodeViewGutter(CodeView* view) : QWidget(view), view_(view) { setCursor(Qt::PointingHandCursor); }
QSize CodeViewGutter::sizeHint() const { return {view_->GutterWidth(), 0}; }
void CodeViewGutter::paintEvent(QPaintEvent* event) { view_->PaintGutter(event); }
void CodeViewGutter::mousePressEvent(QMouseEvent* event) { view_->ClickGutter(event); }
}
