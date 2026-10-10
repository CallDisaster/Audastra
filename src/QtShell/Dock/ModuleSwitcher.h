#pragma once

#include "Layout/DockLayout.h"
#include "Modules/EditorModule.h"

#include <QPoint>
#include <QToolButton>
#include <QWidget>

#include <vector>

class QHBoxLayout;

namespace War3Map::Shell {

// 顶栏模块切换器的一个条目：点击切换，拖出成为浮动窗口，拖到右侧停靠。
class ModuleChip final : public QToolButton {
    Q_OBJECT

public:
    ModuleChip(const ModuleInfo& info, QWidget* parent = nullptr);
    [[nodiscard]] const ModuleId& Id() const noexcept { return id_; }
    void SetPlacement(Area area);
    // “图标 + 文字”模式所需宽度（含样式表内边距）；切换器据此决定是否只显示图标。
    [[nodiscard]] int TextModeWidth() const;
    [[nodiscard]] QSize sizeHint() const override;

signals:
    void DetachRequested(const ModuleId& id, QPoint globalPos);
    void MenuRequested(const ModuleId& id, QPoint globalPos);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    ModuleId id_;
    QPoint pressPos_;
    bool pressed_ = false;
};

// 所有模块常驻在这一行里（没有侧栏、没有标签层）。
// 宽度不够时只显示图标；选中表示该模块正在主编辑区显示，右侧 / 浮动的模块另有标记。
class ModuleSwitcher final : public QWidget {
    Q_OBJECT

public:
    explicit ModuleSwitcher(QWidget* parent = nullptr);

    void SetModules(const std::vector<ModuleInfo>& modules);
    void SetState(const DockLayout& layout, const std::vector<ModuleId>& all);
    [[nodiscard]] ModuleChip* Chip(const ModuleId& id) const;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    void ActivateRequested(const ModuleId& id);
    void DetachRequested(const ModuleId& id, QPoint globalPos);
    void MenuRequested(const ModuleId& id, QPoint globalPos);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void UpdateCompactMode();
    [[nodiscard]] int FullWidth() const;

    QHBoxLayout* layout_ = nullptr;
    std::vector<ModuleChip*> chips_;
    bool compact_ = false;
};

} // namespace War3Map::Shell
