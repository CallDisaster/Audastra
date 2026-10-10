#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListView>
#include <QPointer>
#include <QStandardItemModel>

#include <cstdint>
#include <functional>
#include <vector>

/// Ctrl+K 全局命令面板：快速跳转物体/触发器/页面。
class CommandPalette final : public QDialog {
    Q_OBJECT

public:
    struct Entry {
        QString label;
        QString detail;
        std::function<void()> action;
        QString stableId;
        QString shortcut;
        QString section;
        bool available = true;
    };
    // provider 负责业务查询/排序与输入修订；控件只展示前 100 个结果。
    using QueryProvider = std::function<std::vector<Entry>(const QString&, bool commandOnly)>;

    explicit CommandPalette(QWidget* parent = nullptr);

    void SetEntries(std::vector<Entry> entries);
    void SetQueryProvider(QueryProvider provider);
    void Show();
    void Show(const QString& initial);
    void DismissForContextChange(int source, int signal);
    void TraceSourceStateNotice();
    void done(int result) override;

signals:
    void EntryActivated();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void FilterEntries(const QString& text);
    void SelectNext(int direction);
    void ActivateCurrent();
    void ActivateIndex(const QModelIndex& index);
    void CheckFocusLater();
    void TraceInput(const char* event, int value1 = -1, int value2 = -1, int value3 = -1, int value4 = -1);

    QLineEdit* searchBox_ = nullptr;
    QListView* listView_ = nullptr;
    QStandardItemModel* listModel_ = nullptr;
    QueryProvider queryProvider_;
    std::vector<Entry> entries_;
    std::vector<Entry> results_;
    std::vector<int> rowEntries_;
    QPointer<QWidget> previousFocus_;
    std::uint64_t queryGeneration_ = 0;
    std::uint64_t focusGeneration_ = 0;
    bool composing_ = false, restorePreviousFocus_ = true;
    bool traceInput_ = false;
    QString inputTracePath_;
    QElapsedTimer inputTraceClock_;
    std::uint64_t inputTraceSequence_ = 0;
};
