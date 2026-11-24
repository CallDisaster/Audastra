#include "AnimationWorkers.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace War3Map::Render {
namespace {
// 任务内部再次调用时直接在当前线程执行，不重入线程池。
thread_local bool insideAnimationTask = false;

class AnimationWorkerPool {
public:
    explicit AnimationWorkerPool(std::uint32_t helpers) {
        threads_.reserve(helpers);
        try {
            for (std::uint32_t i = 0; i < helpers; ++i) threads_.emplace_back([this] { Loop(); });
        } catch (...) {
            // 起不来的线程不计入；已起的照常工作，结果与线程数无关。
        }
    }
    ~AnimationWorkerPool() {
        { std::lock_guard lock(mutex_); stop_ = true; }
        wake_.notify_all();
        for (auto& thread : threads_) thread.join();
    }
    AnimationWorkerPool(const AnimationWorkerPool&) = delete;
    AnimationWorkerPool& operator=(const AnimationWorkerPool&) = delete;

    bool Run(std::uint32_t tasks, const std::function<void(std::uint32_t)>& task) noexcept {
        std::unique_lock busy(run_, std::try_to_lock);
        if (!busy.owns_lock() || threads_.empty()) return false;
        {
            std::lock_guard lock(mutex_);
            task_ = &task; tasks_ = tasks; next_.store(0, std::memory_order_relaxed);
            running_ = static_cast<std::uint32_t>(threads_.size()); ++generation_;
        }
        wake_.notify_all();
        Work();
        std::unique_lock lock(mutex_);
        finished_.wait(lock, [this] { return running_ == 0 || stop_; });
        task_ = nullptr;
        return true;
    }

private:
    void Work() noexcept {
        insideAnimationTask = true;
        for (;;) {
            const auto index = next_.fetch_add(1, std::memory_order_relaxed);
            if (index >= tasks_) break;
            (*task_)(index);
        }
        insideAnimationTask = false;
    }
    void Loop() noexcept {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            Work();
            std::lock_guard lock(mutex_);
            if (--running_ == 0) finished_.notify_one();
        }
    }

    std::mutex run_, mutex_;
    std::condition_variable wake_, finished_;
    const std::function<void(std::uint32_t)>* task_ = nullptr;
    std::uint32_t tasks_ = 0, running_ = 0;
    std::atomic<std::uint32_t> next_{0};
    std::uint64_t generation_ = 0;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

AnimationWorkerPool& Pool() {
    static AnimationWorkerPool pool(AnimationTaskParallelism() - 1);
    return pool;
}
} // namespace

std::uint32_t AnimationTaskParallelism() noexcept {
    const auto cores = std::thread::hardware_concurrency();
    return cores > 3 ? std::min(cores - 2, 8u) : 1u;
}

void RunAnimationTasks(std::uint32_t tasks, const std::function<void(std::uint32_t)>& task) noexcept {
    if (!tasks) return;
    if (tasks > 1 && !insideAnimationTask && AnimationTaskParallelism() > 1) {
        try {
            if (Pool().Run(tasks, task)) return;
        } catch (...) {
            // 线程池建不起来时退回当前线程，结果不变。
        }
    }
    for (std::uint32_t i = 0; i < tasks; ++i) task(i);
}
} // namespace War3Map::Render
