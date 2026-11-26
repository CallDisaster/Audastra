#include "AnimationWorkers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace War3Map::Render {
namespace {
// 任务内部再次调用时直接在当前线程执行，不重入线程池。
thread_local bool insideAnimationTask = false;
std::atomic<AnimationJitterHook> jitterHook{nullptr};

// 常驻辅助线程按作业分配：可有几个调用方同时提交，各自亲自参与自己的作业、全部完成才返回。
// 辅助线程先分给普通作业；后台作业只在没有普通作业时使用它们（已在后台作业里的线程做完手头一段即让出）。
// 任务编号决定写入位置，线程分配只影响快慢。
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

    bool Run(std::uint32_t tasks, const std::function<void(std::uint32_t)>& task, bool background) noexcept {
        if (threads_.empty()) return false;
        Job job;
        job.task = &task; job.tasks = tasks; job.background = background;
        {
            std::lock_guard lock(mutex_);
            if (count_ == jobs_.size()) return false; // 同时提交的调用方过多：本次在当前线程执行。
            jobs_[count_++] = &job;
            if (!background) normalJobs_.fetch_add(1, std::memory_order_relaxed);
        }
        wake_.notify_all();
        Work(job, false);
        std::unique_lock lock(mutex_);
        // 编号已全部领完：先撤下作业，不再有新线程加入，再等仍在执行本作业的线程做完。
        const auto end = jobs_.begin() + count_;
        std::copy(std::find(jobs_.begin(), end, &job) + 1, end, std::find(jobs_.begin(), end, &job));
        --count_;
        if (!background) {
            normalJobs_.fetch_sub(1, std::memory_order_relaxed);
            wake_.notify_all(); // 让给普通作业的线程回到后台作业。
        }
        finished_.wait(lock, [&] { return job.helpers == 0; });
        return true;
    }

private:
    struct Job {
        const std::function<void(std::uint32_t)>* task = nullptr;
        std::uint32_t tasks = 0;
        std::atomic<std::uint32_t> next{0};
        std::uint32_t helpers = 0; // 正在执行本作业的辅助线程数（mutex_ 保护）
        bool background = false;
    };
    // mutex_ 内调用：有普通作业时只分给尚有编号的普通作业，否则分给尚有编号的后台作业。
    Job* Pick() const noexcept {
        const bool normal = normalJobs_.load(std::memory_order_relaxed) != 0;
        for (std::size_t i = 0; i < count_; ++i) {
            auto* job = jobs_[i];
            if (job->background == normal || job->next.load(std::memory_order_relaxed) >= job->tasks) continue;
            return job;
        }
        return nullptr;
    }
    void Work(Job& job, bool helper) noexcept {
        insideAnimationTask = true;
        for (;;) {
            if (helper && job.background && normalJobs_.load(std::memory_order_relaxed)) break;
            const auto index = job.next.fetch_add(1, std::memory_order_relaxed);
            if (index >= job.tasks) break;
            AnimationJitter(AnimationJitterSite::Task);
            (*job.task)(index);
        }
        insideAnimationTask = false;
    }
    void Loop() noexcept {
#ifdef _WIN32
        // 与动画工作线程同级（低于普通），不抢界面与绘制线程。
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        std::unique_lock lock(mutex_);
        for (;;) {
            Job* job = nullptr;
            wake_.wait(lock, [&] { return stop_ || (job = Pick()) != nullptr; });
            if (stop_) return;
            ++job->helpers;
            lock.unlock();
            Work(*job, true);
            lock.lock();
            if (--job->helpers == 0) finished_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_, finished_;
    std::array<Job*, 8> jobs_{};
    std::size_t count_ = 0;
    std::atomic<std::uint32_t> normalJobs_{0}; // 已提交未撤下的普通作业数；只在 mutex_ 内修改
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

AnimationWorkerPool& Pool() {
    static AnimationWorkerPool pool(AnimationTaskParallelism() - 1);
    return pool;
}

void Run(std::uint32_t tasks, const std::function<void(std::uint32_t)>& task, bool background) noexcept {
    if (!tasks) return;
    if (tasks > 1 && !insideAnimationTask && AnimationTaskParallelism() > 1) {
        try {
            if (Pool().Run(tasks, task, background)) return;
        } catch (...) {
            // 线程池建不起来时退回当前线程，结果不变。
        }
    }
    for (std::uint32_t i = 0; i < tasks; ++i) task(i);
}
} // namespace

void SetAnimationJitterHook(AnimationJitterHook hook) noexcept { jitterHook.store(hook, std::memory_order_release); }
void AnimationJitter(AnimationJitterSite site) noexcept {
    if (const auto hook = jitterHook.load(std::memory_order_acquire)) hook(site);
}

std::atomic<std::uint32_t> parallelismOverride{0};
void SetAnimationTaskParallelismOverride(std::uint32_t threads) noexcept {
    parallelismOverride.store(std::min(threads, 8u), std::memory_order_relaxed);
}
std::uint32_t AnimationTaskParallelism() noexcept {
    if (const auto threads = parallelismOverride.load(std::memory_order_relaxed)) return threads;
    const auto cores = std::thread::hardware_concurrency();
    return cores > 3 ? std::min(cores - 2, 8u) : 1u;
}

void RunAnimationTasks(std::uint32_t tasks, const std::function<void(std::uint32_t)>& task) noexcept {
    Run(tasks, task, false);
}
void RunAnimationBackgroundTasks(std::uint32_t tasks, const std::function<void(std::uint32_t)>& task) noexcept {
    Run(tasks, task, true);
}
} // namespace War3Map::Render
