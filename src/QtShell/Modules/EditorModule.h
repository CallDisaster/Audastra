#pragma once

#include "Layout/DockLayout.h"

#include <QJsonObject>
#include <QKeySequence>
#include <QString>
#include <QVariantMap>
#include <optional>

class QUndoStack;
class QWidget;

namespace War3Map::Shell {

class ModuleHost;
enum class NavigationRestoreResult { Restored, Invalid, Blocked };

struct ModuleInfo {
    ModuleId id;             // 稳定 ID：triggers、objects、variables……
    QString label;           // 切换器文字
    QString icon;            // SVG 图标名（assets/icons 下的相对路径）
    QKeySequence shortcut;
    // 尚未接通的模块在切换器中显示为禁用并给出原因。
    bool available = true;
    QString unavailableReason;
    // 地图设置等由菜单打开的模块不占用常驻顶栏宽度；仍可导航和停靠。
    bool showInSwitcher = true;
};

// 跨模块定位：物体管理器、搜索、引用列表都通过它打开目标。
struct NavigationTarget {
    ModuleId module;
    QString kind;     // trigger、object、variable、function……由目标模块解释
    QString id;       // 稳定 ID 或 rawcode
    QVariantMap extra;
};

// 每个编辑器模块实现此接口并在 ModuleRegistry 登记；Shell 只认识接口。
// 模块控件首次取用时创建，之后常驻：在主区、右侧组与浮动窗口之间移动只是换父控件，
// 选中项、展开状态和滚动位置天然保留。
// 所有权：控件属于模块（用 QPointer 持有并在析构时删除）；外壳析构前会把控件交还（去掉父控件）。
// 注册表必须比外壳活得久。
class EditorModule {
public:
    virtual ~EditorModule() = default;

    [[nodiscard]] virtual ModuleInfo Info() const = 0;
    [[nodiscard]] virtual QWidget* Widget() = 0;

    // 登记到 Shell 时注入宿主服务；Shell 析构前传 nullptr 解绑。
    // 模块可能比宿主长寿，异步回调使用宿主前须检查绑定是否仍存在。
    virtual void AttachHost(ModuleHost* host) { (void)host; }

    // 定位到对象、语句或字段；不认识的目标返回 false。
    virtual bool Reveal(const NavigationTarget& target) {
        (void)target;
        return false;
    }
    // 视图状态（选中、展开、视图模式）跨会话保存；不含文档数据。
    [[nodiscard]] virtual QJsonObject SaveViewState() const { return {}; }
    virtual void RestoreViewState(const QJsonObject& state) { (void)state; }
    // 本会话导航位置：仅稳定身份与纯视图数据，不持有文档正文或模型 / 控件指针。
    [[nodiscard]] virtual std::optional<QJsonObject> CaptureNavigationBookmark() const { return std::nullopt; }
    virtual NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& state) {
        (void)state;
        return NavigationRestoreResult::Invalid;
    }
    // 草稿门由模块自己负责；拒绝时历史游标保持原处。
    virtual bool PrepareNavigationLeave() { return true; }
    // 新跳转 / 会话切换 / 宿主解除时撤销尚未完成的代码视图恢复。
    virtual void CancelNavigationRestoration() {}
    // 顶栏撤销 / 重做路由到当前模块的撤销栈；没有则返回 nullptr。
    [[nodiscard]] virtual QUndoStack* UndoStack() { return nullptr; }
};

} // namespace War3Map::Shell
