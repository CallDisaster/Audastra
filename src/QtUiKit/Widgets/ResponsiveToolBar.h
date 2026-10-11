#pragma once

#include <QFrame>
#include <QPointer>
#include <memory>
#include <vector>

class QAction;
class QHBoxLayout;
class QMenu;
class QToolButton;

namespace War3Map::UiKit {

// 共用命令条：先收起有图标按钮的文字，再按优先级收入更多菜单，最后收起搜索框。
// QAction原样复用；旧按钮的菜单入口只调用原按钮，不另建编辑或撤销路径。
class ResponsiveToolBar final : public QFrame {
    Q_OBJECT
public:
    explicit ResponsiveToolBar(QWidget* parent = nullptr);
    ~ResponsiveToolBar() override;
    [[nodiscard]] QHBoxLayout* ContentLayout() const noexcept { return row_; }
    QToolButton* AddAction(QAction* action, int priority = 0);
    void AddWidget(QWidget* widget, int priority = 0);
    void AddSeparator();
    void Refresh();
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] QMenu* OverflowMenu() const noexcept { return menu_; }

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry;
    void Synchronize();
    void QueueRefresh();
    void RebuildMenu();
    QHBoxLayout* row_ = nullptr;
    QToolButton* more_ = nullptr;
    QMenu* menu_ = nullptr;
    std::vector<std::unique_ptr<Entry>> entries_;
    int preferredWidth_ = 0;
    int lastWidth_ = -1;
    bool layoutDirty_ = true;
    bool refreshing_ = false;
    bool queued_ = false;
    bool measure_ = true;
};

} // namespace War3Map::UiKit
