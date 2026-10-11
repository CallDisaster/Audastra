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
        std::shared_ptr<const NativeTypeHierarchy> types);
    [[nodiscard]] std::uint64_t Generation() const noexcept override;
    [[nodiscard]] std::optional<NativeSignature> Lookup(std::string_view name) const override;
    [[nodiscard]] bool IsHandleType(std::string_view actual, std::string_view expected) const noexcept override;
    [[nodiscard]] NativeCallResult Invoke(std::string_view name, std::span<const NativeValue> arguments) override;
private:
    std::shared_ptr<GameVm127> vm_;
    std::shared_ptr<const NativeRegistry> registry_;
    std::shared_ptr<const NativeTypeHierarchy> types_;
    std::set<std::string, std::less<>> scalarTypes_;
};

} // namespace Audastra::Runtime32::OwnLua
