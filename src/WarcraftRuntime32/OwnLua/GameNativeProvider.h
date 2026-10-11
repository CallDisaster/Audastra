#pragma once

#include "NativeProvider.h"
#include "NativeRegistry.h"
#include "NativeTypeHierarchy.h"
#include "GameVm127.h"
#include <set>

namespace Audastra::Runtime32::OwnLua {

// 只消费游戏实际观察到的登记与脚本来源。尚未声明 code 回调能力时 binder 会拒绝 code 参数。
class GameNativeProvider final : public NativeProvider {
public:
    GameNativeProvider(std::shared_ptr<GameVm127> vm, std::shared_ptr<const NativeRegistry> registry,
        std::shared_ptr<const NativeTypeHierarchy> types, bool codeDispatchReady = false);
    [[nodiscard]] std::uint64_t Generation() const noexcept override;
    [[nodiscard]] std::optional<NativeSignature> Lookup(std::string_view name) const override;
    [[nodiscard]] std::optional<NativeGlobalDeclaration> LookupGlobal(std::string_view name) const override;
    [[nodiscard]] NativeCallResult ReadGlobal(std::string_view name) override;
    [[nodiscard]] bool IsHandleType(std::string_view actual, std::string_view expected) const noexcept override;
    [[nodiscard]] std::optional<NativeCallbackReturn> CallbackReturn(std::string_view name, std::size_t argument) const override;
    [[nodiscard]] NativeCallResult Invoke(std::string_view name, std::span<const NativeValue> arguments) override;
    [[nodiscard]] std::optional<NativeCallbackResult> DispatchCode(std::uint32_t vmIndex, std::uint32_t code, bool parameters);
    void BeforeVmDestroy(std::uintptr_t vm) noexcept;
    [[nodiscard]] std::uint64_t InvocationCount() const noexcept { return invocationCount_; }
private:
    [[nodiscard]] NativeCallResult ReadHandle(std::uint32_t value, const NativeType& type);
    std::shared_ptr<GameVm127> vm_;
    std::shared_ptr<const NativeRegistry> registry_;
    std::shared_ptr<const NativeTypeHierarchy> types_;
    std::set<std::string, std::less<>> scalarTypes_;
    std::unique_ptr<NativeCodeRegistry> codes_;
    std::uint64_t callbackFailures_ = 0;
    std::uint64_t invocationCount_ = 0;
    std::string callbackError_;
};

} // namespace Audastra::Runtime32::OwnLua
