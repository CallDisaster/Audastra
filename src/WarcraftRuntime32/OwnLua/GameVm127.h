#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Audastra::Runtime32::OwnLua {

class GameImage127;

// 原创 1.27a ABI 适配。只在已核对映像的真实脚本主线程入口附着，不在 DLL attach 调用。
// 创建成功只证明身份/入口数据成立，不证明地图或全部 native 兼容。
class GameVm127 final {
public:
    struct OpenResult { std::shared_ptr<GameVm127> vm; std::string error; };
    [[nodiscard]] static OpenResult OpenAtScriptEntry(HMODULE game, std::uint64_t generation);
    [[nodiscard]] static OpenResult OpenAtScriptEntry(std::shared_ptr<GameImage127> image, std::uint64_t generation);
    ~GameVm127();
    GameVm127(const GameVm127&) = delete;
    GameVm127& operator=(const GameVm127&) = delete;
    [[nodiscard]] bool Alive() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    // 会话所有者必须在地图卸载入口、异常或来源变化时调用，绝不能跨局复用。
    void Invalidate() noexcept;
    [[nodiscard]] std::uintptr_t Base() const noexcept;
    [[nodiscard]] std::uintptr_t Vm() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> InternString(std::string_view text) const;
    [[nodiscard]] std::uintptr_t StringObject(std::uint32_t id) const noexcept;
    [[nodiscard]] std::optional<std::string> ReadString(std::uint32_t id) const;
    [[nodiscard]] static std::optional<std::string> ReadStringObject(std::uintptr_t object);
    void ReleaseString(std::uint32_t id) const noexcept;
    [[nodiscard]] std::uintptr_t HandleObject(std::uint32_t id) const noexcept;
    [[nodiscard]] bool HandleAlive(std::uint32_t id, std::uintptr_t object) const noexcept;
    [[nodiscard]] bool RetainHandle(std::uint32_t id) const noexcept;
    void ReleaseHandle(std::uint32_t id, std::uintptr_t object) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> RegisterCode(std::uintptr_t token) const noexcept;
    void ReleaseCode(std::uint32_t id) const noexcept;
private:
    struct Impl;
    explicit GameVm127(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace Audastra::Runtime32::OwnLua
