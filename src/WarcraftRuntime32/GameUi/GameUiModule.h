#pragma once

#include "GameUiAbi127.h"
#include "GameUiLibrary.h"
#include "GameInput127.h"
#include "GameUiDataPlane.h"

#include "aj/Host.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// 自有界面库的内置 JAPI v2 模块：命令表与 AudastraUI.j 的调用一一对应，
// 由内置运行时在 JAPI 宿主建好后装入，地图侧用 AjModuleLoad("audastraui") 取得。
// 模块随宿主（一个 VM 会话）存活；宿主重建时旧句柄与事件全部作废。
namespace Audastra::Runtime32::GameUi {

inline constexpr std::string_view kGameUiModuleName = "audastraui";
inline constexpr std::string_view kGameUiModuleVersion = "1.3.0";

// 名称、版本和命令顺序；装入时宿主按它逐项核对模块描述表。
[[nodiscard]] audastra::japi::ModuleContract GameUiModuleContract();

// 必须在宿主所属的游戏线程调用。入口不完整时照样装入：ready 为假，
// audastraui.error 报 unavailable 给出的原因，不假装界面可用。
[[nodiscard]] std::int32_t InstallGameUiModule(audastra::japi::Host& host, const GameUiEntryPoints& entries,
    std::string unavailable = {}, UiFaultObserver observer = nullptr, const GameInputEntryPoints& input = {},
    std::shared_ptr<GameUiDataPlane> data = {});

// 按已核验的 Game.dll 基址解析 1.27a 入口后装入。
[[nodiscard]] std::int32_t InstallGameUiModule127(audastra::japi::Host& host, Address base,
    const std::function<bool(Address, std::size_t)>& executable, UiFaultObserver observer = nullptr,
    std::shared_ptr<GameUiDataPlane> data = {}, std::function<void(Address, Address, bool, float, float)> observePointer = {});

} // namespace Audastra::Runtime32::GameUi
