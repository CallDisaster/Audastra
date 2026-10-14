#pragma once

#include "Widgets/CodeViewTypes.h"

#include <QFrame>

class QLabel;
class QListView;
class QStandardItemModel;

namespace War3Map::UiKit {

// 候选按输入前缀排序后的下标；前缀为空时保留全部并按名称排序。
// 档次：区分大小写前缀 > 不区分大小写前缀 > 词首缩写（如 cu → CreateUnit）> 包含 > 按序包含。
[[nodiscard]] QVector<int> RankCompletions(const QVector<CodeCompletionCandidate>& items, const QString& prefix);
// label 中与前缀匹配的字符下标（加粗显示用）。
[[nodiscard]] QVector<int> CompletionMatches(const QString& label, const QString& prefix);
// 匹配档次（0 最好）；不匹配为 -1。
[[nodiscard]] int CompletionTier(const QString& label, const QString& prefix);

// 代码补全列表：左侧候选（图标 + 名称，匹配字符加粗），右侧当前候选的签名与说明。
// 不获取焦点，键盘由编辑器转发。
class CodeCompletionPopup final : public QFrame {
    Q_OBJECT
public:
    explicit CodeCompletionPopup(QWidget* editor);
    void SetItems(QVector<CodeCompletionCandidate> items);
    // 返回是否还有匹配项。
    bool SetFilter(const QString& prefix);
    [[nodiscard]] QString Filter() const { return filter_; }
    void ShowNear(const QRect& globalCursor);
    void MoveSelection(int delta);
    [[nodiscard]] const CodeCompletionCandidate* Current() const;
    [[nodiscard]] int VisibleCount() const { return static_cast<int>(visible_.size()); }

signals:
    void Accepted();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void UpdateDetails();
    QListView* list_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QLabel* details_ = nullptr;
    QVector<CodeCompletionCandidate> items_;
    QVector<int> visible_;
    QString filter_;
};

// 函数参数提示：当前参数加粗高亮，说明在下一行。
class CodeSignatureTip final : public QFrame {
    Q_OBJECT
public:
    explicit CodeSignatureTip(QWidget* editor);
    void ShowHelp(const CodeSignatureHelp& help, const QRect& globalCursor, bool preferBelow = false);
    [[nodiscard]] QString Text() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel* label_ = nullptr;
};

[[nodiscard]] QString CodeCompletionKindText(CodeCompletionKind kind);

} // namespace War3Map::UiKit
