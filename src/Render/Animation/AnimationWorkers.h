#pragma once
#include <cstdint>
#include <functional>

namespace War3Map::Render {
// 动画准备用的进程级常驻线程。任务编号决定写入位置，线程只决定快慢：
// 同一组任务无论几个线程执行，输出都逐位相同。
// 另一服务正占用线程时，本次调用在当前线程依次执行，不排队等待。
// task 不得抛异常，也不得再次调用 RunAnimationTasks。
void RunAnimationTasks(std::uint32_t tasks, const std::function<void(std::uint32_t)>& task) noexcept;

// workers==1（AnimationLimits::poseWorkers，测试用来对照）时在当前线程依次执行，否则同 RunAnimationTasks。
inline void RunAnimationTasksWith(std::uint32_t workers, std::uint32_t tasks,
    const std::function<void(std::uint32_t)>& task) noexcept {
    if (workers == 1) { for (std::uint32_t i = 0; i < tasks; ++i) task(i); return; }
    RunAnimationTasks(tasks, task);
}

// 本机可同时运行的动画任务数（含调用线程）：保留两个核心给界面和绘制，至多 8。
[[nodiscard]] std::uint32_t AnimationTaskParallelism() noexcept;
} // namespace War3Map::Render
