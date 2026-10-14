#pragma once

#include "Widgets/CodeViewTypes.h"

#include <QHash>
#include <QPalette>
#include <QTextCharFormat>
#include <QTextLayout>

namespace War3Map::UiKit {

// 前四类由词法扫描产生；其余在着色时按语义索引或局部词法规则细分（VS 2022 配色分类）。
enum class CodeTokenKind : std::uint8_t {
    Keyword, String, Comment, Number,
    ControlKeyword, Type, Function, Native, Local, Parameter, Global, Constant, Operator,
};
struct CodeTokenSpan { int start = 0, length = 0; CodeTokenKind kind = CodeTokenKind::Keyword; };
struct CodeLexLine { QVector<CodeTokenSpan> spans; int foldEnd = -1; };
struct CodeLexSnapshot {
    QVector<CodeLexLine> lines;
    QHash<int, int> brackets;
    QHash<QString, QVector<int>> references;
    QVector<CodeCompletionItem> declarations;
    bool referenceIndexComplete = true;
    bool budgetExceeded = false;
    QString reason;
    bool cancelled = false;
};

// 纯展示词法扫描，不冒充语法/语义检查。偏移为 QString UTF-16 偏移。
// 语言关键字（vJass 含 library/struct 等扩展），按字母排序。
[[nodiscard]] QStringList CodeKeywords(CodeLanguage language);
[[nodiscard]] CodeLexSnapshot ScanCode(const QString& text, CodeLanguage language, std::stop_token stop = {});
// 名称到语义分类；来自检查器的真实声明（函数/原生函数/类型/全局变量/常量）及当前源的局部量与参数。
using CodeSymbolKinds = QHash<QString, CodeTokenKind>;
[[nodiscard]] CodeTokenKind CodeSymbolKindFor(const QString& kind, const QString& signature);
[[nodiscard]] QColor CodeTokenColor(CodeTokenKind kind, bool light);
// 只为可见行计算：词法片段之外的标识符按语义索引着色，未知时用局部词法规则（调用、声明位置）。
[[nodiscard]] QList<QTextLayout::FormatRange> CodeFormats(const CodeLexLine& line, QStringView text,
    CodeLanguage language, const CodeSymbolKinds& symbols, const QPalette& palette);

} // namespace War3Map::UiKit
