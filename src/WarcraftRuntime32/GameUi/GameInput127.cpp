#include "GameInput127.h"
#include "GameUiMemory.h"
#include "audastra_japi.h"

#include <limits>
#include <windows.h>

namespace Audastra::Runtime32::GameUi {
namespace {
using Detail::ReadWord;
using FnRuntimeType = std::uint32_t(__fastcall*)(Address);

bool Complete(const GameInputEntryPoints& e) noexcept {
    return e.getGameUi && e.resolveAgent && e.isDerivedType && e.clickTargetWidget
        && e.selectVtable && e.targetVtable;
}

int CaptureInputFault(EXCEPTION_POINTERS* information, UiFault& fault) noexcept {
    if (information && information->ExceptionRecord) {
        fault.code = information->ExceptionRecord->ExceptionCode;
        fault.address = reinterpret_cast<Address>(information->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

template <class Call>
bool GuardInput(Call& call, UiFault& fault) noexcept {
    __try {
        call();
        return true;
    } __except (CaptureInputFault(GetExceptionInformation(), fault)) {
        return false;
    }
}

UnitClickResult Failure(std::int32_t status, std::string_view reason) noexcept {
    return {status, -1, reason};
}

} // namespace

bool ResolveGameInputEntryPoints127(Address base,
    const std::function<bool(Address, std::size_t)>& executable,
    GameInputEntryPoints& out, std::string& reason) {
    out = {};
    const auto at = [&](std::uint32_t rva) -> Address {
        if (!base || rva > std::numeric_limits<Address>::max() - base) return 0;
        return base + rva;
    };
    for (const auto rva : {Rva127::GetGameUi, InputRva127::ResolveAgent,
             InputRva127::IsDerivedType, InputRva127::ClickTargetWidget}) {
        if (!executable || !at(rva) || !executable(at(rva), 1)) {
            reason = "单位目标输入不可用：固定版本入口不在已核验的代码段";
            return false;
        }
    }
    if (!at(InputRva127::SelectVtable) || !at(InputRva127::TargetVtable)) {
        reason = "单位目标输入不可用：固定版本地址溢出";
        return false;
    }
    out = {reinterpret_cast<FnGetGameUi>(at(Rva127::GetGameUi)),
        reinterpret_cast<FnResolveInputAgent>(at(InputRva127::ResolveAgent)),
        reinterpret_cast<FnInputDerivedType>(at(InputRva127::IsDerivedType)),
        reinterpret_cast<FnClickTargetWidget>(at(InputRva127::ClickTargetWidget)),
        at(InputRva127::SelectVtable), at(InputRva127::TargetVtable)};
    reason.clear();
    return true;
}

GameInput127::GameInput127(const GameInputEntryPoints& entries, UiFaultObserver observer) noexcept
    : entries_(entries), observer_(observer), thread_(GetCurrentThreadId()) {}

UnitClickResult GameInput127::ClickUnit(std::int32_t handle, std::uint32_t epoch, std::uint32_t request) noexcept {
    if (GetCurrentThreadId() != thread_) return Failure(AJ_THREAD, "单位目标输入只能在游戏线程调用");
    if (stopped_) return Failure(AJ_STALE, "单位目标输入所属会话已结束");
    if (faulted_) return Failure(AJ_INTERNAL, "原版目标输入曾发生异常，此会话的目标输入已停用");
    if (!Complete(entries_)) return Failure(AJ_UNSUPPORTED, "此运行库没有已核验的单位目标输入能力");
    if (inFlight_) return Failure(AJ_REENTRANT, "单位目标输入正在处理，拒绝重入");
    if (handle <= 0) return Failure(AJ_ARGUMENT, "单位句柄为空或无效");
    inFlight_ = true;
    UnitClickResult result;
    UiFault fault{};
    fault.epoch = epoch;
    fault.request = request;
    fault.entry = "audastraui.clickunit";
    auto invoke = [&] { result = ClickUnitImpl(handle); };
    const bool ok = GuardInput(invoke, fault);
    inFlight_ = false;
    if (ok) return result;
    faulted_ = true;
    if (observer_) observer_(fault);
    return Failure(AJ_INTERNAL, "原版单位目标输入异常；本次不重试、不下备用指令");
}

UnitClickResult GameInput127::ClickUnitImpl(std::int32_t handle) {
    const auto root = entries_.getGameUi(0, 0);
    if (!root) return Failure(AJ_STATE, "游戏界面尚未创建");
    Address current = 0, select = 0, target = 0, currentVtable = 0;
    if (!ReadWord(root + InputRva127::CurrentMode, current)
        || !ReadWord(root + InputRva127::SelectMode, select)
        || !ReadWord(root + InputRva127::TargetMode, target)
        || !current || !ReadWord(current, currentVtable))
        return Failure(AJ_UNSUPPORTED, "无法核验当前输入模式");

    // 句柄先经过引擎的表查找，再按引擎类型层级确认是单位，不能把整数当作地址。
    const auto unit = entries_.resolveAgent(static_cast<std::uint32_t>(handle));
    Address unitVtable = 0, getType = 0, typeId = 0;
    if (!unit || !ReadWord(unit, unitVtable) || !unitVtable
        || !ReadWord(unitVtable + InputRva127::RuntimeTypeSlot, getType) || !getType
        || !entries_.isDerivedType(reinterpret_cast<FnRuntimeType>(getType)(unit), InputRva127::UnitRuntimeType)
        || !ReadWord(unit + InputRva127::UnitTypeId, typeId) || !typeId)
        return Failure(AJ_ARGUMENT, "单位句柄无效、类型不符或单位已移除");

    if (current == select && currentVtable == entries_.selectVtable) return {AJ_OK, 0, {}};
    if (current != target || currentVtable != entries_.targetVtable) return {AJ_OK, 3, {}};
    Address flags = 0;
    if (!ReadWord(target + InputRva127::ReleaseFlags, flags))
        return Failure(AJ_UNSUPPORTED, "无法核验目标模式类型");
    const auto kind = flags & 0x3FF;
    // 2走选点分支；4/6才走原版单位目标验证。其他类型不转换为单位位置。
    if (kind != 4 && kind != 6) return {AJ_OK, 3, {}};

    // 原版单位点击链：左键类型1，空命中事件表示从单位自身取得位置。
    // 原版负责目标允许、错误提示、Shift队列、模式退出以及游戏网络命令。
    const int consumed = entries_.clickTargetWidget(target, 0, unit, 1, 0);
    if (!consumed) return Failure(AJ_UNSUPPORTED, "原版输入未消费此次单位目标请求");
    // 引擎的“已处理”包含原版已反馈的非法目标，不能把它报告为施法成功。
    return {AJ_OK, 1, {}};
}

} // namespace Audastra::Runtime32::GameUi
