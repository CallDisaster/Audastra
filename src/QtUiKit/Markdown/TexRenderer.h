#pragma once

#include <QColor>
#include <QImage>
#include <QString>

namespace War3Map::UiKit {

struct TexStyle {
    QColor color = Qt::white;
    QColor errorColor = QColor(0xe0, 0x6c, 0x6c);
    QString textFamily;      // \text{} 与中文回退所用字体；空则用应用默认字体
    qreal pixelSize = 15;
    qreal devicePixelRatio = 1;
    bool display = false;    // 独立公式块（分式全尺寸、求和上下限居中）
};

struct TexImage {
    QImage image;            // 已设置 devicePixelRatio；逻辑尺寸为 width × height
    int width = 0;
    int height = 0;
    int baseline = 0;        // 从图片顶部到基线的逻辑像素
    bool complete = true;    // 存在未识别命令或括号不配对时为 false（仍会画出原文）
};

// 常用 TeX 数学子集原生排版：分式、根号、上下标、求和/积分上下限、伸缩括号、矩阵/cases/aligned、
// 重音、\text、\color 等。不认识的命令以错误色原样画出，不静默丢弃。
[[nodiscard]] TexImage RenderTex(const QString& source, const TexStyle& style);

} // namespace War3Map::UiKit
