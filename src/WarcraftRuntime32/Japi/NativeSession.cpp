#include "NativeSession.h"
#include "aj/NativeBridge.h"
#include <stdexcept>
#include <string>
#include <utility>

namespace Audastra::Runtime32::Japi {
namespace {
struct Depth {
    unsigned& value;
    explicit Depth(unsigned& input) : value(input) { ++value; }
    ~Depth() { --value; }
};
}
NativeSession::NativeSession(std::function<void(std::string_view)> fatal,
    std::function<void(std::string_view, std::string_view)> reply)
    : fatal_(std::move(fatal)), reply_(std::move(reply)) {
    if (!fatal_) throw std::invalid_argument("JAPI session requires a fatal handler");
}
NativeSession::~NativeSession() { if (vm_) vm_->Invalidate(); }
void NativeSession::Fail(std::string_view error) {
    if (failed_) return;
    failed_ = true;
    if (vm_) vm_->Invalidate();
    fatal_(error);
}
std::optional<std::uint32_t> NativeSession::Dispatch(std::uint64_t generation,
    std::string_view input, const Attach& attach) {
    if (!input.starts_with("ajapi:")) return {};
    std::lock_guard lock(mutex_);
    if (failed_) return 0;
    if (thread_ == std::thread::id{}) thread_ = std::this_thread::get_id();
    if (thread_ != std::this_thread::get_id()) {
        Fail("JAPI 字符串调用不在所属游戏线程。"); return 0;
    }
    // 包括附着/返回值分配，销毁通知不能在这段期间拆掉 Host 或 VM。
    Depth call(depth_);
    try {
        if (!generation || generation > INT32_MAX) {
            Fail("JAPI native 登记代际无效或已耗尽。"); return 0;
        }
        if (!host_) {
            if (generation <= generation_ || depth_ != 1 || !attach) {
                Fail("JAPI 禁止复活已销毁的 VM 代际或重入附着。"); return 0;
            }
            auto opened = attach();
            if (failed_) { if (opened.vm) opened.vm->Invalidate(); return 0; }
            if (!opened.vm || opened.vm->Generation() != generation) {
                if (opened.vm) opened.vm->Invalidate();
                Fail(opened.error.empty() ? "JAPI 没有有效的当前脚本 VM。" : opened.error); return 0;
            }
            vm_ = std::move(opened.vm);
            generation_ = generation;
            audastra::japi::HostOptions options;
            options.initialEpoch = static_cast<std::uint32_t>(generation);
            // 基础桥不授予加载器，不提供脚本执行器；同步能力保持默认关闭。
            host_ = std::make_unique<audastra::japi::Host>(std::move(options));
            if (opened.install) {
                const auto installed = opened.install(*host_);
                if (failed_ || installed != AJ_OK) {
                    Fail("JAPI 内置模块装入失败，状态 " + std::to_string(installed) + "。"); return 0;
                }
            }
        }
        if (generation != generation_ || vm_->Generation() != generation_) {
            Fail("JAPI 所属 VM、来源或 native 登记代际已经改变。"); return 0;
        }
        std::string result;
        if (!audastra::japi::HandleLocalizedString(*host_, input, result) || result.size() > AJ_MAX_FRAME) {
            Fail("JAPI 内部响应超出已核定的字符串边界。"); return 0;
        }
        if (failed_ || vm_->Generation() != generation_) {
            Fail("JAPI 请求返回前 VM 已失效。"); return 0;
        }
        const auto id = vm_->ReturnString(result);
        if (failed_ || !id || !*id || vm_->Generation() != generation_) {
            Fail("JAPI 无法向当前 VM 转交有效的响应字符串。"); return 0;
        }
        if (reply_) { try { reply_(input, result); } catch (...) {} }
        return *id;
    } catch (...) { Fail("JAPI 字符串会话发生异常，已禁用当前会话。"); return 0; }
}
void NativeSession::Stop() {
    if (depth_) { Fail("游戏在 JAPI 调用期间重置或销毁其 VM。"); return; }
    if (vm_) vm_->Invalidate();
    // Host 析构调用 Close；尚未证明退出的模块按 SDK 规则保活，绝不强制卸载。
    host_.reset();
    vm_.reset();
    // 保留 thread_ / generation_，旧 ID 和旧登记快照不能让会话重新附着。
}
void NativeSession::Reset() {
    std::lock_guard lock(mutex_);
    if (thread_ != std::this_thread::get_id()) return;
    Stop();
}
void NativeSession::BeforeVmDestroy(std::uintptr_t vm) {
    std::lock_guard lock(mutex_);
    if (thread_ != std::this_thread::get_id() || !vm_ || !vm_->IsMainVm(vm)) return;
    Stop();
}
}
