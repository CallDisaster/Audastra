#include "Icons/GeometricIcons.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>

namespace GeometricIcons {
namespace {

constexpr double kPi = 3.14159265358979323846;

// 命运2 信息图形调色板。
const QColor kStroke(178, 204, 224);      // 冷钢蓝白：主线条
const QColor kGold(201, 162, 39);         // #c9a227：金色点缀
const QColor kCyan(120, 196, 235);        // 高亮青
const QColor kViolet(168, 148, 220);      // 条件紫
const QColor kGreen(110, 214, 172);       // 动作绿
const QColor kAmber(235, 186, 100);       // 调用琥珀
const QColor kRed(217, 83, 79);           // 禁用红

// 图标缓存（刻意泄漏：避免 QApplication 销毁后静态析构崩溃）。
QHash<QString, QIcon>& Cache() {
    static auto* cache = new QHash<QString, QIcon>();
    return *cache;
}

QIcon Cached(const QString& key, int size, void (*draw)(QPainter&, int)) {
    auto& cache = Cache();
    auto found = cache.find(key);
    if (found != cache.end()) {
        return found.value();
    }
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    draw(painter, size);
    painter.end();
    QIcon icon(pixmap);
    cache.insert(key, icon);
    return icon;
}

// ── 侧栏图标绘制函数（20×20） ──

void DrawOverview(QPainter& p, int s) {
    // 六边形轮廓 + 中心点：命运2 据点/区域符号。
    const double c = s / 2.0;
    const double r = s * 0.36;
    p.setPen(QPen(kStroke, 1.3));
    p.setBrush(Qt::NoBrush);
    QPainterPath hex;
    for (int k = 0; k < 6; ++k) {
        const double angle = kPi / 3.0 * k - kPi / 6.0;
        const QPointF pt(c + r * std::cos(angle), c + r * std::sin(angle));
        k == 0 ? hex.moveTo(pt) : hex.lineTo(pt);
    }
    hex.closeSubpath();
    p.drawPath(hex);
    p.setPen(Qt::NoPen);
    p.setBrush(kGold);
    p.drawEllipse(QPointF(c, c), 1.8, 1.8);
}

void DrawTrigger(QPainter& p, int s) {
    // 几何化闪电：角状折线，非圆润 emoji 风格。
    p.setPen(QPen(kCyan, 1.4, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath bolt;
    bolt.moveTo(s * 0.58, s * 0.12);
    bolt.lineTo(s * 0.34, s * 0.52);
    bolt.lineTo(s * 0.52, s * 0.52);
    bolt.lineTo(s * 0.40, s * 0.88);
    bolt.lineTo(s * 0.68, s * 0.44);
    bolt.lineTo(s * 0.50, s * 0.44);
    bolt.lineTo(s * 0.58, s * 0.12);
    p.drawPath(bolt);
}

void DrawObjectEditor(QPainter& p, int s) {
    // 三层堆叠菱形：数据层级。
    p.setPen(QPen(kStroke, 1.1));
    p.setBrush(Qt::NoBrush);
    const double cx = s / 2.0;
    const double w = s * 0.32;
    const double h = s * 0.16;
    for (int layer = 0; layer < 3; ++layer) {
        const double cy = s * 0.30 + layer * s * 0.20;
        if (layer == 2) p.setPen(QPen(kGold, 1.1)); // 底层金色
        QPainterPath diamond;
        diamond.moveTo(cx, cy - h);
        diamond.lineTo(cx + w, cy);
        diamond.lineTo(cx, cy + h);
        diamond.lineTo(cx - w, cy);
        diamond.closeSubpath();
        p.drawPath(diamond);
    }
}

void DrawMapInfo(QPainter& p, int s) {
    // 圆 + 十字准线 + 弧段：天文定位符号。
    const double c = s / 2.0;
    const double r = s * 0.30;
    p.setPen(QPen(kStroke, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(c, c), r, r);
    // 十字准线（超出圆外少许）。
    const double ext = s * 0.42;
    p.drawLine(QPointF(c - ext, c), QPointF(c - r - 1, c));
    p.drawLine(QPointF(c + r + 1, c), QPointF(c + ext, c));
    p.drawLine(QPointF(c, c - ext), QPointF(c, c - r - 1));
    p.drawLine(QPointF(c, c + r + 1), QPointF(c, c + ext));
    // 金色弧段。
    p.setPen(QPen(kGold, 1.2));
    p.drawArc(QRectF(c - r, c - r, r * 2, r * 2), 30 * 16, 60 * 16);
}

void DrawImportAssets(QPainter& p, int s) {
    // 上三角 + 底线：入库/上传符号。
    p.setPen(QPen(kStroke, 1.3, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath tri;
    tri.moveTo(s * 0.5, s * 0.22);
    tri.lineTo(s * 0.74, s * 0.58);
    tri.lineTo(s * 0.26, s * 0.58);
    tri.closeSubpath();
    p.drawPath(tri);
    p.setPen(QPen(kGold, 1.3, Qt::SolidLine, Qt::SquareCap));
    p.drawLine(QPointF(s * 0.26, s * 0.76), QPointF(s * 0.74, s * 0.76));
}

void DrawValidate(QPainter& p, int s) {
    // 角状对勾：几何折线，锐利转角。
    p.setPen(QPen(kGreen, 1.5, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath check;
    check.moveTo(s * 0.20, s * 0.52);
    check.lineTo(s * 0.42, s * 0.74);
    check.lineTo(s * 0.80, s * 0.26);
    p.drawPath(check);
    // 外框弧段。
    p.setPen(QPen(kStroke, 1.0));
    const double c = s / 2.0, r = s * 0.40;
    p.drawArc(QRectF(c - r, c - r, r * 2, r * 2), 200 * 16, 250 * 16);
}

// ── ECA 节点图标绘制函数（16×16） ──

void DrawEcaEvent(QPainter& p, int s) {
    // 角状脉冲线：信号触发波形。
    p.setPen(QPen(kCyan, 1.3, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath pulse;
    pulse.moveTo(s * 0.10, s * 0.62);
    pulse.lineTo(s * 0.32, s * 0.62);
    pulse.lineTo(s * 0.44, s * 0.22);
    pulse.lineTo(s * 0.58, s * 0.78);
    pulse.lineTo(s * 0.70, s * 0.38);
    pulse.lineTo(s * 0.90, s * 0.38);
    p.drawPath(pulse);
    // 起点金色节点。
    p.setPen(Qt::NoPen);
    p.setBrush(kGold);
    p.drawEllipse(QPointF(s * 0.10, s * 0.62), 1.4, 1.4);
}

void DrawEcaCondition(QPainter& p, int s) {
    // 菱形 + 内部垂直线：判定门。
    const double c = s / 2.0;
    const double r = s * 0.36;
    p.setPen(QPen(kViolet, 1.2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath diamond;
    diamond.moveTo(c, c - r);
    diamond.lineTo(c + r, c);
    diamond.lineTo(c, c + r);
    diamond.lineTo(c - r, c);
    diamond.closeSubpath();
    p.drawPath(diamond);
    // 内部分歧线。
    p.setPen(QPen(kViolet, 0.8));
    p.drawLine(QPointF(c, c - r * 0.45), QPointF(c, c + r * 0.45));
}

void DrawEcaAction(QPainter& p, int s) {
    // 双箭头角标：执行流向。
    p.setPen(QPen(kGreen, 1.3, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    // 前箭头（大）。
    QPainterPath a1;
    a1.moveTo(s * 0.30, s * 0.20);
    a1.lineTo(s * 0.72, s * 0.50);
    a1.lineTo(s * 0.30, s * 0.80);
    p.drawPath(a1);
    // 后箭头（小，偏移）。
    p.setPen(QPen(kGreen, 1.0));
    QPainterPath a2;
    a2.moveTo(s * 0.12, s * 0.32);
    a2.lineTo(s * 0.40, s * 0.50);
    a2.lineTo(s * 0.12, s * 0.68);
    p.drawPath(a2);
}

void DrawEcaCall(QPainter& p, int s) {
    // 圆 + 内三角：函数调用入口。
    const double c = s / 2.0;
    p.setPen(QPen(kAmber, 1.1));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(c, c), s * 0.36, s * 0.36);
    QPainterPath tri;
    const double r = s * 0.18;
    tri.moveTo(c + r * std::cos(0.0), c + r * std::sin(0.0));
    tri.lineTo(c + r * std::cos(2.094), c + r * std::sin(2.094));
    tri.lineTo(c + r * std::cos(4.189), c + r * std::sin(4.189));
    tri.closeSubpath();
    p.setPen(QPen(kAmber, 0.9));
    p.drawPath(tri);
}

void DrawEcaDisabled(QPainter& p, int s) {
    // 断裂圆环 + 斜线：禁用态。
    const double c = s / 2.0;
    const double r = s * 0.32;
    p.setPen(QPen(kRed, 1.2));
    p.setBrush(Qt::NoBrush);
    // 断裂环（270°弧）。
    p.drawArc(QRectF(c - r, c - r, r * 2, r * 2), 45 * 16, 270 * 16);
    // 斜线。
    const double d = r * 0.7071;
    p.drawLine(QPointF(c - d, c + d), QPointF(c + d, c - d));
}

void DrawEcaComment(QPainter& p, int s) {
    // 双横线 + 左端点：文本注释标记。
    p.setPen(QPen(kGreen, 1.1, Qt::SolidLine, Qt::SquareCap));
    p.drawLine(QPointF(s * 0.22, s * 0.38), QPointF(s * 0.82, s * 0.38));
    p.setPen(QPen(kGreen, 0.9));
    p.drawLine(QPointF(s * 0.22, s * 0.62), QPointF(s * 0.66, s * 0.62));
    p.setPen(Qt::NoPen);
    p.setBrush(kGold);
    p.drawEllipse(QPointF(s * 0.13, s * 0.38), 1.2, 1.2);
}

// ── 通用功能图标绘制函数 ──

void DrawUndo(QPainter& p, int s) {
    p.setPen(QPen(kStroke, 1.3, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath arrow;
    arrow.moveTo(s * 0.62, s * 0.20);
    arrow.lineTo(s * 0.30, s * 0.46);
    arrow.lineTo(s * 0.62, s * 0.72);
    p.drawPath(arrow);
    p.drawLine(QPointF(s * 0.30, s * 0.46), QPointF(s * 0.82, s * 0.46));
}

void DrawRedo(QPainter& p, int s) {
    p.setPen(QPen(kStroke, 1.3, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath arrow;
    arrow.moveTo(s * 0.38, s * 0.20);
    arrow.lineTo(s * 0.70, s * 0.46);
    arrow.lineTo(s * 0.38, s * 0.72);
    p.drawPath(arrow);
    p.drawLine(QPointF(s * 0.70, s * 0.46), QPointF(s * 0.18, s * 0.46));
}

void DrawAdd(QPainter& p, int s) {
    p.setPen(QPen(kStroke, 1.3, Qt::SolidLine, Qt::SquareCap));
    p.drawLine(QPointF(s * 0.5, s * 0.20), QPointF(s * 0.5, s * 0.80));
    p.drawLine(QPointF(s * 0.20, s * 0.5), QPointF(s * 0.80, s * 0.5));
}

void DrawEdit(QPainter& p, int s) {
    p.setPen(QPen(kStroke, 1.2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    // 角状笔尖。
    QPainterPath pen;
    pen.moveTo(s * 0.24, s * 0.76);
    pen.lineTo(s * 0.24, s * 0.60);
    pen.lineTo(s * 0.62, s * 0.22);
    pen.lineTo(s * 0.78, s * 0.38);
    pen.lineTo(s * 0.40, s * 0.76);
    pen.closeSubpath();
    p.drawPath(pen);
}

void DrawExport(QPainter& p, int s) {
    p.setPen(QPen(kGold, 1.2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::NoBrush);
    // 门框。
    QPainterPath frame;
    frame.moveTo(s * 0.56, s * 0.22);
    frame.lineTo(s * 0.80, s * 0.22);
    frame.lineTo(s * 0.80, s * 0.78);
    frame.lineTo(s * 0.56, s * 0.78);
    p.drawPath(frame);
    // 箭头。
    p.setPen(QPen(kStroke, 1.2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.drawLine(QPointF(s * 0.16, s * 0.50), QPointF(s * 0.62, s * 0.50));
    QPainterPath head;
    head.moveTo(s * 0.48, s * 0.36);
    head.lineTo(s * 0.64, s * 0.50);
    head.lineTo(s * 0.48, s * 0.64);
    p.drawPath(head);
}

void DrawSearch(QPainter& p, int s) {
    p.setPen(QPen(kStroke, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(s * 0.42, s * 0.42), s * 0.22, s * 0.22);
    p.setPen(QPen(kStroke, 1.3, Qt::SolidLine, Qt::SquareCap));
    p.drawLine(QPointF(s * 0.58, s * 0.58), QPointF(s * 0.80, s * 0.80));
}

} // namespace

// ── 公共接口 ──

QIcon Overview()     { return Cached("nav_overview", 20, DrawOverview); }
QIcon Trigger()      { return Cached("nav_trigger", 20, DrawTrigger); }
QIcon ObjectEditor() { return Cached("nav_object", 20, DrawObjectEditor); }
QIcon MapInfo()      { return Cached("nav_mapinfo", 20, DrawMapInfo); }
QIcon ImportAssets() { return Cached("nav_import", 20, DrawImportAssets); }
QIcon Validate()     { return Cached("nav_validate", 20, DrawValidate); }

QIcon EcaEvent()     { return Cached("eca_event", 16, DrawEcaEvent); }
QIcon EcaCondition() { return Cached("eca_condition", 16, DrawEcaCondition); }
QIcon EcaAction()    { return Cached("eca_action", 16, DrawEcaAction); }
QIcon EcaCall()      { return Cached("eca_call", 16, DrawEcaCall); }
QIcon EcaDisabled()  { return Cached("eca_disabled", 16, DrawEcaDisabled); }
QIcon EcaComment()   { return Cached("eca_comment", 16, DrawEcaComment); }

QIcon Undo()   { return Cached("fn_undo", 16, DrawUndo); }
QIcon Redo()   { return Cached("fn_redo", 16, DrawRedo); }
QIcon Add()    { return Cached("fn_add", 16, DrawAdd); }
QIcon Edit()   { return Cached("fn_edit", 16, DrawEdit); }
QIcon Export() { return Cached("fn_export", 16, DrawExport); }
QIcon Search() { return Cached("fn_search", 16, DrawSearch); }

} // namespace GeometricIcons
