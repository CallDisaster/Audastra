#include "TriggerCodeChecking.h"
#include "Widgets/CodeViewHighlight.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QStringView>
#include <QByteArrayView>
#include <algorithm>
#include <span>

namespace War3Map::Modules {
namespace {
int Utf8ScalarLength(const QByteArray& bytes, int at) {
    const auto head = static_cast<unsigned char>(bytes[at]);
    if (head < 0x80) return 1;
    int length = head >= 0xc2 && head <= 0xdf ? 2 : head >= 0xe0 && head <= 0xef ? 3 : head >= 0xf0 && head <= 0xf4 ? 4 : 0;
    if (!length || at + length > bytes.size()) return 0;
    for (int n = 1; n < length; ++n) if ((static_cast<unsigned char>(bytes[at + n]) & 0xc0) != 0x80) return 0;
    const auto second = static_cast<unsigned char>(bytes[at + 1]);
    if ((head == 0xe0 && second < 0xa0) || (head == 0xed && second >= 0xa0) || (head == 0xf0 && second < 0x90) || (head == 0xf4 && second >= 0x90)) return 0;
    return length;
}
int LegacyScalarLength(const QByteArray& bytes, int at) {
    const auto head = static_cast<unsigned char>(bytes[at]);
    if (head < 0x81 || head > 0xfe || at + 1 >= bytes.size()) return 1;
    const auto second = static_cast<unsigned char>(bytes[at + 1]);
    // 有效 UTF8 起点优先保留。字节碰撞没有可证明的唯一编码，始终标为推测显示。
    if (Utf8ScalarLength(bytes, at + 1) > 1) return 1;
    if (second >= 0x30 && second <= 0x39 && at + 3 < bytes.size()) {
        const auto third = static_cast<unsigned char>(bytes[at + 2]), fourth = static_cast<unsigned char>(bytes[at + 3]);
        if (third >= 0x81 && third <= 0xfe && fourth >= 0x30 && fourth <= 0x39) return 4;
    }
    return second >= 0x40 && second <= 0xfe && second != 0x7f ? 2 : 1;
}
struct ByteLines {
    QByteArray bytes;
    QVector<QPair<qsizetype, qsizetype>> spans;
    bool complete = true;
    [[nodiscard]] QByteArrayView At(std::uint32_t line) const {
        if (!line || line > static_cast<std::uint32_t>(spans.size())) return {};
        const auto [offset, length] = spans[static_cast<qsizetype>(line - 1)];
        return QByteArrayView(bytes).sliced(offset, length);
    }
};
ByteLines MakeByteLines(QByteArray bytes, std::size_t& remaining, std::stop_token stop) {
    ByteLines output; output.bytes = std::move(bytes); qsizetype begin = 0;
    for (qsizetype at = 0; at <= output.bytes.size(); ++at) {
        if ((at & 4095) == 0 && stop.stop_requested()) { output.complete = false; break; }
        if (at < output.bytes.size() && output.bytes[at] != '\r' && output.bytes[at] != '\n') continue;
        if (!remaining) { output.complete = false; break; }
        --remaining; output.spans.push_back({begin, at - begin});
        if (at < output.bytes.size() && output.bytes[at] == '\r' && at + 1 < output.bytes.size() && output.bytes[at + 1] == '\n') ++at;
        begin = at + 1;
    }
    return output;
}
// 与 MakeByteLines 相同的换行规则（CRLF/CR/LF），保证解码后的行号与字节行号一致。
QStringList SplitViewLines(const QString& text) {
    QStringList lines; qsizetype begin = 0;
    for (qsizetype at = 0; at <= text.size(); ++at) {
        if (at < text.size() && text[at] != '\r' && text[at] != '\n') continue;
        lines.push_back(text.mid(begin, at - begin));
        if (at < text.size() && text[at] == '\r' && at + 1 < text.size() && text[at + 1] == '\n') ++at;
        begin = at + 1;
    }
    return lines;
}
// 声明的说明：紧邻其上的连续 // 注释块（与头文件函数注释习惯一致），以及声明行的行尾注释。
QString DeclarationComment(const QStringList& lines, std::uint32_t line) {
    if (!line || line > static_cast<std::uint32_t>(lines.size())) return {};
    QStringList block;
    for (auto at = static_cast<qsizetype>(line) - 2; at >= 0 && block.size() < 24; --at) {
        const auto text = QStringView(lines[at]).trimmed();
        if (!text.startsWith(u"//")) break;
        auto body = text.mid(2); while (body.startsWith(u'/')) body = body.mid(1);
        block.prepend(body.trimmed().toString());
    }
    while (!block.isEmpty() && block.front().isEmpty()) block.pop_front();
    const auto& declaration = lines[static_cast<qsizetype>(line) - 1];
    const auto trailing = declaration.indexOf(QStringLiteral("//"));
    if (trailing > 0 && !declaration.left(trailing).contains('"'))
        block.push_back(QStringView(declaration).mid(trailing + 2).trimmed().toString());
    return block.join('\n').trimmed();
}
struct Utf8Columns { bool valid = false, ascii = false; QVector<int> columns; };
Utf8Columns MakeColumns(QByteArrayView bytes, std::size_t& remaining) {
    Utf8Columns output; const auto count = qMin<qsizetype>(bytes.size(), 16384);
    if (!count) { output.ascii = output.valid = true; return output; }
    output.ascii = std::all_of(bytes.begin(), bytes.begin() + count, [](char byte) { return static_cast<unsigned char>(byte) < 0x80; });
    if (output.ascii) { output.valid = true; return output; }
    if (static_cast<std::size_t>(count + 1) > remaining) return output;
    remaining -= static_cast<std::size_t>(count + 1); output.columns.fill(-1, count + 1); output.columns[0] = 1;
    int column = 1;
    const auto verifiedPrefix = [&](qsizetype length) {
        output.columns.resize(length + 1); output.valid = true; return output;
    };
    for (qsizetype at = 0; at < count;) {
        const auto head = static_cast<unsigned char>(bytes[at]);
        const int length = head < 0x80 ? 1 : head >= 0xc2 && head <= 0xdf ? 2 : head >= 0xe0 && head <= 0xef ? 3 : head >= 0xf0 && head <= 0xf4 ? 4 : 0;
        if (!length || at + length > count) return verifiedPrefix(at);
        for (int n = 1; n < length; ++n) if ((static_cast<unsigned char>(bytes[at + n]) & 0xc0) != 0x80) return verifiedPrefix(at);
        if (length > 1) {
            const auto second = static_cast<unsigned char>(bytes[at + 1]);
            if ((head == 0xe0 && second < 0xa0) || (head == 0xed && second >= 0xa0) || (head == 0xf0 && second < 0x90) || (head == 0xf4 && second >= 0x90)) return verifiedPrefix(at);
        }
        at += length; column += length == 4 ? 2 : 1; output.columns[at] = column;
    }
    output.valid = true; return output;
}
}

UiKit::CodeCheckResult PresentCodeCheckResult(const Services::CodeCheckResult& result, const QString& text, const QString& source, std::stop_token stop) {
    UiKit::CodeCheckResult view;
    view.generation = result.generation; view.completed = result.completed; view.syntaxChecked = result.syntaxChecked;
    view.semanticChecked = result.semanticChecked; view.contextComplete = result.contextComplete; view.cancelled = result.cancelled;
    view.externalSourcesComplete = result.externalSourcesComplete;
    view.reason = QString::fromUtf8(result.reason); view.contextBinding = QString::fromUtf8(result.contextBinding);
    const auto cancelled = [&] {
        view.completed = false; view.syntaxChecked = false; view.semanticChecked = false; view.cancelled = true;
        view.reason = QObject::tr("代码结果呈现已取消。"); view.diagnostics.clear(); view.declarations.clear(); view.externalSources.clear();
        view.semanticDefinitions.clear(); view.semanticReferences.clear(); view.semanticIndexComplete = false; return view;
    };
    if (stop.stop_requested()) return cancelled();
    // 同一请求正文的列按字节转换，不用生成脚本或当前变动的文档猜位置。
    std::size_t remainingLines = 524288, remainingColumns = 8 * 1024 * 1024;
    QHash<QString, ByteLines> sourceByteLines;
    sourceByteLines.insert(source, MakeByteLines(text.toUtf8(), remainingLines, stop));
    QHash<QString, QStringList> sourceTextLines;
    sourceTextLines.insert(source, SplitViewLines(text));
    for (const auto& external : result.externalSources) {
        if (stop.stop_requested()) return cancelled();
        const auto bytes = QByteArray::fromStdString(external.text);
        const auto decoded = DecodeMapCodeForView(bytes, stop);
        if (stop.stop_requested()) return cancelled();
        const auto name = QString::fromUtf8(external.name);
        sourceTextLines.insert(name, SplitViewLines(decoded.text));
        view.externalSources.push_back({name, decoded.text, decoded.reason, bytes});
        sourceByteLines.insert(name, MakeByteLines(bytes, remainingLines, stop));
    }
    QHash<QString, QHash<std::uint32_t, Utf8Columns>> columnCache;
    const auto convertColumn = [&](const QString& name, std::uint32_t line, std::uint32_t byteColumn) -> int {
        if (!byteColumn || byteColumn > 16385) return 0;
        const auto table = sourceByteLines.constFind(name);
        if (table == sourceByteLines.cend() || !line || line > static_cast<std::uint32_t>(table->spans.size())) return 0;
        const auto bytes = table->At(line); const auto offset = static_cast<qsizetype>(byteColumn - 1);
        if (offset > bytes.size()) return 0;
        auto& sourceColumns = columnCache[name]; auto found = sourceColumns.find(line);
        if (found == sourceColumns.end()) found = sourceColumns.insert(line, MakeColumns(bytes, remainingColumns));
        if (!found->valid) return 0;
        if (found->ascii) return static_cast<int>(byteColumn);
        return offset < found->columns.size() && found->columns[offset] > 0 ? found->columns[offset] : 0;
    };
    int count = 0;
    for (const auto& issue : result.diagnostics) {
        if ((count++ & 63) == 0 && stop.stop_requested()) return cancelled();
        UiKit::CodeDiagnostic presented;
        presented.severity = issue.severity == DiagnosticSeverity::Error ? UiKit::CodeSeverity::Error
            : issue.severity == DiagnosticSeverity::Warning ? UiKit::CodeSeverity::Warning : UiKit::CodeSeverity::Information;
        presented.source = QString::fromUtf8(issue.source); presented.code = QString::fromUtf8(issue.code); presented.message = QString::fromUtf8(issue.message);
        presented.line = static_cast<int>(issue.line); presented.column = static_cast<int>(issue.byteColumn); presented.length = static_cast<int>(issue.byteLength); presented.originMapped = issue.originMapped;
        presented.byteRangeExact = issue.byteRangeExact;
        const auto column = convertColumn(presented.source, issue.line, issue.byteColumn);
        const auto endColumn = convertColumn(presented.source, issue.line, issue.byteColumn + issue.byteLength);
        presented.column = column ? column : 1;
        presented.length = presented.source == source && column && endColumn ? qMax(0, endColumn - column) : 0;
        if (!column || !endColumn) presented.byteRangeExact = false;
        view.diagnostics.push_back(std::move(presented));
    }
    for (const auto& symbol : result.declarations.symbols) {
        if ((count++ & 63) == 0 && stop.stop_requested()) return cancelled();
        UiKit::CodeCompletionItem item;
        item.name = QString::fromUtf8(symbol.name); item.source = QString::fromUtf8(symbol.source);
        item.line = static_cast<int>(symbol.line); item.column = static_cast<int>(symbol.byteColumn);
        const auto type = QString::fromUtf8(symbol.type);
        if (symbol.kind == Jass::DeclarationKind::Function || symbol.kind == Jass::DeclarationKind::Native) {
            item.kind = symbol.kind == Jass::DeclarationKind::Native ? QStringLiteral("原生函数") : QStringLiteral("函数");
            QStringList parameters; for (const auto& parameter : symbol.parameters) parameters.push_back(QString::fromUtf8(parameter.type) + ' ' + QString::fromUtf8(parameter.name));
            item.signature = item.name + QStringLiteral(" takes ") + (parameters.isEmpty() ? QStringLiteral("nothing") : parameters.join(QStringLiteral(", "))) + QStringLiteral(" returns ") + type;
        } else {
            item.kind = symbol.kind == Jass::DeclarationKind::Type ? QStringLiteral("类型") : QStringLiteral("全局变量");
            item.signature = (symbol.constant ? QStringLiteral("constant ") : QString{}) + type + (symbol.array ? QStringLiteral(" array ") : QStringLiteral(" ")) + item.name;
        }
        if (const auto lines = sourceTextLines.constFind(item.source); lines != sourceTextLines.cend())
            item.documentation = DeclarationComment(*lines, symbol.line);
        const auto column = convertColumn(item.source, symbol.line, symbol.byteColumn); item.column = column ? column : 1;
        view.declarations.push_back(std::move(item));
    }
    const auto& semantic = result.semanticIndex;
    view.semanticIndexComplete = semantic.parsed.completed;
    view.semanticIndexReason = QString::fromUtf8(semantic.parsed.reason);
    if (std::any_of(sourceByteLines.begin(), sourceByteLines.end(), [](const auto& lines) { return !lines.complete; })) {
        view.semanticIndexComplete = false;
        view.semanticIndexReason += QObject::tr(" 原行位置达到显示预算，仅保留已定位的引用。");
    }
    QVector<QString> semanticSources; for (const auto& name : semantic.sources) semanticSources.push_back(QString::fromUtf8(name));
    const auto identity = [](Jass::SymbolKey key) { return (static_cast<std::uint64_t>(key.file) << 32) | key.offset; };
    const auto position = [&](const Services::CodeSymbolPosition& input, auto& output) {
        if (input.file >= static_cast<std::uint32_t>(semanticSources.size())) return;
        output.source = semanticSources[input.file]; output.line = static_cast<int>(input.line); output.column = 1;
        // 超长单行只导航行；后台不为每个引用重复解码无界行前缀。
        if (!input.exact || input.byteColumn > 16384) return;
        const auto column = convertColumn(output.source, input.line, input.byteColumn);
        const auto endColumn = convertColumn(output.source, input.line, input.byteColumn + input.byteLength);
        if (!column || !endColumn) return;
        output.column = column; output.length = qMax(0, endColumn - column); output.exact = true;
    };
    for (std::size_t i = 0; i < semantic.parsed.definitions.size() && i < semantic.definitions.size(); ++i) {
        if ((count++ & 63) == 0 && stop.stop_requested()) return cancelled();
        const auto& input = semantic.parsed.definitions[i]; UiKit::CodeSemanticDefinition output;
        output.identity = identity(input.position.key); output.name = QString::fromUtf8(input.name);
        output.kind = input.kind == Jass::SymbolKind::Local ? QObject::tr("局部变量")
            : input.kind == Jass::SymbolKind::Parameter ? QObject::tr("参数")
            : input.kind == Jass::SymbolKind::Global ? QObject::tr("全局变量")
            : input.kind == Jass::SymbolKind::Native ? QObject::tr("原生函数")
            : input.kind == Jass::SymbolKind::Type ? QObject::tr("类型") : QObject::tr("函数");
        output.signature = QString::fromUtf8(input.type) + ' ' + output.name;
        position(semantic.definitions[i], output); view.semanticDefinitions.push_back(std::move(output));
    }
    for (std::size_t i = 0; i < semantic.parsed.references.size() && i < semantic.references.size(); ++i) {
        if ((count++ & 63) == 0 && stop.stop_requested()) return cancelled();
        const auto& input = semantic.parsed.references[i]; UiKit::CodeSemanticReference output;
        output.targetKnown = input.target.has_value(); if (input.target) output.target = identity(*input.target);
        output.use = input.use == Jass::SymbolUse::Write ? QObject::tr("写入")
            : input.use == Jass::SymbolUse::Call ? QObject::tr("调用")
            : input.use == Jass::SymbolUse::FunctionReference ? QObject::tr("函数引用") : QObject::tr("读取");
        position(semantic.references[i], output); view.semanticReferences.push_back(std::move(output));
    }
    return stop.stop_requested() ? cancelled() : view;
}

UiKit::CodeCheckTask PrepareTriggerCodeCheck(const QString& text, std::uint64_t generation, const QString& source,
    UiKit::CodeLanguage language, Services::CodeCheckContext context) {
    Services::CodeCheckRequest request; request.source = {source.toStdString(), text.toStdString()}; request.generation = generation;
    request.language = language == UiKit::CodeLanguage::Lua ? Services::CodeLanguage::Lua53
        : language == UiKit::CodeLanguage::VJass ? Services::CodeLanguage::VJass : Services::CodeLanguage::Jass;
    request.context = std::move(context);
    if (!request.context.complete && request.context.reason.empty()) request.context.reason = "尚未冻结完整地图与运行库声明，仅检查当前正文的语法；保存仍走完整编译门禁。";
    return [request = std::move(request), text, source](std::stop_token stop) {
        return PresentCodeCheckResult(Services::CheckCode(request, stop), text, source, stop);
    };
}

UiKit::CodeCheckTask PrepareProjectCodeCheck(std::shared_ptr<const Services::CodeCheckProjectSnapshot> snapshot,
    std::shared_ptr<Services::CodeCheckProjectCache> cache, Trigger::CodeBlockTarget target, const QString& text,
    std::uint64_t generation, UiKit::CodeLanguage language, std::shared_ptr<const Services::CodeCheckLuaSources> lua) {
    const auto codeLanguage = language == UiKit::CodeLanguage::Lua ? Services::CodeLanguage::Lua53
        : language == UiKit::CodeLanguage::VJass ? Services::CodeLanguage::VJass : Services::CodeLanguage::Jass;
    const auto source = QString::fromUtf8(Services::CodeCheckTargetSourceName(target, codeLanguage));
    return [snapshot = std::move(snapshot), cache = std::move(cache), target, text, generation, codeLanguage, source, lua = std::move(lua)](std::stop_token stop) {
        auto checked = std::make_shared<const Services::CodeCheckResult>(Services::CheckProjectCode(snapshot, *cache, target, text.toStdString(),
            codeLanguage, generation, stop, lua ? *lua : Services::CodeCheckLuaSources{}));
        auto presented = PresentCodeCheckResult(*checked, text, source, stop);
        if (!presented.cancelled) presented.ownedProjectIndex = std::move(checked);
        return presented;
    };
}

QHash<QString, QString> UiFunctionNotes(const Trigger::UiCatalog& catalog) {
    QHash<QString, QString> notes;
    for (const auto kind : {Trigger::UiDefinitionKind::Call, Trigger::UiDefinitionKind::Action,
             Trigger::UiDefinitionKind::Condition, Trigger::UiDefinitionKind::Event}) {
        for (const auto* definition : catalog.Definitions(kind)) {
            const auto note = Trigger::PresentDefinitionNote(*definition, catalog);
            if (note.title.empty() && note.hint.empty()) continue;
            // 脚本名覆盖时代码里出现的是脚本名；同一函数的多类定义只取第一条说明。
            QString name;
            for (const auto& [key, value] : definition->Active().fields)
                if (key == "script_name" && !value.empty()) name = QString::fromUtf8(value);
            if (name.isEmpty()) name = QString::fromUtf8(definition->name);
            if (notes.contains(name)) continue;
            auto text = QString::fromUtf8(note.title);
            if (!note.hint.empty()) text += (text.isEmpty() ? QString{} : QStringLiteral("\n")) + QString::fromUtf8(note.hint);
            notes.insert(name, text);
        }
    }
    return notes;
}

UiKit::CodeDecodedSource DecodeMapCodeForView(const QByteArray& bytes, std::stop_token stop) {
    UiKit::CodeDecodedSource result;
    int begin = 0, at = 0, nextPoll = 0;
    while (at < bytes.size()) {
        if (at >= nextPoll) { nextPoll = at + 4096; if (stop.stop_requested()) return {{}, false, QObject::tr("查看已取消")}; }
        const int length = Utf8ScalarLength(bytes, at);
        if (length) { at += length; continue; }
        result.text += QString::fromUtf8(bytes.constData() + begin, at - begin);
        begin = at;
        do {
            if (at >= nextPoll) { nextPoll = at + 4096; if (stop.stop_requested()) return {{}, false, QObject::tr("查看已取消")}; }
            at += LegacyScalarLength(bytes, at);
        } while (at < bytes.size() && !Utf8ScalarLength(bytes, at));
        const auto legacy = Trigger::DecodeSourceText(std::span(reinterpret_cast<const std::byte*>(bytes.constData() + begin), static_cast<std::size_t>(at - begin)));
        result.text += QString::fromUtf8(legacy.text); result.complete = false; begin = at;
    }
    result.text += QString::fromUtf8(bytes.constData() + begin, bytes.size() - begin);
    if (!result.complete) result.reason = QObject::tr("混合/未声明编码：有效 UTF-8 保留，非法片段按旧编码推测；仅供查看，原字节不变，可切换原字节视图。");
    return result;
}

UiKit::CodeDecodedSource ShowMapCodeRawBytes(const QByteArray& bytes, std::stop_token stop) {
    UiKit::CodeDecodedSource result; result.reason = QObject::tr("原字节视图：非 ASCII 字节显示为 \\xHH，仅供查看。");
    static constexpr char hex[] = "0123456789ABCDEF";
    result.text.reserve(bytes.size());
    for (int i = 0; i < bytes.size(); ++i) {
        if ((i & 4095) == 0 && stop.stop_requested()) return {{}, false, QObject::tr("查看已取消")};
        const auto byte = static_cast<unsigned char>(bytes[i]);
        if ((byte >= 32 && byte < 127 && byte != '\\') || byte == '\n' || byte == '\r' || byte == '\t') result.text += QChar(byte);
        else if (byte == '\\') result.text += QStringLiteral("\\\\");
        else { result.text += QStringLiteral("\\x"); result.text += QChar(hex[byte >> 4]); result.text += QChar(hex[byte & 15]); }
    }
    return result;
}

QVector<UiKit::CodeCompletionItem> CodeDeclarationsForView(const QString& text, const QString& source, UiKit::CodeLanguage language) {
    auto result = UiKit::ScanCode(text, language).declarations;
    for (auto& declaration : result) declaration.source = source;
    return result;
}
} // namespace War3Map::Modules
