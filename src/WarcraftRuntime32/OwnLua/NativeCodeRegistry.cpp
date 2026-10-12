#include "NativeCodeRegistry.h"

#include <windows.h>
#include <map>
#include <atomic>
#include <mutex>
#include <vector>

namespace Audastra::Runtime32::OwnLua {
namespace {
struct CodeState {
    struct Entry { std::uint32_t id; std::weak_ptr<NativeCodeLease> lease; };
    std::shared_ptr<NativeCodeBackend> backend;
    std::map<std::uintptr_t, Entry> codes;
    mutable std::mutex mutex;
    std::vector<std::uint32_t> pendingRelease;
    DWORD thread = GetCurrentThreadId();
    std::uint64_t generation = 0;
    std::atomic<bool> active{true};
    bool Alive() const noexcept {
        return active && thread == GetCurrentThreadId() && backend && generation && backend->Generation() == generation;
    }
    void Drain() {
        if (thread != GetCurrentThreadId()) return;
        std::vector<std::uint32_t> pending;
        { std::lock_guard lock(mutex); pending.swap(pendingRelease); }
        if (backend && backend->Generation() == generation) for (const auto id : pending) backend->Release(id);
    }
};
}
struct NativeCodeLease::Impl {
    std::shared_ptr<CodeState> state;
    std::shared_ptr<NativeCallback> callback;
    std::uint32_t id = 0;
};
NativeCodeLease::NativeCodeLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeCodeLease::~NativeCodeLease() {
    auto& self = *impl_;
    {
        std::lock_guard lock(self.state->mutex);
        self.state->codes.erase(reinterpret_cast<std::uintptr_t>(this));
        if (self.id && self.state->thread != GetCurrentThreadId()) self.state->pendingRelease.push_back(self.id);
    }
    if (self.id && self.state->thread == GetCurrentThreadId() && self.state->backend
        && self.state->backend->Generation() == self.state->generation) self.state->backend->Release(self.id);
}
std::uint32_t NativeCodeLease::Id() const noexcept { return impl_->state->Alive() ? impl_->id : 0; }
struct NativeCodeRegistry::Impl {
    std::shared_ptr<CodeState> state;
    std::map<NativeCodeOwner, std::shared_ptr<NativeCodeLease>> owners;
    std::optional<NativeCodeOwner> after;
};
NativeCodeRegistry::NativeCodeRegistry(std::shared_ptr<NativeCodeBackend> backend) : impl_(std::make_unique<Impl>()) {
    impl_->state = std::make_shared<CodeState>();
    impl_->state->pendingRelease.reserve(4096);
    impl_->state->backend = std::move(backend);
    if (impl_->state->backend) impl_->state->generation = impl_->state->backend->Generation();
}
NativeCodeRegistry::~NativeCodeRegistry() { impl_->state->Drain(); impl_->state->active = false; }
NativeCodeCreate NativeCodeRegistry::Create(std::shared_ptr<NativeCallback> callback) {
    auto& self = *impl_;
    if (!callback || !self.state->Alive()) return {{}, "游戏 code 登记需要当前线程、有效地图和 Lua 闭包。"};
    Sweep();
    if (CodeCount() >= 4096) return {{}, "存活的游戏 Lua code 引用超过 4096 个。"};
    auto value = std::make_unique<NativeCodeLease::Impl>();
    value->state = self.state; value->callback = std::move(callback);
    auto lease = std::shared_ptr<NativeCodeLease>(new NativeCodeLease(std::move(value)));
    const auto token = reinterpret_cast<std::uintptr_t>(lease.get());
    const auto id = self.state->backend->Register(token);
    if (!id || !*id) return {{}, "实际 VM 未接受 Lua code 的唯一身份。"};
    lease->impl_->id = *id;
    { std::lock_guard lock(self.state->mutex); self.state->codes.emplace(token, CodeState::Entry{*id, lease}); }
    return {std::move(lease), {}};
}
bool NativeCodeRegistry::RetainFor(const NativeCodeOwner& owner, std::shared_ptr<NativeCodeLease> lease) {
    auto& self = *impl_;
    if (!self.state->Alive() || !owner.object || !self.state->backend->OwnerAlive(owner)
        || !lease || lease->impl_->state != self.state || !lease->Id()) return false;
    if (!self.owners.contains(owner) && self.owners.size() >= 4096) return false;
    self.owners.insert_or_assign(owner, std::move(lease));
    return true;
}
void NativeCodeRegistry::Forget(const NativeCodeOwner& owner) {
    if (impl_->state->thread == GetCurrentThreadId()) impl_->owners.erase(owner);
}
void NativeCodeRegistry::Sweep(std::size_t maxOwners) {
    auto& self = *impl_;
    if (!self.state->Alive() || !maxOwners) return;
    self.state->Drain();
    auto current = self.after ? self.owners.upper_bound(*self.after) : self.owners.begin();
    if (current == self.owners.end()) current = self.owners.begin();
    for (std::size_t count = 0; current != self.owners.end() && count < maxOwners; ++count) {
        self.after = current->first;
        if (!self.state->backend->OwnerAlive(current->first)) current = self.owners.erase(current);
        else ++current;
    }
}
std::optional<NativeCallbackResult> NativeCodeRegistry::Dispatch(std::uint32_t vmIndex, std::uint32_t code, bool hasParameters) {
    auto& self = *impl_;
    if (!self.state->Alive()) {
        std::lock_guard lock(self.state->mutex);
        for (const auto& [token, stored] : self.state->codes)
            if (stored.id == code) return NativeCallbackResult{false, false, "Lua code 所属地图或线程已经失效。"};
        return {};
    }
    self.state->Drain();
    const auto token = self.state->backend->Resolve(vmIndex, code);
    std::shared_ptr<NativeCodeLease> lease;
    {
        std::lock_guard lock(self.state->mutex);
        const auto found = self.state->codes.find(token);
        if (found == self.state->codes.end()) return {};
        lease = found->second.lease.lock();
    }
    // 回调可以在执行中销毁自身、替换 timer、清空 trigger；保持局部持有至调用返回。
    if (!lease || lease->Id() != code) return NativeCallbackResult{false, false, "游戏 Lua code 身份已失效。"};
    if (hasParameters) return NativeCallbackResult{false, false, "无参 Lua code 收到未知 JASS 参数表。"};
    try { return lease->impl_->callback->Invoke(); }
    catch (const std::exception& error) { return NativeCallbackResult{false, false, error.what()}; }
    catch (...) { return NativeCallbackResult{false, false, "游戏 Lua code 执行发生异常。"}; }
}
std::size_t NativeCodeRegistry::CodeCount() const noexcept { std::lock_guard lock(impl_->state->mutex); return impl_->state->codes.size(); }
std::size_t NativeCodeRegistry::OwnerCount() const noexcept { return impl_->owners.size(); }
} // namespace Audastra::Runtime32::OwnLua
