#pragma once

#include "GameUiAbi127.h"
#include "GameUiLibrary.h"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace Audastra::Runtime32::GameUi {

// ABI只对应已核验的经典1.27a映像；单位参数是JASS句柄，绝不是框架token或地址。
namespace InputRva127 {
inline constexpr std::uint32_t ResolveAgent = 0x1D1550;
inline constexpr std::uint32_t IsDerivedType = 0x02FBF0;
inline constexpr std::uint32_t ClickTargetWidget = 0x3A62A0;
inline constexpr std::uint32_t SelectVtable = 0x98E0EC;
inline constexpr std::uint32_t TargetVtable = 0x98BE80;
inline constexpr std::uint32_t CurrentMode = 0x1B4;
inline constexpr std::uint32_t TargetMode = 0x210;
inline constexpr std::uint32_t SelectMode = 0x214;
inline constexpr std::uint32_t ReleaseFlags = 0x14;
inline constexpr std::uint32_t RuntimeTypeSlot = 0x1C;
inline constexpr std::uint32_t UnitTypeId = 0x30;
inline constexpr std::uint32_t UnitRuntimeType = 0x2B773373;
} // namespace InputRva127

using FnResolveInputAgent = Address(__fastcall*)(std::uint32_t handle);
using FnInputDerivedType = int(__fastcall*)(std::uint32_t actual, std::uint32_t expected);
using FnClickTargetWidget = int(__fastcall*)(Address mode, int edx, Address widget, int button, Address event);

struct GameInputEntryPoints {
    FnGetGameUi getGameUi = nullptr;
    FnResolveInputAgent resolveAgent = nullptr;
    FnInputDerivedType isDerivedType = nullptr;
    FnClickTargetWidget clickTargetWidget = nullptr;
    Address selectVtable = 0;
    Address targetVtable = 0;
};

// 与界面表独立：输入能力缺失不应让已经可用的绘制能力停用。
[[nodiscard]] bool ResolveGameInputEntryPoints127(Address base,
    const std::function<bool(Address, std::size_t)>& executable,
    GameInputEntryPoints& out, std::string& reason);

struct UnitClickResult {
    std::int32_t status = 0;
    std::int32_t value = -1;
    std::string_view reason;
};

class GameInput127 {
public:
    explicit GameInput127(const GameInputEntryPoints& entries, UiFaultObserver observer = nullptr) noexcept;
    // 一次同步调用，不保存单位、命令或目标模式，不在稍后的轮询重放。
    [[nodiscard]] UnitClickResult ClickUnit(std::int32_t handle, std::uint32_t epoch, std::uint32_t request) noexcept;
    void Stop() noexcept { stopped_ = true; }

private:
    [[nodiscard]] UnitClickResult ClickUnitImpl(std::int32_t handle);
    GameInputEntryPoints entries_;
    UiFaultObserver observer_ = nullptr;
    std::uint32_t thread_ = 0;
    bool stopped_ = false;
    bool inFlight_ = false;
    bool faulted_ = false;
};

} // namespace Audastra::Runtime32::GameUi
