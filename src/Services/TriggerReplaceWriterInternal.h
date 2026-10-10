#pragma once

#include "War3MapCore/Hash.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace War3Map::Services::detail {

// 仅共用服务内部字节帧；不是编辑许可。CheckedAdd 复用调用方原唯一预算函数。
// Hash 是终结；生产落点均只在写完后 Hash，可重复读缓存，终结后写入明确失败。
template<auto CheckedAdd>
class TriggerReplaceWriter final {
public:
    static constexpr std::size_t kBufferBytes = 8 * 1024;
    explicit TriggerReplaceWriter(std::size_t limit) : limit_(limit) {}

    bool Text(std::string_view value) {
        if (finished_) { closedWrite_ = true; return false; }
        std::array<char, 32> digits{};
        const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value.size());
        if (converted.ec != std::errc{}) { valid_ = false; return false; }
        const std::string_view length(digits.data(), static_cast<std::size_t>(converted.ptr - digits.data()));
        if (!CheckedAdd(used_, length.size() + 1, limit_) || !CheckedAdd(used_, value.size(), limit_)) { valid_ = false; return false; }
        Append(length); Append(":"); Append(value); return true;
    }
    template<class T> bool Number(T value) {
        static_assert(std::is_integral_v<T>);
        if (finished_) { closedWrite_ = true; return false; }
        std::array<char, 32> digits{};
        const auto converted = [&] {
            if constexpr (std::is_signed_v<T>) return std::to_chars(digits.data(), digits.data() + digits.size(), static_cast<std::int64_t>(value));
            else return std::to_chars(digits.data(), digits.data() + digits.size(), static_cast<std::uint64_t>(value));
        }();
        if (converted.ec != std::errc{}) { valid_ = false; return false; }
        return Text(std::string_view(digits.data(), static_cast<std::size_t>(converted.ptr - digits.data())));
    }
    std::size_t Used() const { return used_; }
    Result<std::string> Hash() const {
        if (closedWrite_) return Result<std::string>::Failure("Trigger.Replace", "TRIGGER_REPLACE_WRITER_CLOSED", "来源摘要已终结，不能再写入或返回旧摘要。");
        if (cached_) return *cached_;
        finished_ = true;
        if (!valid_) cached_ = Result<std::string>::Failure("Trigger.Replace", "TRIGGER_REPLACE_TEXT_BUDGET", "来源摘要超过有界文本预算。");
        else if (feedInterrupted_) cached_ = Result<std::string>::Failure("Hash", "HASH_STREAM_EXCEPTION", "SHA-256 流内部准备或更新异常，已封闭。");
        else if (!stream_) cached_ = Sha256Text(std::string_view(buffer_.data(), pending_));
        else {
            Flush();
            cached_ = failure_.Succeeded() ? stream_->Finish() : Result<std::string>{std::nullopt, failure_.diagnostics};
        }
        return *cached_;
    }

private:
    // 未送入流的小帧复用有界 Sha256Text；原 8KiB flush 才建立 64KiB 流状态。
    void Feed(std::string_view bytes) const {
        if (!failure_.Succeeded() || feedInterrupted_) return;
        // 构造/更新/诊断分配若抛异常，先无分配锁存；不能稍后返回前缀摘要。
        feedInterrupted_ = true;
        if (!stream_) stream_.emplace(limit_);
        auto status = stream_->Update(std::as_bytes(std::span(bytes.data(), bytes.size())));
        if (!status.Succeeded()) failure_ = std::move(status);
        feedInterrupted_ = false;
    }
    void Flush() const {
        if (pending_) Feed(std::string_view(buffer_.data(), pending_));
        pending_ = 0;
    }
    void Append(std::string_view bytes) {
        if (!failure_.Succeeded() || feedInterrupted_) return;
        while (!bytes.empty()) {
            if (!pending_ && bytes.size() >= kBufferBytes) { Feed(bytes); return; }
            const auto count = (std::min)(bytes.size(), kBufferBytes - pending_);
            std::memcpy(buffer_.data() + pending_, bytes.data(), count);
            pending_ += count; bytes.remove_prefix(count);
            if (pending_ == kBufferBytes) { Flush(); if (!failure_.Succeeded()) return; }
        }
    }
    std::size_t limit_, used_ = 0;
    mutable std::optional<Sha256Stream> stream_;
    mutable std::array<char, kBufferBytes> buffer_{};
    mutable std::size_t pending_ = 0;
    mutable Status failure_;
    mutable std::optional<Result<std::string>> cached_;
    mutable bool finished_ = false;
    mutable bool feedInterrupted_ = false;
    bool valid_ = true, closedWrite_ = false;
};

} // namespace War3Map::Services::detail
