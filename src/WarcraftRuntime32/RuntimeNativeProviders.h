#pragma once

#include "OwnLua/GameNativeObserver.h"
#include "RuntimeProtocol.h"

namespace Audastra::Runtime32 {
class RuntimeNativeProviders final {
public:
    explicit RuntimeNativeProviders(SharedEvidence& evidence) : evidence_(evidence) {}
    [[nodiscard]] std::string Load();
    [[nodiscard]] OwnLua::NativeExtensionHooks Hooks();
    [[nodiscard]] std::string InitializeYdwe();
private:
    [[nodiscard]] std::string Validate(std::uintptr_t address, std::string_view name, std::string_view signature) const;
    [[nodiscard]] std::string Register(const OwnLua::RegisterObservedNative& add);
    [[nodiscard]] std::string Verify(const OwnLua::NativeRegistry& registry);
    SharedEvidence& evidence_;
    std::array<HMODULE, kProviderModuleLimit> modules_{};
};
}
