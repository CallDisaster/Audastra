#include "NativeRegistry.h"

#include <array>
#include <charconv>

namespace Audastra::Runtime32::OwnLua {
namespace {
std::string DiagnosticText(std::string_view text, std::size_t limit) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char ch : text.substr(0, limit)) {
        if (ch >= 0x20 && ch <= 0x7e && ch != '\\' && ch != '"') result += static_cast<char>(ch);
        else {
            result += "\\x";
            result += digits[ch >> 4]; result += digits[ch & 15];
        }
    }
    if (text.size() > limit) result += "...";
    return result + '"';
}
std::string Address(std::uintptr_t value) {
    std::array<char, sizeof(value) * 2> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value, 16);
    return "0x" + std::string(text.data(), converted.ptr);
}
}

std::string DescribeNativeRegistration(std::size_t acceptedEntries, std::string_view name,
    std::string_view signature, std::uintptr_t function) {
    return "acceptedEntries=" + std::to_string(acceptedEntries) + "; name=" + DiagnosticText(name, 128)
        + "; signature=" + DiagnosticText(signature, 192) + "; address=" + Address(function);
}

bool NativeRegistry::Observe(std::string_view name, std::string_view signatureText, std::uintptr_t function) {
    if (failed_) return false;
    const auto fail = [&](const char* gate, const char* message) {
        failed_ = true;
        // 只保留第一个失败条目。转义控制字符，明确记录被哪一道检查拒绝，不输出完整登记表。
        error_ = std::string(message) + " gate=" + gate + "; "
            + DescribeNativeRegistration(entries_.size(), name, signatureText, function);
        return false;
    };
    if (!Collecting()) return fail(Sealed() ? "sealed" : "registration-phase", "native 表当前不允许修改。");
    if (!NativeIdentifier(name)) return fail("name", "native 注册名称无效。");
    const auto signature = ParseNativeSignature(signatureText);
    if (!signature) return fail("signature", "native 注册签名无效。");
    if (!function) return fail("address", "native 注册地址无效。");
    const auto found = entries_.find(name);
    if (found != entries_.end()) {
        if (found->second.function == function && found->second.signature == *signature) return true;
        return fail("changed-registration", "同名 native 注册内容发生变化，不能继续使用旧绑定。");
    }
    if (entries_.size() >= 8192) return fail("entry-budget", "native 注册数量超出上限。");
    std::string key(name);
    entries_.emplace(key, NativeRegistration{key, *signature, function});
    return true;
}

bool NativeRegistry::Advance(Phase expected, Phase next) {
    if (failed_) return false;
    if (phase_ != expected) {
        failed_ = true;
        error_ = "native 登记阶段顺序无效，不能复用不完整或已交给 Lua 的快照。";
        return false;
    }
    phase_ = next;
    return true;
}
bool NativeRegistry::CompleteStock() { return Advance(Phase::Stock, Phase::StockReady); }
bool NativeRegistry::BeginSupplement() { return Advance(Phase::StockReady, Phase::Supplement); }
bool NativeRegistry::CompleteSupplement() { return Advance(Phase::Supplement, Phase::SupplementReady); }
bool NativeRegistry::Seal() {
    if (failed_) return false;
    if (phase_ == Phase::Sealed) return true;
    // 某些脚本入口只使用 stock 表；只接受已经完整退出的明确登记阶段。
    return Advance(phase_ == Phase::StockReady ? Phase::StockReady : Phase::SupplementReady, Phase::Sealed);
}
bool NativeRegistry::Collecting() const noexcept {
    return !failed_ && (phase_ == Phase::Stock || phase_ == Phase::Supplement);
}
bool NativeRegistry::Ready() const noexcept {
    return !failed_ && (phase_ == Phase::StockReady || phase_ == Phase::SupplementReady || phase_ == Phase::Sealed);
}
bool NativeRegistry::Sealed() const noexcept { return phase_ == Phase::Sealed; }

const NativeRegistration* NativeRegistry::Find(std::string_view name) const noexcept {
    if (failed_) return nullptr;
    const auto found = entries_.find(name);
    return found == entries_.end() ? nullptr : &found->second;
}

} // namespace Audastra::Runtime32::OwnLua
