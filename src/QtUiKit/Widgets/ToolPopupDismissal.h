#pragma once

#include <QObject>
#include <QPointer>

#include <functional>

class QWidget;

namespace War3Map::UiKit {

// 带输入框的贴附浮层用的窗口类型：会激活的无边框工具窗口。
// Qt::Popup 在 Windows 上不会成为活动窗口，输入法组字送不进浮层里的输入框（只能打英文），
// 所以要输入文字的浮层不用 Qt::Popup，改用这个类型并由 ToolPopupDismissal 补上收起规则。
inline constexpr Qt::WindowFlags kToolPopupWindowFlags = Qt::Tool | Qt::FramelessWindowHint;

// 给工具窗口浮层补上 Qt::Popup 原有的收起规则：
// 点击浮层外部、整个应用失去激活或按 Esc 时收起；浮层自己的下拉框、菜单等子弹出层不算外部。
// 输入法正在组字时不因 Esc 或应用失活收起，Esc 留给输入法取消组字。
// 外部点击不吞掉，点到的控件照常收到这次点击（例如直接点下一个参数）。
class ToolPopupDismissal final : public QObject {
public:
    enum class Reason { OutsideClick, Escape, Deactivated };
    // window 是浮层顶层控件；dismiss 执行实际收起（hide 或 close），可按原因决定焦点去向。
    ToolPopupDismissal(QWidget& window, std::function<void(Reason)> dismiss);
    ~ToolPopupDismissal() override;

    // 显示、置前、激活浮层并把焦点交给 focus（为空时交给浮层）。
    void Open(QWidget* focus = nullptr);
    // 浮层内输入框是否正处于输入法组字状态。
    [[nodiscard]] bool Composing() const noexcept { return composing_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void StartWatching();
    void StopWatching();
    void Dismiss(Reason reason);
    [[nodiscard]] bool Inside(const QObject* object) const;

    QPointer<QWidget> window_;
    std::function<void(Reason)> dismiss_;
    bool watching_ = false;
    bool composing_ = false;
};

} // namespace War3Map::UiKit
