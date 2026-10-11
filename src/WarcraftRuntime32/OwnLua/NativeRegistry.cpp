#include "NativeRegistry.h"

namespace Audastra::Runtime32::OwnLua {

bool NativeRegistry::Observe(std::string_view name, std::string_view signatureText, std::uintptr_t function) {
    if (failed_) return false;
    const auto fail = [&](const char* message) { failed_ = true; error_ = message; return false; };
    const auto signature = ParseNativeSignature(signatureText);
    if (!NativeIdentifier(name) || !signature || !function) return fail("native 注册名称、签名或地址无效。");
    const auto found = entries_.find(name);
    if (found != entries_.end()) {
        if (found->second.function == function && found->second.signature == *signature) return true;
        return fail("同名 native 注册内容发生变化，不能继续使用旧绑定。");
    }
    if (entries_.size() >= 8192) return fail("native 注册数量超出上限。");
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
