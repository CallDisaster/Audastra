#pragma once

#include <QString>

#include <atomic>
#include <functional>

class QWidget;

namespace War3Map::UiKit {

// 在工作线程里跑一段不访问界面的任务，期间显示窗口模态的进度框（超过 400 毫秒才出现）。
// progress 可在工作线程调用，排队到界面线程更新文字。cancelText 为空时不能取消；
// 否则作者点取消后 stop 置位，任务应在合适的位置停下。返回假表示作者取消了。
struct BlockingTaskControl {
    std::function<void(const QString&)> progress;
    const std::atomic_bool* stop = nullptr;
};
bool RunBlockingTask(QWidget* parent, const QString& label, const QString& cancelText,
    const std::function<void(const BlockingTaskControl&)>& work);

} // namespace War3Map::UiKit
