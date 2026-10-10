#pragma once

#include "Layout/DockLayout.h"
#include "Modules/EditorModule.h"

#include <QPoint>
#include <QRect>
#include <QWidget>

class QVBoxLayout;

namespace War3Map::Shell {

// 浮动模块窗口：系统顶层窗口（归属主窗口，始终在主窗口之上，彼此之间按点击调整前后），
// 使用系统标题栏以保留原生缩放、贴靠与阴影。用标题栏拖到主窗口顶栏或右边缘即可停靠回去；
// 关闭窗口等于放回主编辑区，模块本身不会被关闭。
class FloatFrame final : public QWidget {
    Q_OBJECT

public:
    FloatFrame(const ModuleInfo& info, QWidget* owner);

    [[nodiscard]] const ModuleId& Id() const noexcept { return id_; }
    void SetContent(QWidget* content);
    // 取出模块控件（重新安置到别处前调用），窗口本身随后销毁。
    [[nodiscard]] QWidget* TakeContent();
    // 按布局设置几何与最大化状态；程序化设置不回报 GeometryEdited。
    void ApplyGeometry(const FloatWindow& model);

signals:
    void GeometryEdited(const War3Map::Shell::FloatWindow& model);
    void Activated(const War3Map::Shell::ModuleId& id);
    void MoveTracking(const War3Map::Shell::ModuleId& id, QPoint globalCursor);
    void MoveFinished(const War3Map::Shell::ModuleId& id, QPoint globalCursor);
    void DockRequested(const War3Map::Shell::ModuleId& id, War3Map::Shell::Area area);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    bool event(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void ReportGeometry();

    ModuleId id_;
    QVBoxLayout* layout_ = nullptr;
    QWidget* content_ = nullptr;
    bool applying_ = false;
    bool inSizeMove_ = false;
    QSize sizeAtMoveStart_;
};

} // namespace War3Map::Shell
