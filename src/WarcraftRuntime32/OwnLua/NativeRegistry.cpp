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

const NativeRegistration* NativeRegistry::Find(std::string_view name) const noexcept {
    if (failed_) return nullptr;
    const auto found = entries_.find(name);
    return found == entries_.end() ? nullptr : &found->second;
}

} // namespace Audastra::Runtime32::OwnLua
