#include "GameSession.h"
#include "../OwnLua/GameVm127.h"
#include <utility>

namespace Audastra::Runtime32::Japi {
namespace {
class GameStringVm final : public StringVm {
public:
    explicit GameStringVm(std::shared_ptr<OwnLua::GameVm127> vm) : vm_(std::move(vm)) {}
    std::uint64_t Generation() const noexcept override { return vm_->Generation(); }
    bool IsMainVm(std::uintptr_t vm) const noexcept override { return vm_->IsMainVm(vm); }
    bool MatchesCurrentMainVm() const noexcept override { return vm_->MatchesCurrentMainVm(); }
    std::optional<std::uint32_t> ReturnString(std::string_view text) override {
        // 与现有 provider 的 native string 返回 ABI 对称：输入 object，返回带引用 ID。
        // 此引用转交原版 VM；不能像 Lua 读取 native 返回值后那样在这里 ReleaseString。
        return vm_->InternString(text);
    }
    void Invalidate() noexcept override { vm_->Invalidate(); }
private:
    std::shared_ptr<OwnLua::GameVm127> vm_;
};
}
AttachResult AttachGameStringVm(std::shared_ptr<OwnLua::GameImage127> image,
    std::uint64_t generation, BuiltinModules builtins) {
    auto opened = OwnLua::GameVm127::OpenAtScriptEntry(image, generation);
    if (!opened.vm) return {{}, std::move(opened.error), {}};
    AttachResult result{std::make_shared<GameStringVm>(std::move(opened.vm)), {}, {}};
    if (builtins) result.install = [image = std::move(image), builtins = std::move(builtins)](audastra::japi::Host& host) {
        return builtins(*image, host);
    };
    return result;
}
std::uint32_t DispatchGameString(NativeSession& session, const OwnLua::GameNativeContext& context,
    std::string_view input, const BuiltinModules& builtins) {
    return session.Dispatch(context.registrationGeneration, input, [&]() -> AttachResult {
        if (!context.image || !context.registry || !context.registry->Valid() || !context.registry->Sealed())
            return {{}, "JAPI 需要已核对映像及完整封存的 native 登记快照。"};
        return AttachGameStringVm(context.image, context.registrationGeneration, builtins);
    }).value_or(0);
}
}
