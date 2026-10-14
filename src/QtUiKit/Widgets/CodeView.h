#pragma once

#include "Widgets/CodeViewTypes.h"
#include <QHash>
#include <QPlainTextEdit>
#include <memory>

class QPaintEvent;
class QMouseEvent;
class QKeyEvent;
class QFocusEvent;

namespace War3Map::UiKit {
class CodeViewGutter;
struct CodeLexSnapshot;

// 控件撤销只作用于草稿，地图事务由宿主提交；保持已有 QPlainTextEdit 接口。
class CodeView final : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeView(QWidget* parent = nullptr);
    ~CodeView() override;
    void SetText(const QString& text);
    void SetBytesAsync(QByteArray bytes);
    void SetEditable(bool editable);
    void SetLanguage(CodeLanguage language);
    [[nodiscard]] CodeLanguage Language() const;
    void SetSourceName(QString name);
    void SetDecoder(DecodeCodeBytes decoder);
    void SetPrepareCheck(PrepareCodeCheck prepare);
    void SetCheckBindingValidator(std::function<bool(const QString&)> validate);
    void SetCompletionItems(QVector<CodeCompletionItem> items);
    // 语言服务（补全、参数提示、悬停）；未设置的部分使用声明表。
    void SetAssist(CodeAssist assist);
    // 函数库作者说明（标题与提示），按函数名索引；悬停时排在签名之后、源注释之前。
    void SetSymbolNotes(QHash<QString, QString> notes);
    // 悬停内容（VS 风格）：分类与签名、函数库说明、声明前注释、定义位置。没有可显示内容时为空。
    [[nodiscard]] QString HoverText(const QString& word) const;
    void RequestCheck();
    void CancelPendingWork();
    [[nodiscard]] bool IsLoading() const noexcept;
    [[nodiscard]] bool IsChecking() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] CodeWorkerStats WorkerStats() const;
    [[nodiscard]] const CodeCheckResult& CheckResult() const;
    [[nodiscard]] const CodeLexSnapshot& LexSnapshot() const;
    [[nodiscard]] QString SourceName() const;
    [[nodiscard]] QString DecodeNotice() const;
    bool FindNext(const QString& needle);
    void GoToLine(int line, int column = 1);
    void GoToUtf8ByteColumn(int line, int byteColumn);
    bool ToggleFold(int line);
    [[nodiscard]] bool IsFolded(int line) const;
    [[nodiscard]] int MatchingBracket(int position) const;
    [[nodiscard]] int GutterWidth() const;
    void ShowCompletion();
    bool ShowExternalSource(const QString& source, int line, int column);
    bool ShowFrozenSource(const CodeExternalSource& source, int line, int column, const QString& binding = {}, bool sourceByteColumn = false);

signals:
    void TextLoaded();
    void AnalysisReady();
    void CheckStateChanged();
    void JumpRequested(const QString& name, const QString& source, int line, int column);
    void ReferencesRequested(const QString& name);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;

private:
    friend class CodeViewGutter;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void PaintGutter(QPaintEvent* event);
    void ClickGutter(QMouseEvent* event);
    void UpdateGutterWidth();
    void UpdateGutter(const QRect& rect, int dy);
    void AdoptDocument(QTextDocument* document);
    void QueueAnalysis(bool check);
    void QueueLexicalThenCheck();
    void StartPendingWork();
    void ApplyVisibleFormats();
    void UpdateSelections();
    void IndentSelection(bool outdent);
    [[nodiscard]] QString WordAtCursor() const;
    void RequestJump();
    [[nodiscard]] const CodeSemanticDefinition* SemanticDefinitionAtCursor() const;
    void ShowReferences();
    void UpdateCompletionItems();
    [[nodiscard]] CodeCompletionList DeclarationCompletion(const QTextCursor& cursor) const;
    void OpenCompletion(bool explicitRequest);
    void RefilterCompletion();
    void AcceptCompletion();
    void HideAssist();
    void RepositionAssist();
    void UpdateSignature();
    void AfterTyping(const QKeyEvent* event);
    [[nodiscard]] QRect GlobalCursorRect() const;
    bool ShowHover(QEvent* event);
};
} // namespace War3Map::UiKit
