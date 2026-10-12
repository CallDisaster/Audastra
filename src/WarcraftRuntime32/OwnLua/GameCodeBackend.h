#pragma once
#include "NativeCodeRegistry.h"
#include "GameVm127.h"

namespace Audastra::Runtime32::OwnLua {
class GameCodeBackend final : public NativeCodeBackend {
public:
    explicit GameCodeBackend(std::shared_ptr<GameVm127> vm) : vm_(std::move(vm)) {}
    std::uint64_t Generation() const noexcept override { return vm_ ? vm_->Generation() : 0; }
    std::optional<std::uint32_t> Register(std::uintptr_t token) override { return vm_ ? vm_->RegisterCode(token) : std::nullopt; }
    void Release(std::uint32_t code) noexcept override { if (vm_) vm_->ReleaseCode(code); }
    std::uintptr_t Resolve(std::uint32_t vmIndex, std::uint32_t code) const noexcept override { return vm_ ? vm_->ResolveCode(vmIndex, code) : 0; }
    bool OwnerAlive(const NativeCodeOwner& owner) const noexcept override { return vm_ && vm_->CodeOwnerAlive(owner); }
private:
    std::shared_ptr<GameVm127> vm_;
};
}
