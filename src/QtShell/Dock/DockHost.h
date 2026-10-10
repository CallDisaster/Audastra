#pragma once

#include "Layout/DockLayout.h"
#include "Modules/ModuleRegistry.h"

#include <QPointer>
#include <QWidget>

#include <map>
#include <optional>
#include <set>

class QFrame;
class QSplitter;
class QStackedWidget;

namespace War3Map::Shell {

class FloatFrame;
class SideGroup;

// 中央停靠区：主编辑区（一次显示一个模块）+ 右侧停靠组 + 归属于主窗口的浮动窗口。
// 只负责把模块控件放到布局指定的位置；布局本身由 ShellWindow 持有，这里只发出意图。
// 模块控件在首次可见时才创建，之后常驻，移动位置只是换父控件。
class DockHost final : public QWidget {
    Q_OBJECT

public:
    DockHost(const ModuleRegistry& registry, QWidget* owner);
    ~DockHost() override;

    void Apply(const DockLayout& layout);
    // 交还全部模块控件（去掉父控件）并销毁浮动窗口；控件由模块自己释放。可重复调用。
    void ReleaseModuleWidgets();
    // 顶栏是“放回主编辑区”的停靠目标；浮动窗口拖动时据此判断。
    void SetHeaderTarget(QWidget* header) { header_ = header; }
    // 浮动窗口移动到 globalPos 时的停靠目标：顶栏 → 主区，右侧组或右边缘 → 右侧；其他位置不停靠。
    [[nodiscard]] std::optional<Placement> DockTargetAt(QPoint globalPos) const;
    void ShowDropHint(std::optional<Area> area);

    [[nodiscard]] FloatFrame* Frame(const ModuleId& id) const;
    [[nodiscard]] SideGroup* Side() const noexcept { return side_; }
    [[nodiscard]] QStackedWidget* MainStack() const noexcept { return main_; }
    [[nodiscard]] bool IsCreated(const ModuleId& id) const { return created_.contains(id); }

signals:
    void PlaceRequested(const War3Map::Shell::ModuleId& id, const War3Map::Shell::Placement& target);
    void ActivateRequested(const War3Map::Shell::ModuleId& id, bool reveal);
    void FloatGeometryEdited(const War3Map::Shell::FloatWindow& window);
    void SideWidthEdited(int width);
    void MenuRequested(const War3Map::Shell::ModuleId& id, QPoint globalPos);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* WidgetFor(const ModuleId& id);
    FloatFrame* EnsureFrame(const ModuleId& id);
    void PlaceFloats(const DockLayout& layout);
    void PlaceSide(const DockLayout& layout);
    void PlaceMain(const DockLayout& layout);
    [[nodiscard]] QRect EdgeZoneRect() const;
    void PositionEdgeZone();

    const ModuleRegistry& registry_;
    QWidget* owner_ = nullptr;
    QPointer<QWidget> header_;
    QSplitter* splitter_ = nullptr;
    QStackedWidget* main_ = nullptr;
    SideGroup* side_ = nullptr;
    QFrame* edgeZone_ = nullptr;
    std::map<ModuleId, FloatFrame*> frames_;
    std::set<ModuleId> created_;
    int sideWidth_ = 520;
    bool applying_ = false;
};

} // namespace War3Map::Shell
