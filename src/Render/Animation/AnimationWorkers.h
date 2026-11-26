#pragma once
#include <cstdint>
#include <functional>

namespace War3Map::Render {
// 动画准备用的进程级常驻线程。任务编号决定写入位置，线程只决定快慢：
// 同一组任务无论几个线程执行，输出都逐位相同。
// 几个调用方可同时提交，各自参与自己的任务并共用常驻线程；同时提交过多时本次在当前线程依次执行。
// task 不得抛异常，也不得再次调用 RunAnimationTasks。
void RunAnimationTasks(std::uint32_t tasks, const std::function<void(std::uint32_t)>& task) noexcept;
// 后台任务：常驻线程先给普通任务，后台只用当时空闲的线程；调用线程自己始终参与。输出同样与线程数无关。
void RunAnimationBackgroundTasks(std::uint32_t tasks, const std::function<void(std::uint32_t)>& task) noexcept;

// workers==1（AnimationLimits::poseWorkers，测试用来对照）时在当前线程依次执行，否则同 RunAnimationTasks。
inline void RunAnimationTasksWith(std::uint32_t workers, std::uint32_t tasks,
    const std::function<void(std::uint32_t)>& task, bool background = false) noexcept {
    if (workers == 1) { for (std::uint32_t i = 0; i < tasks; ++i) task(i); return; }
    if (background) RunAnimationBackgroundTasks(tasks, task); else RunAnimationTasks(tasks, task);
}

// 测试用：在几处跨线程交接点调用的让步钩子，用来放大竞争窗口；正式运行从不设置，此时每处只多一次原子读。
enum class AnimationJitterSite : std::uint32_t {
    PrepareStart = 1, PreviousRead, PendingMark, FinalizeStart, FinalizeBounds, FinalizeDone, Task, Take, Return
};
using AnimationJitterHook = void (*)(AnimationJitterSite site) noexcept;
void SetAnimationJitterHook(AnimationJitterHook hook) noexcept;
void AnimationJitter(AnimationJitterSite site) noexcept;

// 本机可同时运行的动画任务数（含调用线程）：保留两个核心给界面和绘制，至多 8。
[[nodiscard]] std::uint32_t AnimationTaskParallelism() noexcept;
} // namespace War3Map::Render
