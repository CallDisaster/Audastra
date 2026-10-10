#pragma once

#include "Layout/DockLayout.h"

#include <QFrame>
#include <QIcon>
#include <QPoint>
#include <QTabBar>

#include <optional>
#include <vector>

class QStackedWidget;

namespace War3Map::Shell {

// 右侧组的标签：拖出成为浮动窗口，拖到顶栏回到主编辑区。
class SideTabBar final : public QTabBar {
    Q_OBJECT

public:
    explicit SideTabBar(QWidget* parent = nullptr);

signals:
    void DetachRequested(int index, QPoint globalPos);
    void DragStarted(int index);
    void MenuRequested(int index, QPoint globalPos);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    QPoint pressPos_;
    int pressIndex_ = -1;
};

// 右侧停靠组：多个模块以标签切换。自身是停靠目标，按释放位置决定插入顺序。
class SideGroup final : public QFrame {
    Q_OBJECT

public:
    explicit SideGroup(QWidget* parent = nullptr);

    // 按布局重排标签与内容；widgets 与 ids 一一对应。
    void SetModules(const std::vector<ModuleId>& ids, const std::vector<QWidget*>& widgets,
        const std::vector<QString>& labels, const std::vector<QIcon>& icons, const std::optional<ModuleId>& active);
    [[nodiscard]] const std::vector<ModuleId>& Ids() const noexcept { return ids_; }
    [[nodiscard]] QTabBar* TabBar() const noexcept { return tabs_; }
    // 释放在 globalPos 时应插入的位置。
    [[nodiscard]] int DropIndex(QPoint globalPos) const;

signals:
    void ActivateRequested(const ModuleId& id);
    void DetachRequested(const ModuleId& id, QPoint globalPos);
    void MenuRequested(const ModuleId& id, QPoint globalPos);
    void ModuleDropped(const ModuleId& id, int index);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void SetDropHighlight(bool on);

    SideTabBar* tabs_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    std::vector<ModuleId> ids_;
    bool updating_ = false;
};

} // namespace War3Map::Shell
