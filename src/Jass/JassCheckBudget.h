#pragma once

#include "War3MapCore/Jass/JassChecker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// 检查调用内共享的硬预算。异常只携带固定枚举，不在失败线程分配动态错误文本。
namespace War3Map::Jass::detail {

enum class CheckFailure : std::uint8_t {
    None, Sources, SourceBytes, TotalBytes, Tokens, AstNodes, PoolNodes, AstDepth,
    Nesting, Diagnostics, DiagnosticBytes, Lexical, Cancelled, Deadline, Allocation, Worker, InvalidTree,
};

// 先摘要再拼接，防止短调用重复复制巨大的声明名称；仅用于消息，原字节和 origin 不变。
inline std::string DiagnosticText(std::string_view text, std::size_t limit = 256) {
    if (text.size() <= limit) return std::string(text);
    auto end = limit < 3 ? limit : limit - 3;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0u) == 0x80u) --end;
    if (limit < 3) return std::string(text.substr(0, end));
    return std::string(text.substr(0, end)) + "…";
}

class CheckAborted final : public std::exception {
public:
    explicit CheckAborted(CheckFailure cause) noexcept : cause_(cause) {}
    [[nodiscard]] CheckFailure Cause() const noexcept { return cause_; }
    const char* what() const noexcept override { return "JASS check interrupted"; }
private:
    CheckFailure cause_;
};

class CheckBudget {
public:
    explicit CheckBudget(const JassCheckOptions& options) : options_(options) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = (std::chrono::steady_clock::time_point::max)() - now;
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        deadline_ = options.maxDuration <= std::chrono::milliseconds::zero() ? now
            : options.maxDuration >= duration ? (std::chrono::steady_clock::time_point::max)()
            : now + options.maxDuration;
        deadline_ = (std::min)(deadline_, options.deadline);
    }

    [[noreturn]] void Fail(CheckFailure cause) {
        MarkFailure(cause);
        throw CheckAborted(Failure());
    }
    void MarkFailure(CheckFailure cause) noexcept {
        auto expected = CheckFailure::None;
        failure_.compare_exchange_strong(expected, cause, std::memory_order_relaxed);
    }
    [[nodiscard]] CheckFailure Failure() const noexcept { return failure_.load(std::memory_order_relaxed); }

    void Poll() {
        if (const auto cause = Failure(); cause != CheckFailure::None) throw CheckAborted(cause);
        if (options_.cancel.stop_requested()) Fail(CheckFailure::Cancelled);
        if (std::chrono::steady_clock::now() >= deadline_) Fail(CheckFailure::Deadline);
    }
    void PollOccasionally(std::size_t position) { if ((position & 255u) == 0) Poll(); }

    void ConsumeAst(std::size_t count = 1) { Consume(ast_, count, AstLimit(), CheckFailure::AstNodes); }
    void ConsumeDiagnostic(std::size_t bytes = 0) {
        Consume(diagnostics_, 1, options_.maxProducedDiagnostics, CheckFailure::Diagnostics);
        ConsumeDiagnosticBytes(bytes);
    }
    void ConsumeDiagnosticBytes(std::size_t bytes) { Consume(diagnosticBytes_, bytes, options_.maxDiagnosticBytes, CheckFailure::DiagnosticBytes); }
    [[nodiscard]] std::string Message(std::string_view text) const { return DiagnosticText(text, options_.maxDiagnosticMessageBytes); }
    [[nodiscard]] std::size_t DiagnosticBytes() const noexcept { return diagnosticBytes_.load(std::memory_order_relaxed); }
    void RejectLexical(std::vector<Diagnostic> diagnostics) {
        if (std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.code == "SOURCE_LEX_TOKEN_BUDGET"; })) {
            Fail(CheckFailure::Tokens);
        }
        {
            std::lock_guard lock(lexicalMutex_);
            if (lexicalDiagnostics_.empty()) lexicalDiagnostics_ = std::move(diagnostics);
        }
        Fail(CheckFailure::Lexical);
    }
    [[nodiscard]] std::vector<Diagnostic> TakeLexicalDiagnostics() {
        std::lock_guard lock(lexicalMutex_);
        return std::move(lexicalDiagnostics_);
    }
    void ConsumeTokens(std::size_t count) { Consume(tokens_, count, options_.maxTokens, CheckFailure::Tokens); }
    void RegisterSource(std::size_t bytes) {
        Poll();
        if (bytes > options_.maxSourceBytes || bytes >= (std::numeric_limits<std::uint32_t>::max)()) Fail(CheckFailure::SourceBytes);
        Consume(sources_, 1, options_.maxSources, CheckFailure::Sources);
        Consume(bytes_, bytes, options_.maxTotalBytes, CheckFailure::TotalBytes);
    }
    [[nodiscard]] std::size_t Tokens() const noexcept { return tokens_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t RemainingTokens() const noexcept { return options_.maxTokens - Tokens(); }
    [[nodiscard]] std::size_t AstNodes() const noexcept { return ast_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t MaxDepth() const noexcept { return depth_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t DiagnosticLimit() const noexcept { return options_.maxProducedDiagnostics; }
    [[nodiscard]] std::size_t AstLimit() const noexcept {
        return (std::min)(options_.maxAstNodes, static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()));
    }
    [[nodiscard]] std::size_t PoolLimit() const noexcept {
        return (std::min)(options_.maxPoolNodes, static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()));
    }
    void CheckPool(std::size_t existing, std::size_t count = 1) {
        Poll();
        if (existing > PoolLimit() || count > PoolLimit() - existing) Fail(CheckFailure::PoolNodes);
    }
    void CheckDepth(std::size_t depth) {
        if (depth > (std::min)(options_.maxAstDepth, std::size_t{512})) Fail(CheckFailure::AstDepth);
        auto observed = depth_.load(std::memory_order_relaxed);
        while (observed < depth && !depth_.compare_exchange_weak(observed, depth, std::memory_order_relaxed)) {}
    }
    void Enter(std::size_t& nesting) {
        Poll();
        if (nesting >= (std::min)(options_.maxNesting, std::size_t{128})) Fail(CheckFailure::Nesting);
        ++nesting;
    }

private:
    void Consume(std::atomic<std::size_t>& counter, std::size_t count, std::size_t limit, CheckFailure cause) {
        Poll();
        auto used = counter.load(std::memory_order_relaxed);
        for (;;) {
            if (used > limit || count > limit - used) Fail(cause);
            if (counter.compare_exchange_weak(used, used + count, std::memory_order_relaxed)) return;
        }
    }
    const JassCheckOptions& options_;
    std::chrono::steady_clock::time_point deadline_;
    std::atomic<CheckFailure> failure_{CheckFailure::None};
    std::atomic<std::size_t> tokens_{0}, ast_{0}, diagnostics_{0}, depth_{0}, sources_{0}, bytes_{0};
    std::atomic<std::size_t> diagnosticBytes_{0};
    std::mutex lexicalMutex_;
    std::vector<Diagnostic> lexicalDiagnostics_;
};

class NestingGuard {
public:
    NestingGuard(CheckBudget* budget, std::size_t& depth) : budget_(budget), depth_(depth) {
        if (budget_) budget_->Enter(depth_);
    }
    ~NestingGuard() { if (budget_) --depth_; }
    NestingGuard(const NestingGuard&) = delete;
    NestingGuard& operator=(const NestingGuard&) = delete;
private:
    CheckBudget* budget_;
    std::size_t& depth_;
};

// 在 push/insert 之前扩容且不超过预算，避免 vector 隐式倍增越过限制。
template<class T>
void ReserveBounded(std::vector<T>& values, std::size_t required, std::size_t limit, CheckBudget& budget) {
    if (required > limit || required > values.max_size()) budget.Fail(CheckFailure::PoolNodes);
    if (required <= values.capacity()) return;
    const auto capacity = values.capacity();
    const auto growth = (std::min)((std::max)(capacity, std::size_t{1}), limit - capacity);
    values.reserve((std::max)(required, capacity + growth));
}

} // namespace War3Map::Jass::detail
