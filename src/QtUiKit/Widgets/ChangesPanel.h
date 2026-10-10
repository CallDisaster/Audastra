#pragma once

#include <QLabel>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStringList>
#include <QTextEdit>
#include <QTreeView>
#include <QWidget>

#include <functional>
#include <vector>

// 修改面板只显示宿主提供的差异；导航、逆操作、保存和检查均由宿主执行。
class ChangesPanel final : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Modified, Added, Removed, Member };
    struct Entry {
        QString id, member, record, title, before, after, reason;
        bool canRevert = false;
        std::function<void()> reveal, revert;
        Kind kind = Kind::Modified;
    };
    explicit ChangesPanel(QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {420, 640}; }
    void SetEntries(std::vector<Entry> entries, const QStringList& unsupported = {}, bool hasMore = false);
    void SetPreflightResult(const QString& summary, const QStringList& details = {});
    void SetActionsAvailable(bool canSave, bool canCheck, const QString& reason = {});
    [[nodiscard]] QString SelectedId() const;

signals:
    void SaveRequested();
    void CheckSaveRequested();

private:
    void SelectEntry(const QModelIndex& index);
    void InvokeSelected(bool reverse);
    QTreeView* tree_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QLabel *summary_ = nullptr, *unsupported_ = nullptr, *truncated_ = nullptr;
    QLabel *before_ = nullptr, *after_ = nullptr, *reason_ = nullptr;
    QLabel *preflight_ = nullptr, *saveReason_ = nullptr;
    QTextEdit* preflightDetails_ = nullptr;
    QPushButton *reveal_ = nullptr, *revert_ = nullptr, *save_ = nullptr, *check_ = nullptr;
    std::vector<Entry> entries_;
    int selected_ = -1;
};
