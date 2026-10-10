#pragma once

#include <functional>

class QThread;
class QWidget;

namespace War3Map::QtApp {

// 仅等待宿主拥有的恢复线程。取消回调必须合作式停止写入，不能终止线程。
// 返回 false 表示用户选择返回编辑；退出事件已开始时不能返回编辑。
[[nodiscard]] bool WaitForRecoveryExit(QThread* worker, const std::function<bool()>& cancelWrite,
    QWidget* parent = nullptr, bool allowReturnToEditor = true);

} // namespace War3Map::QtApp
