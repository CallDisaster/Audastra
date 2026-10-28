#pragma once
#include "War3MapCore/Types.h"
#include <functional>
#include <memory>
#include <string>
#include <cstdint>
namespace War3Map::Services {
struct RuntimeTestSnapshot {
    std::string sessionNonce, mapSha256, runtimeSha256, expectedSha256, state = "waiting_for_module";
    std::uint32_t processId = 0, epoch = 0;
    std::uint64_t processCreated = 0, loadedMs = 0, installedMs = 0, helloMs = 0, reportMs = 0;
    std::int32_t status = 8;
    bool moduleLoaded = false, handshakeSuccess = false, hostDeadline = false;
};
class RuntimeTestSession final {
public:
    ~RuntimeTestSession();
    static Result<std::shared_ptr<RuntimeTestSession>> Create(std::string mapSha256,
        std::function<bool(std::uint32_t, std::uint64_t)> ownsProcess);
    [[nodiscard]] const std::string& Nonce() const;
    [[nodiscard]] Result<RuntimeTestSnapshot> Query() const;
private:
    struct Impl;
    explicit RuntimeTestSession(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
