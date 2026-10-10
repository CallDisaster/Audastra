#pragma once

#include <QColor>
#include <QString>

#include <functional>

class QTextDocument;

namespace War3Map::UiKit {

struct MarkdownTheme {
    QColor text{0xc9, 0xcf, 0xcc};
    QColor muted{0x9b, 0xa5, 0xa0};
    QColor link{0x7f, 0xd6, 0xb4};
    QColor codeText{0xd8, 0xde, 0xdb};
    QColor codeBackground{0x1f, 0x24, 0x26};
    QColor inlineCodeBackground{0x2b, 0x31, 0x33};
    QColor border{0x3a, 0x42, 0x40};
    QColor tableHeader{0x26, 0x2c, 0x2e};
    QColor quoteBar{0x4c, 0x57, 0x54};
    QColor mathError{0xe0, 0x6c, 0x6c};
    QString textFamily;
    QString codeFamily;
    int pixelSize = 15;
    bool dark = true;        // 代码高亮配色
    qreal devicePixelRatio = 1;
    QColor headingText{0xe6, 0xe9, 0xe8};
    QColor inlineCodeAccent{0xa8, 0xc6, 0xb6};
};

// 公式回调：返回已登记的图片 HTML（<img …>）；返回空串时按原文显示。
using MathImageFactory = std::function<QString(const QString& tex, bool display)>;

// GFM（表格、删除线、任务列表、脚注、自动链接）+ TeX 公式（$…$、$$…$$、\(…\)、\[…\]），
// 规则对齐 DSH 会话渲染：中文标点旁的 ** 也能闭合；流式阶段不解析公式（math=false）。
// 模型正文里的 HTML 一律按文字显示，不加载远程或本地图片。
[[nodiscard]] QString MarkdownToHtml(const QString& markdown, const MarkdownTheme& theme, bool math,
    const MathImageFactory& mathImage = {});

// 渲染到文档并替换原内容；公式排版成图片资源登记在该文档内。
void RenderMarkdown(QTextDocument& document, const QString& markdown, const MarkdownTheme& theme, bool math);

} // namespace War3Map::UiKit
