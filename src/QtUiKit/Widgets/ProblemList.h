#pragma once
#include <QWidget>

#include <array>
#include <functional>

class QAbstractItemModel;
class QLabel;
class QModelIndex;
class QTimer;
class QToolButton;
class QTreeView;

namespace War3Map::UiKit {

enum class ProblemSeverity { Error, Warning, Info };

// 与 Visual Studio 错误列表相同的用法：上方“错误 / 警告 / 消息”三个开关带计数，
// 关掉某一类就不在下方列表中显示。列表只显示来源模型的行，不改动模型本身。
class ProblemList final : public QWidget {
    Q_OBJECT
public:
    using SeverityOf = std::function<ProblemSeverity(const QAbstractItemModel& model, int row)>;

    explicit ProblemList(QWidget* parent = nullptr);
    void SetSourceModel(QAbstractItemModel* model, SeverityOf severityOf);
    // 开关右侧的简短状态（如“检查中…”）；空串则不显示。
    void SetState(const QString& text);
    [[nodiscard]] int Count(ProblemSeverity severity) const;
    [[nodiscard]] bool IsShown(ProblemSeverity severity) const;
    void SetShown(ProblemSeverity severity, bool shown);
    [[nodiscard]] QTreeView* View() const noexcept { return view_; }
    // 级别文字“错误 / 警告 / 信息”对应的严重度；其余按消息处理。
    [[nodiscard]] static ProblemSeverity SeverityFromText(const QString& text);

signals:
    void Activated(const QModelIndex& sourceIndex);
    // 计数刷新后发出，供状态栏等处显示“N 错误 · M 警告”。
    void CountsChanged();

private:
    class Filter;
    void ScheduleCounts();
    void RefreshCounts();

    Filter* filter_ = nullptr;
    QTreeView* view_ = nullptr;
    QLabel* state_ = nullptr;
    QTimer* countTimer_ = nullptr;
    std::array<QToolButton*, 3> toggles_{};
    std::array<int, 3> counts_{};
};

} // namespace War3Map::UiKit
