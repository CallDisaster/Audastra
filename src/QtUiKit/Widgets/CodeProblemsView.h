#pragma once
#include "Widgets/CodeViewTypes.h"
#include <QWidget>

class QLabel;
class QTreeWidget;
namespace War3Map::UiKit {
class CodeView;
class CodeProblemsView final : public QWidget {
    Q_OBJECT
public:
    explicit CodeProblemsView(QWidget* parent = nullptr);
    void SetView(CodeView* view);
    void SetResult(const CodeCheckResult& result, bool checking = false, const QString& decodeNotice = {});
    [[nodiscard]] QString StatusText() const;
    [[nodiscard]] int IssueCount() const;
signals:
    void LocationRequested(const QString& source, int line, int column);
private:
    QLabel* status_ = nullptr;
    QTreeWidget* issues_ = nullptr;
};
}
