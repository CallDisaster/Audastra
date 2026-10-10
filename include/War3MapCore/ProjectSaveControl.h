#pragma once

#include "War3MapCore/Types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

namespace War3Map {

enum class ProjectSaveStage { Read, Hash, Build, Copy, Write, Verify, BeforePublication, Publication };

// 单个工程快照任务的协作取消。进度回调在 worker 同步调用，必须自己拥有输入，
// 不得访问 UI/文档/Undo。开始任务后不能修改回调；默认不设置回调。
class ProjectSaveControl final {
public:
    using Progress = std::function<void(ProjectSaveStage, std::uint64_t)>;
    explicit ProjectSaveControl(Progress progress = {}) : progress_(std::move(progress)) {}
    ProjectSaveControl(const ProjectSaveControl&) = delete;
    ProjectSaveControl& operator=(const ProjectSaveControl&) = delete;

    [[nodiscard]] bool RequestCancellation() noexcept {
        auto expected = State::Active;
        if (state_.compare_exchange_strong(expected, State::CancellationRequested, std::memory_order_acq_rel)) {
            stop_.request_stop();
            return true;
        }
        return expected == State::CancellationRequested;
    }
    [[nodiscard]] bool CancellationRequested() const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return state == State::CancellationRequested || state == State::Cancelled || stop_.stop_requested();
    }
    [[nodiscard]] bool PublicationStarted() const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return state == State::Publishing || state == State::PublishedOrFailed;
    }
    [[nodiscard]] bool CanRun() const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return state == State::Active || state == State::CancellationRequested;
    }
    [[nodiscard]] std::stop_token StopToken() const noexcept { return stop_.get_token(); }
    [[nodiscard]] bool Checkpoint(ProjectSaveStage stage, std::uint64_t bytes = 0) const {
        if (CancellationRequested()) return false;
        if (progress_) progress_(stage, bytes);
        return !CancellationRequested();
    }
    // 必须紧邻首次对最终目标的改名。CAS 与请求取消互斥；获胜后不能再取消。
    [[nodiscard]] bool BeginPublication() {
        if (!Checkpoint(ProjectSaveStage::BeforePublication)) return false;
        auto expected = State::Active;
        if (!state_.compare_exchange_strong(expected, State::Publishing, std::memory_order_acq_rel)) return false;
        if (progress_) progress_(ProjectSaveStage::Publication, 0);
        return true;
    }
    // 发布过程中仅用于自有诊断/测试观察，不重新开放取消窗口。
    void PublicationStep(std::uint64_t step) const {
        if (progress_) progress_(ProjectSaveStage::Publication, step);
    }
    // 关闭尚未发布任务的取消窗口。返回值表示此前确实接受了取消。
    [[nodiscard]] bool Complete() noexcept {
        auto current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (current == State::Finished || current == State::PublishedOrFailed) return false;
            if (current == State::Cancelled) return true;
            const auto terminal = current == State::Publishing ? State::PublishedOrFailed
                : current == State::CancellationRequested ? State::Cancelled : State::Finished;
            if (state_.compare_exchange_weak(current, terminal, std::memory_order_acq_rel))
                return terminal == State::Cancelled;
        }
    }

private:
    enum class State { Active, CancellationRequested, Publishing, Finished, Cancelled, PublishedOrFailed };
    std::atomic<State> state_{State::Active};
    std::stop_source stop_;
    const Progress progress_;
};

inline constexpr const char* kProjectSaveCancelledCode = "PROJECT_SAVE_CANCELLED";

[[nodiscard]] inline Status ProjectSaveCancelledStatus() {
    return Status::Failure("ProjectSave", kProjectSaveCancelledCode,
        "工程快照写入已取消，上一份恢复点和当前编辑保持不变。");
}

// 只复制调用方拥有的输入；恢复任务每 MiB 检查一次，默认路径保持原来的一次复制。
[[nodiscard]] inline bool CopyProjectSaveBytes(std::span<std::byte> destination,
    std::span<const std::byte> source, const ProjectSaveControl* control = nullptr) {
    if (destination.size() != source.size()) return false;
    for (std::size_t offset = 0; offset < source.size();) {
        if (control && !control->Checkpoint(ProjectSaveStage::Copy, offset)) return false;
        const auto count = control ? (std::min)(source.size() - offset, std::size_t{1024 * 1024}) : source.size();
        std::memcpy(destination.data() + offset, source.data() + offset, count);
        offset += count;
    }
    return !control || control->Checkpoint(ProjectSaveStage::Copy, source.size());
}

[[nodiscard]] inline bool EqualProjectSaveBytes(std::span<const std::byte> left,
    std::span<const std::byte> right, const ProjectSaveControl* control = nullptr) {
    if (left.size() != right.size()) return false;
    for (std::size_t offset = 0; offset < left.size();) {
        if (control && !control->Checkpoint(ProjectSaveStage::Verify, offset)) return false;
        const auto count = control ? (std::min)(left.size() - offset, std::size_t{1024 * 1024}) : left.size();
        if (std::memcmp(left.data() + offset, right.data() + offset, count) != 0) return false;
        offset += count;
    }
    return !control || control->Checkpoint(ProjectSaveStage::Verify, left.size());
}

} // namespace War3Map
