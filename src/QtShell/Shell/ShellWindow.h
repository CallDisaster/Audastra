#pragma once

#include "Layout/DockLayout.h"
#include "Modules/ModuleHost.h"
#include "Modules/ModuleRegistry.h"
#include "Modules/NavigationHistory.h"

#include <QMainWindow>
#include <QPointer>

#include <functional>
#include <optional>

class QAction;
class QLabel;
class QSettings;
class QTimer;
class QUndoGroup;

namespace War3Map::Shell {

class DockHost;
class HeaderBar;

// 主窗口外壳：顶栏模块切换器 + 停靠区 + 状态栏。只认识 EditorModule 接口，
// 新增模块只需在注册表登记。布局（DockLayout）在这里是唯一真值，
// 停靠区、切换器、浮动窗口都只发出意图，由这里更新布局后统一套用。
// 应用主窗口可以继承它并添加顶栏操作；外壳本身不认识任何具体模块。
class ShellWindow : public QMainWindow, public ModuleHost {
    Q_OBJECT

public:
    // settings 为空时不读写本机布局（测试用）。
    ShellWindow(ModuleRegistry& registry, QSettings* settings, QWidget* parent = nullptr);
    ~ShellWindow() override;

    [[nodiscard]] HeaderBar* Header() const noexcept { return header_; }
    [[nodiscard]] DockHost* Dock() const noexcept { return dock_; }
    [[nodiscard]] QUndoGroup* UndoGroup() const noexcept { return undoGroup_; }
    [[nodiscard]] const DockLayout& Layout() const noexcept { return layout_; }
    [[nodiscard]] std::optional<ModuleId> CurrentModule() const { return current_; }
    // 标准操作，由组装处决定放在顶栏的位置。
    [[nodiscard]] QAction* UndoAction() const noexcept { return undoAction_; }
    [[nodiscard]] QAction* RedoAction() const noexcept { return redoAction_; }
    [[nodiscard]] QAction* BackAction() const noexcept { return backAction_; }
    [[nodiscard]] QAction* ForwardAction() const noexcept { return forwardAction_; }
    [[nodiscard]] QAction* LayoutAction() const noexcept { return layoutAction_; }
    // 关闭前的确认（例如有未保存修改）；返回 false 取消关闭。
    void SetCloseGuard(std::function<bool()> guard) { closeGuard_ = std::move(guard); }
    // 主应用可路由顶栏/模块快捷键到同一命令表；独立外壳保持原有激活路径。
    void SetModuleCommandHandler(std::function<void(const ModuleId&)> handler) { moduleCommandHandler_ = std::move(handler); }

    void SetLayout(const DockLayout& layout);
    bool ApplyPreset(const std::string& presetId);
    void PlaceModule(const ModuleId& id, const Placement& target);
    bool GoBack();
    bool GoForward();
    bool TryNavigate(const NavigationTarget& target);
    void ResetNavigationHistory();
    [[nodiscard]] std::uint64_t NavigationEpoch() const override { return navigationEpoch_; }
    // 状态栏右侧的常驻信息（如问题数量）。
    void AddStatusWidget(QWidget* widget);

    // ModuleHost
    void Navigate(const NavigationTarget& target) override;
    void ActivateModule(const ModuleId& id) override;
    void ShowStatusMessage(const QString& text, int timeoutMs = 0) override;
    void UndoStackChanged(const ModuleId& id) override;

signals:
    // 显式显示意图（含重复显示当前模块）；布局恢复和焦点变化不会触发。
    // 在有效激活/放置前通知，宿主可先移开启动页再由停靠区显示模块。
    void ModuleRevealRequested(const War3Map::Shell::ModuleId& id);
    void CurrentModuleChanged(const War3Map::Shell::ModuleId& id);
    void LayoutChanged();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void Activate(const ModuleId& id, bool reveal);
    void Commit(DockLayout next, bool apply);
    void SetCurrent(const ModuleId& id);
    void SelectVisibleModule(bool preferMain);
    void OnFocusChanged(QWidget* now);
    void ShowModuleMenu(const ModuleId& id, QPoint globalPos);
    void BuildDefaultActions();
    void ScheduleSave();
    void SaveNow();
    void RestoreViewStates();
    std::optional<NavigationBookmark> CaptureCurrentNavigation() const;
    bool ReplayNavigation(bool forward);
    void RefreshNavigationActions();
    void CancelNavigationRestorations();

    ModuleRegistry& registry_;
    QSettings* settings_ = nullptr;
    HeaderBar* header_ = nullptr;
    DockHost* dock_ = nullptr;
    QUndoGroup* undoGroup_ = nullptr;
    std::function<void(const ModuleId&)> moduleCommandHandler_;
    QTimer* saveTimer_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* backAction_ = nullptr;
    QAction* forwardAction_ = nullptr;
    QAction* layoutAction_ = nullptr;
    std::function<bool()> closeGuard_;
    DockLayout layout_;
    NavigationHistory history_;
    std::uint64_t navigationEpoch_ = 1;
    bool navigating_ = false;
    std::optional<ModuleId> current_;
};

} // namespace War3Map::Shell
