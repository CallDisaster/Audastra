#include "Widgets/CodeJassAssist.h"
#include "Widgets/CodeViewHighlight.h"

#include <QRegularExpression>
#include <QVector>

namespace War3Map::UiKit {
namespace {
bool IdentifierPart(QChar value) { return value.isLetterOrNumber() || value == '_'; }
}

bool JassSignatureLabel(const QString& name, const QString& signature, CodeSignatureHelp& help) {
    static const QRegularExpression pattern(QStringLiteral("\\btakes\\s+(.*?)\\s+returns\\s+(\\S+)"));
    const auto match = pattern.match(signature);
    if (!match.hasMatch()) return false;
    help.label = name + QStringLiteral(" takes ");
    help.parameters.clear();
    const auto parameters = match.captured(1).trimmed();
    if (parameters == QStringLiteral("nothing")) {
        help.label += parameters;
    } else {
        const auto items = parameters.split(',');
        for (int index = 0; index < items.size(); ++index) {
            if (index) help.label += QStringLiteral(", ");
            const auto parameter = items[index].simplified();
            help.parameters.push_back({static_cast<int>(help.label.size()), static_cast<int>(help.label.size() + parameter.size())});
            help.label += parameter;
        }
    }
    help.label += QStringLiteral(" returns ") + match.captured(2);
    return true;
}

std::optional<CodeSignatureHelp> JassSignatureHelp(const QString& line, int column, CodeLanguage language,
    const std::function<QString(const QString&)>& signatureOf) {
    column = qBound(0, column, static_cast<int>(line.size()));
    const auto before = line.left(column);
    // 注释、字符串和四字码里的括号与逗号不算。
    QVector<bool> text(before.size(), false);
    const auto scanned = ScanCode(before, language);
    if (!scanned.lines.isEmpty()) {
        for (const auto& span : scanned.lines.front().spans) {
            if (span.kind != CodeTokenKind::Comment && span.kind != CodeTokenKind::String && span.kind != CodeTokenKind::Number) continue;
            for (int at = span.start; at < span.start + span.length && at < text.size(); ++at) text[at] = true;
            if (span.start + span.length < before.size() || span.kind == CodeTokenKind::Number) continue;
            // 光标停在注释或未闭合的字符串里。
            const bool closed = span.kind == CodeTokenKind::String && span.length >= 2
                && before[span.start + span.length - 1] == before[span.start];
            if (!closed) return std::nullopt;
        }
    }
    struct Open { int at = 0; int commas = 0; };
    QVector<Open> stack;
    for (int at = 0; at < before.size(); ++at) {
        if (text[at]) continue;
        const auto ch = before[at];
        if (ch == '(' || ch == '[') stack.push_back({at, ch == '[' ? -1 : 0});
        else if ((ch == ')' || ch == ']') && !stack.isEmpty()) stack.pop_back();
        else if (ch == ',' && !stack.isEmpty() && stack.back().commas >= 0) ++stack.back().commas;
    }
    // 从最内层往外找第一个函数调用括号（数组下标 [ ] 和分组括号跳过）。
    for (auto open = stack.crbegin(); open != stack.crend(); ++open) {
        if (open->commas < 0) continue;
        int end = open->at;
        while (end > 0 && before[end - 1].isSpace()) --end;
        int start = end;
        while (start > 0 && IdentifierPart(before[start - 1])) --start;
        if (start == end) continue;
        const auto name = before.mid(start, end - start);
        const auto signature = signatureOf(name);
        CodeSignatureHelp help;
        if (signature.isEmpty() || !JassSignatureLabel(name, signature, help)) continue;
        help.active = help.parameters.isEmpty() ? -1 : qMin(open->commas, static_cast<int>(help.parameters.size()) - 1);
        if (open->commas >= help.parameters.size() && !help.parameters.isEmpty()) help.active = -1; // 参数已经多于声明
        return help;
    }
    return std::nullopt;
}

} // namespace War3Map::UiKit
