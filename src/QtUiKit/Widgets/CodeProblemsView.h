#pragma once
#include "Widgets/CodeViewTypes.h"
#include <QWidget>

class QStandardItemModel;
namespace War3Map::UiKit {
class CodeView;
class ProblemList;
// 代码检查结果：错误 / 警告 / 消息列表。检查范围等补充情况作为“消息”条目列出，不另写说明文字。
class CodeProblemsView final : public QWidget {
    Q_OBJECT
public:
    explicit CodeProblemsView(QWidget* parent = nullptr);
    void SetView(CodeView* view);
    void SetResult(const CodeCheckResult& result, bool checking = false, const QString& decodeNotice = {});
    // 简短状态：检查中 / 未检查 / “N 错误 · M 警告”。
    [[nodiscard]] QString StatusText() const;
    // 检查发现的问题数（不含检查范围等消息条目）。
    [[nodiscard]] int IssueCount() const;
    [[nodiscard]] ProblemList* List() const noexcept { return list_; }
signals:
    void LocationRequested(const QString& source, int line, int column);
private:
    ProblemList* list_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QString status_;
    int issues_ = 0;
};
}
