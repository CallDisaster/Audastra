#include "Widgets/CodeView.h"
#include "Widgets/CodeAssistPopup.h"
#include "Widgets/CodeViewGutter.h"
#include "Widgets/CodeViewHighlight.h"
#include "Fonts/FontLoader.h"

#include <QAbstractItemView>
#include <QContextMenuEvent>
#include <QDialog>
#include <QLabel>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QFutureWatcher>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextDocumentLayout>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QPromise>
#include <QSet>
#include <QStringDecoder>
#include <QTextBlock>
#include <QFontMetricsF>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QToolTip>

#include <optional>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace War3Map::UiKit {
namespace {
struct OwnedDocument {
    QTextDocument* document = nullptr;
    ~OwnedDocument() { if (document) document->deleteLater(); }
};
struct WorkResult {
    std::uint64_t generation = 0;
    std::shared_ptr<OwnedDocument> document;
    CodeLexSnapshot lexical;
    std::optional<CodeCheckResult> check;
    CodeDecodedSource decoded;
    QString failure;
};
struct WorkRequest {
    std::uint64_t generation = 0;
    bool load = false;
    QByteArray bytes;
    QString text, source;
    CodeLanguage language = CodeLanguage::Jass;
    QFont font;
    QThread* uiThread = nullptr;
    DecodeCodeBytes decoder;
    CodeCheckTask check;
};
CodeDecodedSource StrictUtf8(const QByteArray& bytes, std::stop_token) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    CodeDecodedSource result{decoder.decode(bytes)};
    result.complete = !decoder.hasError();
    if (!result.complete) result.reason = QObject::tr("源编码未声明，包含非法 UTF-8；替换字符仅用于查看，不能据此转码保存。");
    return result;
}
QTextCursor SymbolCursor(QTextCursor cursor) {
    const auto block = cursor.block(); const auto text = block.text();
    int start = cursor.positionInBlock(), end = start;
    const auto symbol = [](QChar ch) { return ch.isLetterOrNumber() || ch == '_' || ch == '.' || ch == ':'; };
    while (start > 0 && symbol(text[start - 1])) --start;
    while (end < text.size() && symbol(text[end])) ++end;
    cursor.setPosition(block.position() + start); cursor.setPosition(block.position() + end, QTextCursor::KeepAnchor);
    return cursor;
}
bool IdentifierChar(QChar ch) { return ch.isLetterOrNumber() || ch == '_'; }
bool PathChar(QChar ch) { return IdentifierChar(ch) || ch == '.' || ch == '/' || ch == '\\'; }
// 光标前的文本是否停在注释或字符串里（只看当前行）。
bool InTextToken(const QString& before, CodeLanguage language) {
    const auto tokens = ScanCode(before, language);
    if (!tokens.lines.isEmpty()) for (const auto& span : tokens.lines.front().spans)
        if (span.start + span.length == before.size() && (span.kind == CodeTokenKind::Comment || span.kind == CodeTokenKind::String)) return true;
    return false;
}
CodeCompletionKind CompletionKindFor(const QString& kind, const QString& signature) {
    switch (CodeSymbolKindFor(kind, signature)) {
    case CodeTokenKind::Native: case CodeTokenKind::Function: return CodeCompletionKind::Function;
    case CodeTokenKind::Type: return CodeCompletionKind::Type;
    case CodeTokenKind::Local: return CodeCompletionKind::Local;
    case CodeTokenKind::Parameter: return CodeCompletionKind::Parameter;
    case CodeTokenKind::Constant: return CodeCompletionKind::Constant;
    default: return CodeCompletionKind::Variable;
    }
}
void ApplyCodeTabWidth(QPlainTextEdit& editor) {
    editor.setTabStopDistance(QFontMetricsF(editor.font()).horizontalAdvance(QLatin1Char(' ')) * 4);
}
}

struct CodeView::Impl {
    CodeViewGutter* gutter = nullptr;
    QTextDocument* document = nullptr;
    QTimer* debounce = nullptr;
    QFutureWatcher<std::shared_ptr<WorkResult>>* watcher = nullptr;
    std::stop_source stop;
    std::optional<WorkRequest> pending;
    CodeWorkerStats stats;
    CodeLexSnapshot lexical;
    CodeCheckResult result;
    QSet<int> formatted, folded;
    CodeLanguage language = CodeLanguage::Jass;
    QString source, decodeNotice;
    QByteArray loadingBytes;
    DecodeCodeBytes decoder = StrictUtf8;
    PrepareCodeCheck prepare;
    std::function<bool(const QString&)> validateBinding;
    QVector<CodeCompletionItem> completions;
    QVector<CodeCompletionItem> externalCompletions;
    CodeSymbolKinds symbols, semanticLocals;
    QHash<QString, QString> notes;
    CodeAssist assist;
    CodeCompletionPopup* popup = nullptr;
    CodeSignatureTip* signatureTip = nullptr;
    int replaceStart = -1;
    bool pathList = false, typing = false;
    QPointer<QDialog> externalDialog;
    std::uint64_t generation = 0;
    bool loading = false, suppress = false, formatting = false, desiredEditable = false;
};

CodeView::CodeView(QWidget* parent) : QPlainTextEdit(parent), impl_(std::make_unique<Impl>()) {
    // 代码族名独立于界面字体；Tab 的显示宽度与四空格缩进一致。
    setFont(MonoFont(Tokens::Size::kFontBody));
    ApplyCodeTabWidth(*this);
    setReadOnly(true); setLineWrapMode(QPlainTextEdit::NoWrap); setUndoRedoEnabled(false); setMouseTracking(true);
    impl_->gutter = new CodeViewGutter(this); impl_->gutter->setObjectName("codeGutter");
    impl_->debounce = new QTimer(this); impl_->debounce->setSingleShot(true); impl_->debounce->setInterval(180);
    connect(impl_->debounce, &QTimer::timeout, this, [this] { QueueAnalysis(true); });
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int) { UpdateGutterWidth(); });
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeView::UpdateGutter);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeView::UpdateSelections);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        // 点击或方向键移动光标：列表按新位置重新过滤，参数提示按新位置重算。
        if (impl_->typing) return;
        if (impl_->popup->isVisible()) RefilterCompletion();
        if (impl_->signatureTip->isVisible()) UpdateSignature();
    });
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (impl_->suppress || impl_->formatting) return;
        if (impl_->loading) { impl_->loading = false; impl_->loadingBytes.clear(); setReadOnly(!impl_->desiredEditable); }
        ++impl_->generation; impl_->stop.request_stop(); impl_->pending.reset(); impl_->stats.pending = 0;
        // 编辑可能改变块号；旧折叠身份不能套到新正文上。
        if (!impl_->folded.isEmpty()) {
            impl_->formatting = true;
            for (auto block = document()->begin(); block.isValid(); block = block.next()) { block.setVisible(true); block.setLineCount(1); }
            impl_->folded.clear(); document()->markContentsDirty(0, document()->characterCount()); impl_->formatting = false;
        }
        impl_->lexical = {}; impl_->formatted.clear();
        impl_->result = {}; impl_->result.generation = impl_->generation;
        if (!impl_->typing) HideAssist();
        UpdateCompletionItems();
        UpdateSelections(); impl_->debounce->start(); emit CheckStateChanged();
    });
    impl_->popup = new CodeCompletionPopup(this);
    impl_->signatureTip = new CodeSignatureTip(this);
    connect(impl_->popup, &CodeCompletionPopup::Accepted, this, &CodeView::AcceptCompletion);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &CodeView::RepositionAssist);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &CodeView::RepositionAssist);
    UpdateGutterWidth();
}
CodeView::~CodeView() { impl_->stop.request_stop(); impl_->pending.reset(); }
void CodeView::SetEditable(bool editable) { impl_->desiredEditable = editable; setReadOnly(impl_->loading || !editable); setUndoRedoEnabled(editable); }
void CodeView::SetLanguage(CodeLanguage language) {
    if (impl_->language == language) return;
    const bool loading = impl_->loading; const auto bytes = impl_->loadingBytes;
    impl_->language = language; CancelPendingWork();
    if (loading) SetBytesAsync(bytes); else QueueLexicalThenCheck();
}
CodeLanguage CodeView::Language() const { return impl_->language; }
void CodeView::SetSourceName(QString name) {
    if (impl_->source == name) return;
    const bool loading = impl_->loading; const auto bytes = impl_->loadingBytes;
    impl_->source = std::move(name); CancelPendingWork();
    if (loading) SetBytesAsync(bytes); else QueueLexicalThenCheck();
}
QString CodeView::SourceName() const { return impl_->source; }
QString CodeView::DecodeNotice() const { return impl_->decodeNotice; }
void CodeView::SetDecoder(DecodeCodeBytes decoder) {
    const bool loading = impl_->loading;
    impl_->decoder = decoder ? std::move(decoder) : DecodeCodeBytes(StrictUtf8);
    if (loading) SetBytesAsync(impl_->loadingBytes); else CancelPendingWork();
}
void CodeView::SetPrepareCheck(PrepareCodeCheck prepare) {
    impl_->prepare = std::move(prepare); ++impl_->generation; impl_->result = {}; UpdateSelections(); UpdateCompletionItems();
    if (impl_->loading) SetBytesAsync(impl_->loadingBytes); else QueueLexicalThenCheck();
}
void CodeView::SetCheckBindingValidator(std::function<bool(const QString&)> validate) { impl_->validateBinding = std::move(validate); }
bool CodeView::IsLoading() const noexcept { return impl_->loading; }
bool CodeView::IsChecking() const noexcept { return impl_->watcher || impl_->pending.has_value() || impl_->debounce->isActive(); }
std::uint64_t CodeView::Generation() const noexcept { return impl_->generation; }
CodeWorkerStats CodeView::WorkerStats() const { return impl_->stats; }
const CodeCheckResult& CodeView::CheckResult() const { return impl_->result; }
const CodeLexSnapshot& CodeView::LexSnapshot() const { return impl_->lexical; }

void CodeView::CancelPendingWork() {
    ++impl_->generation; impl_->debounce->stop(); impl_->stop.request_stop();
    impl_->pending.reset(); impl_->stats.pending = 0; impl_->loading = false; impl_->loadingBytes.clear();
    setReadOnly(!impl_->desiredEditable);
    if (!impl_->folded.isEmpty()) {
        impl_->formatting = true;
        for (auto block = document()->begin(); block.isValid(); block = block.next()) { block.setVisible(true); block.setLineCount(1); }
        impl_->folded.clear(); document()->markContentsDirty(0, document()->characterCount()); impl_->formatting = false;
    }
    impl_->result = {}; impl_->lexical = {}; impl_->formatted.clear(); impl_->semanticLocals.clear();
    HideAssist(); if (impl_->externalDialog) impl_->externalDialog->close(); UpdateCompletionItems();
    UpdateSelections(); emit CheckStateChanged();
}
void CodeView::SetText(const QString& text) {
    if (text.size() > 2 * 1024 * 1024) { SetBytesAsync(text.toUtf8()); return; }
    CancelPendingWork(); impl_->decodeNotice.clear(); impl_->suppress = true;
    auto* doc = new QTextDocument(this); doc->setDocumentLayout(new QPlainTextDocumentLayout(doc));
    doc->setDefaultFont(font()); doc->setPlainText(text); AdoptDocument(doc); impl_->suppress = false; QueueLexicalThenCheck();
}
void CodeView::SetBytesAsync(QByteArray bytes) {
    CancelPendingWork(); impl_->loadingBytes = bytes; impl_->loading = true; setReadOnly(true); impl_->decodeNotice.clear(); setPlaceholderText(tr("正在载入…"));
    if (bytes.size() > 32 * 1024 * 1024) {
        impl_->loading = false; setReadOnly(!impl_->desiredEditable); impl_->loadingBytes.clear(); impl_->result.reason = tr("脚本超过 32 MiB 查看预算；当前正文保留。"); emit CheckStateChanged(); return;
    }
    impl_->suppress = true; AdoptDocument(nullptr); impl_->suppress = false;
    WorkRequest task; task.generation = impl_->generation; task.load = true; task.bytes = std::move(bytes);
    task.language = impl_->language; task.source = impl_->source; task.font = font(); task.uiThread = thread(); task.decoder = impl_->decoder;
    impl_->pending = std::move(task); impl_->stats.pending = 1; impl_->stats.peakPending = 1;
    StartPendingWork(); emit CheckStateChanged();
}
void CodeView::RequestCheck() { impl_->debounce->stop(); QueueAnalysis(true); }
// 程序载入正文/语言/来源/检查器时：高亮立即刷新；完整检查（需要在界面线程冻结工程快照）
// 走与键入相同的去抖动，连续切换、转换或撤销时不在每一步同步捕获快照。
void CodeView::QueueLexicalThenCheck() { QueueAnalysis(false); if (impl_->prepare) impl_->debounce->start(); }
void CodeView::QueueAnalysis(bool check) {
    if (impl_->loading) return;
    impl_->stop.request_stop();
    WorkRequest task; task.generation = impl_->generation; task.text = toPlainText(); task.language = impl_->language; task.source = impl_->source;
    if (check && impl_->prepare) {
        try { task.check = impl_->prepare(task.text, task.generation); }
        catch (const std::exception& error) { impl_->result = {}; impl_->result.reason = QString::fromUtf8(error.what()); }
    }
    impl_->pending = std::move(task); impl_->stats.pending = 1; impl_->stats.peakPending = 1;
    StartPendingWork(); emit CheckStateChanged();
}
void CodeView::StartPendingWork() {
    if (impl_->watcher || !impl_->pending) return;
    auto request = std::move(*impl_->pending); impl_->pending.reset(); impl_->stats.pending = 0;
    const auto requestGeneration = request.generation;
    if (request.load && requestGeneration == impl_->generation) { impl_->loading = true; setReadOnly(true); }
    impl_->stop = std::stop_source{}; const auto stop = impl_->stop.get_token();
    try {
    auto promise = std::make_shared<QPromise<std::shared_ptr<WorkResult>>>(); promise->start();
    auto* watcher = new QFutureWatcher<std::shared_ptr<WorkResult>>(this);
    impl_->watcher = watcher; impl_->stats.running = 1; impl_->stats.peakRunning = 1; ++impl_->stats.submitted;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, requestGeneration] {
        impl_->watcher = nullptr; impl_->stats.running = 0; watcher->deleteLater();
        try {
        const auto result = watcher->result();
        const bool bindingMatches = !result->check || result->check->contextBinding.isEmpty() || !impl_->validateBinding
            || impl_->validateBinding(result->check->contextBinding);
        if (result->generation == impl_->generation && bindingMatches && result->failure.isEmpty() && !result->lexical.cancelled) {
            ++impl_->stats.adopted;
            if (result->document && result->document->document) {
                impl_->suppress = true;
                auto* doc = std::exchange(result->document->document, nullptr); doc->setParent(this); AdoptDocument(doc);
                impl_->suppress = false; impl_->loading = false; SetEditable(impl_->desiredEditable); setPlaceholderText({}); impl_->decodeNotice = result->decoded.reason;
                emit TextLoaded();
            }
            impl_->lexical = std::move(result->lexical); impl_->formatted.clear();
            if (result->check && !result->check->cancelled) impl_->result = std::move(*result->check);
            else if (!result->check) impl_->result.generation = result->generation; // 仅词法索引也必须绑定当前正文，检查完成标志仍为 false。
            if (impl_->lexical.budgetExceeded) {
                impl_->lexical.referenceIndexComplete = false;
                if (!impl_->result.reason.isEmpty()) impl_->result.reason += '\n';
                impl_->result.reason += impl_->lexical.reason + tr(" 正文已完整载入；显示索引仅覆盖预算内的部分内容。");
            }
            UpdateCompletionItems();
            ApplyVisibleFormats(); UpdateSelections(); impl_->gutter->update(); emit AnalysisReady();
        } else {
            ++impl_->stats.discarded;
            if (result->generation == impl_->generation) {
                impl_->loading = false; SetEditable(impl_->desiredEditable); impl_->result = {};
                impl_->result.reason = bindingMatches ? result->failure : tr("工程上下文已变化，已丢弃旧检查结果。"); setPlaceholderText(result->failure);
            }
        }
        } catch (const std::exception& error) {
            if (requestGeneration == impl_->generation) { impl_->loading = false; SetEditable(impl_->desiredEditable); impl_->result = {}; impl_->result.reason = QString::fromUtf8(error.what()); }
            else ++impl_->stats.discarded;
        } catch (...) {
            if (requestGeneration == impl_->generation) { impl_->loading = false; SetEditable(impl_->desiredEditable); impl_->result = {}; impl_->result.reason = tr("后台代码任务未完成；正文保留。"); }
            else ++impl_->stats.discarded;
        }
        StartPendingWork(); emit CheckStateChanged();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([request = std::move(request), stop, promise]() mutable {
        try {
            auto result = std::make_shared<WorkResult>(); result->generation = request.generation;
            std::unique_ptr<QTextDocument> loadedDocument;
            if (request.load) { result->decoded = request.decoder(request.bytes, stop); request.text = result->decoded.text; }
            if (request.load && !stop.stop_requested()) {
                loadedDocument = std::make_unique<QTextDocument>();
                loadedDocument->setDocumentLayout(new QPlainTextDocumentLayout(loadedDocument.get()));
                loadedDocument->setDefaultFont(request.font); loadedDocument->setPlainText(request.text);
                // 所有展示偏移均按实际文档正文建立；Qt 会规范 CRLF、NBSP 和段分隔符。
                request.text = loadedDocument->toPlainText();
            }
            if (!stop.stop_requested()) result->lexical = ScanCode(request.text, request.language, stop);
            if (loadedDocument) {
                if (!stop.stop_requested()) {
                    auto owner = std::make_shared<OwnedDocument>();
                    if (!loadedDocument->moveToThread(request.uiThread)) throw std::runtime_error("code document thread transfer failed");
                    owner->document = loadedDocument.release(); result->document = std::move(owner);
                }
            }
            if (request.check && !stop.stop_requested()) result->check = request.check(stop);
            if (stop.stop_requested()) result->lexical.cancelled = true;
            promise->addResult(std::move(result));
        } catch (...) { promise->setException(std::current_exception()); }
        promise->finish();
    });
    } catch (const std::exception& error) {
        if (impl_->watcher) { disconnect(impl_->watcher, nullptr, this, nullptr); impl_->watcher->deleteLater(); impl_->watcher = nullptr; }
        impl_->stats.running = 0; impl_->loading = false; SetEditable(impl_->desiredEditable);
        impl_->result = {}; impl_->result.reason = QString::fromUtf8(error.what()); emit CheckStateChanged();
    } catch (...) {
        if (impl_->watcher) { disconnect(impl_->watcher, nullptr, this, nullptr); impl_->watcher->deleteLater(); impl_->watcher = nullptr; }
        impl_->stats.running = 0; impl_->loading = false; SetEditable(impl_->desiredEditable);
        impl_->result = {}; impl_->result.reason = tr("无法启动后台代码任务；正文保留。"); emit CheckStateChanged();
    }
}
void CodeView::AdoptDocument(QTextDocument* doc) {
    auto* previous = impl_->document;
    if (!doc) { doc = new QTextDocument(this); doc->setDocumentLayout(new QPlainTextDocumentLayout(doc)); doc->setDefaultFont(font()); }
    // 后台任务捕获的字体可能已过期；Qt 接管现有文档时不会同步当前控件字体。
    if (doc->defaultFont() != font()) doc->setDefaultFont(font());
    setDocument(doc); impl_->document = doc; doc->setUndoRedoEnabled(!isReadOnly());
    // 异步文档有独立默认 TextOption；接管时恢复当前代码 Tab 度量。
    ApplyCodeTabWidth(*this);
    if (previous && previous != doc) previous->deleteLater();
    impl_->folded.clear(); impl_->formatted.clear(); UpdateGutterWidth();
}
bool CodeView::FindNext(const QString& needle) {
    if (needle.isEmpty()) return false;
    bool found = find(needle);
    if (!found) { auto cursor = textCursor(); cursor.movePosition(QTextCursor::Start); setTextCursor(cursor); found = find(needle); }
    if (found) {
        const int line = textCursor().blockNumber(); const auto folded = impl_->folded.values();
        for (const int start : folded) if (start < line && start < impl_->lexical.lines.size() && impl_->lexical.lines[start].foldEnd >= line) ToggleFold(start + 1);
        ensureCursorVisible();
    }
    return found;
}
void CodeView::GoToLine(int line, int column) {
    if (line < 1) return;
    auto block = document()->findBlockByNumber(line - 1); if (!block.isValid()) return;
    const auto folded = impl_->folded.values();
    for (const int start : folded) if (start < line - 1 && impl_->lexical.lines[start].foldEnd >= line - 1) ToggleFold(start + 1);
    QTextCursor cursor(block); cursor.setPosition(block.position() + qBound(0, column - 1, block.length() - 1));
    setTextCursor(cursor); ensureCursorVisible(); setFocus();
}
void CodeView::GoToUtf8ByteColumn(int line, int byteColumn) {
    const auto block = document()->findBlockByNumber(line - 1);
    if (!block.isValid()) return;
    const auto bytes = block.text().toUtf8();
    const auto length = qBound<qsizetype>(0, static_cast<qsizetype>(byteColumn) - 1, bytes.size());
    GoToLine(line, static_cast<int>(QString::fromUtf8(bytes.constData(), length).size()) + 1);
}
bool CodeView::ToggleFold(int line) {
    const int start = line - 1; if (start < 0 || start >= impl_->lexical.lines.size()) return false;
    const int end = impl_->lexical.lines[start].foldEnd; if (end <= start) return false;
    const bool fold = !impl_->folded.contains(start);
    if (fold) impl_->folded.insert(start); else impl_->folded.remove(start);
    for (int n = start + 1; n <= end; ++n) {
        bool hidden = false;
        for (int parent : impl_->folded) if (parent < n && impl_->lexical.lines[parent].foldEnd >= n) { hidden = true; break; }
        auto block = document()->findBlockByNumber(n); block.setVisible(!hidden); block.setLineCount(hidden ? 0 : 1);
    }
    const auto first = document()->findBlockByNumber(start), last = document()->findBlockByNumber(end);
    document()->markContentsDirty(first.position(), last.position() + last.length() - first.position());
    viewport()->update(); impl_->gutter->update(); return true;
}
bool CodeView::IsFolded(int line) const { return impl_->folded.contains(line - 1); }
int CodeView::MatchingBracket(int position) const { return impl_->lexical.brackets.value(position, -1); }
int CodeView::GutterWidth() const {
    int digits = 1; for (int count = qMax(1, blockCount()); count >= 10; count /= 10) ++digits;
    return 34 + fontMetrics().horizontalAdvance('9') * digits;
}
void CodeView::UpdateGutterWidth() { setViewportMargins(GutterWidth(), 0, 0, 0); }
void CodeView::UpdateGutter(const QRect& rect, int dy) {
    if (dy) impl_->gutter->scroll(0, dy); else impl_->gutter->update(0, rect.y(), impl_->gutter->width(), rect.height());
    ApplyVisibleFormats();
}
void CodeView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    // 下方面板展开使编辑区变矮时，原本可见的光标行不能被挤到可视区外。
    if (const int shrink = event->oldSize().height() - event->size().height(); hasFocus() && shrink > 0) {
        const auto caret = cursorRect();
        if (caret.top() >= 0 && caret.bottom() > viewport()->height() && caret.top() < viewport()->height() + shrink) ensureCursorVisible();
    }
    RepositionAssist();
    const auto area = contentsRect();
    impl_->gutter->setGeometry(area.left(), area.top(), GutterWidth(), area.height()); ApplyVisibleFormats();
}
void CodeView::ApplyVisibleFormats() {
    if (impl_->formatting || impl_->lexical.lines.isEmpty()) return;
    impl_->formatting = true; auto block = firstVisibleBlock();
    while (block.isValid() && blockBoundingGeometry(block).translated(contentOffset()).top() <= viewport()->height()) {
        const int line = block.blockNumber();
        if (block.isVisible() && line < impl_->lexical.lines.size() && !impl_->formatted.contains(line)) {
            block.layout()->setFormats(CodeFormats(impl_->lexical.lines[line], block.text(), impl_->language, impl_->symbols, palette()));
            impl_->formatted.insert(line);
            document()->markContentsDirty(block.position(), block.length());
        }
        block = block.next();
    }
    impl_->formatting = false;
}
void CodeView::PaintGutter(QPaintEvent* event) {
    QPainter painter(impl_->gutter); painter.fillRect(event->rect(), palette().color(QPalette::AlternateBase)); painter.setFont(font());
    auto block = firstVisibleBlock();
    while (block.isValid()) {
        const int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top()); if (top > event->rect().bottom()) break;
        if (block.isVisible() && top + fontMetrics().height() >= event->rect().top()) {
            const int n = block.blockNumber();
            painter.setPen(n == textCursor().blockNumber() ? palette().color(QPalette::Text) : palette().color(QPalette::PlaceholderText));
            painter.drawText(10, top, impl_->gutter->width() - 28, fontMetrics().height(), Qt::AlignRight, QString::number(n + 1));
            if (n < impl_->lexical.lines.size() && impl_->lexical.lines[n].foldEnd > n)
                painter.drawText(impl_->gutter->width() - 16, top, 14, fontMetrics().height(), Qt::AlignCenter, impl_->folded.contains(n) ? "+" : "−");
            for (const auto& diagnostic : impl_->result.diagnostics) if (diagnostic.source == impl_->source && diagnostic.line == n + 1) {
                painter.setBrush(diagnostic.severity == CodeSeverity::Error ? QColor("#ec8d89") : QColor("#d3ba79")); painter.setPen(Qt::NoPen);
                painter.drawEllipse(3, top + fontMetrics().height() / 2 - 2, 5, 5); break;
            }
        }
        block = block.next();
    }
}
void CodeView::ClickGutter(QMouseEvent* event) {
    const auto block = cursorForPosition(QPoint(0, qRound(event->position().y()))).block();
    if (event->position().x() >= impl_->gutter->width() - 20) ToggleFold(block.blockNumber() + 1);
    else GoToLine(block.blockNumber() + 1);
}
void CodeView::UpdateSelections() {
    QList<QTextEdit::ExtraSelection> selections;
    QTextEdit::ExtraSelection current; current.cursor = textCursor(); current.cursor.clearSelection();
    current.format.setBackground(palette().color(QPalette::AlternateBase)); current.format.setProperty(QTextFormat::FullWidthSelection, true); selections.push_back(current);
    int position = textCursor().position(); if (!impl_->lexical.brackets.contains(position) && position > 0) --position;
    const int mate = MatchingBracket(position);
    if (mate >= 0) for (const int p : {position, mate}) {
        QTextEdit::ExtraSelection bracket; bracket.cursor = QTextCursor(document()); bracket.cursor.setPosition(p); bracket.cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
        bracket.format.setBackground(QColor("#355448")); bracket.format.setForeground(QColor("#c7efd8")); selections.push_back(bracket);
    }
    int count = 0;
    for (const auto& diagnostic : impl_->result.diagnostics) {
        if (++count > 512) break;
        if (diagnostic.source != impl_->source || diagnostic.line < 1) continue;
        const auto block = document()->findBlockByNumber(diagnostic.line - 1); if (!block.isValid()) continue;
        QTextEdit::ExtraSelection marker; marker.cursor = QTextCursor(block);
        const int start = qBound(0, diagnostic.column - 1, block.length() - 1); marker.cursor.setPosition(block.position() + start);
        marker.cursor.setPosition(block.position() + qMin(block.length() - 1, start + qMax(1, diagnostic.length)), QTextCursor::KeepAnchor);
        marker.format.setUnderlineStyle(QTextCharFormat::WaveUnderline); marker.format.setUnderlineColor(diagnostic.severity == CodeSeverity::Error ? QColor("#ec8d89") : QColor("#d3ba79")); selections.push_back(marker);
    }
    setExtraSelections(selections);
}
QString CodeView::WordAtCursor() const { return SymbolCursor(textCursor()).selectedText(); }
void CodeView::SetSymbolNotes(QHash<QString, QString> notes) { impl_->notes = std::move(notes); }
QString CodeView::HoverText(const QString& word) const {
    if (word.isEmpty()) return {};
    QStringList blocks; QSet<QString> seen;
    const auto muted = palette().color(QPalette::PlaceholderText).name();
    const auto paragraph = [](const QString& text) { return text.toHtmlEscaped().replace('\n', QStringLiteral("<br>")); };
    for (const auto& item : impl_->completions) {
        if (item.name != word) continue;
        const auto location = item.source + '\n' + QString::number(item.line);
        if (seen.contains(location)) continue;
        seen.insert(location);
        QString block = QStringLiteral("<p style='white-space:pre'><span style='color:%1'>%2</span> <code>%3</code></p>")
            .arg(muted, item.kind.toHtmlEscaped(), (item.signature.isEmpty() ? item.name : item.signature).toHtmlEscaped());
        if (blocks.isEmpty() && impl_->notes.contains(word)) block += QStringLiteral("<p>%1</p>").arg(paragraph(impl_->notes.value(word)));
        if (!item.documentation.isEmpty()) block += QStringLiteral("<p>%1</p>").arg(paragraph(item.documentation));
        if (item.line > 0 && !item.source.isEmpty())
            block += QStringLiteral("<p style='color:%1'>%2</p>").arg(muted,
                tr("定义：%1 第 %2 行 · F12 或 Ctrl+单击 转到").arg(item.source.toHtmlEscaped()).arg(item.line));
        blocks.push_back(block);
    }
    if (blocks.isEmpty() && impl_->notes.contains(word))
        blocks.push_back(QStringLiteral("<p><code>%1</code></p><p>%2</p>").arg(word.toHtmlEscaped(), paragraph(impl_->notes.value(word))));
    return blocks.join(QStringLiteral("<hr>"));
}
void CodeView::SetCompletionItems(QVector<CodeCompletionItem> items) {
    impl_->externalCompletions = std::move(items); UpdateCompletionItems();
}
void CodeView::UpdateCompletionItems() {
    impl_->completions = impl_->externalCompletions;
    impl_->completions += impl_->result.declarations;
    QSet<QString> locations;
    for (const auto& known : impl_->completions) locations.insert(known.name + '\n' + known.source + '\n' + QString::number(known.line));
    for (const auto& declaration : impl_->lexical.declarations) {
        auto local = declaration; local.source = impl_->source;
        const auto location = local.name + '\n' + local.source + '\n' + QString::number(local.line);
        if (!locations.contains(location)) { locations.insert(location); impl_->completions.push_back(std::move(local)); }
    }
    // 语义着色：真实声明给出函数/类型/全局量，当前源的检查结果再补局部量与参数（同名时局部优先）。
    CodeSymbolKinds symbols;
    for (const auto& item : impl_->completions)
        if (!item.name.isEmpty() && !symbols.contains(item.name)) symbols.insert(item.name, CodeSymbolKindFor(item.kind, item.signature));
    // 编辑后到新检查完成前沿用上次的局部量分类，避免着色闪回。
    if (impl_->result.completed && impl_->result.generation == impl_->generation) {
        impl_->semanticLocals.clear();
        for (const auto& definition : impl_->result.semanticDefinitions)
            if (definition.source == impl_->source && !definition.name.isEmpty()) {
                const auto kind = CodeSymbolKindFor(definition.kind, definition.signature);
                if (kind == CodeTokenKind::Local || kind == CodeTokenKind::Parameter) impl_->semanticLocals.insert(definition.name, kind);
            }
    }
    for (auto local = impl_->semanticLocals.cbegin(); local != impl_->semanticLocals.cend(); ++local) symbols.insert(local.key(), local.value());
    if (symbols != impl_->symbols) {
        impl_->symbols = std::move(symbols); impl_->formatted.clear(); ApplyVisibleFormats();
    }
}
void CodeView::ShowCompletion() { OpenCompletion(true); }
void CodeView::SetAssist(CodeAssist assist) { HideAssist(); impl_->assist = std::move(assist); }
CodeCompletionList CodeView::DeclarationCompletion(const QTextCursor& cursor) const {
    CodeCompletionList list;
    const auto text = cursor.block().text();
    int start = cursor.positionInBlock();
    while (start > 0 && IdentifierChar(text[start - 1])) --start;
    list.replaceStart = cursor.block().position() + start;
    QSet<QString> seen;
    for (const auto& item : impl_->completions) {
        if (item.name.isEmpty() || seen.contains(item.name)) continue;
        seen.insert(item.name);
        auto documentation = item.documentation;
        if (const auto note = impl_->notes.value(item.name); !note.isEmpty())
            documentation = documentation.isEmpty() ? note : note + QStringLiteral("\n\n") + documentation;
        list.items.push_back({item.name, item.signature.isEmpty() ? item.name : item.signature, documentation,
            CompletionKindFor(item.kind, item.signature)});
    }
    return list;
}
QRect CodeView::GlobalCursorRect() const {
    const auto rect = cursorRect();
    return {viewport()->mapToGlobal(rect.topLeft()), rect.size()};
}
void CodeView::OpenCompletion(bool explicitRequest) {
    if (isReadOnly()) return;
    const auto cursor = textCursor();
    const int position = cursor.position();
    auto list = impl_->assist.complete ? impl_->assist.complete(toPlainText(), position) : DeclarationCompletion(cursor);
    const int blockStart = cursor.block().position();
    if (list.items.isEmpty() || list.replaceStart < blockStart || list.replaceStart > position) { impl_->popup->hide(); return; }
    impl_->replaceStart = list.replaceStart;
    impl_->pathList = list.path;
    impl_->popup->SetItems(std::move(list.items));
    const auto prefix = cursor.block().text().mid(list.replaceStart - blockStart, position - list.replaceStart);
    // 主动请求（Ctrl+空格）且没有匹配时列出全部，方便浏览。
    if (!impl_->popup->SetFilter(prefix) && !(explicitRequest && impl_->popup->SetFilter({}))) { impl_->popup->hide(); return; }
    impl_->popup->ShowNear(GlobalCursorRect());
}
void CodeView::RefilterCompletion() {
    if (!impl_->popup->isVisible()) return;
    const auto cursor = textCursor(); const auto block = cursor.block(); const int position = cursor.position();
    if (impl_->replaceStart < block.position() || position < impl_->replaceStart) { impl_->popup->hide(); return; }
    const auto prefix = block.text().mid(impl_->replaceStart - block.position(), position - impl_->replaceStart);
    const auto valid = impl_->pathList ? PathChar : IdentifierChar;
    if (!std::all_of(prefix.begin(), prefix.end(), valid) || !impl_->popup->SetFilter(prefix)) impl_->popup->hide();
}
void CodeView::AcceptCompletion() {
    const auto* item = impl_->popup->Current();
    auto cursor = textCursor(); const auto block = cursor.block();
    if (!item || isReadOnly() || impl_->replaceStart < block.position() || impl_->replaceStart > cursor.position()) { impl_->popup->hide(); return; }
    const auto text = block.text(); const auto valid = impl_->pathList ? PathChar : IdentifierChar;
    int end = cursor.position();
    while (end - block.position() < text.size() && valid(text[end - block.position()])) ++end;
    const auto label = item->label;
    impl_->popup->hide();
    cursor.setPosition(impl_->replaceStart); cursor.setPosition(end, QTextCursor::KeepAnchor);
    impl_->typing = true; cursor.insertText(label); impl_->typing = false;
    setTextCursor(cursor);
}
void CodeView::HideAssist() {
    if (!impl_ || !impl_->popup) return;
    impl_->popup->hide(); impl_->signatureTip->hide();
}
void CodeView::RepositionAssist() {
    if (!impl_ || !impl_->popup) return;
    const bool caretVisible = viewport()->rect().intersects(cursorRect());
    if (impl_->popup->isVisible()) { if (caretVisible) impl_->popup->ShowNear(GlobalCursorRect()); else impl_->popup->hide(); }
    if (impl_->signatureTip->isVisible()) { if (caretVisible) UpdateSignature(); else impl_->signatureTip->hide(); }
}
void CodeView::UpdateSignature() {
    if (!impl_->assist.signature || isReadOnly()) { impl_->signatureTip->hide(); return; }
    const auto help = impl_->assist.signature(toPlainText(), textCursor().position());
    if (!help || help->label.isEmpty()) { impl_->signatureTip->hide(); return; }
    impl_->signatureTip->ShowHelp(*help, GlobalCursorRect());
}
void CodeView::AfterTyping(const QKeyEvent* event) {
    const auto typed = event->text();
    const bool erase = event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete;
    const bool plain = typed.size() == 1 && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && typed[0].isPrint();
    const QChar ch = plain ? typed[0] : QChar();
    const bool member = plain && (ch == '.' || ch == ':') && impl_->assist.complete;
    if (impl_->popup->isVisible()) {
        if (erase || (plain && (impl_->pathList ? PathChar(ch) : IdentifierChar(ch)))) RefilterCompletion();
        else if (member) OpenCompletion(false);
        else impl_->popup->hide();
    } else if (plain && IdentifierChar(ch) && !ch.isDigit()) {
        const auto cursor = textCursor(); const auto before = cursor.block().text().left(cursor.positionInBlock());
        auto start = before.size();
        while (start > 0 && IdentifierChar(before[start - 1])) --start;
        const auto length = before.size() - start;
        // 声明表至少两个字符才弹出；语言服务自行判断注释、字符串和声明位置。
        if (impl_->assist.complete ? length >= 1 : (length >= 2 && !before[start].isDigit() && !InTextToken(before, impl_->language)))
            OpenCompletion(false);
    } else if (member) {
        OpenCompletion(false);
    } else if (plain && (ch == '"' || ch == '\'') && impl_->assist.complete) {
        // require " 之后列出模块名；其它字符串不弹出。
        if (const auto list = impl_->assist.complete(toPlainText(), textCursor().position()); list.path && !list.items.isEmpty())
            OpenCompletion(false);
    }
    if (impl_->assist.signature && ((plain && (ch == '(' || ch == ',' || ch == ')')) || impl_->signatureTip->isVisible()))
        UpdateSignature();
}
const CodeSemanticDefinition* CodeView::SemanticDefinitionAtCursor() const {
    if (impl_->result.generation != impl_->generation || !impl_->result.completed
        || (impl_->validateBinding && !impl_->validateBinding(impl_->result.contextBinding))) return nullptr;
    const auto cursor = textCursor(); const int line = cursor.blockNumber() + 1, column = cursor.positionInBlock() + 1;
    std::optional<std::uint64_t> target;
    const auto choose = [&](std::uint64_t identity) {
        if (target && *target != identity) return false;
        target = identity; return true;
    };
    const auto at = [&](const auto& position) {
        return position.exact && position.source == impl_->source && position.line == line
            && column >= position.column && column <= position.column + position.length;
    };
    for (const auto& reference : impl_->result.semanticReferences) if (at(reference)) {
        if (!reference.targetKnown || !choose(reference.target)) return nullptr;
    }
    for (const auto& definition : impl_->result.semanticDefinitions) if (at(definition) && !choose(definition.identity)) return nullptr;
    if (!target) return nullptr;
    for (const auto& definition : impl_->result.semanticDefinitions) if (definition.identity == *target) return &definition;
    return nullptr;
}
void CodeView::RequestJump() {
    if (IsChecking() || impl_->result.generation != impl_->generation
        || (impl_->validateBinding && !impl_->validateBinding(impl_->result.contextBinding))) {
        emit JumpRequested(WordAtCursor(), {}, 0, 1); return;
    }
    if (const auto* definition = SemanticDefinitionAtCursor()) {
        if (definition->source == impl_->source) GoToLine(definition->line, definition->column);
        else ShowExternalSource(definition->source, definition->line, definition->column);
        emit JumpRequested(definition->name, definition->source, definition->line, definition->column); return;
    }
    // 已有实际作用域记录时，不用同名全局/词法命中覆盖未知或不精确的位置。
    if (!impl_->result.semanticDefinitions.isEmpty() || impl_->lexical.budgetExceeded) {
        emit JumpRequested(WordAtCursor(), {}, 0, 1); return;
    }
    const auto name = WordAtCursor(); QVector<const CodeCompletionItem*> matches;
    for (const auto& item : impl_->completions) if (item.name == name && item.line > 0) matches.push_back(&item);
    if (matches.size() == 1) {
        const auto& item = *matches.front();
        if (item.source == impl_->source) GoToLine(item.line, item.column);
        else ShowExternalSource(item.source, item.line, item.column);
        emit JumpRequested(name, item.source, item.line, item.column);
    }
    else emit JumpRequested(name, {}, 0, 1);
}
bool CodeView::ShowExternalSource(const QString& source, int line, int column) {
    if (IsChecking() || impl_->result.generation != impl_->generation
        || (impl_->validateBinding && !impl_->validateBinding(impl_->result.contextBinding))) return false;
    const CodeExternalSource* unique = nullptr;
    for (const auto& frozen : impl_->result.externalSources) if (frozen.name == source) {
        if (unique && (unique->bytes != frozen.bytes || unique->text != frozen.text)) {
            impl_->result.reason = tr("同名冻结来源存在不同正文，无法唯一导航。"); emit CheckStateChanged(); return false;
        }
        unique = &frozen;
    }
    if (!unique) return false;
    return ShowFrozenSource(*unique, line, column, impl_->result.contextBinding);
}
bool CodeView::ShowFrozenSource(const CodeExternalSource& frozen, int line, int column, const QString& binding, bool sourceByteColumn) {
    const auto& source = frozen.name;
    if (impl_->externalDialog) impl_->externalDialog->close();
    auto* dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->resize(1000, 640);
    dialog->setWindowTitle(tr("冻结来源 · %1（只读）").arg(source)); dialog->setObjectName("codeExternalSourceDialog"); impl_->externalDialog = dialog;
    auto* layout = new QVBoxLayout(dialog);
    dialog->setToolTip(tr("工程绑定 %1").arg(binding.left(12)));
    if (!frozen.reason.isEmpty()) { auto* note = new QLabel(frozen.reason, dialog); note->setWordWrap(true); layout->addWidget(note); }
    auto* view = new CodeView(dialog); view->setFont(font());
    view->SetLanguage(source.endsWith(".lua", Qt::CaseInsensitive) ? CodeLanguage::Lua
        : source.endsWith(".j", Qt::CaseInsensitive) ? CodeLanguage::VJass : impl_->language);
    view->SetSourceName(source); layout->addWidget(view);
    auto navigationColumn = std::make_shared<std::atomic<int>>(column);
    const auto decoder = impl_->decoder ? impl_->decoder : DecodeCodeBytes{StrictUtf8};
    view->SetDecoder([decoder, navigationColumn, sourceByteColumn, line, column](const QByteArray& bytes, std::stop_token stop) {
        auto decoded = decoder(bytes, stop);
        if (!sourceByteColumn || stop.stop_requested()) return decoded;
        // 真实来源列是原字节位置；定位和解码同在后台，混合换行不在UI扫描整份文件。
        qsizetype start = 0; int currentLine = 1;
        for (qsizetype at = 0; at < bytes.size() && currentLine < line; ++at) {
            if ((at & 4095) == 0 && stop.stop_requested()) return decoded;
            if (bytes[at] == '\r' || bytes[at] == '\n') {
                if (bytes[at] == '\r' && at + 1 < bytes.size() && bytes[at + 1] == '\n') ++at;
                start = at + 1; ++currentLine;
            }
        }
        qsizetype end = start;
        while (end < bytes.size() && bytes[end] != '\r' && bytes[end] != '\n') {
            if ((end & 4095) == 0 && stop.stop_requested()) return decoded;
            ++end;
        }
        const auto length = qBound<qsizetype>(0, static_cast<qsizetype>(column) - 1, end - start);
        const auto prefix = decoder(bytes.mid(start, length), stop);
        navigationColumn->store(static_cast<int>(prefix.text.size()) + 1);
        return decoded;
    });
    connect(view, &CodeView::TextLoaded, view, [view, line, navigationColumn] { view->GoToLine(line, navigationColumn->load()); });
    view->SetBytesAsync(frozen.bytes.isEmpty() ? frozen.text.toUtf8() : frozen.bytes);
    dialog->show(); return true;
}
void CodeView::ShowReferences() {
    const auto name = WordAtCursor();
    emit ReferencesRequested(name);
    if (const auto* definition = SemanticDefinitionAtCursor()) {
        auto* dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("语义引用 · %1 · %2:%3").arg(definition->name, definition->source).arg(definition->line));
        dialog->setObjectName("codeReferencesDialog"); dialog->resize(850, 380); auto* layout = new QVBoxLayout(dialog);
        auto* note = new QLabel(tr("部分引用：%1").arg(impl_->result.semanticIndexReason), dialog);
        note->setWordWrap(true); note->setVisible(!impl_->result.semanticIndexComplete); layout->addWidget(note);
        auto* rows = new QTreeWidget(dialog); rows->setHeaderLabels({tr("来源"), tr("位置"), tr("用途")}); rows->setRootIsDecorated(false); layout->addWidget(rows);
        const auto openedGeneration = impl_->generation; const auto openedBinding = impl_->result.contextBinding;
        int count = 0;
        for (const auto& reference : impl_->result.semanticReferences) if (reference.targetKnown && reference.target == definition->identity) {
            if (++count > 512) continue;
            auto* row = new QTreeWidgetItem(rows, {reference.source,
                tr("%1:%2%3").arg(reference.line).arg(reference.column).arg(reference.exact ? QString{} : tr("（行定位）")), reference.use});
            row->setData(0, Qt::UserRole, reference.source); row->setData(1, Qt::UserRole, reference.line); row->setData(2, Qt::UserRole, reference.column);
        }
        note->setText(note->text() + tr(" 共记录 %1 处，列表显示前 %2 处。").arg(count).arg(qMin(count, 512)));
        connect(rows, &QTreeWidget::itemActivated, this, [this, openedGeneration, openedBinding](QTreeWidgetItem* row, int) {
            if (impl_->generation != openedGeneration || impl_->result.contextBinding != openedBinding
                || (impl_->validateBinding && !impl_->validateBinding(openedBinding))) return;
            const auto source = row->data(0, Qt::UserRole).toString(); const auto line = row->data(1, Qt::UserRole).toInt(), column = row->data(2, Qt::UserRole).toInt();
            if (source == impl_->source) GoToLine(line, column); else ShowExternalSource(source, line, column);
        });
        dialog->show(); return;
    }
    auto* dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("当前正文引用 · %1").arg(name)); dialog->setObjectName("codeReferencesDialog"); dialog->resize(700, 340);
    auto* layout = new QVBoxLayout(dialog);
    const bool current = !IsChecking() && impl_->result.generation == impl_->generation;
    const auto positions = current ? impl_->lexical.references.value(name) : QVector<int>{};
    auto* note = new QLabel(impl_->lexical.lines.isEmpty() ? tr("索引未完成")
        : !current ? tr("索引更新中")
        : impl_->lexical.referenceIndexComplete ? tr("%1 处词法命中").arg(positions.size())
        : tr("部分词法命中"), dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto* rows = new QTreeWidget(dialog); rows->setHeaderLabels({tr("位置"), tr("正文")}); rows->setRootIsDecorated(false); layout->addWidget(rows);
    const int visible = qMin(512, static_cast<int>(positions.size()));
    if (positions.size() > visible) note->setText(note->text() + tr(" · 显示前 %1 处").arg(visible));
    for (int i = 0; i < visible; ++i) {
        const int position = positions[i];
        const auto block = document()->findBlock(position);
        auto* row = new QTreeWidgetItem(rows, {tr("%1:%2").arg(block.blockNumber() + 1).arg(position - block.position() + 1), block.text().trimmed()});
        row->setData(0, Qt::UserRole, block.blockNumber() + 1); row->setData(1, Qt::UserRole, position - block.position() + 1);
    }
    const auto openedGeneration = impl_->generation; const auto openedBinding = impl_->result.contextBinding;
    connect(rows, &QTreeWidget::itemActivated, this, [this, openedGeneration, openedBinding](QTreeWidgetItem* row, int) {
        if (impl_->generation != openedGeneration || impl_->result.contextBinding != openedBinding
            || (impl_->validateBinding && !impl_->validateBinding(openedBinding))) return;
        GoToLine(row->data(0, Qt::UserRole).toInt(), row->data(1, Qt::UserRole).toInt());
    });
    dialog->show();
}
void CodeView::IndentSelection(bool outdent) {
    auto cursor = textCursor(); const int start = cursor.selectionStart(), end = cursor.selectionEnd();
    const int first = document()->findBlock(start).blockNumber(); int last = document()->findBlock(end).blockNumber();
    if (cursor.hasSelection() && document()->findBlock(end).position() == end) --last;
    cursor.beginEditBlock();
    for (int n = last; n >= first; --n) {
        auto block = document()->findBlockByNumber(n); QTextCursor edit(block);
        if (outdent) {
            const auto text = block.text(); int count = 0; while (count < 4 && count < text.size() && text[count] == ' ') ++count;
            if (count == 0 && text.startsWith('\t')) count = 1;
            edit.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, count); edit.removeSelectedText();
        } else edit.insertText(QStringLiteral("    "));
    }
    cursor.endEditBlock();
}
void CodeView::keyPressEvent(QKeyEvent* event) {
    if (impl_->popup->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Up: impl_->popup->MoveSelection(-1); event->accept(); return;
        case Qt::Key_Down: impl_->popup->MoveSelection(1); event->accept(); return;
        case Qt::Key_PageUp: impl_->popup->MoveSelection(-10); event->accept(); return;
        case Qt::Key_PageDown: impl_->popup->MoveSelection(10); event->accept(); return;
        case Qt::Key_Escape: impl_->popup->hide(); event->accept(); return;
        case Qt::Key_Tab: case Qt::Key_Enter: case Qt::Key_Return: {
            // 已完整输入的词（如 end）按 Enter 直接换行，不吞掉回车。
            const auto* item = impl_->popup->Current();
            if (event->key() != Qt::Key_Tab && item && item->label == impl_->popup->Filter()) { impl_->popup->hide(); break; }
            AcceptCompletion(); event->accept(); return;
        }
        default:
            // 输入 ( . : 时先接受前缀或词首缩写匹配的候选，再输入该字符（与 VS 相同）。
            if (const auto typed = event->text(); typed.size() == 1 && !impl_->popup->Filter().isEmpty() && !impl_->pathList
                && (typed[0] == '(' || typed[0] == '.' || typed[0] == ':') && impl_->popup->Current()
                && CompletionTier(impl_->popup->Current()->label, impl_->popup->Filter()) <= 2)
                AcceptCompletion();
            break;
        }
    }
    if (impl_->signatureTip->isVisible() && event->key() == Qt::Key_Escape) { impl_->signatureTip->hide(); event->accept(); return; }
    if (event->key() == Qt::Key_F12) {
        if (event->modifiers() & Qt::ShiftModifier) ShowReferences(); else RequestJump();
        event->accept(); return;
    }
    if (event->key() == Qt::Key_B && (event->modifiers() & Qt::ControlModifier) && (event->modifiers() & Qt::ShiftModifier)) { RequestCheck(); event->accept(); return; }
    if (!isReadOnly() && event->key() == Qt::Key_Space && (event->modifiers() & Qt::ControlModifier)) { ShowCompletion(); event->accept(); return; }
    if (!isReadOnly() && (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab)) { IndentSelection(event->key() == Qt::Key_Backtab || (event->modifiers() & Qt::ShiftModifier)); event->accept(); return; }
    if (!isReadOnly() && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        const auto line = textCursor().block().text(); QString indent;
        for (const auto ch : line) { if (ch != ' ' && ch != '\t') break; indent += ch; }
        const auto before = line.left(textCursor().positionInBlock()).trimmed();
        const auto lineTokens = ScanCode(before, impl_->language);
        bool textToken = false;
        if (!lineTokens.lines.isEmpty()) for (const auto& span : lineTokens.lines.front().spans)
            if (span.start + span.length == before.size() && (span.kind == CodeTokenKind::Comment || span.kind == CodeTokenKind::String)) textToken = true;
        if (!textToken && (before.endsWith("then") || before.endsWith("do") || before == "loop" || before == "globals" || before.startsWith("function ") || before.startsWith("method ") || before.startsWith("library ") || before.startsWith("scope ") || before.startsWith("struct ") || before.startsWith("module ") || before == "repeat")) indent += QStringLiteral("    ");
        HideAssist();
        auto cursor = textCursor(); cursor.beginEditBlock(); cursor.insertText('\n' + indent); cursor.endEditBlock(); setTextCursor(cursor); event->accept(); return;
    }
    impl_->typing = true;
    QPlainTextEdit::keyPressEvent(event);
    impl_->typing = false;
    if (!isReadOnly()) AfterTyping(event);
}
void CodeView::focusOutEvent(QFocusEvent* event) {
    if (!impl_->popup->underMouse()) HideAssist();
    QPlainTextEdit::focusOutEvent(event);
}
void CodeView::contextMenuEvent(QContextMenuEvent* event) {
    std::unique_ptr<QMenu> menu(createStandardContextMenu()); menu->addSeparator();
    menu->addAction(tr("转到定义 · F12"), this, &CodeView::RequestJump);
    menu->addAction(tr("查找引用 · Shift+F12"), this, &CodeView::ShowReferences);
    menu->addAction(tr("检查代码 · Ctrl+Shift+B"), this, &CodeView::RequestCheck); menu->exec(event->globalPos());
}
bool CodeView::ShowHover(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event); const auto cursor = cursorForPosition(help->pos()); QStringList tips;
        for (const auto& diagnostic : impl_->result.diagnostics) if (diagnostic.source == impl_->source && diagnostic.line == cursor.blockNumber() + 1) tips.push_back((diagnostic.code + ": " + diagnostic.message).toHtmlEscaped());
        QString hover = impl_->assist.hover ? impl_->assist.hover(toPlainText(), cursor.position()) : QString();
        if (hover.isEmpty()) hover = HoverText(SymbolCursor(cursor).selectedText());
        if (!hover.isEmpty()) tips.push_back(hover);
        if (!tips.isEmpty()) { QToolTip::showText(help->globalPos(), tips.join("<hr>"), this); return true; }
    }
    return false;
}
bool CodeView::event(QEvent* event) {
    if (ShowHover(event)) return true;
    const bool handled = QPlainTextEdit::event(event);
    if (event->type() == QEvent::FontChange) {
        // 主题字号及外部字体变更完成后，以当前实际度量重算 Tab 和行号区。
        ApplyCodeTabWidth(*this);
        if (impl_ && impl_->gutter) {
            UpdateGutterWidth(); const auto area = contentsRect();
            impl_->gutter->setGeometry(area.left(), area.top(), GutterWidth(), area.height());
        }
    }
    if (event->type() == QEvent::PaletteChange && impl_) {
        // 主题切换只重设已显示行的格式；词法快照和后台检查身份不变。
        impl_->formatted.clear(); ApplyVisibleFormats(); UpdateSelections();
        impl_->gutter->update(); viewport()->update();
    }
    return handled;
}
bool CodeView::viewportEvent(QEvent* event) {
    // Ctrl+单击与 F12 相同：转到定义。
    if (event->type() == QEvent::MouseButtonRelease) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && (mouse->modifiers() & Qt::ControlModifier)) {
            setTextCursor(cursorForPosition(mouse->position().toPoint())); RequestJump(); return true;
        }
    }
    return ShowHover(event) || QPlainTextEdit::viewportEvent(event);
}
} // namespace War3Map::UiKit
