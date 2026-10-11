#pragma once

#include "Layout/DockLayout.h"

#include <QFrame>

class QAction;
class QHBoxLayout;
class QLabel;
class QToolButton;

namespace War3Map::UiKit { class ResponsiveToolBar; }

namespace War3Map::Shell {

class ModuleSwitcher;

// 顶栏：品牌与工程名 | 模块切换器 | 右侧操作区。整条顶栏是“放回主编辑区”的停靠目标。
class HeaderBar final : public QFrame {
    Q_OBJECT

public:
    explicit HeaderBar(QWidget* parent = nullptr);

    [[nodiscard]] ModuleSwitcher* Switcher() const noexcept { return switcher_; }
    void SetProjectTitle(const QString& title, const QString& tooltip = {});
    // 右侧操作区：按添加顺序从左到右排列。
    QToolButton* AddAction(QAction* action);
    void AddWidget(QWidget* widget);
    void AddSeparator();

signals:
    void ModuleDropped(const War3Map::Shell::ModuleId& id);

protected:
    bool event(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void UpdateActionLayout();
    void SetDropHighlight(bool on);

    QLabel* project_ = nullptr;
    ModuleSwitcher* switcher_ = nullptr;
    UiKit::ResponsiveToolBar* actions_ = nullptr;
    bool updatingLayout_ = false;
};

} // namespace War3Map::Shell
